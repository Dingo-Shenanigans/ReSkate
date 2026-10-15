#include "style_capture.h"
#include "style_batch.h"
#include "menu_room.h"
#include "style_editor.h"
#include "style_internal.h"
#include "style_camera.h"
#include "style_layer.h"
#include "style_layer_editor.h"
#include "style_clips.h"
#include "trick_gestures.h"
#include "trick_recorder.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Style/style_pose.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <Xinput.h>
#include <atomic>
#include <format>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace dingosdk::style_capture {
namespace {
using style::notify_player;
enum class Step { idle, leave, world, stage, take, back, home };
constexpr unsigned fade_ms = 300, hold_ms = 1500;
constexpr std::uint64_t world_settle_ms = 500, menu_close_wait_ms = 10000;
constexpr unsigned banner_ms = 700;
constexpr const char *going_text = "Taking your skater to the practice stage";

struct State {
    std::mutex mutex;
    Step step{};
    std::uint64_t since{}; // ms, not game ticks: those follow the frame rate outside a recording
    bool camera{}, player_camera{}, from_editor{};
    std::vector<std::uint8_t> queue;
    std::size_t place{}, recorded{};
    std::vector<style::NotKept> not_kept;
    bool stopping{}, ui_hidden{};
    std::string why;
    style_camera::OrbitState orbit;
    std::atomic<bool> framing{};
    std::optional<StagePlace> map;
};
State &state() {
    static auto *value = new State;
    return *value;
}
void next(State &s, Step step) {
    s.step = step;
    s.since = GetTickCount64();
}
std::uint64_t ms_in_step(const State &s) { return GetTickCount64() - s.since; }
const char *coming_text(const State &s) { return s.from_editor ? "Back to the style editor" : "Back to the game"; }
std::uint8_t trick(const State &s) { return s.queue[s.place]; }
void show_progress(const State &s) {
    overlay::banner(style::progress_text(s.place + 1, s.queue.size(), trick(s), s.stopping), banner_ms);
}
// Esc, or Back (View) on a controller.
bool stop_pressed() {
    constexpr unsigned escape_key = 1u << 3;
    ControllerInput pad{};
    DingoSDKOverlayReadControllerInput(&pad, true);
    return (DingoSDKOverlayReadPromptKeys() & escape_key) != 0 || (pad.buttons & XINPUT_GAMEPAD_BACK) != 0;
}
void watch_stop(State &s) {
    if (s.stopping || !stop_pressed()) return;
    s.stopping = true;
    logging::log(logging::Level::info, logging::Channel::skater, "Style capture: stop asked; the batch ends after this trick.");
}
void dark(const std::string &text) { overlay::cover(text, hold_ms); }
void camera(State &s, bool on) {
    if (s.camera == on) return;
    s.camera = on;
    s.framing.store(on, std::memory_order_release);
    // A free camera that the player had on stays on after the capture.
    if (on) s.player_camera = client_free_camera_active();
    if (!s.player_camera) console::request_debug(overlay::DebugAction::set_free_camera, on);
}
// As in the editor, the facing comes from the trajectory joint.
bool frame(State &s, std::uintptr_t base, std::uintptr_t client) {
    const auto local = multiplayer::capture_local(base, client, true);
    const auto rig = style_layer::rig();
    if (!local.ready || !rig.parents || local.pose.skater.size() != rig.parents->size()) return false;
    const auto root = style::world(local.pose.skater, rig.joints.parents, rig.joints.trajectory);
    s.orbit.aim(root.position);
    s.orbit.frame(style_camera::editor_yaw(root.rotation));
    return true;
}
void back_to_editor(State &s) {
    s.step = Step::idle;
    overlay::banner({}, 0);
    if (!s.why.empty()) notify_player(overlay::NoticeLevel::warning, std::format("the recording stopped: {}", s.why));
    const bool all = s.not_kept.empty() && !s.stopping && s.why.empty();
    notify_player(all ? overlay::NoticeLevel::info : overlay::NoticeLevel::warning,
                  style::summary_text(s.queue.size(), s.recorded, s.not_kept, s.stopping || !s.why.empty()));
    if (!s.from_editor) return;
    // Only on the map the capture left: an unknown map can be a load whose menu is not there yet.
    if (const auto map = s.map ? stage_map() : std::nullopt; map != s.map) {
        logging::log(logging::Level::info, logging::Channel::skater, "Style capture: {}, so the editor stays closed.",
                     map ? "the map changed" : "the map is not known now");
        return;
    }
    style_editor::open_editor();
}
void end(State &s, std::string why) {
    s.why = std::move(why);
    camera(s, false);
    if (trick_stage::active()) {
        trick_stage::close();
        next(s, Step::home);
    } else back_to_editor(s);
}
} // namespace

