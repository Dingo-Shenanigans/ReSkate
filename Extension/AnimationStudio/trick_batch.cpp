#include "trick_batch.h"
#include "anim_studio.h"
#include "trick_gestures.h"
#include "pop_grades.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/HallOfMeat/hall_of_meat_skeleton.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/ai_skaters.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <future>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::anim_studio::batch {
namespace {
using logging::Channel;
using logging::Level;
// Every take is the quick flick of PR #230's trick pad: the game knows every trick from it. The game grades each pop
// low, medium or high (EnumGS.PopHeight, pop_grades.h), which is what the player's Minimum Pop Height setting moves,
// and a take is kept only when the game grades it as asked.
constexpr std::uint32_t flick_ms = 24;
struct Pop {
    const char* name;
    std::uint32_t crouch_ticks;         // how long the stick is held down before the flick, on the first try
    std::uint32_t shortest, longest;    // where a try graded under or over it searches next: halfway to these
};
// The pop builds while the stick is held down, so the crouch's length is the pop, as a player does it. Measured
// 2026-10-20, standing, game ticks at 60 Hz: a crouch of 3-5 ticks was graded low, 7-11 medium, 13 and up high
// (kickflip and nollie inward heelflip alike). Holding the pop's strength down instead made some gestures lose their
// flip (a nollie inward heelflip came out a nollie, 3 tries of 3). A try graded under or over what was asked crouches
// halfway toward the other side on the next one.
constexpr std::array<Pop, 3> pops{{{"low", 4, 1, 6}, {"mid", 9, 6, 13}, {"high", 18, 18, 18}}};
// One more than PR #230's recorder: a try that came out another trick moves the crouch, and needs room to.
constexpr int tries_each = 4;
constexpr std::uint8_t last_recorded_trick = 30;
// Riding on the ground (100), and the state seen after some landings (101); on foot (504). Measured 2026-10-04 (PR #230).
constexpr std::uint32_t riding_state = 100, landed_state = 101, on_foot_state = 504;
constexpr std::uint64_t stage_wait_ms = 30000, board_wait_ms = 20000, mount_retry_ms = 2000;
constexpr std::uint64_t still_ms = 500, preroll_ms = 700, take_wait_ms = 10000, home_wait_ms = 30000;
// The game's menu: settled closed this long before the stage opens; B pressed when it is still up after menu_press_ms.
constexpr std::uint64_t world_settle_ms = 500, menu_press_ms = 1500, menu_close_wait_ms = 15000;
// A menu that keeps opening during one trick ends the run rather than going round for ever.
constexpr int menu_returns_each = 3;

enum class Step { idle, gestures, world, stage, board, preroll, take, home };
struct Job {
    gestures::Gesture gesture;
    Pop pop;
};
struct State {
    std::mutex mutex;
    Step step{};
    std::future<std::vector<gestures::Gesture>> reading;
    std::vector<Job> jobs;
    std::size_t tricks{}, place{}, kept{};
    int tries{}, menu_returns{};
    // The crouch the next try holds, and the band it is searched in (Pop).
    struct CrouchSearch {
        std::uint32_t ticks{}, shortest{}, longest{}, first_ticks{};
        int wrong_tricks{}; // tries that came out another trick than asked
    } crouch_search;
    std::uint64_t since{}, still_since{}, next_press{}, last_step_at{};
    bool waiting_for_focus{};
    overlay::PadReads pad_reads_at; // the scripted pad's answers when the step began
    std::atomic<bool> stopping{};
    std::string status, summary, last_trick;
    std::vector<std::string> not_kept;
    // The physics states the skater went through in the take under way, each once in a row: why a take gave nothing.
    std::vector<std::uint32_t> take_states;
    // The simulation settings held for the run, with the values they had before (put back after).
    std::vector<std::pair<std::string_view, std::string>> held_simulation;
};
State& state() {
    static auto* value = new State;
    return *value;
}
void say(State& s, std::string text) {
    logging::log(Level::info, Channel::skater, "Record all tricks: {}.", text);
    s.status = std::move(text);
}
void next(State& s, Step step) {
    s.step = step;
    s.since = s.last_step_at = GetTickCount64();
    s.still_since = 0;
    s.pad_reads_at = overlay::pad_reads_answered();
}
void end_without_starting(State& s, std::string why) {
    s.step = Step::idle;
    s.summary = std::format("Record all tricks could not start: {}.", why);
    say(s, s.summary);
}
// The game reads the pad on its own thread only while its window has the focus; behind another window the
// scripted pad does nothing (seen 2026-10-19).
bool game_has_focus() {
    DWORD owner{};
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}
// What the game could have done with the scripted pad in this step.
std::string pad_report(const State& s, std::uintptr_t base) {
    const auto reads = overlay::pad_reads_answered();
    return std::format("game menu {}; game window {}; the pad answered {} reads, {} from the game's code",
                       multiplayer::sample_game_ui_state(base).in_menu ? "up" : "closed", game_has_focus() ? "in front" : "behind another window",
                       reads.all - s.pad_reads_at.all, reads.by_game_code - s.pad_reads_at.by_game_code);
}
// The game's own name for the job's trick, as it names a trick it does: its gesture's name without the prefix
// (kickflip, n_varialheelflip).
std::string game_name(const Job& job) {
    constexpr std::string_view prefix = "gesture_fliptrick_";
    return job.gesture.name.starts_with(prefix) ? job.gesture.name.substr(prefix.size()) : job.gesture.name;
}
std::string label(const Job& job) {
    auto name = game_name(job);
    for (auto& c : name) if (c == '_') c = ' ';
    return std::format("{} ({} pop)", name, job.pop.name);
}
// The take's crouch and the game's grade of its pop.
std::string take_report(const State& s) {
    const auto graded = pop_grades::pop_name(pop_grades::take_pop_height());
    return std::format("{} tick crouch, the game graded it {}", s.crouch_search.ticks, graded.empty() ? "nothing" : graded);
}
void end_take() noexcept {
    pop_grades::end_take();
    anim_studio::cancel_batch_take();
}
// A fixed 60 Hz simulation at normal speed for the run, as PR #230's recorder held it: the gestures' arcs have
// windows of 75 ms, so at 30 Hz the long gestures (shuvits, varials) are missed.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> run_simulation{
    {{"SimulationTime.ForceSimRate", "60"}, {"SimulationTime.TimeScale", "1"}}};
