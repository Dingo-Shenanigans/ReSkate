#include "trick_recorder.h"
#include "style_editor.h"
#include "style_layer.h"
#include "trick_pad.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <array>
#include <cstdlib>
#include <format>
#include <mutex>
#include <utility>

namespace dingosdk::trick_recorder {
namespace {
// Physics states: on foot, and on the board on the ground.
constexpr std::uint32_t on_foot = addr::style::offboard_state, on_board = addr::style::riding_ground_state;
constexpr int tries_each = 3;
// The game's simulation while it records: the tick rate of the recording, at normal speed. A slow machine then only takes
// longer to record the same ticks.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> recording_settings{
    {{"SimulationTime.ForceSimRate", "60"}, {"SimulationTime.TimeScale", "1"}}};
static_assert(trick_pad::tick_hz == 60);

enum class Phase { idle, stage, board, still, take };
struct State {
    std::mutex mutex;
    Phase phase{};
    std::vector<std::uint8_t> tricks;
    std::size_t next{};
    int tries{}, kept{};
    std::string flick, message{"No recording session."}, missed; // missed: why the stick did not move
    std::uint32_t flick_ms{};
    std::vector<std::string> failed;
    bool ui_hidden{}, opened{}, want_stop{}, simulation_set{}, keep_stage{};
    std::vector<std::pair<std::string, std::string>> simulation_before; // the settings that the recording changed
    std::uint64_t since{}, mounted{};
    std::uint64_t since_tick{}, steady_tick{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
void say(State &s, std::string message) {
    logging::log(logging::Level::info, logging::Channel::skater, "Trick recorder: {}", message);
    s.message = std::move(message);
}
std::string_view name(std::uint8_t trick) { return style::flip_trick_names[trick]; }
// Sets the recording's simulation settings. Game thread only.
bool set_simulation(State &s, std::string &error) {
    s.simulation_set = true;
    for (const auto &[name, value] : recording_settings) {
        const auto before = named_setting_value(name).value_or(std::string{});
        if (before.empty()) {
            error = std::format("The game's {} setting could not be read.", name);
            return false;
        }
        if (std::strtod(before.c_str(), nullptr) == std::strtod(std::string(value).c_str(), nullptr)) continue;
        if (const auto result = change_named_setting(name, value, false); result.starts_with("error")) {
            error = std::format("The game's {} could not be set to {} ({}).", name, value, result);
            return false;
        }
        s.simulation_before.emplace_back(name, before);
    }
    return true;
}
void finish(State &s, std::string message) {
    for (const auto &[name, value] : std::exchange(s.simulation_before, {})) (void)change_named_setting(name, value, false);
    if (s.opened && !s.keep_stage) trick_stage::close();
    s.phase = Phase::idle;
    overlay::notify(overlay::NoticeLevel::info, "Trick recorder", message);
    say(s, std::move(message));
}
void wait_on_board(State &s, std::uint64_t now) {
    s.phase = Phase::board;
    s.since = now;
    s.steady_tick = 0;
}
void next_trick(State &s, std::uint64_t now) {
    s.tries = 0;
    if (++s.next < s.tricks.size()) return wait_on_board(s, now);
    std::string message = std::format("Recorded {} of {} tricks.", s.kept, s.tricks.size());
    if (!s.failed.empty()) {
        message += " Not kept:";
        for (const auto &failure : s.failed) message += (&failure == &s.failed.front() ? " " : "; ") + failure;
        message += ".";
    }
    finish(s, std::move(message));
}
void failed(State &s, std::uint64_t now, std::string reason) {
    const auto trick = s.tricks[s.next];
    if (s.tries < tries_each) {
        say(s, std::format("the {} was not kept ({}), so it is recorded again.", name(trick), reason));
        return wait_on_board(s, now);
    }
    s.failed.push_back(std::format("{} ({})", name(trick), reason));
    next_trick(s, now);
}
} // namespace

bool start(std::vector<std::uint8_t> tricks, std::string flick, bool ui_hidden, std::string &error, bool keep_stage) {
    unsigned ms{};
    if (!flick.empty() && (sscanf_s(flick.c_str(), "%u", &ms) != 1 || ms > 1000)) {
        error = "Give the flick's duration in milliseconds, 0 to 1000.";
        return false;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.phase != Phase::idle) {
        error = "A recording session is running (style record stop).";
        return false;
    }
    if (tricks.empty()) {
        error = "There are no tricks to record.";
        return false;
    }
    s.tricks = std::move(tricks);
    s.flick = std::move(flick);
    s.flick_ms = s.flick.empty() ? trick_pad::default_flick_ms : ms;
    s.ui_hidden = ui_hidden;
    s.keep_stage = keep_stage;
    s.next = 0;
    s.tries = s.kept = 0;
    s.failed.clear();
    s.opened = s.want_stop = s.simulation_set = false;
    s.mounted = 0;
    s.phase = Phase::stage;
    say(s, std::format("recording {} trick(s).", s.tricks.size()));
    return true;
}
void stop() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.want_stop = s.phase != Phase::idle;
}
bool active() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.phase != Phase::idle;
}
std::string status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.phase == Phase::idle) return s.message;
    return std::format("Recording the {} ({} of {}, {} kept so far). {}", name(s.tricks[s.next]), s.next + 1, s.tricks.size(), s.kept, s.message);
}

