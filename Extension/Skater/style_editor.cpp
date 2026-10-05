#include "style_editor.h"
#include "style_stage.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "style_internal.h"
#include "style_layer.h"
#include "style_takes.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Extension/Profile/local_profile.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
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
using style::ClipFrame;
// The stand-in uses the last remote-player slot. No session uses that slot in solo play.
constexpr std::size_t stand_in_slot = multiplayer::max_remote_players - 1;
constexpr std::uint64_t loop_pause_ms = 500;
// Measured 2026-10-04: the position of Skatepedia's skater on its stage.
constexpr auto &skatepedia_room = addr::style::skatepedia_stage, &skatepedia_reach = addr::style::skatepedia_stage_reach;

struct State {
    std::mutex mutex;
    // Requests.
    std::optional<std::uint8_t> show; // the trick to show. 0 clears the stand-in
    bool hide{}, playing{true};
    std::uint64_t retry_after{}; // the earliest time for the next learn attempt
    float hold_time{};
    int step{};
    std::uint8_t learn{}; // the trick that the current recording is for
    // One clip for each trick, learned from Skatepedia's demonstration. Read from disk on first use.
    std::map<std::uint8_t, Clip> references;
    std::uint64_t on_disk{}, learned{}; // one bit for each trick
    bool listed{};
    std::uint64_t stage_until{}, next_named{};
    // Samples of the game's camera, on the recording clock.
    struct View {
        std::uint32_t at{};
        std::array<float, 16> matrix{};
        float fov{};
    };
    std::vector<View> views;
    // The recorded fov of the shown clip. 0 when the clip has no camera.
    float view_fov{};
    std::uint8_t framed{}; // the trick that last set the initial camera framing
    std::uint8_t named{}; // the trick that Skatepedia highlights
    std::uint64_t next_park{};
    bool parked{}; // Skatepedia shows the board-only entry
    bool open{};   // Skatepedia was open at the last check
    std::uint8_t fetching{}; // the trick to show when its clip is learned
    std::uint64_t named_since{}, named_seen{};
    bool spoiled{};   // the highlight moved during the recording
    int forget{-1};   // the trick whose clip to delete. 0 is all, -1 is none
    std::uint64_t demos_wanted{}; // learned clips that the layer does not have yet, one bit for each trick
    // Playback.
    std::optional<Clip> shown;
    std::uint64_t started{}, next_cosmetics{};
    bool spawned{}, dressed{};
    std::string detail, note;
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

// GetTickCount64 is too coarse to separate two frames.
std::uint64_t clock_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || !memory::peek(object + offset, value) || value < 0x10000 || value > memory::highest_user_address - 0x10000) return 0;
    return value;
}
std::filesystem::path folder() { return profile::default_path().parent_path() / L"style-clips"; }
std::filesystem::path file_of(std::uint8_t trick) {
    return folder() / (std::string(style::flip_trick_names[trick]) + ".clip");
}
void say(State &s, std::string text) {
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: {}.", text);
    s.note = std::move(text);
}
// Finds the tricks that have a learned clip on disk.
void list(State &s) {
    s.listed = true;
    for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick) {
        std::ifstream file(file_of(trick), std::ios::binary);
        std::array<std::uint32_t, 4> head{}; // magic, version, trick, learned
        // Only a clip learned from Skatepedia's demonstration counts.
        if (!file.read(reinterpret_cast<char *>(head.data()), sizeof(head)) || !head[3] || head[1] != 3) continue;
        s.on_disk |= 1ull << trick;
        s.learned |= 1ull << trick;
    }
    s.demos_wanted = s.learned;
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
        // Remove the slowed parts from a clip that an older version saved.
        if (style::unwarp(clip)) (void)style::retime(clip);
        (void)use_game_board(clip);
        return &(s.references[trick] = std::move(clip));
    } catch (const std::exception &failure) {
        s.on_disk &= ~(1ull << trick);
        s.learned &= ~(1ull << trick);
        say(s, std::format("could not read the saved {} clip: {}", style::flip_trick_names[trick], failure.what()));
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
    } catch (const std::exception &failure) {
        say(s, std::format("could not save the {} clip: {}", style::flip_trick_names[trick], failure.what()));
    }
    if (clip.learned) s.learned |= 1ull << trick;
    else s.learned &= ~(1ull << trick);
    s.references[trick] = std::move(clip);
    // The layer rebuilds its demo frames from the current clips.
    style_layer::clear_demos();
    s.demos_wanted = s.learned;
}
// Makes the clip of the trick from the layer's recording of Skatepedia's demonstration.
void learn(State &s, const multiplayer::NativeFrame &local, const std::vector<std::uint8_t> &recorded) {
    const auto trick = std::exchange(s.learn, std::uint8_t{});
    const auto rig = style_layer::rig();
    if (!trick) return;
    if (!rig.parents) {
        say(s, "nothing was learned: the skater skeleton is not read yet");
        return;
    }
    struct Bone {
        std::array<float, 4> scale, rotation, position;
    };
    std::map<std::uint64_t, std::vector<style::RigFrame>> rigs, boards;
    for (std::size_t at = 0; at + 24 <= recorded.size();) {
        std::uint64_t header[3];
        std::memcpy(header, recorded.data() + at, sizeof(header));
        at += sizeof(header);
        const auto count = static_cast<std::size_t>(header[2]);
        if (count > 512 || at + count * sizeof(Bone) > recorded.size()) break;
        if (count == rig.parents->size()) {
            style::RigFrame frame{static_cast<std::uint32_t>(header[0]), {}};
            frame.joints.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                Bone bone;
                std::memcpy(&bone, recorded.data() + at + i * sizeof(Bone), sizeof(Bone));
                frame.joints.push_back({{bone.position[0], bone.position[1], bone.position[2]}, style::normalized(bone.rotation),
                                        {bone.scale[0], bone.scale[1], bone.scale[2]}});
            }
            rigs[header[1]].push_back(std::move(frame));
        } else if (count >= 2 && count <= 32) {
            // A small rig is a skateboard. Its recorded scale can be the hidden scale, so use 1.
            style::RigFrame frame{static_cast<std::uint32_t>(header[0]), {}};
            frame.joints.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                Bone bone;
                std::memcpy(&bone, recorded.data() + at + i * sizeof(Bone), sizeof(Bone));
                frame.joints.push_back({{bone.position[0], bone.position[1], bone.position[2]}, style::normalized(bone.rotation), {1, 1, 1}});
            }
            boards[header[1]].push_back(std::move(frame));
        }
        at += count * sizeof(Bone);
    }
    if (rigs.empty()) {
        say(s, "nothing was learned: no other skater was on screen. Open the editor on the trick first");
        return;
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
    // World skaters also jump. Skatepedia's stage is far from the player, so the farthest jumping skater is Skatepedia's.
    std::string why;
    std::optional<Clip> clip;
    float farthest = -1;
    for (const auto &[holder, frames] : rigs) {
        std::string reason;
        auto found = style::clip_from_capture(trick, frames, joints, local.pose.board, deck_to_board, reason);
        if (!found) {
            if (why.empty()) why = reason;
            continue;
        }
        const auto &at = found->frames.front().pose.root.position, &here = local.pose.root.position;
        // Accept only a skater on Skatepedia's stage.
        if (std::abs(at[0] - skatepedia_room[0]) > 2 * skatepedia_reach[0] || std::abs(at[1] - skatepedia_room[1]) > 2 * skatepedia_reach[1] ||
            std::abs(at[2] - skatepedia_room[2]) > 2 * skatepedia_reach[2]) {
            if (why.empty()) why = "the only jump seen was not on the stage";
            continue;
        }
        const float away = std::sqrt((at[0] - here[0]) * (at[0] - here[0]) + (at[1] - here[1]) * (at[1] - here[1]) + (at[2] - here[2]) * (at[2] - here[2]));
        if (away > farthest) {
            farthest = away;
            clip = std::move(found);
        }
    }
    if (!clip) {
        say(s, std::format("could not learn the {}: {}", style::flip_trick_names[trick], why));
        return;
    }
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: {} skater(s) were on screen. The learned skater was {:.0f} m away.",
                 rigs.size(), farthest);
    // Each frame gets the camera sample nearest to its time.
    std::size_t viewed{};
    if (!s.views.empty())
        for (auto &frame : clip->frames) {
            const auto when = clip->began + frame.at;
            const State::View *nearest{};
            std::uint32_t apart = 60;
            for (const auto &view : s.views)
                if (const auto gap = view.at > when ? view.at - when : when - view.at; gap < apart) {
                    apart = gap;
                    nearest = &view;
                }
            if (!nearest) continue;
            frame.view = nearest->matrix;
            frame.fov = nearest->fov;
            ++viewed;
        }
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
        std::size_t boarded{};
        if (board)
            for (auto &frame : clip->frames) {
                const auto when = clip->began + frame.at;
                const style::RigFrame *best{};
                std::uint32_t apart = 60;
                for (const auto &shot : *board)
                    if (const auto gap = shot.at > when ? shot.at - when : when - shot.at; gap < apart) {
                        apart = gap;
                        best = &shot;
                    }
                if (!best) continue;
                frame.rig = best->joints;
                ++boarded;
            }
        if (boarded != clip->frames.size())
            for (auto &frame : clip->frames) frame.rig.clear();
        logging::log(logging::Level::info, logging::Channel::skater, "Style editor: {} of {} frames have the game's board ({} small rigs seen, nearest {:.2f} m).", boarded,
                     clip->frames.size(), boards.size(), board ? nearest : -1.0f);
    }
    if (const float apart = use_game_board(*clip); apart >= 0)
        logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the board uses the game's trucks and wheels. Its anchor is {:.3f} m from the calculated anchor.", apart);
    // A clip keeps its camera only if every frame has one.
    if (viewed != clip->frames.size())
        for (auto &frame : clip->frames) frame.fov = 0;
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: {} of {} frames have a camera ({} samples).", viewed, clip->frames.size(), s.views.size());
    say(s, std::format("learned the {} from the game's demonstration ({} frames)", style::flip_trick_names[trick], clip->frames.size()));
    s.retry_after = 0;
    keep(s, std::move(*clip));
}
// The timeline time of the shown clip: held, or played at recorded speed with a pause before each loop.
float time_now(const State &s, std::uint64_t now) {
    if (!s.playing) return s.hold_time;
    const auto length = style::duration(*s.shown);
    const auto into = (now - s.started) % (length + loop_pause_ms);
    return style::time_at(*s.shown, static_cast<std::uint32_t>(std::min<std::uint64_t>(into, length)));
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
    s.show = trick;
    s.hide = false;
}
void request_hide() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.hide = true;
    s.show.reset();
    s.fetching = 0;
    s.wanted.store(false, std::memory_order_relaxed);
}
void request_hold(float time) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.playing = false;
    s.hold_time = std::clamp(std::isfinite(time) ? time : 0.0f, -2.0f, style::trick_end + 2.0f);
}
void request_play() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.playing = true;
    s.started = clock_ms();
}
void request_step(int frames) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.step += frames;
}
std::uint32_t recording_clock() noexcept {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000);
}
bool wants_view() noexcept { return style_layer::stage_present(); }
void note_view(const std::array<float, 16> &matrix, float fov) {
    for (const auto value : matrix)
        if (!std::isfinite(value)) return;
    if (!std::isfinite(fov) || fov < 1.0f || fov > 175.0f) return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.views.size() >= 4000) s.views.erase(s.views.begin(), s.views.begin() + 2000);
    s.views.push_back({recording_clock(), matrix, fov});
}
float camera_fov() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.view_fov;
}
void request_open() {
    auto &s = state();
    s.wanted_until.store(clock_ms() + 8000, std::memory_order_relaxed);
    s.wanted.store(true, std::memory_order_relaxed);
}
void screen_open(bool open) noexcept { state().screen.store(open, std::memory_order_relaxed); }
void request_forget(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.forget = trick;
}
void expect(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.fetching = trick;
}
bool has_clip(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.listed) list(s);
    return trick < 64 && (s.learned >> trick & 1);
}
void request_orbit(float yaw, float pitch, float distance, float height) {
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
    const float yaw = value(s.yaw), pitch = value(s.pitch), distance = value(s.distance);
    // Rows: right, up, backward, position. Backward points from the stand-in to the camera.
    const std::array<float, 3> back{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const auto cross = [](const std::array<float, 3> &a, const std::array<float, 3> &b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto right = cross({0, 1, 0}, back);
    const float length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (length < 1e-4f) return false;
    for (auto &axis : right) axis /= length;
    const auto up = cross(back, right);
    // The camera keeps the handedness it had.
    const auto had = cross({matrix[0], matrix[1], matrix[2]}, {matrix[4], matrix[5], matrix[6]});
    if (had[0] * matrix[8] + had[1] * matrix[9] + had[2] * matrix[10] < 0)
        for (auto &axis : right) axis = -axis;
    for (std::size_t i = 0; i < 3; ++i) {
        matrix[i] = right[i];
        matrix[4 + i] = up[i];
        matrix[8 + i] = back[i];
        matrix[12 + i] = value(s.target[i]) + (i == 1 ? value(s.height) : 0.0f) + back[i] * distance;
    }
    return true;
}
style::Playhead playhead() noexcept {
    auto &s = state();
    if (const auto trick = s.shown_trick.load(std::memory_order_relaxed))
        return {static_cast<std::uint8_t>(trick), std::bit_cast<float>(s.shown_time.load(std::memory_order_relaxed)), true,
                s.shown_playing.load(std::memory_order_relaxed), true};
    // While the editor is wanted, the timeline does not follow Skatepedia's skater.
    if (s.wanted.load(std::memory_order_relaxed)) return {0, 0, false, false, true};
    return style_layer::replay_playhead();
}
void fill(style::StyleModel &model) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    model.clips = s.on_disk;
    model.editor_session_test = style_layer::session_test();
    model.editor_note = s.note;
}
std::string status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    std::string result;
    for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick)
        if (s.on_disk >> trick & 1)
            result += std::format("{}{}{}", result.empty() ? "Clips: " : ", ", style::flip_trick_names[trick], s.learned >> trick & 1 ? " (demonstration)" : "");
    if (result.empty()) result = "No clips yet";
    result += s.shown ? std::format(". Showing the {}, {}", style::flip_trick_names[s.shown->trick], s.playing ? "playing" : "held")
                      : std::string(". Nothing shown");
    if (!s.detail.empty()) result += " (" + s.detail + ")";
    if (!s.note.empty()) result += ". " + s.note;
    return result;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (!s.listed) list(s);
        // The editor screen did not open: the request for it ends.
        if (s.wanted.load(std::memory_order_relaxed) && !s.screen.load(std::memory_order_relaxed) && clock_ms() > s.wanted_until.load(std::memory_order_relaxed))
            s.wanted.store(false, std::memory_order_relaxed);
        if (!ready || !base || !style_layer::enabled()) {
            remove(base, s);
            s.shown.reset();
            return;
        }
        // Nothing to show, load or learn: the full capture of the skater is not paid for.
        if (!s.shown && !s.show && !s.hide && !s.learn && !s.fetching && !s.demos_wanted && s.forget < 0 && !s.parked && !s.wanted.load(std::memory_order_relaxed)) {
            (void)style_layer::collect_learned();
            return;
        }
        const auto local = multiplayer::capture_local(base, client, true);
        if (!local.ready || local.pose.skater.size() < 2) return;
        const auto now = clock_ms();
        // Each tick gives one learned clip to the layer, for the restyle of Skatepedia's skater.
        if (s.demos_wanted) {
            const auto trick = static_cast<std::uint8_t>(std::countr_zero(s.demos_wanted));
            if (style_layer::rig().parents) {
                s.demos_wanted &= s.demos_wanted - 1;
                if (const auto *clip = reference(s, trick); clip && clip->learned)
                    for (const auto &frame : clip->frames)
                        if (frame.time >= 0 && frame.time <= style::trick_end) (void)style_layer::add_demo(trick, frame.time, frame.pose.skater);
            }
        }
        if (const auto recorded = style_layer::collect_learned(); !recorded.empty()) {
            if (std::exchange(s.spoiled, false)) {
                // The recording contains part of a different trick.
                say(s, std::format("could not learn the {}: the editor's stage changed to a different trick during the recording", style::flip_trick_names[s.learn]));
                s.learn = 0;
            } else {
                const auto wanted = s.learn;
                s.retry_after = now + 60000;
                learn(s, local, recorded);
                // If the learn of the fetched trick failed, record it again after 500 ms.
                if (wanted && wanted == s.fetching && !(s.learned >> wanted & 1)) s.retry_after = now + 500;
            }
        }
        if (s.forget >= 0) {
            for (std::uint8_t trick = 1; trick < style::flip_trick_names.size(); ++trick) {
                if (s.forget && s.forget != trick) continue;
                std::error_code error;
                std::filesystem::remove(file_of(trick), error);
                s.references.erase(trick);
                s.on_disk &= ~(1ull << trick);
                s.learned &= ~(1ull << trick);
            }
            s.forget = -1;
            s.retry_after = 0;
            style_layer::clear_demos();
            s.demos_wanted = s.learned;
            say(s, "the clip was deleted. The editor learns it again the next time it loads that trick");
        }
        // Read Skatepedia's highlighted trick two times each second.
        if (now >= s.next_named) {
            s.next_named = now + 500;
            // A parked Skatepedia has no skater on its stage, so `parked` also permits the check.
            std::string title;
            // Only for the editor. Nothing is learned when the player browses Skatepedia.
            const bool look = (s.wanted.load(std::memory_order_relaxed) || s.fetching) && (style_layer::stage_present() || s.parked);
            const auto named = look ? style_stage::shown_trick(base, &title) : std::uint8_t{};
            s.open = look && !title.empty();
            if (!s.open) s.parked = false;
            if (named) {
                if (named != s.named) {
                    s.named_since = now;
                    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: Skatepedia now shows the {} (learning {}, waiting {} ms)",
                                 style::flip_trick_names[named], s.learn, now < s.retry_after ? s.retry_after - now : 0);
                }
                s.named = named;
                s.named_seen = now;
            } else if (now > s.named_seen + 3000) s.named = 0;
            if (s.learn && s.named && s.named != s.learn) s.spoiled = true;
        }
        // Learn only the fetched trick, and only after the highlight was on it for 2.5 s.
        if (const auto named = !s.learn && now >= s.retry_after && now >= s.named_since + 2500 ? s.named : std::uint8_t{};
            named && !(s.learned >> named & 1) && named == s.fetching) {
            s.spoiled = false;
            s.learn = named;
            say(s, std::format("watching the game's demonstration of the {}", style::flip_trick_names[named]));
            style_layer::request_learn();
        }
        if (s.hide) {
            s.hide = false;
            s.shown.reset();
        }
        if (s.show) {
            const auto trick = *std::exchange(s.show, std::nullopt);
            const Clip *clip = trick ? reference(s, trick) : nullptr;
            if (clip) {
                s.shown = *clip;
                s.started = now;
                s.playing = true;
                s.detail.clear();
            } else if (trick && style_layer::stage_present()) {
                // Skatepedia is open, so fetch the trick from it.
                s.shown.reset();
                s.fetching = trick;
                style_stage::fetch(trick);
                s.detail = std::format("loading the {}", style::flip_trick_names[trick]);
            } else {
                s.shown.reset();
                s.detail = trick ? std::format("no clip of the {} yet", style::flip_trick_names[trick]) : std::string();
            }
        }
        if (s.fetching && (s.learned >> s.fetching & 1)) {
            const auto fetched = std::exchange(s.fetching, std::uint8_t{});
            if (s.wanted.load(std::memory_order_relaxed)) s.show = fetched;
        }
        // While a clip is shown, Skatepedia shows the board-only entry.
        if (now >= s.next_park) {
            s.next_park = now + 1000;
            const bool park = s.shown && s.wanted.load(std::memory_order_relaxed) && !s.learn && !s.fetching &&
                              (style_layer::stage_present() || (s.parked && s.open));
            // A fetch sets its own entry, so do not park over it.
            if (!park && (s.learn || s.fetching)) s.parked = false;
            else if (park != s.parked && style_stage::park(base, park)) s.parked = park;
        }
        if (!s.shown || s.shown->frames.empty()) {
            remove(base, s);
            return;
        }
        // Solo only, until the editor is tested with other players in the session.
        if (multiplayer::other_remote_skaters(stand_in_slot) && !style_layer::session_test()) {
            remove(base, s);
            s.shown.reset();
            s.detail = "the stand-in is for solo play. Leave the session first";
            return;
        }
        // A frame step holds the clip on the new frame.
        if (s.step) {
            s.hold_time = style::step(*s.shown, time_now(s, now), s.step);
            s.playing = false;
            s.step = 0;
        }
        // The clip pose with the current style.
        const float time = time_now(s, now);
        auto pose = style::sample(*s.shown, time);
        // The fov that Skatepedia's camera had on the first frame.
        const auto &opening = s.shown->frames.front();
        s.view_fov = opening.fov;
        const bool recorded_place = s.view_fov > 0;
        // The camera starts with Skatepedia's framing, calculated one time for each shown trick.
        if (recorded_place && s.framed != s.shown->trick) {
            s.framed = s.shown->trick;
            const auto &view = opening.view;
            const auto &root = opening.pose.root.position;
            const std::array<float, 3> back{view[8], view[9], view[10]}, away{view[12] - root[0], view[13] - root[1], view[14] - root[2]};
            const float flat = std::sqrt(back[0] * back[0] + back[2] * back[2]);
            if (flat > 0.1f && std::isfinite(flat)) {
                const float distance = std::clamp(std::sqrt(away[0] * away[0] + away[2] * away[2]) / flat, 0.6f, 12.0f);
                s.yaw.store(std::bit_cast<std::uint32_t>(std::atan2(back[0], back[2])), std::memory_order_relaxed);
                s.pitch.store(std::bit_cast<std::uint32_t>(std::clamp(std::asin(std::clamp(back[1], -1.0f, 1.0f)), -0.15f, 1.45f)), std::memory_order_relaxed);
                s.distance.store(std::bit_cast<std::uint32_t>(distance), std::memory_order_relaxed);
                s.height.store(std::bit_cast<std::uint32_t>(std::clamp(away[1] - back[1] * distance, 0.0f, 2.2f)), std::memory_order_relaxed);
            }
        }
        if (!recorded_place) s.framed = 0;
        s.shown_time.store(std::bit_cast<std::uint32_t>(std::clamp(time, 0.0f, style::trick_end)), std::memory_order_relaxed);
        s.shown_playing.store(s.playing, std::memory_order_relaxed);
        s.shown_trick.store(s.shown->trick, std::memory_order_relaxed);
        style_layer::rotations_at(s.shown->trick, std::clamp(time, 0.0f, style::trick_end), s.rotations);
        for (const auto &delta : s.rotations)
            if (delta.joint < pose.skater.size())
                pose.skater[delta.joint].rotation = style::normalized(style::multiply(pose.skater[delta.joint].rotation, delta.rotation));
        // The anchor is Skatepedia's stage position, or a position 2 m from the player.
        const auto &here = local.pose.root.position;
        // Skatepedia's skater disappears briefly on each loop, so the stage state holds for 4 s.
        if (style_layer::stage_present() || (s.parked && s.open)) s.stage_until = GetTickCount64() + 4000;
        const bool on_stage = GetTickCount64() < s.stage_until;
        const auto anchor = on_stage ? std::array<float, 3>{skatepedia_room[0], style_layer::stage_floor(skatepedia_room[1]), skatepedia_room[2]}
                                     : std::array<float, 3>{here[0] + 2.0f, here[1], here[2]};
        // The camera target follows the skater at ground height.
        const std::array<float, 3> focus = recorded_place ? std::array<float, 3>{pose.root.position[0], opening.pose.root.position[1], pose.root.position[2]} : anchor;
        for (std::size_t i = 0; i < 3; ++i) s.target[i].store(std::bit_cast<std::uint32_t>(focus[i]), std::memory_order_relaxed);
        s.targeted.store(true, std::memory_order_relaxed);
        const auto &first = s.shown->frames.front().pose.root.position;
        // A clip with a recorded camera plays at its recorded position. Other clips stay at the anchor.
        if (!recorded_place) multiplayer::offset_pose(pose, {anchor[0] - pose.root.position[0], anchor[1] - first[1], anchor[2] - pose.root.position[2]});
        const multiplayer::PeerScope scope(stand_in_slot);
        s.spawned = multiplayer::show_remote(base, client, local, pose, s.detail) || multiplayer::remote_skater_entity();
        if (const auto entity = multiplayer::remote_skater_entity())
            style_layer::ignore_holder(pointer(pointer(entity, addr::style::entity_component), addr::style::component_pose_holder),
                                       pointer(multiplayer::remote_board_entity(), addr::style::board_pose_holder));
        // On Skatepedia's stage, the stand-in replaces Skatepedia's skater.
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