// Each setting goes back, unless something else changed it during the run.
void release_simulation(State& s) {
    for (auto it = s.held_simulation.rbegin(); it != s.held_simulation.rend(); ++it) {
        const auto ours = std::ranges::find(run_simulation, it->first, &std::pair<std::string_view, std::string_view>::first);
        if (ours != run_simulation.end() && named_setting_value(it->first) == std::string(ours->second))
            (void)change_named_setting(it->first, it->second, false);
    }
    s.held_simulation.clear();
}
// The empty string, or why the simulation could not be held. What was held before a failure is put back.
std::string hold_simulation(State& s) {
    for (const auto& [name, value] : run_simulation) {
        const auto before = named_setting_value(name);
        if (!before) {
            release_simulation(s);
            return std::format("{} could not be read", name);
        }
        if (const auto result = change_named_setting(name, value, false); result.starts_with("error")) {
            release_simulation(s);
            return std::format("{} could not be set ({})", name, result);
        }
        s.held_simulation.emplace_back(name, *before);
        logging::log(Level::info, Channel::skater, "Record all tricks: {} {} for the run (was {}).", name, value, *before);
    }
    return {};
}
// The stage closes and the skater goes back; the studio opens when they are there.
void go_home(State& s, std::string why) {
    end_take();
    release_simulation(s);
    stage::close();
    s.summary = why.empty() ? std::format("{} takes recorded{}.", s.kept, s.not_kept.empty() ? "" : std::format(", {} not kept", s.not_kept.size()))
                            : std::format("Record all tricks stopped: {}. {} takes recorded.", why, s.kept);
    say(s, s.summary);
    next(s, Step::home);
}
void next_job(State& s) {
    ++s.place;
    s.tries = s.menu_returns = 0;
    if (s.place >= s.jobs.size() || s.stopping.load()) return go_home(s, s.stopping.load() ? "you stopped it" : "");
    next(s, Step::board);
}
// Another try of the same take, unless the player asked to stop.
void try_again(State& s) {
    if (s.stopping.load()) return go_home(s, "you stopped it");
    next(s, Step::board);
}
// Why the last try of a take was not kept, for the summary. A pop graded higher even from the shortest crouch is most
// likely the player's Minimum Pop Height setting.
std::string not_kept_reason(const State& s, const BatchTake& taken, pop_grades::PopHeight graded) {
    const auto& job = s.jobs[s.place];
    const auto wanted = pop_grades::pop_height_of(job.pop.name);
    if (taken.trick != game_name(job) || !graded) return std::format("{} ({})", s.last_trick, taken.message);
    if (graded > wanted)
        return std::format("{} (graded {} even from a {} tick crouch: is Gameplay > Assists > Minimum Pop Height above Low?)", s.last_trick,
                           pop_grades::pop_name(graded), s.crouch_search.ticks);
    return std::format("{} (graded {})", s.last_trick, pop_grades::pop_name(graded));
}
// Esc, or Back (View) on a controller.
bool stop_pressed() {
    constexpr unsigned escape_key = 1u << 3;
    ControllerInput pad{};
    DingoSDKOverlayReadControllerInput(&pad, true);
    return (DingoSDKOverlayReadPromptKeys() & escape_key) != 0 || (pad.buttons & XINPUT_GAMEPAD_BACK) != 0;
}
void press(WORD buttons) {
    XINPUT_GAMEPAD pressed{}, neutral{};
    pressed.wButtons = buttons;
    overlay::play_pad({{0, pressed}, {9, neutral}, {24, neutral}});
}
// The game's menu takes the pad: a mount or a trick done under it does nothing. The game opens its menu by itself
// after a level load. While it is up, B (back) is pressed every so often, from `press_from` on.
bool menu_up(State& s, std::uintptr_t base, std::uint64_t now, std::uint64_t press_from) {
    if (!multiplayer::sample_game_ui_state(base).in_menu) return false;
    if (now >= press_from && now >= s.next_press) {
        press(XINPUT_GAMEPAD_B);
        s.next_press = now + mount_retry_ms;
    }
    return true;
}
void step(State& s, std::uintptr_t base, std::uintptr_t component) {
    const auto now = GetTickCount64();
    if (s.step == Step::idle || s.step == Step::home) {
        if (s.step == Step::home && (!stage::active() || now > s.since + home_wait_ms)) {
            s.step = Step::idle;
            anim_studio::request_open(true); // back to the studio, its list now with the recordings
        }
        return;
    }
    // A session or another skater would take the pop override too: the run ends at once.
    if (multiplayer_session_active()) return go_home(s, "a multiplayer session started");
    if (ai_skaters::active_count() > 0) return go_home(s, "AI skaters came into the game");
    if (!s.stopping.load() && stop_pressed()) {
        s.stopping.store(true);
        say(s, "stopping after this take");
    }
    // Before a take, a stop ends the run at once (Esc also opens the game's menu, which would cost another take).
    if (s.stopping.load() && (s.step == Step::world || s.step == Step::stage || s.step == Step::board)) return go_home(s, "you stopped it");
    // Behind another window the clock stops: the step goes on, as it was, once the game is in front again. A take
    // under way is let run out: the skater is already in it.
    const bool pad_steps = s.step == Step::world || s.step == Step::board || s.step == Step::preroll;
    if (pad_steps && !game_has_focus()) {
        if (!s.waiting_for_focus) say(s, "waiting: the game is behind another window; click into it to go on");
        s.waiting_for_focus = true;
        const auto paused = now - s.last_step_at;
        s.since += paused;
        if (s.still_since) s.still_since += paused;
        s.next_press += paused;
        s.last_step_at = now;
        return;
    }
    if (s.waiting_for_focus) say(s, "the game is in front again: going on");
    s.waiting_for_focus = false;
    s.last_step_at = now;
    switch (s.step) {
    case Step::idle:
    case Step::home: return;
    case Step::gestures: {
        if (s.reading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto list = s.reading.get();
            s.jobs.clear();
            for (const auto& g : list) {
                // The flip tricks 1 to 30, as PR #230 records them. The quick ollie and nollie (31, 32) pop with no crouch,
                // so the game grades every one low; the impossibles' gestures do a pop shuvit (all 12 takes, 2026-10-20).
                if (g.trick < 1 || g.trick > last_recorded_trick || g.name.find("impossible") != std::string::npos) continue;
                ++s.tricks;
                for (const auto& pop : pops) s.jobs.push_back({g, pop});
            }
        } catch (const std::exception& e) {
            return end_without_starting(s, e.what());
        }
        if (s.jobs.empty()) return end_without_starting(s, "the game's data has no flip tricks to record");
        // Without the game's grade of each pop a take cannot be told low, mid or high.
        if (std::string why; !pop_grades::install(base, why)) return end_without_starting(s, why);
        say(s, std::format("{} tricks, {} takes: opening the practice stage", s.tricks, s.jobs.size()));
        next(s, Step::world);
        return;
    }
    case Step::world:
        if (menu_up(s, base, now, s.since + menu_press_ms)) s.still_since = 0;
        else if (!s.still_since) s.still_since = now;
        if (s.still_since && now >= s.still_since + world_settle_ms) {
            if (!stage::active()) {
                // Held just before the skater goes to the stage: the teleport there starts their step at the held rate.
                if (s.held_simulation.empty())
                    if (const auto why = hold_simulation(s); !why.empty()) return go_home(s, why);
                stage::open(false);
            }
            return next(s, Step::stage);
        }
        if (now > s.since + menu_close_wait_ms) return go_home(s, std::format("the game's menu did not close ({})", pad_report(s, base)));
        return;
    case Step::stage:
        if (stage::ready()) return next(s, Step::board);
        if (now > s.since + stage_wait_ms || (now > s.since + 2000 && !stage::active())) return go_home(s, "the practice stage did not open: " + stage::status());
        return;
    case Step::board: {
        if (!stage::ready()) return go_home(s, "the practice stage closed");
        if (multiplayer::sample_game_ui_state(base).in_menu) {
            if (++s.menu_returns > menu_returns_each) return go_home(s, "the game's menu kept opening");
            return next(s, Step::world);
        }
        const auto state_now = anim_studio::physics_state(base, component);
        if (state_now && *state_now == on_foot_state && now >= s.next_press) {
            press(XINPUT_GAMEPAD_Y); // onto the board
            s.next_press = now + mount_retry_ms;
        }
        const bool riding = state_now && (*state_now == riding_state || *state_now == landed_state);
        if (!riding) s.still_since = 0;
        else if (!s.still_since) s.still_since = now;
        if (s.still_since && now >= s.still_since + still_ms) {
            if (!s.tries) {
                const auto& pop = s.jobs[s.place].pop;
                s.crouch_search = {pop.crouch_ticks, pop.shortest, pop.longest, pop.crouch_ticks, 0};
            }
            ++s.tries;
            s.last_trick = label(s.jobs[s.place]);
            say(s, std::format("{} of {}: {} (try {})", s.place + 1, s.jobs.size(), s.last_trick, s.tries));
            anim_studio::arm_batch_take(game_name(s.jobs[s.place]), s.jobs[s.place].pop.name);
            return next(s, Step::preroll);
        }
        if (now > s.since + board_wait_ms) return go_home(s, std::format("the skater did not get on the board (physics state {}; {})", state_now.value_or(0), pad_report(s, base)));
        return;
    }
    case Step::preroll: {
        // The recording keeps 0.7 s before the takeoff: the skater stands still that long first.
        if (now < s.since + preroll_ms) return;
        const auto& job = s.jobs[s.place];
        pop_grades::begin_take(take_wait_ms + 2000);
        overlay::play_pad(gestures::flick(job.gesture, flick_ms, s.crouch_search.ticks));
        s.take_states.clear();
        return next(s, Step::take);
    }
    case Step::take: {
        if (const auto state_now = anim_studio::physics_state(base, component); state_now && (s.take_states.empty() || s.take_states.back() != *state_now))
            s.take_states.push_back(*state_now);
        if (const auto taken = anim_studio::take_batch_result()) {
            const auto graded = pop_grades::take_pop_height();
            const auto report = take_report(s);
            end_take();
            if (taken->ok) {
                ++s.kept;
                say(s, std::format("kept: {}, {:.2f} s in the air; {}", s.last_trick, taken->air, report));
                return next_job(s);
            }
            if (s.tries < tries_each) {
                const auto wanted = pop_grades::pop_height_of(s.jobs[s.place].pop.name);
                // The pop of another trick than asked says nothing about this one's band.
                if (taken->trick == game_name(s.jobs[s.place]) && graded && graded != wanted) {
                    auto& search = s.crouch_search;
                    if (graded < wanted) {
                        search.shortest = search.ticks;
                        search.ticks = std::min(search.longest, (search.ticks + search.longest + 1) / 2);
                    } else {
                        search.longest = search.ticks;
                        search.ticks = std::max(search.shortest, (search.shortest + search.ticks) / 2);
                    }
                } else if (taken->trick != game_name(s.jobs[s.place])) {
                    // Another trick than asked: the same crouch tends to give it again (a nollie varial kickflip from a
                    // 4 tick crouch came out a nollie kickflip 3 times of 3), so the next try crouches a tick to either
                    // side, inside the band.
                    auto& search = s.crouch_search;
                    const int side = ++search.wrong_tricks % 2 ? 1 : -1, reach = (search.wrong_tricks + 1) / 2;
                    const auto moved = static_cast<int>(search.first_ticks) + side * reach;
                    search.ticks = static_cast<std::uint32_t>(std::clamp(moved, static_cast<int>(search.shortest), static_cast<int>(search.longest)));
                }
                say(s, std::format("{} not kept ({}; {}), again", s.last_trick, taken->message, report));
                return try_again(s);
            }
            s.not_kept.push_back(not_kept_reason(s, *taken, graded));
            return next_job(s);
        }
        if (now > s.since + take_wait_ms) {
            std::string states;
            for (const auto state_seen : s.take_states) states += std::format("{}{}", states.empty() ? "" : " ", state_seen);
            say(s, std::format("{}: nothing recorded in {} s (physics states {}; {}; {})", s.last_trick, take_wait_ms / 1000,
                               states.empty() ? "none read" : states, take_report(s), pad_report(s, base)));
            end_take();
            if (s.tries < tries_each) return try_again(s);
            s.not_kept.push_back(s.last_trick + " (no trick landed)");
            return next_job(s);
        }
        return;
    }
    }
}
} // namespace

