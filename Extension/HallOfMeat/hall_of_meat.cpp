#include "hall_of_meat.h"
#include "hall_of_meat_hud.h"
#include "hall_of_meat_overlay.h"
#include "hall_of_meat_render.h"
#include "hall_of_meat_skater.h"
#include "hall_of_meat_skeleton.h"
#include "hall_of_meat_slow_motion.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace dingosdk::hall_of_meat {
namespace {
constexpr const char* preference = "HallOfMeat";
constexpr std::string_view best_prefix = "HallOfMeat.Best."; // + the level, lower case

struct State {
    std::atomic<bool> ready{}, on{}; // the switch: started, and switched on
    SRWLOCK lock = SRWLOCK_INIT; // guards the rest; never held across game reads
    Tracker tracker;
    bool summary_pending{};
    Summary summary;
    // The current map's best Meat (known once the client tick named the map), where the last
    // finished bail that showed stands against it, and whether a new best is still to be saved.
    bool best_known{}, best_unsaved{};
    int best{};
    Standing standing;
    std::string level; // client thread only
    SlowMotion slow_motion; // client thread only: after a break
    bool active{}; // client thread only: the hooks on, as the switch is or until what it changed is given back
};
State& state() { static auto* value = new State; return *value; }

// A bail is over (under the lock): it is logged on the client tick, and stands against the map's best.
void finished(State& s, const Summary& ended) noexcept {
    s.summary = ended;
    s.summary_pending = true;
    s.standing = ended.shown && s.best_known ? standing(s.best, ended.tally.score) : Standing{};
    if (s.standing.new_best) {
        s.best = s.standing.best;
        s.best_unsaved = true;
    }
}

// Physics thread, every step of the local skater (no_bail.h): what each body touched in the step
// and whether it is a ragdoll, and so where the bail is and what it did.
void observe_step(const NoBailSkater& skater, float seconds, bool wipeout) noexcept {
    if (!enabled()) return;
    auto& s = state();
    const auto now = GetTickCount64();
    const auto step = read_step(skater, seconds, wipeout);
    Summary ended;
    AcquireSRWLockExclusive(&s.lock);
    if (s.tracker.step(now, step, &ended)) finished(s, ended);
    ReleaseSRWLockExclusive(&s.lock);
}

// One debug line per bail: what it scored and why.
void log_bail(const Summary& summary) {
    std::vector<std::size_t> order;
    for (std::size_t index = 1; index < skater_body::count; ++index)
        if (summary.peaks[index] > 0) order.push_back(index);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return summary.peaks[a] > summary.peaks[b]; });
    std::string hits;
    for (const auto index : order) {
        const float peak = summary.peaks[index];
        if (peak < hit_speed && !hits.empty()) break;
        hits += std::format("{}{} {:.1f} m/s{}", hits.empty() ? "" : ", ", skater_body::names[index], peak,
            peak >= broken_speed ? " (broken)" : peak >= hit_speed ? " (hit)" : " (hardest, not hurt)");
    }
    std::string scrapes;
    for (std::size_t index = 1; index < skater_body::count; ++index)
        if (summary.scraped[index] >= 0.05f)
            scrapes += std::format("{}{} {:.1f} m", scrapes.empty() ? "" : ", ", skater_body::names[index], summary.scraped[index]);
    const auto& tally = summary.tally;
    logging::log(logging::Level::debug, logging::Channel::skater,
        "Hall of Meat: bail over, {} Meat ({} damage: {} from {} impacts, head +{}, vehicle +{}, road rash {:.1f} m +{}; "
        "{} broken +{}; {:.1f} s +{}; {:.1f} s airtime +{}; fell {:.1f} m +{}; top speed {:.1f} m/s +{}; {:.1f} rotations +{}). "
        "Hits: {}. Road rash: {}",
        tally.score, tally.damage, tally.hit_points, tally.impacts, tally.head_bonus, tally.vehicle_bonus,
        static_cast<double>(tally.scraped), tally.scrape_points, tally.broken, tally.broken * points_per_break,
        static_cast<double>(tally.seconds), tally.time_points, static_cast<double>(tally.airtime), tally.airtime_points,
        static_cast<double>(tally.fallen), tally.fall_points, static_cast<double>(tally.top_speed), tally.speed_points,
        static_cast<double>(tally.rotations), tally.rotation_points,
        hits.empty() ? std::string("no body contact") : hits, scrapes.empty() ? std::string("none") : scrapes);
}

std::string best_key(std::string_view level) {
    std::string key(best_prefix);
    for (const char c : level) key += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return key;
}
int saved_best(std::string_view level) noexcept {
    const auto value = profile_runtime::local_value(best_key(level));
    if (!value || !value->is_number()) return 0;
    const auto best = value->get<double>();
    return std::isfinite(best) && best > 0 && best < 1e9 ? static_cast<int>(best) : 0;
}
}

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return true;
    if (!start_skater(base)) return false; // logged
    // The hooks are prepared here and switched on and off with the feature (on_client_tick).
    (void)start_render(base); // without it no skeleton shows: logged
    start_hud(base);
    const bool on = profile_runtime::local_preference(preference).value_or(false);
    // The overlay builds its atlas once, when its graphics start, so the images are read now.
    overlay::prepare_hall_of_meat_images();
    s.on.store(on, std::memory_order_release);
    if (on) set_no_bail_step_observer(&observe_step);
    s.ready.store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Hall of Meat ready ({}).", on ? "on" : "off");
    return true;
}

