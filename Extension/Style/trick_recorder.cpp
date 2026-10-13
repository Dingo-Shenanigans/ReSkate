#include "trick_recorder.h"
#include "style_editor.h"
#include "style_internal.h"
#include "style_layer.h"
#include "style_layer_editor.h"
#include "trick_gestures.h"
#include "trick_pad.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Style/style_pose.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include <windows.h>
#include <array>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace dingosdk::trick_recorder {
namespace {
constexpr int tries_each = 3;
constexpr std::uint64_t stage_wait_ms = 20000, board_wait_ms = 20000;
constexpr std::uint64_t mount_retry_ms = 2000;
constexpr std::uint32_t still_on_board_ms = 500;
constexpr std::uint64_t take_start_wait_ms = 10000;
// In game time, as the recording: a slow or paused game does not end the take early.
constexpr std::uint32_t take_end_wait_ms = 10000;
// A fixed 60 Hz simulation at normal speed while recording: a slow machine only takes longer to record the same ticks.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> recording_settings{
    {{"SimulationTime.ForceSimRate", "60"}, {"SimulationTime.TimeScale", "1"}}};
static_assert(style::tick_hz == 60);

std::string write_setting(std::string_view name, std::string_view value) { return change_named_setting(name, value, false); }

enum class Phase { idle, stage, board, still, take };
struct State {
    std::mutex mutex;
    Phase phase{};
    std::uint8_t trick{};
    int tries{};
    std::string message{"no recording session"}, stick_failure;
    std::optional<std::string> skipped;
    bool ui_hidden{}, simulation_set{}, kept{};
    HeldSettings simulation{&named_setting_value, &write_setting};
    std::uint64_t since{}, next_mount{};
    std::uint64_t since_tick{}, steady_tick{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
void say(State &s, std::string message) {
    logging::log(logging::Level::info, logging::Channel::skater, "Trick recorder: {}.", message);
    s.message = std::move(message);
}
std::string_view title(std::uint8_t trick) { return style::flip_trick_titles[trick]; }
void finish(State &s, std::string message) {
    (void)s.simulation.release();
    s.phase = Phase::idle;
    say(s, std::move(message));
}
void wait_on_board(State &s, std::uint64_t now) {
    s.phase = Phase::board;
    s.since = now;
    s.steady_tick = 0;
}
void failed(State &s, std::uint64_t now, std::string reason) {
    if (s.tries < tries_each) {
        say(s, std::format("the {} was not kept ({}), so it is recorded again", title(s.trick), reason));
        return wait_on_board(s, now);
    }
    finish(s, std::format("the {} was not kept: {}", title(s.trick), reason));
    s.skipped = std::move(reason);
}
} // namespace

bool start(std::uint8_t trick, bool ui_hidden, std::string &error) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.phase != Phase::idle) {
        error = std::string(style::busy_text);
        return false;
    }
    if (!style::is_flip_trick(trick)) {
        error = std::string(style::not_flip_trick_text);
        return false;
    }
    s.trick = trick;
    s.ui_hidden = ui_hidden;
    s.tries = 0;
    s.simulation_set = false;
    s.kept = false;
    s.skipped.reset();
    s.next_mount = 0;
    s.since = 0;
    s.phase = Phase::stage;
    say(s, std::format("recording the {}", title(trick)));
    return true;
}
bool active() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.phase != Phase::idle;
}
std::string message() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.message;
}
std::optional<std::string> skipped_reason() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.skipped;
}
bool kept() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.kept;
}

