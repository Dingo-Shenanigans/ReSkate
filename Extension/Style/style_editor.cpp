#include "style_editor.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "menu_room.h"
#include "style_internal.h"
#include "style_camera.h"
#include "style_batch.h"
#include "style_capture.h"
#include "style_layer.h"
#include "style_layer_editor.h"
#include "style_recording.h"
#include "style_clips.h"
#include "style_presets.h"
#include "trick_pad.h"
#include "trick_recorder.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
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
#include <ctime>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dingosdk::style_editor {
namespace {
using style::Clip;
using style::notify_player;
using Phase = style::EditorLoad::Phase;
constexpr std::uint64_t editor_open_wait_ms = 8000;
constexpr std::uint64_t room_wait_ms = 4000;
constexpr std::uint64_t dress_first_ms = 700, dress_again_ms = 3000;
constexpr float loop_blend_ms = 250;
// The editor camera's limits: tilt (radians), distance and target height (metres).
constexpr float min_pitch = -0.15f, max_pitch = 1.45f, min_distance = 0.6f, max_distance = 12.0f, max_height = 2.2f;
constexpr std::uint32_t nearest_window_ms = 60;
constexpr std::uint32_t board_window_ms = 40;
constexpr float board_reach_m = 3.0f, max_deck_offset_m = 1.0f;
// A held clip eases to a new moment with this time constant, so a scrub or frame step does not jump.
constexpr float ease_ms = 45;
constexpr float frame_ms = 1000.0f / 60.0f;
constexpr auto &room_centre = addr::style::menu_room;

struct State {
    std::mutex mutex;
    std::optional<std::uint8_t> show; // 0 clears the stand-in
    bool hide{};
    std::optional<std::vector<std::uint8_t>> record_request; // empty: every trick that needs it
    std::uint8_t recording_trick{};
    std::uint8_t trick_to_record{};
    std::int32_t local_flip{-1}; // the local skater's last flip trick in the recording, -1 for none
    bool did_recorded_trick{};
    std::string take_failure;
    std::optional<Recorded> recorded;
    std::map<std::uint8_t, Clip> references;
    style::TrickSet on_disk, outdated; // outdated: a newer recording is available
    bool listed{};
    std::uint64_t room_until{};
    std::uint8_t framed{};
    std::optional<std::uint8_t> forget; // 0 deletes all
    std::optional<Clip> shown;
    bool playing{true};
    // What ran once for this opening of the editor screen.
    struct Opening {
        bool full_server_closed{};
        bool room_wait_started{};
        bool room_closed{};
        std::uint64_t close_retry_until{};
    } opening;
    float speed{1.0f}; // a fraction of the recorded speed
    double started{}, last_tick{};
    float shown_ms{}, target_ms{};
    float low_ms{}, high_ms{}; // the trick part of the shown clip: the flick and the end
    style::Pace pace{style::even_pace};
    std::uint64_t next_cosmetics{};
    // `controls` guards only the menu's playback requests, so a request never waits for file work.
    std::mutex controls;
    std::optional<float> hold_request; // a timeline time
    std::optional<bool> play_request;
    int step_request{};
    std::optional<float> speed_request;
    bool spawned{}, dressed{};
    style::EditorLoad load;
    std::vector<style::SkeletonDelta> rotations;
    style_camera::OrbitState orbit;
    std::atomic<bool> targeted{};
    std::atomic<std::uint8_t> shown_trick{};
    std::atomic<float> shown_time{};
    std::atomic<bool> shown_playing{}, wanted{};
    std::atomic<bool> screen_is_open{};
    std::atomic<std::uint64_t> wanted_until{};
};
State &state() { static auto *value = new State; return *value; }

std::filesystem::path folder() { return profile::default_path().parent_path() / L"style-clips"; }
std::filesystem::path file_of(std::uint8_t trick) {
    return folder() / (std::string(style::flip_trick_names[trick]) + ".clip");
}
void take_failed(State &s, std::uint8_t trick, std::string reason) {
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the take of the {} was not kept: {}.",
                 style::flip_trick_names[trick], reason);
    s.take_failure = std::move(reason);
}
style::Recipe recipe() {
    return {style::recipe_version, std::string(supported_build::steam_build_id), trick_pad::flick_ms, trick_recorder::still_ms};
}
// True if a clip has the key of the recordings that `style record` makes now.
bool current(const style::Capture &capture) {
    static const auto key = style::recipe_key(recipe());
    return capture.key == key;
}
void list(State &s) {
    s.listed = true;
    for (const auto trick : style::flip_trick_ids()) {
        std::ifstream file(file_of(trick), std::ios::binary);
        std::array<std::uint8_t, style::clip_header_bytes> head{};
        if (!file.read(reinterpret_cast<char *>(head.data()), head.size())) continue;
        const auto capture = style::clip_capture(head);
        if (!capture) continue;
        s.on_disk.set(trick);
        if (!current(*capture)) s.outdated.set(trick);
    }
}
// Copies the trucks and wheels of the game's board into the shown board. The anchor's distance on the first frame, or nothing.
std::optional<float> use_game_board(Clip &clip) {
    std::optional<float> apart;
    for (auto &frame : clip.frames) {
        auto &board = frame.pose.board;
        if (frame.rig.size() <= style::rig_deck + 1 || board.size() != frame.rig.size() + 1) return std::nullopt;
        if (!apart) apart = style::distance(frame.rig[style::rig_deck].position, board[style::board_anchor].position);
        // The root and the anchor place the board, so start after them. The shown board keeps its scale.
        for (std::size_t joint = style::rig_deck + 1; joint < frame.rig.size(); ++joint) {
            board[joint + 1].position = frame.rig[joint].position;
            board[joint + 1].rotation = frame.rig[joint].rotation;
        }
    }
    if (!apart) return apart;
    // Follow the game's board: the deck joint turns back after a half-turn board trick, but the game's board lands turned.
    const auto &first = clip.frames.front();
    const auto to_board = style::compose(style::inverse(first.rig[style::rig_deck]), first.pose.board[style::board_entity]);
    for (auto &frame : clip.frames) {
        auto &board = frame.pose.board;
        const auto from_deck = board[style::board_entity];
        auto turned = style::compose(frame.rig[style::rig_deck], to_board);
        turned.scale = from_deck.scale;
        board[style::board_anchor] = style::anchored(turned, from_deck, board[style::board_anchor]);
        board[style::board_entity] = turned;
    }
    return apart;
}
const Clip *reference(State &s, std::uint8_t trick) {
    if (const auto found = s.references.find(trick); found != s.references.end()) return &found->second;
    if (!style::is_flip_trick(trick) || !s.on_disk.test(trick)) return nullptr;
    try {
        std::ifstream file(file_of(trick), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto clip = style::decode_clip({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
        if (clip.trick != trick) throw std::runtime_error("it is for another trick");
        (void)use_game_board(clip);
        return &(s.references[trick] = std::move(clip));
    } catch (const std::exception &failure) {
        s.on_disk.reset(trick);
        s.outdated.reset(trick);
        if (s.load.trick == trick) s.load = {trick, Phase::failed, std::format("the saved clip could not be read ({})", failure.what())};
        return nullptr;
    }
}
void keep(State &s, Clip clip) {
    const auto trick = clip.trick;
    try {
        const auto bytes = style::encode_clip(clip);
        const auto issue = style::presets::write_atomically(file_of(trick), {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        if (!issue.empty()) throw std::runtime_error(issue);
        s.on_disk.set(trick);
        s.outdated.set(trick, !current(clip.capture));
    } catch (const std::exception &failure) {
        notify_player(overlay::NoticeLevel::warning,
                       std::format("the {} clip could not be saved: {}", style::flip_trick_titles[trick], failure.what()));
    }
    s.references[trick] = std::move(clip);
}
// Samples have a time `at` in ms; null when none is within nearest_window_ms.
template <class Sample> const Sample *nearest_at(const std::vector<Sample> &samples, std::uint32_t when) noexcept {
    const Sample *nearest{};
    std::uint32_t apart = nearest_window_ms;
    for (const auto &sample : samples)
        if (const auto gap = sample.at > when ? sample.at - when : when - sample.at; gap < apart) {
            apart = gap;
            nearest = &sample;
        }
    return nearest;
}
// Board frames are keyed by their pose holder.
struct RecordedRigs {
    std::vector<style::RigFrame> skater;
    std::map<std::uint64_t, std::vector<style::RigFrame>> boards;
};
RecordedRigs split_rigs(std::span<const std::uint8_t> recorded) {
    RecordedRigs rigs;
    for (auto &rig : style::read_recording(recorded))
        (rig.kind == style::RigKind::skater ? rigs.skater : rigs.boards[rig.holder]).push_back(std::move(rig.frame));
    return rigs;
}
std::optional<std::string> wrong_trick(const State &s, std::uint8_t trick) {
    if (s.did_recorded_trick && s.local_flip == trick) return std::nullopt;
    if (!style::is_flip_trick(s.local_flip)) return "the skater did no flip trick";
    return std::format("the skater did a {}", style::flip_trick_titles[static_cast<std::size_t>(s.local_flip)]);
}
style::Transform deck_to_board(const multiplayer::NativeFrame &local, const style_layer::Rig &rig) {
    style::Transform result;
    if (local.pose.board.empty() || local.pose.skater.size() != rig.parents->size()) return result;
    const auto deck_world = style::world(local.pose.skater, rig.joints.parents, rig.joints.deck);
    const auto measured = style::compose(style::inverse(deck_world), local.pose.board[style::board_entity]);
    if (game::length(measured.position) < max_deck_offset_m) result = measured;
    result.scale = {1, 1, 1};
    return result;
}
const std::vector<style::RigFrame> *nearest_board(const Clip &clip,
                                                  const std::map<std::uint64_t, std::vector<style::RigFrame>> &boards) {
    const std::vector<style::RigFrame> *board{};
    float nearest = board_reach_m;
    const auto &start = clip.frames.front().pose.root.position;
    for (const auto &[holder, frames] : boards)
        for (const auto &frame : frames) {
            const bool at_start = frame.at + board_window_ms >= clip.began && frame.at <= clip.began + board_window_ms;
            if (!at_start || frame.joints.size() <= style::rig_deck) continue;
            if (const float apart = style::ground_distance(frame.joints[style::rig_deck].position, start); apart < nearest) {
                nearest = apart;
                board = &frames;
            }
        }
    return board;
}
// If one frame has no board frame near in time, no frame keeps a board.
void borrow_game_board(Clip &clip, const std::map<std::uint64_t, std::vector<style::RigFrame>> &boards) {
    const auto *board = nearest_board(clip, boards);
    for (auto &frame : clip.frames) {
        const auto *best = board ? nearest_at(*board, clip.began + frame.at) : nullptr;
        if (!best) {
            for (auto &each : clip.frames) each.rig.clear();
            return;
        }
        frame.rig = best->joints;
    }
}
void keep_take(State &s, Clip clip) {
    const auto trick = clip.trick;
    const auto apart = use_game_board(clip);
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: recorded the {} ({} frames, {}).",
                 style::flip_trick_names[trick], clip.frames.size(),
                 apart ? std::format("the game's board, its anchor {:.3f} m from the calculated one", *apart)
                       : std::string("without the game's board"));
    const auto made = recipe();
    clip.capture = {style::recipe_key(made), style::recipe_text(made), std::string(reskate_version),
                    static_cast<std::uint64_t>(std::time(nullptr))};
    keep(s, std::move(clip));
}
void take(State &s, const multiplayer::NativeFrame &local, std::span<const std::uint8_t> recorded) {
    const auto trick = std::exchange(s.recording_trick, std::uint8_t{});
    const auto rig = style_layer::rig();
    if (!trick) return;
    if (!rig.parents) return take_failed(s, trick, "the skater skeleton is not read yet");
    const auto rigs = split_rigs(recorded);
    if (rigs.skater.empty()) return take_failed(s, trick, "the skater was not recorded");
    if (const auto wrong = wrong_trick(s, trick)) return take_failed(s, trick, *wrong);
    std::string why;
    auto clip = style::clip_from_capture(trick, rigs.skater, rig.joints, local.pose.board, deck_to_board(local, rig), why);
    if (!clip) return take_failed(s, trick, why);
    borrow_game_board(*clip, rigs.boards);
    keep_take(s, std::move(*clip));
}
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
    s.shown.reset();
    s.shown_trick.store(0, std::memory_order_relaxed);
    s.targeted.store(false, std::memory_order_relaxed);
    if (!s.spawned) return;
    // A player who took the slot owns the skater in it now.
    if (multiplayer_slots_in_use() <= stand_in_slot) {
        const multiplayer::PeerScope scope(stand_in_slot);
        multiplayer::remove_remote(base);
    }
    style_layer::ignore_holder(0);
    style_layer::release_room();
    s.spawned = s.dressed = false;
}
void request_open() {
    auto &s = state();
    s.wanted_until.store(GetTickCount64() + editor_open_wait_ms, std::memory_order_relaxed);
    s.wanted.store(true, std::memory_order_relaxed);
    std::lock_guard lock(s.mutex);
    // A load that failed before this open does not show on the new screen.
    if (s.load.phase == Phase::failed) s.load = {s.load.trick, Phase::none, {}};
}
} // namespace

void request_show(std::uint8_t trick) {
    if (trick && !style::is_flip_trick(trick)) return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (trick && s.show != trick)
        logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the {} was picked.", style::flip_trick_names[trick]);
    // A new trick, another try after a failure, or the first ask after a close starts a new load.
    if (trick != s.load.trick || s.load.phase == Phase::failed || s.load.phase == Phase::none)
        s.load = {trick, trick ? Phase::loading : Phase::none, {}};
    s.show = trick;
    s.hide = false;
}
void request_record_tricks() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.record_request = std::vector<std::uint8_t>{};
}
void request_capture(std::uint8_t trick) {
    if (!style::capturable(trick)) return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.record_request = std::vector<std::uint8_t>{trick};
}
void request_hide() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.hide = true;
    s.show.reset();
    // The picked trick stays, so that the editor opens on it again.
    s.load = {s.load.trick, Phase::none, {}};
    s.wanted.store(false, std::memory_order_relaxed);
}
void request_hold(float time) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.hold_request = time;
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
    s.step_request = std::clamp(s.step_request + frames, -style::max_step_frames, style::max_step_frames);
}
void request_speed(float speed) noexcept {
    auto &s = state();
    std::lock_guard lock(s.controls);
    s.speed_request = speed;
}
float camera_fov() noexcept { return state().targeted.load(std::memory_order_relaxed) ? addr::style::editor_camera_fov : 0.0f; }
void open_editor() {
    style_layer::request_history_clear();
    menu_room::open([] {
        request_open();
        (void)console::request_debug(overlay::DebugAction::set_style_editor, true);
    });
}
bool close_editor() {
    const bool queued = console::request_debug(overlay::DebugAction::set_style_editor, false);
    style_layer::request_history_clear();
    return queued;
}
void close_and_hide() {
    (void)close_editor();
    request_hide(); // the stand-in goes now: a close of a closed screen runs no hook
}
void screen_closed() noexcept {
    screen_open(false);
    try {
        request_hide();
        style_layer::request_history_clear();
    } catch (...) {}
    menu_room::leave();
}
void screen_open(bool open) noexcept { state().screen_is_open.store(open, std::memory_order_relaxed); }
bool screen_open() noexcept { return state().screen_is_open.load(std::memory_order_relaxed); }
std::optional<Recorded> take_recorded() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.recorded, std::nullopt);
}
bool request_record(std::uint8_t trick, std::string &error) {
    if (!style::is_flip_trick(trick)) {
        error = std::string(style::not_flip_trick_text);
        return false;
    }
    if (!style_layer::enabled()) {
        error = "the style layer is off; 'style 1' turns it on";
        return false;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.recording_trick || s.trick_to_record) {
        error = std::string(style::busy_text);
        return false;
    }
    s.trick_to_record = trick;
    s.recorded.reset();
    return true;
}
void request_forget(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.forget = trick;
}
void request_orbit(float yaw, float pitch, float distance, float height) noexcept {
    auto &orbit = state().orbit;
    const auto add = [](std::atomic<float> &value, float by, float low, float high) {
        value.store(std::clamp(value.load(std::memory_order_relaxed) + by, low, high), std::memory_order_relaxed);
    };
    orbit.yaw.store(style::wrap_angle(orbit.yaw.load(std::memory_order_relaxed) + yaw), std::memory_order_relaxed);
    add(orbit.pitch, pitch, min_pitch, max_pitch);
    add(orbit.distance, distance, min_distance, max_distance);
    add(orbit.height, height, 0.0f, max_height);
}
bool camera_pose(std::array<float, 16> &matrix) noexcept {
    return state().targeted.load(std::memory_order_relaxed) && state().orbit.pose(matrix);
}
style::Playhead playhead() noexcept {
    auto &s = state();
    const bool wanted = s.wanted.load(std::memory_order_relaxed);
    if (const auto trick = s.shown_trick.load(std::memory_order_relaxed))
        return {trick, s.shown_time.load(std::memory_order_relaxed), true, s.shown_playing.load(std::memory_order_relaxed), wanted};
    return {0, 0, false, false, wanted};
}
void fill(style::StyleModel &model) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    model.clips = s.on_disk;
    model.outdated_clips = s.outdated;
    model.to_record = style::tricks_to_record(s.on_disk, s.outdated);
    model.editor_load = s.load;
    model.pace = s.pace;
    model.server_too_full = !stand_in_slot_free();
    model.recording_tricks = style_capture::active();
}

