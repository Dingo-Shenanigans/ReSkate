#include "style_editor.h"
#include "style_stage.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "style_internal.h"
#include "style_capture.h"
#include "style_layer.h"
#include "style_takes.h"
#include "trick_pad.h"
#include "trick_recorder.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Extension/Profile/local_profile.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Game/Build/supported_build.h"
#include "reskate_version.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace dingosdk::style_editor {
namespace {
using style::Clip;
using Phase = style::EditorLoad::Phase;
// The stand-in uses the last remote-player slot. No session uses that slot in solo play.
constexpr std::size_t stand_in_slot = multiplayer::max_remote_players - 1;
// The loop blends the last pose into the first over this time.
constexpr float loop_blend_ms = 250;
// A held clip eases to a new moment with this time constant, so a scrub or a frame step does not jump.
constexpr float ease_ms = 45;
constexpr float frame_ms = 1000.0f / 60.0f;
constexpr auto &menu_room = addr::style::menu_room;

struct State {
    std::mutex mutex;
    // Requests.
    std::optional<std::uint8_t> show; // the trick to show. 0 clears the stand-in
    bool hide{};
    std::uint8_t recording{}; // the trick that the current recording is for
    std::uint8_t record{};    // a recording of the local skater to start
    std::uint32_t flick_ms{}; // the flick of the recording
    std::int32_t local_flip{-1}; // the last flip trick the local skater did during the recording
    bool local_did{};            // the local skater did the recorded trick
    std::string failure;         // why the last take was not kept
    std::optional<Recorded> recorded; // the end of the last recording
    // One clip for each trick, recorded on the trick stage. Read from disk on first use.
    std::map<std::uint8_t, Clip> references;
    std::uint64_t on_disk{}, outdated{}; // one bit for each trick. outdated: a newer recording is available
    bool listed{}, record_always{};      // record_always: every clip counts as outdated (for development)
    std::uint64_t stage_until{};
    std::uint8_t framed{}; // the trick that last set the initial camera framing
    int forget{-1};   // the trick whose clip to delete. 0 is all, -1 is none
    std::uint64_t demos_wanted{}; // clips that the layer does not have yet, one bit for each trick
    // Playback, in milliseconds of the shown clip.
    std::optional<Clip> shown;
    bool playing{true};
    float speed{1.0f}; // a fraction of the recorded speed
    double started{}, last_tick{};
    float shown_ms{}, target_ms{}; // a held clip eases from shown_ms to target_ms
    float low_ms{}, high_ms{};     // the trick part of the shown clip: the flick and the end
    style::Pace pace{style::even_pace}; // the length of the shown clip's pop, fall and landing
    std::uint64_t next_cosmetics{};
    // Playback requests from the menu. `controls` guards only them, so that a request never waits for file work.
    std::mutex controls;
    std::optional<float> hold_request; // a timeline time
    std::optional<bool> play_request;  // false pauses
    int step_request{};
    std::optional<float> speed_request;
    bool spawned{}, dressed{}, anchored_on_stage{};
    std::string detail;
    style::EditorLoad load; // the load of the trick that the menu selected
    std::vector<style::JointDelta> rotations;
    // The editor camera orbits the stand-in.
    std::array<std::atomic<std::uint32_t>, 3> target{};
    std::atomic<std::uint32_t> yaw{std::bit_cast<std::uint32_t>(0.7f)}, pitch{std::bit_cast<std::uint32_t>(0.1f)},
        distance{std::bit_cast<std::uint32_t>(3.0f)}, height{std::bit_cast<std::uint32_t>(0.45f)}; // height: the camera target above the ground
    std::atomic<bool> targeted{};
    // The state of the stand-in, for the menu timeline.
    std::atomic<std::uint32_t> shown_trick{}, shown_time{};
    std::atomic<bool> shown_playing{}, wanted{};
    std::atomic<bool> screen{};               // the editor screen is open
    std::atomic<std::uint64_t> wanted_until{}; // the screen must open before this
};
State &state() { static auto *value = new State; return *value; }

// GetTickCount64 is too coarse to separate two frames. The layer's recording is on this clock too.
std::uint64_t clock_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Playback runs on fractions of a millisecond, so that each frame advances the clip by its real duration.
double playback_ms() noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::filesystem::path folder() { return profile::default_path().parent_path() / L"style-clips"; }
std::filesystem::path file_of(std::uint8_t trick) {
    return folder() / (std::string(style::flip_trick_names[trick]) + ".clip");
}
// A message that is not about the selected trick's load.
void tell(overlay::NoticeLevel level, const std::string &text) {
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: {}.", text);
    overlay::notify(level, "Style editor", text + ".");
}
// A take of `trick` was not kept.
void take_failed(State &s, std::uint8_t trick, std::string reason) {
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the take of the {} was not kept: {}.", style::flip_trick_names[trick], reason);
    s.failure = std::move(reason);
}
// The recipe of a recording with this flick, on this game build.
style::Recipe recipe(std::uint32_t flick_ms) {
    return {style::recipe_version, std::string(supported_build::steam_build_id), flick_ms, trick_recorder::still_ms};
}
// True if a clip has the key of the recordings that `style record` makes now.
bool current(const style::Capture &capture) {
    static const auto key = style::recipe_key(recipe(trick_pad::default_flick_ms));
    return capture.key == key;
}
// The clips that a new recording would replace.
std::uint64_t stale(const State &s) { return s.record_always ? s.on_disk : s.outdated; }
// Finds the tricks that have a clip on disk, and the clips that are outdated.
void list(State &s) {
    s.listed = true;
    s.record_always = profile_runtime::local_preference("Style.RecordAlways").value_or(false);
    for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick) {
        std::ifstream file(file_of(trick), std::ios::binary);
        std::array<std::uint8_t, style::clip_header_bytes> head{};
        if (!file.read(reinterpret_cast<char *>(head.data()), head.size())) continue;
        const auto capture = style::clip_capture(head);
        if (!capture) continue;
        s.on_disk |= 1ull << trick;
        if (!current(*capture)) s.outdated |= 1ull << trick;
    }
    s.demos_wanted = s.on_disk;
}
// Copies the trucks and wheels of the game's board into the shown board. Returns the anchor distance on the first frame, or -1 if unused.
float use_game_board(Clip &clip) {
    float apart = -1;
    for (auto &frame : clip.frames) {
        auto &board = frame.pose.board;
        if (frame.rig.size() < 3 || board.size() != frame.rig.size() + 1) return -1;
        if (apart < 0) {
            const auto &a = frame.rig[1].position, &b = board[2].position;
            apart = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }
        // The root and the anchor place the board, so start after them. The shown board keeps its scale.
        for (std::size_t joint = 2; joint < frame.rig.size(); ++joint) {
            board[joint + 1].position = frame.rig[joint].position;
            board[joint + 1].rotation = frame.rig[joint].rotation;
        }
    }
    if (apart < 0) return apart;
    // The board turns with the game's board, with its offset at the first frame. The skater's deck joint turns back after a trick
    // that turns the board half a turn, so the board would swing back on the catch; the game's board lands turned.
    const auto to_board = style::compose(style::inverse(clip.frames.front().rig[1]), clip.frames.front().pose.board.front());
    for (auto &frame : clip.frames) {
        auto &board = frame.pose.board;
        const auto from_deck = board.front();
        auto turned = style::compose(frame.rig[1], to_board);
        turned.scale = from_deck.scale;
        auto anchor = style::compose(turned, style::compose(style::inverse(from_deck), board[2]));
        anchor.scale = board[2].scale;
        board.front() = turned;
        board[2] = anchor;
    }
    return apart;
}
const Clip *reference(State &s, std::uint8_t trick) {
    if (const auto found = s.references.find(trick); found != s.references.end()) return &found->second;
    if (!(s.on_disk >> trick & 1)) return nullptr;
    try {
        std::ifstream file(file_of(trick), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto clip = style::decode_clip({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
        if (clip.trick != trick) throw std::runtime_error("it is for another trick");
        (void)use_game_board(clip);
        return &(s.references[trick] = std::move(clip));
    } catch (const std::exception &failure) {
        s.on_disk &= ~(1ull << trick);
        if (s.load.trick == trick) s.load = {trick, Phase::failed, std::format("the saved clip could not be read ({})", failure.what())};
        return nullptr;
    }
}
void keep(State &s, Clip clip) {
    const auto trick = clip.trick;
    try {
        const auto bytes = style::encode_clip(clip);
        std::error_code error;
        std::filesystem::create_directories(folder(), error);
        auto temporary = file_of(trick);
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.flush();
            if (!file) throw std::runtime_error("the file write failed");
        }
        if (!MoveFileExW(temporary.c_str(), file_of(trick).c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("the file replace failed");
        s.on_disk |= 1ull << trick;
        s.outdated = current(clip.capture) ? s.outdated & ~(1ull << trick) : s.outdated | 1ull << trick;
    } catch (const std::exception &failure) {
        tell(overlay::NoticeLevel::warning, std::format("Could not save the {} clip: {}", style::flip_trick_titles[trick], failure.what()));
    }
    s.references[trick] = std::move(clip);
    // The layer rebuilds its demo frames from the current clips.
    style_layer::clear_demos();
    s.demos_wanted = s.on_disk;
}
// The sample nearest to `when` within 60 ms, or null. Samples have a millisecond time `at`.
template <class Sample> const Sample *nearest_at(const std::vector<Sample> &samples, std::uint32_t when) noexcept {
    const Sample *nearest{};
    std::uint32_t apart = 60;
    for (const auto &sample : samples)
        if (const auto gap = sample.at > when ? sample.at - when : when - sample.at; gap < apart) {
            apart = gap;
            nearest = &sample;
        }
    return nearest;
}
// Makes the trick's clip from the layer's recording of the local skater and the boards.
void take(State &s, const multiplayer::NativeFrame &local, const std::vector<std::uint8_t> &recorded) {
    const auto trick = std::exchange(s.recording, std::uint8_t{});
    const auto rig = style_layer::rig();
    if (!trick) return;
    if (!rig.parents) return take_failed(s, trick, "the skater skeleton is not read yet");
    struct Bone {
        std::array<float, 4> scale, rotation, position;
    };
    std::vector<style::RigFrame> skater;
    std::map<std::uint64_t, std::vector<style::RigFrame>> boards;
    for (std::size_t at = 0; at + 24 <= recorded.size();) {
        std::uint64_t header[3];
        std::memcpy(header, recorded.data() + at, sizeof(header));
        at += sizeof(header);
        const auto count = static_cast<std::size_t>(header[2]);
        if (count > 512 || at + count * sizeof(Bone) > recorded.size()) break;
        // A small rig is a skateboard. Its recorded scale can be the hidden scale, so it gets 1.
        const bool is_skater = count == rig.parents->size();
        if (is_skater || (count >= 2 && count <= 32)) {
            style::RigFrame frame{static_cast<std::uint32_t>(header[0]), {}};
            frame.joints.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                Bone bone;
                std::memcpy(&bone, recorded.data() + at + i * sizeof(Bone), sizeof(Bone));
                frame.joints.push_back({{bone.position[0], bone.position[1], bone.position[2]}, style::normalized(bone.rotation),
                                        is_skater ? std::array<float, 3>{bone.scale[0], bone.scale[1], bone.scale[2]} : std::array<float, 3>{1, 1, 1}});
            }
            (is_skater ? skater : boards[header[1]]).push_back(std::move(frame));
        }
        at += count * sizeof(Bone);
    }
    if (skater.empty()) return take_failed(s, trick, "the skater was not recorded");
    if (!s.local_did || s.local_flip != trick) {
        const bool named = s.local_flip > 0 && s.local_flip < static_cast<std::int32_t>(style::flip_trick_names.size());
        return take_failed(s, trick, named ? std::format("the skater did a {}", style::flip_trick_names[static_cast<std::size_t>(s.local_flip)])
                                           : std::string("the skater did no flip trick"));
    }
    // The clip borrows the player's board. A measured deck offset of 1 m or more is ignored.
    const style::RigJoints joints{rig.trajectory, rig.deck, rig.left_foot, rig.right_foot, *rig.parents};
    style::Transform deck_to_board;
    if (!local.pose.board.empty() && local.pose.skater.size() == rig.parents->size()) {
        const auto measured = style::compose(style::inverse(style::world(local.pose.skater, *rig.parents, rig.deck)), local.pose.board.front());
        const auto &p = measured.position;
        const float apart = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Style editor: the player's board is {:.3f} m from the deck joint ({:.3f} {:.3f} {:.3f}, rotation {:.3f} {:.3f} {:.3f} {:.3f}).",
                     apart, p[0], p[1], p[2], measured.rotation[0], measured.rotation[1], measured.rotation[2], measured.rotation[3]);
        if (apart < 1.0f) deck_to_board = measured;
        deck_to_board.scale = {1, 1, 1};
    }
    std::string why;
    auto clip = style::clip_from_capture(trick, skater, joints, local.pose.board, deck_to_board, why);
    if (!clip) return take_failed(s, trick, why);
    // The game's board is the small rig nearest to the skater at the start of the clip.
    {
        const std::vector<style::RigFrame> *board{};
        float nearest = 3.0f;
        const auto &start = clip->frames.front().pose.root.position;
        for (const auto &[holder, frames] : boards)
            for (const auto &frame : frames) {
                if (frame.at + 40 < clip->began || frame.at > clip->began + 40 || frame.joints.size() < 2) continue;
                const auto &p = frame.joints[1].position;
                if (const float apart = std::sqrt((p[0] - start[0]) * (p[0] - start[0]) + (p[2] - start[2]) * (p[2] - start[2])); apart < nearest) {
                    nearest = apart;
                    board = &frames;
                }
            }
        std::size_t boarded{}, unboarded{clip->frames.size()}; // unboarded: the first frame with no board
        if (board)
            for (auto &frame : clip->frames) {
                const auto *best = nearest_at(*board, clip->began + frame.at);
                if (!best) {
                    unboarded = std::min(unboarded, static_cast<std::size_t>(&frame - clip->frames.data()));
                    continue;
                }
                frame.rig = best->joints;
                ++boarded;
            }
        if (boarded != clip->frames.size())
            for (auto &frame : clip->frames) frame.rig.clear();
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Style editor: {} of {} frames have the game's board ({} small rigs seen, nearest {:.2f} m, first frame without it {}).", boarded,
                     clip->frames.size(), boards.size(), board ? nearest : -1.0f, unboarded);
    }
    if (const float apart = use_game_board(*clip); apart >= 0)
        logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the board uses the game's trucks and wheels. Its anchor is {:.3f} m from the calculated anchor.", apart);
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: recorded the {} ({} frames).", style::flip_trick_names[trick], clip->frames.size());
    tell(overlay::NoticeLevel::info, std::format("Recorded the {}", style::flip_trick_titles[trick]));
    const auto made = recipe(s.flick_ms);
    clip->capture = {style::recipe_key(made), style::recipe_text(made), std::string(reskate_version), static_cast<std::uint64_t>(std::time(nullptr))};
    keep(s, std::move(*clip));
}
// Applies the menu's requests, then moves shown_ms: at recorded speed with a blend into each loop, or eased to the held moment.
void advance(State &s, double now) {
    const auto &clip = *s.shown;
    const float length = static_cast<float>(style::duration(clip));
    // A held clip stays inside the trick, where the timeline can show it.
    const float low = s.low_ms, high = s.high_ms;
    std::optional<float> hold;
    std::optional<bool> play;
    int step{};
    std::optional<float> speed;
    {
        std::lock_guard lock(s.controls);
        speed = std::exchange(s.speed_request, std::nullopt);
        hold = std::exchange(s.hold_request, std::nullopt);
        play = std::exchange(s.play_request, std::nullopt);
        step = std::exchange(s.step_request, 0);
    }
    const auto pause = [&] {
        if (!s.playing) return;
        s.playing = false;
        s.target_ms = std::clamp(s.shown_ms, low, high);
    };
    if (speed && *speed != s.speed) {
        // The clip continues from the shown moment.
        if (s.playing) s.started = now - s.shown_ms / *speed;
        s.speed = *speed;
    }
    if (play && *play && !s.playing) {
        s.playing = true;
        s.started = now - (s.shown_ms < length ? s.shown_ms : 0.0f) / s.speed;
    } else if (play && !*play) pause();
    if (hold) {
        pause();
        s.target_ms = style::ms_at(clip, *hold);
    }
    if (step) {
        pause();
        s.target_ms = std::clamp((std::round(s.target_ms / frame_ms) + static_cast<float>(step)) * frame_ms, low, high);
    }
    const auto elapsed = static_cast<float>(std::clamp(now - s.last_tick, 0.0, 100.0));
    s.last_tick = now;
    if (s.playing) s.shown_ms = static_cast<float>(std::fmod((now - s.started) * s.speed, static_cast<double>(length + loop_blend_ms)));
    else if (const float gap = s.target_ms - s.shown_ms; std::abs(gap) < 0.25f) s.shown_ms = s.target_ms;
    else s.shown_ms += gap * (1 - std::exp(-elapsed / ease_ms));
}
// The clip pose at `ms` with the current style.
style::Pose styled(State &s, float ms) {
    const auto &clip = *s.shown;
    const float time = style::time_at(clip, ms);
    auto pose = style::sample(clip, time);
    // Keyframes move at the clip's own pace, so their speed does not change where the parts meet.
    style_layer::rotations_at(clip.trick, std::clamp(time, 0.0f, style::trick_end), s.pace, s.rotations);
    for (const auto &delta : s.rotations)
        if (delta.joint < pose.skater.size())
            pose.skater[delta.joint].rotation = style::normalized(style::multiply(pose.skater[delta.joint].rotation, delta.rotation));
    return pose;
}
void remove(std::uintptr_t base, State &s) {
    s.shown_trick.store(0, std::memory_order_relaxed);
    s.targeted.store(false, std::memory_order_relaxed);
    if (!s.spawned) return;
    const multiplayer::PeerScope scope(stand_in_slot);
    multiplayer::remove_remote(base);
    style_layer::ignore_holder(0);
    s.spawned = s.dressed = false;
}
} // namespace

