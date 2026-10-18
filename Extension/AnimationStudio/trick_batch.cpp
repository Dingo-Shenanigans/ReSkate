#include "trick_batch.h"
#include "anim_studio.h"
#include "trick_gestures.h"
#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/HallOfMeat/hall_of_meat_skeleton.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <format>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace dingosdk::anim_studio::batch {
namespace {
using logging::Channel;
using logging::Level;
// The flicks for a low, a mid and a high pop: the game takes the pop's height from the flick's speed.
constexpr std::array<std::uint32_t, 3> flicks_ms{260, 110, 24};
constexpr int tries_each = 2;
// Riding on the ground (100), and the state seen after some landings (101); on foot (504). Measured 2026-10-04 (PR #230).
constexpr std::uint32_t riding_state = 100, landed_state = 101, on_foot_state = 504;
constexpr std::uint64_t stage_wait_ms = 30000, board_wait_ms = 20000, mount_retry_ms = 2000;
constexpr std::uint64_t still_ms = 500, preroll_ms = 700, take_wait_ms = 10000, home_wait_ms = 30000;

enum class Step { idle, gestures, stage, board, preroll, take, home };
struct Job {
    gestures::Gesture gesture;
    std::uint32_t flick_ms{};
};
struct State {
    std::mutex mutex;
    Step step{};
    std::future<std::vector<gestures::Gesture>> reading;
    std::vector<Job> jobs;
    std::size_t place{}, kept{};
    int tries{};
    std::uint64_t since{}, still_since{}, next_mount{};
    std::atomic<bool> stopping{};
    std::string status, summary, last_trick;
    std::vector<std::string> not_kept;
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
    s.since = GetTickCount64();
    s.still_since = 0;
}
std::string label(const Job& job) {
    auto name = job.gesture.name;
    if (name.starts_with("gesture_fliptrick_")) name = name.substr(18);
    for (auto& c : name) if (c == '_') c = ' ';
    const char* pop = job.flick_ms >= 200 ? "low" : job.flick_ms >= 80 ? "mid" : "high";
    return std::format("{} ({} pop)", name, pop);
}
// The stage closes and the skater goes back; the studio opens when they are there.
void go_home(State& s, std::string why) {
    anim_studio::cancel_batch_take();
    stage::close();
    s.summary = why.empty() ? std::format("{} takes recorded{}.", s.kept, s.not_kept.empty() ? "" : std::format(", {} not kept", s.not_kept.size()))
                            : std::format("Record all tricks stopped: {}. {} takes recorded.", why, s.kept);
    say(s, s.summary);
    next(s, Step::home);
}
void next_job(State& s) {
    ++s.place;
    s.tries = 0;
    if (s.place >= s.jobs.size() || s.stopping.load()) return go_home(s, s.stopping.load() ? "you stopped it" : "");
    next(s, Step::board);
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
void step(State& s, std::uintptr_t base, std::uintptr_t component) {
    const auto now = GetTickCount64();
    if (s.step != Step::idle && s.step != Step::home && !s.stopping.load() && stop_pressed()) {
        s.stopping.store(true);
        say(s, "stopping after this take");
    }
    switch (s.step) {
    case Step::idle: return;
    case Step::gestures: {
        if (s.reading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto list = s.reading.get();
            s.jobs.clear();
            for (const auto& g : list)
                for (const auto ms : flicks_ms) s.jobs.push_back({g, ms});
        } catch (const std::exception& e) {
            s.step = Step::idle;
            s.summary = std::format("Record all tricks could not start: {}.", e.what());
            say(s, s.summary);
            return;
        }
        say(s, std::format("{} tricks, {} takes: opening the practice stage", s.jobs.size() / flicks_ms.size(), s.jobs.size()));
        stage::open(false);
        next(s, Step::stage);
        return;
    }
    case Step::stage:
        if (stage::ready()) return next(s, Step::board);
        if (now > s.since + stage_wait_ms || (now > s.since + 2000 && !stage::active())) return go_home(s, "the practice stage did not open: " + stage::status());
        return;
    case Step::board: {
        if (!stage::ready()) return go_home(s, "the practice stage closed");
        const auto state_now = anim_studio::physics_state(base, component);
        if (state_now && *state_now == on_foot_state && now >= s.next_mount) {
            press(XINPUT_GAMEPAD_Y); // onto the board
            s.next_mount = now + mount_retry_ms;
        }
        const bool riding = state_now && (*state_now == riding_state || *state_now == landed_state);
        if (!riding) s.still_since = 0;
        else if (!s.still_since) s.still_since = now;
        if (s.still_since && now >= s.still_since + still_ms) {
            ++s.tries;
            s.last_trick = label(s.jobs[s.place]);
            say(s, std::format("{} of {}: {} (try {})", s.place + 1, s.jobs.size(), s.last_trick, s.tries));
            anim_studio::arm_batch_take();
            return next(s, Step::preroll);
        }
        if (now > s.since + board_wait_ms) return go_home(s, std::format("the skater did not get on the board (physics state {})", state_now.value_or(0)));
        return;
    }
    case Step::preroll:
        // The recording keeps 0.7 s before the takeoff: the skater stands still that long first.
        if (now < s.since + preroll_ms) return;
        overlay::play_pad(gestures::flick(s.jobs[s.place].gesture, s.jobs[s.place].flick_ms));
        return next(s, Step::take);
    case Step::take: {
        if (const auto taken = anim_studio::take_batch_result()) {
            if (taken->ok) {
                ++s.kept;
                say(s, std::format("kept: {} ({} pop, {:.2f} s in the air)", taken->trick, taken->pop, taken->air));
                return next_job(s);
            }
            if (s.tries < tries_each) {
                say(s, std::format("{} not kept ({}), again", s.last_trick, taken->message));
                return next(s, Step::board);
            }
            s.not_kept.push_back(s.last_trick);
            return next_job(s);
        }
        if (now > s.since + take_wait_ms) {
            anim_studio::cancel_batch_take();
            if (s.tries < tries_each) return next(s, Step::board);
            s.not_kept.push_back(s.last_trick + " (no trick landed)");
            return next_job(s);
        }
        return;
    }
    case Step::home:
        if (!stage::active() || now > s.since + home_wait_ms) {
            s.step = Step::idle;
            anim_studio::request_open(true); // back to the studio, its list now with the recordings
        }
        return;
    }
}
} // namespace

bool start(std::string& why) {
    if (multiplayer_session_active()) {
        why = "Your tricks are recorded in solo play only.";
        return false;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.step != Step::idle) {
        why = "Record all tricks is already running.";
        return false;
    }
    s.jobs.clear();
    s.place = s.kept = 0;
    s.tries = 0;
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
        }
    }
}
} // namespace dingosdk::anim_studio::batch