namespace {
void stage_step(State &s, std::uintptr_t base, std::uint64_t now) {
    if (trick_stage::ready()) {
        wait_on_board(s, now);
    } else if (s.since && now > s.since + stage_wait_ms) {
        finish(s, style::with_stage_status("the stage did not open", trick_stage::status()));
    } else if (!s.since) {
        // A game menu takes the pad, so the skater could not get on the board.
        if (multiplayer::sample_game_ui_state(base).in_menu) return finish(s, "close the game's menu first, then record again");
        trick_stage::open(s.ui_hidden);
        s.since = now;
        say(s, "opening the stage");
    } else if (!trick_stage::active()) {
        // The stage took the request on its previous tick and turned it down.
        finish(s, style::with_stage_status("the stage did not open", trick_stage::status()));
    }
}
void board_step(State &s, std::uint64_t now, std::uint64_t tick) {
    const auto local = style_layer::local_trick();
    if (!local) {
        if (now > s.since + board_wait_ms) finish(s, "the style layer could not read the skater");
        return;
    }
    if (style_layer::on_foot(local->state) && now >= s.next_mount) {
        trick_pad::mount();
        s.next_mount = now + mount_retry_ms;
    }
    if (!style_layer::riding_still(local->state)) s.steady_tick = 0;
    else if (!s.steady_tick) s.steady_tick = tick;
    if (s.steady_tick && tick >= s.steady_tick + style::to_ticks(still_on_board_ms) && trick_pad::gestures_ready()) {
        const auto trick = s.trick;
        std::string error;
        ++s.tries;
        if (!style_editor::request_record(trick, error)) return finish(s, error);
        s.phase = Phase::still;
        s.since = now;
        s.since_tick = tick;
        say(s, std::format("recording the {} (try {})", title(trick), s.tries));
    } else if (now > s.since + board_wait_ms) {
        // A menu that is open takes the button, so no later trick can start either.
        finish(s, std::format("the skater did not get on the board (physics state {}); close any menu and record again",
                              local->state));
    }
}
void still_step(State &s, std::uint64_t now, std::uint64_t tick) {
    if (tick < s.since_tick + style::to_ticks(still_ms)) {
        if (now > s.since + take_start_wait_ms) finish(s, "the recording did not start");
        return;
    }
    // A stick that does not move gives a take with no trick. Its reason is told when the take ends.
    s.stick_failure.clear();
    if (!trick_pad::play_trick(s.trick, s.stick_failure) && s.stick_failure.empty())
        s.stick_failure = "the stick did not move";
    s.phase = Phase::take;
}
void take_step(State &s, std::uint64_t now, std::uint64_t tick) {
    if (const auto taken = style_editor::take_recorded()) {
        if (!s.stick_failure.empty()) return failed(s, now, s.stick_failure);
        if (!taken->failure.empty()) return failed(s, now, taken->failure);
        finish(s, std::format("recorded the {}", title(s.trick)));
        s.kept = true;
    } else if (tick > s.since_tick + style::to_ticks(take_end_wait_ms)) {
        finish(s, "the recording did not end");
    }
}
void step(State &s, std::uintptr_t base, bool ready) {
    std::lock_guard lock(s.mutex);
    if (s.phase == Phase::idle || !ready) return;
    const auto now = GetTickCount64();
    const auto tick = trick_pad::game_ticks();
    if (!std::exchange(s.simulation_set, true))
        if (std::string error; !s.simulation.hold(recording_settings, error)) return finish(s, error);
    if (s.phase != Phase::stage && !trick_stage::ready())
        return finish(s, style::with_stage_status("the stage closed", trick_stage::status()));
    switch (s.phase) {
    case Phase::stage:
        return stage_step(s, base, now);
    case Phase::board:
        return board_step(s, now, tick);
    case Phase::still:
        return still_step(s, now, tick);
    case Phase::take:
        return take_step(s, now, tick);
    case Phase::idle:
        return;
    }
}
} // namespace

void tick(std::uintptr_t base, bool ready) noexcept {
    auto &s = state();
    // A failure that repeats would leave the game's speed forced: the recording ends and gives it back.
    style::guarded_tick(logging::Channel::skater, "Trick recorder: the recording", [&] { step(s, base, ready); }, [&] {
        std::lock_guard lock(s.mutex);
        if (s.phase == Phase::idle) return false;
        (void)s.simulation.release();
        s.phase = Phase::idle;
        s.message = "the recording stopped after an error";
        return true;
    });
}
} // namespace dingosdk::trick_recorder