bool start(std::vector<std::uint8_t> tricks, std::string &why) {
    if (multiplayer_session_active()) {
        why = std::string(style::solo_recording_text);
        return false;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.step != Step::idle || trick_recorder::active()) {
        why = std::string(style::busy_text);
        return false;
    }
    s.queue = std::move(tricks);
    s.place = s.recorded = 0;
    s.not_kept.clear();
    s.stopping = false;
    s.why.clear();
    s.from_editor = style_editor::screen_open();
    next(s, Step::leave);
    logging::log(logging::Level::info, logging::Channel::skater, "Style capture: recording {} trick(s).", s.queue.size());
    return true;
}
bool active() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.step != Step::idle;
}
bool camera_pose(std::array<float, 16> &matrix) noexcept {
    return state().framing.load(std::memory_order_acquire) && state().orbit.pose(matrix);
}
namespace {
// The first trick (place 0) or the next one, on the stage that stays open.
void start_trick(State &s) {
    std::string error;
    if (!trick_recorder::start(trick(s), s.ui_hidden, error)) return end(s, error);
    next(s, Step::stage);
}
void trick_done(State &s) {
    if (trick_recorder::kept()) ++s.recorded;
    else if (const auto reason = trick_recorder::skipped_reason()) s.not_kept.push_back({trick(s), *reason});
    else return end(s, trick_recorder::message());
    if (s.stopping && s.place + 1 >= s.queue.size()) s.stopping = false;
    if (s.stopping || s.place + 1 >= s.queue.size()) {
        overlay::banner({}, 0);
        dark(coming_text(s));
        return next(s, Step::back);
    }
    ++s.place;
    start_trick(s);
}
void world_step(State &s, std::uintptr_t base) {
    dark(going_text);
    const auto ui = multiplayer::sample_game_ui_state(base);
    if (ms_in_step(s) >= world_settle_ms && menu_room::idle() && !ui.in_menu) {
        s.ui_hidden = ui.ui_hidden;
        start_trick(s);
    } else if (ms_in_step(s) > menu_close_wait_ms) {
        end(s, "the game's menu did not close");
    }
}
void step(State &s, std::uintptr_t base, std::uintptr_t client, bool ready) {
    std::lock_guard lock(s.mutex);
    if (s.step == Step::idle || !ready || !base) return;
    if (s.step != Step::idle && s.step != Step::back && s.step != Step::home) watch_stop(s);
    switch (s.step) {
    case Step::leave:
        dark(going_text);
        if (ms_in_step(s) < fade_ms) break;
        // Closing the editor screen also takes the game out of the menu room.
        style_editor::close_and_hide();
        s.map = stage_map();
        next(s, Step::world);
        break;
    case Step::world:
        return world_step(s, base);
    case Step::stage:
        if (s.place == 0) dark(going_text);
        else show_progress(s);
        if (!trick_recorder::active()) return trick_done(s);
        if (trick_stage::ready() && frame(s, base, client)) {
            camera(s, true);
            overlay::cover({}, 0);
            next(s, Step::take);
        }
        break;
    case Step::take:
        show_progress(s);
        if (trick_recorder::active()) break;
        trick_done(s);
        break;
    case Step::back:
        dark(coming_text(s));
        if (ms_in_step(s) < fade_ms) break;
        camera(s, false);
        end(s, {});
        break;
    case Step::home:
        dark(coming_text(s));
        // The stage brings the skater back to where they were before it counts as closed.
        if (!trick_stage::active()) back_to_editor(s);
        break;
    case Step::idle:
        break;
    }
}
// A failure that repeats would keep the cover on and the game's input blocked: the capture ends and lets go of both.
bool stop_after_error(State &s) {
    std::lock_guard lock(s.mutex);
    if (s.step == Step::idle) return false;
    s.step = Step::idle;
    overlay::cover({}, 0);
    overlay::banner({}, 0);
    try {
        trick_stage::close();
    } catch (...) {}
    try {
        camera(s, false);
    } catch (...) {}
    notify_player(overlay::NoticeLevel::warning, "the capture stopped after an error");
    return true;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    style::guarded_tick(logging::Channel::skater, "Style capture: the capture", [&] { step(s, base, client, ready); },
                        [&] { return stop_after_error(s); });
}
} // namespace dingosdk::style_capture