void on_client_tick() noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    const bool on = enabled();
    if (!on && !s.active) return; // switched off: nothing hooked, nothing to give back
    if (on && !s.active) {
        prepare_skeleton(); // once, in the background
        (void)hook_render(true);
        (void)hook_hud(true);
        s.active = true;
    }
    try {
        NoBailSkater skater;
        const bool gone = !no_bail_skater(skater);
        const auto now = GetTickCount64();
        Summary summary, ended;
        bool pending{}, unsaved{}, card{};
        int best{};
        float speed{};
        AcquireSRWLockExclusive(&s.lock);
        if (gone && s.tracker.lose(now, &ended)) finished(s, ended);
        card = s.tracker.view(now).phase != Phase::riding;
        speed = s.tracker.game_speed(now);
        std::swap(pending, s.summary_pending);
        if (pending) summary = s.summary;
        std::swap(unsaved, s.best_unsaved);
        best = s.best;
        ReleaseSRWLockExclusive(&s.lock);
        // The renderer's picture of the skater is only read while a bail shows.
        follow_render(card);
        // The card takes the place of skate.'s bottom left HUD while it shows: the bail is what counts.
        const bool corner_back = hide_hud(card);
        // A break slows the game down (in single player only: hall_of_meat_slow_motion.h).
        (void)s.slow_motion.set(speed);
        if (pending && logging::enabled(logging::Level::debug)) log_bail(summary);
        if (unsaved && !s.level.empty()) profile_runtime::set_local_values({{best_key(s.level), static_cast<double>(best)}});
        // Switched off (set_enabled ended the bail): the hooks go once the corner and the game's speed are given back.
        if (!on && corner_back && s.slow_motion.idle() && hook_hud(false) && hook_render(false)) s.active = false;
    } catch (...) { /* A lost log line or best is never worth the client tick. */ }
}

void set_level(std::string_view level) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire) || level == s.level) return;
    try {
        s.level = level;
        const int best = level.empty() ? 0 : saved_best(level);
        AcquireSRWLockExclusive(&s.lock);
        s.best_known = !level.empty();
        s.best = best;
        s.best_unsaved = false;
        s.standing = {};
        ReleaseSRWLockExclusive(&s.lock);
    } catch (...) { /* The map's best stays unknown: no card shows one. */ }
}

bool available() noexcept { return state().ready.load(std::memory_order_acquire); }
bool enabled() noexcept { return available() && state().on.load(std::memory_order_acquire); }

void set_enabled(bool enabled) noexcept {
    auto& s = state();
    s.on.store(enabled, std::memory_order_release);
    // Switched off, No Bail's hook hands no step on: the physics step pays nothing for it.
    if (s.ready.load(std::memory_order_acquire)) set_no_bail_step_observer(enabled ? &observe_step : nullptr);
    if (!enabled) {
        AcquireSRWLockExclusive(&s.lock);
        s.tracker.reset();
        s.summary_pending = false;
        ReleaseSRWLockExclusive(&s.lock);
    }
    profile_runtime::set_local_preference(preference, enabled);
}

Frame frame() {
    auto& s = state();
    if (!enabled()) return {};
    const auto now = GetTickCount64();
    View view;
    Standing against; // the card's: the bail against the map's best before it
    AcquireSRWLockExclusive(&s.lock);
    view = s.tracker.view(now);
    if (view.phase == Phase::bailing || view.phase == Phase::down)
        against = s.best_known ? standing(s.best, view.tally.score) : Standing{};
    else if (view.phase == Phase::getting_up) against = s.standing; // the best already counts this bail
    ReleaseSRWLockExclusive(&s.lock);
    if (view.phase == Phase::riding) return {};

    Frame result;
    result.card = card(view, against);
    result.break_pulse = view.break_pulse;
    // The skeleton as the renderer drew the skater in the latest picture, seen by its camera.
    auto mesh = skeleton_mesh();
    Picture picture;
    PosedSkeleton posed;
    if (!mesh || !latest_picture(picture) || !pose_skeleton(*mesh, picture.skin, posed)) return result;
    auto& skeleton = result.skeleton;
    skeleton.camera = picture.camera;
    skeleton.vertical_fov = picture.vertical_fov;
    skeleton.positions = std::move(posed.positions);
    skeleton.normals = std::move(posed.normals);
    skeleton.mesh = std::move(mesh);
    skeleton.alpha = view.alpha;
    skeleton.injuries = view.injuries;
    skeleton.flashes = view.flashes;
    return result;
}
}
