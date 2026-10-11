#include "style_capture.h"
#include "style_editor.h"
#include "style_internal.h"
#include "style_layer.h"
#include "style_stage.h"
#include "style_takes.h"
#include "trick_pad.h"
#include "trick_recorder.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <format>
#include <mutex>

namespace dingosdk::style_capture {
namespace {
// leave: the view fades to dark before the editor closes. stage: dark until the skater stands on the stage. trick: the trick
// shows. back: dark again after the landing. take: the recording ends. home: the skater returns from the stage.
enum class Step { idle, leave, world, stage, trick, back, take, home };
constexpr unsigned fade_ms = 300, hold_ms = 1500;
// After the landing the trick shows this long before the view fades.
constexpr std::uint32_t after_landing_ms = 500;

struct State {
    std::mutex mutex;
    Step step{};
    std::uint8_t trick{};
    std::uint64_t since{};  // the time (ms) when the step started. Game ticks follow the frame rate outside a recording
    std::uint64_t failed{}; // one bit for each trick whose capture failed in this game
    bool airborne{}, camera{};
    std::uint64_t landed{};
    std::string why; // why the capture failed, empty if it did not
    // The camera: what it looks at and its turn around it (radians).
    std::array<std::atomic<std::uint32_t>, 3> target{};
    std::atomic<std::uint32_t> yaw{};
    std::atomic<bool> framing{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
void next(State &s, Step step) {
    s.step = step;
    s.since = GetTickCount64();
}
// Milliseconds in the current step.
std::uint64_t in_step(const State &s) { return GetTickCount64() - s.since; }
std::string title(const State &s) { return std::string(style::flip_trick_titles[s.trick]); }
void dark(const std::string &text) { overlay::cover(text, hold_ms); }
void camera(State &s, bool on) {
    if (s.camera == on) return;
    s.camera = on;
    s.framing.store(on, std::memory_order_release);
    console::request_debug(overlay::DebugAction::set_free_camera, on);
}
// The editor's angle to the skater, from where the skater stands now. Like the editor, it takes the skater's facing from the
// trajectory joint.
bool frame(State &s, std::uintptr_t base, std::uintptr_t client) {
    const auto local = multiplayer::capture_local(base, client, true);
    const auto rig = style_layer::rig();
    if (!local.ready || !rig.parents || local.pose.skater.size() != rig.parents->size()) return false;
    const auto root = style::world(local.pose.skater, *rig.parents, rig.trajectory);
    s.yaw.store(std::bit_cast<std::uint32_t>(style_layer::editor_yaw(root.rotation)), std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) s.target[i].store(std::bit_cast<std::uint32_t>(root.position[i]), std::memory_order_relaxed);
    return true;
}
// The editor opens again on its selected trick, with the new clip or with why there is none.
void back_to_editor(State &s) {
    s.step = Step::idle;
    overlay::banner({}, 0);
    if (!s.why.empty()) {
        s.failed |= 1ull << s.trick;
        logging::log(logging::Level::warning, logging::Channel::skater, "Style capture: the {} was not captured: {}.", style::flip_trick_names[s.trick], s.why);
        overlay::notify(overlay::NoticeLevel::warning, "Style editor", std::format("The {} was not captured: {}.", title(s), s.why));
    } else {
        logging::log(logging::Level::info, logging::Channel::skater, "Style capture: the {} was captured.", style::flip_trick_names[s.trick]);
    }
    style_stage::open([] {
        style_editor::request_open();
        console::request_debug(overlay::DebugAction::set_style_editor, true);
    });
}
// The capture ends: the camera goes back, and the stage takes the skater back first if it is open.
void end(State &s, std::string why) {
    s.why = std::move(why);
    camera(s, false);
    if (trick_stage::active()) {
        trick_stage::close();
        next(s, Step::home);
    } else back_to_editor(s);
}
} // namespace

bool start(std::uint8_t trick, std::string &why) {
    if (!trick || trick >= style::flip_trick_names.size()) {
        why = "that is not a flip trick";
        return false;
    }
    if (multiplayer_session_active()) {
        why = "the trick stage is offline only. Capture it in solo play";
        return false;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.failed >> trick & 1) {
        why = std::format("its capture failed in this game. 'style editor capture {}' tries again", style::flip_trick_names[trick]);
        return false;
    }
    if (s.step != Step::idle || trick_recorder::active()) {
        why = "a capture or a recording is running";
        return false;
    }
    s.trick = trick;
    s.why.clear();
    s.airborne = false;
    s.landed = 0;
    next(s, Step::leave);
    logging::log(logging::Level::info, logging::Channel::skater, "Style capture: the {} has no clip, so it is captured.", style::flip_trick_names[trick]);
    return true;
}
void forget_failure(std::uint8_t trick) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (trick < 64) s.failed &= ~(1ull << trick);
}
bool active() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.step != Step::idle;
}
bool camera_pose(std::array<float, 16> &matrix) noexcept {
    auto &s = state();
    if (!s.framing.load(std::memory_order_acquire)) return false;
    const auto value = [](const std::atomic<std::uint32_t> &bits) { return std::bit_cast<float>(bits.load(std::memory_order_relaxed)); };
    return style_layer::orbit_camera(matrix, value(s.yaw), style_layer::editor_pitch, style_layer::editor_distance,
                                     {value(s.target[0]), value(s.target[1]), value(s.target[2])}, style_layer::editor_height);
}
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (s.step == Step::idle || !ready || !base) return;
        const auto going = "Taking your skater to the trick stage", coming = "Back to the style editor";
        switch (s.step) {
        case Step::leave:
            dark(going);
            if (in_step(s) < fade_ms) break;
            // Closing the editor screen also takes the game out of the menu room.
            style_editor::request_hide();
            console::request_debug(overlay::DebugAction::set_style_editor, false);
            next(s, Step::world);
            break;
        case Step::world: {
            dark(going);
            const auto ui = multiplayer::sample_game_ui_state(base);
            if (in_step(s) >= 500u && style_stage::idle() && !ui.in_menu) {
                std::string error;
                if (!trick_recorder::start({s.trick}, {}, ui.ui_hidden, error, true)) return end(s, error);
                next(s, Step::stage);
            } else if (in_step(s) > 10000u) {
                end(s, "the game's menu did not close");
            }
            break;
        }
        case Step::stage:
            dark(going);
            if (!trick_recorder::active()) return end(s, trick_recorder::status());
            // The view fades up when the skater stands on the stage.
            if (trick_stage::ready() && frame(s, base, client)) {
                camera(s, true);
                overlay::cover({}, 0);
                next(s, Step::trick);
            }
            break;
        case Step::trick: {
            overlay::banner(std::format("Capturing the {}", title(s)), 700);
            std::uint32_t physics{};
            std::int32_t flip{};
            if (style_layer::local_trick(physics, flip)) {
                if (physics >= addr::style::air_states_begin && physics < addr::style::riding_states_end) s.airborne = true;
                else if (s.airborne && !s.landed && (physics == addr::style::riding_ground_state || physics == addr::style::riding_landed_state))
                    s.landed = trick_pad::game_ticks();
            }
            // The view fades after the landing, before the skater can roll off the stage.
            const bool shown = s.landed && trick_pad::game_ticks() >= s.landed + trick_pad::to_ticks(after_landing_ms);
            if (shown || !trick_recorder::active() || in_step(s) > 30000u) {
                overlay::banner({}, 0);
                dark(coming);
                next(s, Step::back);
            }
            break;
        }
        case Step::back:
            dark(coming);
            if (in_step(s) < fade_ms) break;
            camera(s, false);
            next(s, Step::take);
            break;
        case Step::take:
            dark(coming);
            if (trick_recorder::active()) break;
            end(s, style_editor::has_clip(s.trick) ? std::string() : trick_recorder::status());
            break;
        case Step::home:
            dark(coming);
            // The stage brings the skater back to where they were before it counts as closed.
            if (!trick_stage::active()) back_to_editor(s);
            break;
        case Step::idle:
            break;
        }
    } catch (...) {}
}
} // namespace dingosdk::style_capture