namespace {
void end_unopened_screen(State &s) {
    if (!s.wanted.load(std::memory_order_relaxed) || s.screen_is_open.load(std::memory_order_relaxed) ||
        GetTickCount64() <= s.wanted_until.load(std::memory_order_relaxed))
        return;
    s.wanted.store(false, std::memory_order_relaxed);
    menu_room::leave();
    notify_player(overlay::NoticeLevel::warning, "the editor screen did not open");
}
// A refused close waits this long, so a long load does not log a refusal each tick.
constexpr std::uint64_t close_retry_ms = 1000;
bool try_close(State &s) {
    const auto now = GetTickCount64();
    if (now < s.opening.close_retry_until) return false;
    if (close_editor()) return true;
    s.opening.close_retry_until = now + close_retry_ms;
    return false;
}
void reset_opening(State &s) {
    if (!s.screen_is_open.load(std::memory_order_relaxed) && !s.wanted.load(std::memory_order_relaxed)) s.opening = {};
}
// A server that comes close to the stand-in's slot closes each opening once.
void full_server_gate(std::uintptr_t base, State &s) {
    const bool open = s.screen_is_open.load(std::memory_order_relaxed) || s.wanted.load(std::memory_order_relaxed);
    if (stand_in_slot_free() || !open || s.opening.full_server_closed || !try_close(s)) return;
    s.opening.full_server_closed = true;
    s.wanted.store(false, std::memory_order_relaxed);
    menu_room::leave();
    remove(base, s);
    notify_player(overlay::NoticeLevel::warning, style::full_server_text);
}
void stand_down(std::uintptr_t base, State &s) {
    remove(base, s);
    s.opening.room_wait_started = false;
}
// A recording of the local skater starts here, so that its end is collected later in the tick.
void start_recording(State &s) {
    const auto trick = std::exchange(s.trick_to_record, std::uint8_t{});
    if (!trick || s.recording_trick) return;
    s.recording_trick = trick;
    s.local_flip = -1;
    s.did_recorded_trick = false;
    style_layer::request_recording();
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: recording the skater's {}.",
                 style::flip_trick_names[trick]);
}
// Nothing to show, record or forget: the full capture of the skater is not paid for.
bool idle(const State &s) {
    return !s.shown && !s.show && !s.hide && !s.recording_trick && !s.forget && !s.record_request &&
           !s.wanted.load(std::memory_order_relaxed);
}
void follow_local_trick(State &s) {
    const auto local = style_layer::local_trick();
    if (!local || local->flip <= 0) return;
    s.local_flip = local->flip;
    s.did_recorded_trick |= local->flip == s.recording_trick;
}
void collect_take(State &s, std::uintptr_t base, std::uintptr_t client) {
    auto recorded = style_layer::collect_recording();
    if (!recorded || !s.recording_trick) return;
    const auto trick = s.recording_trick;
    s.take_failure.clear();
    take(s, multiplayer::capture_local(base, client, true), *recorded);
    s.recorded = Recorded{trick, s.take_failure};
}
void forget_clips(State &s) {
    const auto forget = std::exchange(s.forget, std::nullopt);
    if (!forget) return;
    for (const auto trick : style::flip_trick_ids()) {
        if (*forget && *forget != trick) continue;
        std::error_code error;
        std::filesystem::remove(file_of(trick), error);
        s.references.erase(trick);
        s.on_disk.reset(trick);
        s.outdated.reset(trick);
    }
    notify_player(overlay::NoticeLevel::info, "the clip was deleted; 'Record all my tricks' records it again");
}
void show_clip(State &s, std::uint8_t trick, const Clip &clip) {
    if (s.load.trick == trick) s.load = {trick, Phase::ready, {}};
    s.shown = clip;
    s.room_until = GetTickCount64() + room_wait_ms;
    const float flick = style::ms_at(clip, 0), caught = style::ms_at(clip, 1), touchdown = style::ms_at(clip, 2);
    s.low_ms = flick;
    s.high_ms = style::ms_at(clip, style::trick_end);
    s.pace = {caught - flick, touchdown - caught, s.high_ms - touchdown};
    style_layer::note_editor_pace(s.pace);
    s.started = s.last_tick = static_cast<double>(multiplayer::now_us()) / 1000.0;
    s.shown_ms = s.target_ms = 0;
    s.playing = true;
}
void start_asked_recording(State &s) {
    auto asked = std::exchange(s.record_request, std::nullopt);
    if (!asked) return;
    if (asked->empty() && style_capture::active()) return;
    auto tricks = asked->empty() ? style::in_order(style::tricks_to_record(s.on_disk, s.outdated)) : std::move(*asked);
    if (tricks.empty()) return notify_player(overlay::NoticeLevel::info, "your tricks are all recorded");
    std::string why;
    if (!style_capture::start(std::move(tricks), why))
        notify_player(overlay::NoticeLevel::warning, std::format("your tricks cannot be recorded now: {}", why));
}
void show_what_was_asked(State &s) {
    if (s.hide) {
        s.hide = false;
        s.shown.reset();
    }
    if (!s.show) return;
    const auto trick = *s.show;
    s.show.reset();
    if (const Clip *clip = trick ? reference(s, trick) : nullptr) return show_clip(s, trick, *clip);
    s.shown.reset();
    if (trick && s.load.trick == trick && s.load.phase != Phase::failed) s.load = {trick, Phase::unrecorded, {}};
}
// The menu room's skater can be missing for a moment; false when the room is gone.
bool room_check(std::uintptr_t base, State &s) {
    const bool screen = s.screen_is_open.load(std::memory_order_relaxed);
    if (!screen && !s.shown) return true;
    const auto now = GetTickCount64();
    if (screen && !std::exchange(s.opening.room_wait_started, true)) s.room_until = now + room_wait_ms;
    if (style_layer::room_present()) s.room_until = now + room_wait_ms;
    if (now < s.room_until) return true;
    remove(base, s);
    if (screen && stand_in_slot_free() && !s.opening.room_closed && try_close(s)) {
        s.opening.room_closed = true;
        notify_player(overlay::NoticeLevel::warning, "the editor closed: the game's menu room went away");
    }
    return false;
}
style::Pose looped_pose(State &s) {
    const float length = static_cast<float>(style::duration(*s.shown));
    if (s.shown_ms <= length) return styled(s, s.shown_ms);
    const float amount = std::clamp((s.shown_ms - length) / loop_blend_ms, 0.0f, 1.0f);
    return style::blend(styled(s, length), styled(s, 0), amount * amount * (3 - 2 * amount));
}
void publish_shown(State &s) {
    const auto &first = s.shown->frames.front();
    // The editor's angle to the skater's facing, not the board's: a board can start a clip turned round.
    if (s.framed != s.shown->trick) {
        s.framed = s.shown->trick;
        s.orbit.frame(style_camera::editor_yaw(first.pose.root.rotation));
    }
    const float length = static_cast<float>(style::duration(*s.shown));
    const float time = style::time_at(*s.shown, std::min(s.shown_ms, length));
    s.shown_time.store(std::clamp(time, 0.0f, style::trick_end), std::memory_order_relaxed);
    s.shown_playing.store(s.playing, std::memory_order_relaxed);
    s.shown_trick.store(s.shown->trick, std::memory_order_relaxed);
}
void ignore_stand_in() {
    const auto entity = multiplayer::remote_skater_entity();
    if (!entity) return;
    const auto component = memory::peek_pointer(entity, addr::style::entity_component);
    const auto holder = memory::peek_pointer(component, addr::style::component_pose_holder);
    style_layer::ignore_holder(holder, memory::peek_pointer(multiplayer::remote_board_entity(), addr::style::board_pose_holder));
}
void dress_stand_in(State &s, std::uintptr_t base, const multiplayer::NativeFrame &local) {
    const auto now = GetTickCount64();
    if (!s.spawned || now < s.next_cosmetics) return;
    s.next_cosmetics = now + (s.dressed ? dress_again_ms : dress_first_ms);
    std::string detail;
    if (const auto appearance = multiplayer::capture_cosmetics(base, local, detail)) {
        multiplayer::update_remote_cosmetics(base, local, *appearance, detail);
        s.dressed = true;
    }
}
void place_stand_in(State &s, std::uintptr_t base, std::uintptr_t client, const multiplayer::NativeFrame &local) {
    advance(s, static_cast<double>(multiplayer::now_us()) / 1000.0);
    auto pose = looped_pose(s);
    publish_shown(s);
    const auto &first = s.shown->frames.front();
    // The clip's board starts on the room's floor, wherever the clip was recorded.
    const float lift =
        first.pose.board.empty() ? 0.0f : addr::style::menu_room_board_height - first.pose.board[style::board_entity].position[1];
    const std::array<float, 3> anchor{room_centre[0], first.pose.root.position[1] + lift, room_centre[2]};
    // The stand-in skates in place, so the camera neither lags it nor jumps back at each loop.
    s.orbit.aim(anchor);
    s.targeted.store(true, std::memory_order_relaxed);
    multiplayer::offset_pose(pose, {anchor[0] - pose.root.position[0], lift, anchor[2] - pose.root.position[2]});
    const multiplayer::PeerScope scope(stand_in_slot);
    std::string detail;
    s.spawned = multiplayer::show_remote(base, client, local, pose, detail) || multiplayer::remote_skater_entity();
    ignore_stand_in();
    // In the menu room, the stand-in replaces the menu's skater.
    style_layer::keep_room_clear();
    dress_stand_in(s, base, local);
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (!s.listed) list(s);
        end_unopened_screen(s);
        reset_opening(s);
        full_server_gate(base, s);
        if (!ready || !base) {
            if (std::exchange(s.record_request, std::nullopt))
                notify_player(overlay::NoticeLevel::warning, "your tricks cannot be recorded now: load into the game first");
            return stand_down(base, s);
        }
        if (!style_layer::enabled()) {
            // The editor needs the layer, so Style switched off closes its screen too.
            if (s.screen_is_open.load(std::memory_order_relaxed)) (void)try_close(s);
            return stand_down(base, s);
        }
        if (!stand_in_slot_free()) {
            s.show.reset();
            remove(base, s);
        }
        start_recording(s);
        start_asked_recording(s);
        if (idle(s)) {
            (void)style_layer::collect_recording();
            return;
        }
        style_layer::watch_room();
        if (s.recording_trick) follow_local_trick(s);
        // Bones are read only for a take; the stand-in needs only the entity and the root.
        const auto local = multiplayer::capture_local(base, client, false);
        if (!local.ready) return;
        collect_take(s, base, client);
        forget_clips(s);
        show_what_was_asked(s);
        if (!room_check(base, s)) return;
        if (!s.shown || s.shown->frames.empty()) return remove(base, s);
        place_stand_in(s, base, client, local);
    } catch (...) {} // the stand-in waits for the next tick
}
} // namespace dingosdk::style_editor