bool start(std::string& why) {
    if (multiplayer_session_active()) {
        why = "Your tricks are recorded in solo play only.";
        return false;
    }
    // The game's pops are told apart by skater nowhere in what is read: another skater's pop would count as the player's.
    if (ai_skaters::active_count() > 0) {
        why = "Your tricks are recorded with no AI skaters in the game: clear them first.";
        return false;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.step != Step::idle) {
        why = "Record all tricks is already running.";
        return false;
    }
    s.jobs.clear();
    s.tricks = s.place = s.kept = 0;
    s.tries = s.menu_returns = 0;
    s.not_kept.clear();
    s.summary.clear();
    s.stopping.store(false);
    try {
        s.reading = std::async(std::launch::async, [] { return gestures::read(hall_of_meat::game_folder()); });
    } catch (...) {
        why = "The game's gestures could not be read.";
        return false;
    }
    next(s, Step::gestures);
    say(s, "reading the game's flip trick gestures");
    return true;
}
void stop() noexcept { state().stopping.store(true); }
bool active() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.step != Step::idle;
}
Progress progress() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    Progress p;
    p.active = s.step != Step::idle;
    p.stopping = s.stopping.load();
    p.place = s.place;
    p.total = s.jobs.size();
    p.kept = s.kept;
    p.trick = s.last_trick;
    p.status = s.status;
    return p;
}
std::string take_summary() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.step == Step::idle ? std::exchange(s.summary, {}) : std::string{};
}
void tick(std::uintptr_t base, std::uintptr_t client, std::uintptr_t component, bool ready) noexcept {
    auto& s = state();
    try {
        stage::tick(base, client, ready);
        std::lock_guard lock(s.mutex);
        if (ready && base) step(s, base, component);
    } catch (...) {
        try {
            std::lock_guard lock(s.mutex);
            if (s.step != Step::idle && s.step != Step::home) go_home(s, "an error");
        } catch (...) {
            pop_grades::end_take();
        }
    }
}
} // namespace dingosdk::anim_studio::batch