void tick(std::uintptr_t base, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (s.phase == Phase::idle || !ready) return;
        const auto now = GetTickCount64();
        const auto tick = trick_pad::game_ticks();
        if (std::exchange(s.want_stop, false)) return finish(s, "The recording session was stopped.");
        if (std::string error; !s.simulation_set && !set_simulation(s, error)) return finish(s, error);
        if (s.phase != Phase::stage && !trick_stage::ready()) return finish(s, "The stage closed: " + trick_stage::status());
        switch (s.phase) {
        case Phase::stage:
            if (trick_stage::ready()) {
                wait_on_board(s, now);
            } else if (!trick_stage::active()) {
                // The stage takes the request on its next tick, which runs before this one.
                if (s.opened) return finish(s, "The stage did not open: " + trick_stage::status());
                // A game menu takes the pad, so the skater could not get on the board.
                if (multiplayer::sample_game_ui_state(base).in_menu) return finish(s, "Close the game's menu first, then record again.");
                trick_stage::open(s.ui_hidden);
                s.opened = true;
                say(s, "opening the stage.");
            }
            break;
        case Phase::board: {
            std::uint32_t physics{};
            std::int32_t flip{};
            if (!style_layer::local_trick(physics, flip)) break;
            if (physics == on_foot && now >= s.mounted + 2000) {
                std::string error;
                (void)trick_pad::play("mount", {}, error);
                s.mounted = now;
            }
            // The skater has stood still on the board for a moment.
            if (physics != on_board) s.steady_tick = 0;
            else if (!s.steady_tick) s.steady_tick = tick;
            if (s.steady_tick && tick >= s.steady_tick + trick_pad::to_ticks(500) && trick_pad::gestures_ready()) {
                const auto trick = s.tricks[s.next];
                std::string error;
                ++s.tries;
                if (!style_editor::request_record(trick, s.flick_ms, error)) return finish(s, error);
                s.phase = Phase::still;
                s.since = now;
                s.since_tick = tick;
                say(s, std::format("recording the {} ({} of {}, try {}).", name(trick), s.next + 1, s.tricks.size(), s.tries));
            } else if (now > s.since + 20000) {
                // A menu that is open takes the button, so no later trick can start either.
                finish(s, std::format("The skater did not get on the board (physics state {}). Close any menu and record again.", physics));
            }
            break;
        }
        case Phase::still:
            if (tick < s.since_tick + trick_pad::to_ticks(still_ms)) break;
            // A stick that does not move gives a take with no trick. Its reason is told when the take ends.
            s.missed.clear();
            if (!trick_pad::play(name(s.tricks[s.next]), s.flick, s.missed) && s.missed.empty()) s.missed = "the stick did not move";
            s.phase = Phase::take;
            break;
        case Phase::take:
            if (const auto taken = style_editor::take_recorded()) {
                if (!s.missed.empty()) return failed(s, now, s.missed);
                if (!taken->failure.empty()) return failed(s, now, taken->failure);
                ++s.kept;
                next_trick(s, now);
            } else if (now > s.since + 10000) {
                finish(s, std::format("The recording of the {} did not end.", name(s.tricks[s.next])));
            }
            break;
        case Phase::idle:
            break;
        }
    } catch (...) {}
}
} // namespace dingosdk::trick_recorder