void request_show(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (trick && trick < style::flip_trick_names.size() && s.show != trick)
        logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the {} was picked.", style::flip_trick_names[trick]);
    // A new trick, or another try after a failure, starts a new load.
    if (trick != s.load.trick || s.load.phase == Phase::failed) s.load = {trick, trick ? Phase::loading : Phase::none, {}};
    s.show = trick;
    s.hide = false;
}
void request_hide() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.hide = true;
    s.show.reset();
    s.load = {};
    s.wanted.store(false, std::memory_order_relaxed);
}
void request_hold(float time) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.hold_request = std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, style::trick_end);
    s.step_request = 0;
}
void request_play(bool play) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.play_request = play;
    s.hold_request.reset();
    s.step_request = 0;
}
void request_step(int frames) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.step_request = std::clamp(s.step_request + frames, -600, 600);
}
void request_speed(float speed) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.speed_request = std::clamp(std::isfinite(speed) ? speed : 1.0f, 0.1f, 1.0f);
}
float camera_fov() noexcept { return state().targeted.load(std::memory_order_relaxed) ? addr::style::editor_camera_fov : 0.0f; }
void request_open() {
    auto &s = state();
    s.wanted_until.store(clock_ms() + 8000, std::memory_order_relaxed);
    s.wanted.store(true, std::memory_order_relaxed);
    std::lock_guard lock(s.mutex);
    if (!s.listed) list(s);
    if (const auto old = std::popcount(stale(s)))
        tell(overlay::NoticeLevel::info, std::format("{} trick(s) have a newer recording available. 'style record outdated' records them", old));
}
void screen_open(bool open) noexcept { state().screen.store(open, std::memory_order_relaxed); }
std::optional<Recorded> take_recorded() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.recorded, std::nullopt);
}
bool request_record(std::uint8_t trick, std::uint32_t flick_ms, std::string &error) {
    if (!trick || trick >= style::flip_trick_names.size()) {
        error = "That is not a flip trick.";
        return false;
    }
    if (!style_layer::enabled()) {
        error = "The style layer is off (style enabled 1).";
        return false;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.recording || s.record) {
        error = "A recording is already running.";
        return false;
    }
    s.record = trick;
    s.flick_ms = flick_ms;
    return true;
}
std::uint64_t to_record() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.listed) list(s);
    return (~s.on_disk | stale(s)) & ((1ull << style::flip_trick_names.size()) - 2);
}
void request_record_always(bool always) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.listed) list(s);
    s.record_always = always;
    profile_runtime::set_local_preference("Style.RecordAlways", always);
}
bool record_always() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.record_always;
}
void request_forget(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.forget = trick;
}
bool has_clip(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.listed) list(s);
    return trick < 64 && (s.on_disk >> trick & 1);
}
void request_orbit(float yaw, float pitch, float distance, float height) noexcept {
    auto &s = state();
    const auto add = [](std::atomic<std::uint32_t> &value, float by, float low, float high, bool wrap) {
        float next = std::bit_cast<float>(value.load(std::memory_order_relaxed)) + (std::isfinite(by) ? by : 0.0f);
        if (wrap) next = std::remainder(next, 6.2831853f);
        value.store(std::bit_cast<std::uint32_t>(std::clamp(next, low, high)), std::memory_order_relaxed);
    };
    add(s.yaw, yaw, -6.2831853f, 6.2831853f, true);
    add(s.pitch, pitch, -0.15f, 1.45f, false);
    add(s.distance, distance, 0.6f, 12.0f, false);
    add(s.height, height, 0.0f, 2.2f, false);
}
bool camera_pose(std::array<float, 16> &matrix) noexcept {
    auto &s = state();
    if (!s.targeted.load(std::memory_order_relaxed)) return false;
    const auto value = [](const std::atomic<std::uint32_t> &bits) { return std::bit_cast<float>(bits.load(std::memory_order_relaxed)); };
    return style_layer::orbit_camera(matrix, value(s.yaw), value(s.pitch), value(s.distance),
                                     {value(s.target[0]), value(s.target[1]), value(s.target[2])}, value(s.height));
}
style::Playhead playhead() noexcept {
    auto &s = state();
    const bool wanted = s.wanted.load(std::memory_order_relaxed);
    if (const auto trick = s.shown_trick.load(std::memory_order_relaxed))
        return {static_cast<std::uint8_t>(trick), std::bit_cast<float>(s.shown_time.load(std::memory_order_relaxed)), true,
                s.shown_playing.load(std::memory_order_relaxed), wanted};
    // While the editor is wanted, the timeline does not follow Skatepedia's skater.
    if (wanted) return {0, 0, false, false, true};
    return style_layer::replay_playhead();
}
void fill(style::StyleModel &model) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    model.clips = s.on_disk;
    model.outdated_clips = stale(s);
    model.editor_load = s.load;
    model.pace = s.pace;
}
std::string status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    std::string result;
    for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick)
        if (s.on_disk >> trick & 1)
            result += std::format("{}{}{}", result.empty() ? "Clips: " : ", ", style::flip_trick_names[trick], stale(s) >> trick & 1 ? " (outdated)" : "");
    if (result.empty()) result = "No clips yet";
    if (s.record_always) result += ". Every clip counts as outdated (style record always 1)";
    result += s.shown ? std::format(". Showing the {}, {}", style::flip_trick_names[s.shown->trick], s.playing ? "playing" : "held")
                      : std::string(". Nothing shown");
    if (!s.detail.empty()) result += " (" + s.detail + ")";
    if (s.load.phase == Phase::failed) result += std::format(". The {} did not load: {}", style::flip_trick_names[s.load.trick], s.load.reason);
    return result;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (!s.listed) list(s);
        // The editor screen did not open: the request for it ends, and the game leaves the menu.
        if (s.wanted.load(std::memory_order_relaxed) && !s.screen.load(std::memory_order_relaxed) && clock_ms() > s.wanted_until.load(std::memory_order_relaxed)) {
            s.wanted.store(false, std::memory_order_relaxed);
            style_stage::leave();
            tell(overlay::NoticeLevel::warning, "The editor screen did not open");
        }
        if (!ready || !base || !style_layer::enabled()) {
            remove(base, s);
            s.shown.reset();
            return;
        }
        // A recording of the local skater starts here, so that its end is collected below.
        if (const auto trick = std::exchange(s.record, std::uint8_t{}); trick && !s.recording) {
            s.recording = trick;
            s.local_flip = -1;
            s.local_did = false;
            style_layer::request_recording();
            logging::log(logging::Level::info, logging::Channel::skater, "Style editor: recording the skater's {}.", style::flip_trick_names[trick]);
        }
        // Nothing to show, record or forget: the full capture of the skater is not paid for.
        if (!s.shown && !s.show && !s.hide && !s.recording && !s.demos_wanted && s.forget < 0 && !s.wanted.load(std::memory_order_relaxed)) {
            (void)style_layer::collect_recording();
            return;
        }
        style_layer::watch_stage();
        // The take is kept only if the skater did the recorded trick.
        if (s.recording) {
            std::uint32_t physics{};
            std::int32_t flip{};
            if (style_layer::local_trick(physics, flip) && flip > 0) {
                s.local_flip = flip;
                s.local_did |= flip == s.recording;
            }
        }
        // The skater's bones are read only for a take. The stand-in needs the entity and the root.
        const auto local = multiplayer::capture_local(base, client, false);
        if (!local.ready) return;
        const auto now = clock_ms();
        // Each tick gives one clip to the layer, for the restyle of Skatepedia's skater.
        if (s.demos_wanted) {
            const auto trick = static_cast<std::uint8_t>(std::countr_zero(s.demos_wanted));
            if (style_layer::rig().parents) {
                s.demos_wanted &= s.demos_wanted - 1;
                if (const auto *clip = reference(s, trick))
                    for (const auto &frame : clip->frames)
                        if (frame.time >= 0 && frame.time <= style::trick_end) (void)style_layer::add_demo(trick, frame.time, frame.pose.skater);
            }
        }
        if (const auto recorded = style_layer::collect_recording(); !recorded.empty() && s.recording) {
            const auto trick = s.recording;
            s.failure.clear();
            take(s, multiplayer::capture_local(base, client, true), recorded);
            s.recorded = Recorded{trick, s.failure};
        }
        if (s.forget >= 0) {
            for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick) {
                if (s.forget && s.forget != trick) continue;
                std::error_code error;
                std::filesystem::remove(file_of(trick), error);
                s.references.erase(trick);
                s.on_disk &= ~(1ull << trick);
            }
            s.forget = -1;
            style_layer::clear_demos();
            s.demos_wanted = s.on_disk;
            tell(overlay::NoticeLevel::info, "The clip was deleted. Record it again with 'style record'");
        }
        if (s.hide) {
            s.hide = false;
            s.shown.reset();
        }
        if (s.show) {
            const auto trick = *s.show;
            s.show.reset();
            if (const Clip *clip = trick ? reference(s, trick) : nullptr) {
                if (s.load.trick == trick) s.load = {trick, Phase::ready, {}};
                s.shown = *clip;
                const float flick = style::ms_at(*clip, 0), caught = style::ms_at(*clip, 1), touchdown = style::ms_at(*clip, 2);
                s.low_ms = flick;
                s.high_ms = style::ms_at(*clip, style::trick_end);
                s.pace = {caught - flick, touchdown - caught, s.high_ms - touchdown};
                style_layer::note_editor_pace(s.pace);
                s.started = s.last_tick = playback_ms();
                s.shown_ms = s.target_ms = 0;
                s.playing = true;
                s.detail.clear();
            } else {
                s.shown.reset();
                s.detail = trick ? std::format("no clip of the {} yet", style::flip_trick_names[trick]) : std::string();
                // A trick picked on the editor screen is captured on the trick stage, in view.
                if (trick && s.load.trick == trick && s.load.phase != Phase::failed) {
                    std::string why = "the editor screen is not open";
                    if (s.screen.load(std::memory_order_relaxed) && style_capture::start(trick, why)) s.load = {trick, Phase::loading, {}};
                    else s.load = {trick, Phase::failed, std::format("it has no clip yet, and it cannot be captured now: {}", why)};
                }
            }
        }
        if (!s.shown || s.shown->frames.empty()) {
            remove(base, s);
            return;
        }
        // A skater in the stand-in's slot before the stand-in exists is a player: the lobby is full.
        const bool slot_taken = [] {
            const multiplayer::PeerScope scope(stand_in_slot);
            return multiplayer::remote_skater_entity() != 0;
        }();
        if (!s.spawned && slot_taken) {
            s.shown.reset();
            s.detail = "the lobby is full, so there is no slot for the stand-in";
            if (s.load.trick && s.load.phase == Phase::ready) s.load = {s.load.trick, Phase::failed, s.detail};
            return;
        }
        advance(s, playback_ms());
        // The clip pose with the current style. Each loop ends with a blend from the last pose into the first.
        const float length = static_cast<float>(style::duration(*s.shown));
        const float time = style::time_at(*s.shown, std::min(s.shown_ms, length));
        auto pose = s.shown_ms <= length ? styled(s, s.shown_ms) : [&] {
            const float amount = std::clamp((s.shown_ms - length) / loop_blend_ms, 0.0f, 1.0f);
            return style::blend(styled(s, length), styled(s, 0), amount * amount * (3 - 2 * amount));
        }();
        const auto &opening = s.shown->frames.front();
        // The camera starts with the editor's angle to the skater, farther back and lower, so that the whole skater shows at the top
        // of the pop. One time for each shown trick. The skater's facing, not the board's: a board can start a clip turned round.
        if (s.framed != s.shown->trick) {
            s.framed = s.shown->trick;
            s.yaw.store(std::bit_cast<std::uint32_t>(style_layer::editor_yaw(opening.pose.root.rotation)), std::memory_order_relaxed);
            s.pitch.store(std::bit_cast<std::uint32_t>(style_layer::editor_pitch), std::memory_order_relaxed);
            s.distance.store(std::bit_cast<std::uint32_t>(style_layer::editor_distance), std::memory_order_relaxed);
            s.height.store(std::bit_cast<std::uint32_t>(style_layer::editor_height), std::memory_order_relaxed);
        }
        s.shown_time.store(std::bit_cast<std::uint32_t>(std::clamp(time, 0.0f, style::trick_end)), std::memory_order_relaxed);
        s.shown_playing.store(s.playing, std::memory_order_relaxed);
        s.shown_trick.store(s.shown->trick, std::memory_order_relaxed);
        // The anchor is the menu room, or a position 2 m from the player.
        const auto &here = local.pose.root.position;
        // The stage state holds for 4 s, so that a moment without the room's skater does not move the stand-in.
        if (style_layer::stage_present()) s.stage_until = GetTickCount64() + 4000;
        const bool on_stage = GetTickCount64() < s.stage_until;
        if (std::exchange(s.anchored_on_stage, on_stage) != on_stage)
            logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the stand-in skates {}.", on_stage ? "in the menu room" : "beside the player");
        // In the menu room the clip's board starts on the room's floor, wherever the clip was recorded.
        const float lift = !on_stage ? here[1] - opening.pose.root.position[1]
                           : opening.pose.board.empty() ? 0.0f
                                                        : addr::style::menu_room_board_height - opening.pose.board.front().position[1];
        const auto anchor = on_stage ? std::array<float, 3>{menu_room[0], opening.pose.root.position[1] + lift, menu_room[2]}
                                     : std::array<float, 3>{here[0] + 2.0f, here[1], here[2]};
        // The stand-in skates in place, so the camera neither lags it nor jumps back at each loop.
        for (std::size_t i = 0; i < 3; ++i) s.target[i].store(std::bit_cast<std::uint32_t>(anchor[i]), std::memory_order_relaxed);
        s.targeted.store(true, std::memory_order_relaxed);
        multiplayer::offset_pose(pose, {anchor[0] - pose.root.position[0], lift, anchor[2] - pose.root.position[2]});
        const multiplayer::PeerScope scope(stand_in_slot);
        s.spawned = multiplayer::show_remote(base, client, local, pose, s.detail) || multiplayer::remote_skater_entity();
        if (const auto entity = multiplayer::remote_skater_entity())
            style_layer::ignore_holder(memory::peek_pointer(memory::peek_pointer(entity, addr::style::entity_component), addr::style::component_pose_holder),
                                       memory::peek_pointer(multiplayer::remote_board_entity(), addr::style::board_pose_holder));
        // In the menu room, the stand-in replaces the menu's skater.
        if (on_stage) style_layer::keep_stage_clear();
        // The stand-in wears the player's cosmetics.
        if (s.spawned && now >= s.next_cosmetics) {
            s.next_cosmetics = now + (s.dressed ? 3000 : 700);
            std::string detail;
            if (const auto appearance = multiplayer::capture_cosmetics(base, local, detail)) {
                multiplayer::update_remote_cosmetics(base, local, *appearance, detail);
                s.dressed = true;
            }
        }
    } catch (...) {
        std::lock_guard lock(s.mutex);
        s.detail = "could not show the stand-in";
    }
}
} // namespace dingosdk::style_editor
