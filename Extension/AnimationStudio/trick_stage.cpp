#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace dingosdk::anim_studio::stage {
namespace {
// 20 km from the map's origin and 300 m up: no map draws anything there.
constexpr std::array<float, 3> spot{20000, 300, 20000};
constexpr const char* floor_item = "own_bkpads_generic_padlow_00001";
constexpr std::uint64_t floor_id = 1;
// A padlow is 3 x 0.5 x 3 m; scaled by 20 it is a 60 m square with its top 10 m above its origin.
constexpr float floor_scale = 20;
constexpr float floor_top = 0.5f * floor_scale;
constexpr float standing_root_height = 0.95f;
// The floor moves under the skater when they are this far from its middle (its edge is 30 m away).
constexpr float follow = 12;
constexpr std::uint64_t height_hold_ms = 300, fall_wait_ms = 10000;
constexpr std::uint64_t floor_make_ms = 15000, floor_move_ms = 1500, floor_ready_ms = 2000;
constexpr std::uint64_t arrive_stand_ms = 1000, teleport_show_ms = 1000, teleport_retry_ms = 4000;
constexpr std::uint64_t return_stand_ms = 500, return_retry_ms = 500;
constexpr int max_arrive_tries = 3, max_return_tries = 20;

using Point = std::array<float, 3>;
float ground_distance(const Point& a, const Point& b) noexcept { return std::hypot(a[0] - b[0], a[2] - b[2]); }

enum class Phase { closed, placing, arriving, open, returning };
struct State {
    std::mutex mutex;
    Phase phase{};
    bool want_open{}, want_close{}, ui_was_hidden{}, hid_ui{};
    StagePlace place;
    std::string message{"the stage is closed"};
    Point back{};
    std::array<float, 2> centre{spot[0], spot[2]};
    std::uint64_t since{}, steady_since{}, made{};
    std::uint64_t height_held_until{}, fall_wait_until{};
    float checked_y{};
    bool moved{};
    int tries{};
};
State& state() {
    static auto* value = new State;
    return *value;
}
void say(State& s, std::string message, logging::Level level = logging::Level::info) {
    logging::log(level, logging::Channel::objects, "Animator Studio stage: {}.", message);
    s.message = std::move(message);
}
float floor_height() { return spot[1] + floor_top + standing_root_height; }
bool place(State& s) {
    const profile::PlacedObject floor{floor_id, floor_item, {s.centre[0], spot[1], s.centre[1]}, {0, 0, 0, 1}, floor_scale};
    return set_stage_objects(s.place.map, std::span(&floor, 1));
}
// Takes the skater back unless the floor is still being made. `left` is false when the floor went away with the map.
void finish(State& s, std::string message, bool left = true) {
    if (left) (void)set_stage_objects(s.place.map, {});
    const bool sent = left && s.phase != Phase::placing && teleport_local_skater(s.back);
    if (s.hid_ui) console::request_debug(overlay::DebugAction::set_game_ui_hidden, false);
    s.hid_ui = false;
    s.phase = sent ? Phase::returning : Phase::closed;
    s.since = GetTickCount64();
    s.steady_since = 0;
    s.tries = 0;
    say(s, std::move(message));
}
Point centre_point(const State& s) { return {s.centre[0], 0, s.centre[1]}; }
// Above or below the floor's middle: the teleport has happened.
bool on_floor_column(const State& s, const Point& at) { return ground_distance(at, centre_point(s)) < 3; }
bool on_floor(const State& s, const Point& at, float reach) {
    return ground_distance(at, centre_point(s)) < reach && std::abs(at[1] - floor_height()) < 0.5f;
}
} // namespace

void open(bool ui_hidden) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.want_open = s.phase == Phase::closed || s.phase == Phase::returning;
    s.want_close = false;
    s.height_held_until = s.fall_wait_until = 0;
    s.ui_was_hidden = ui_hidden;
}
void close() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.want_open = false;
    s.want_close = s.phase != Phase::closed;
}
bool active() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.phase != Phase::closed || s.want_open;
}
bool ready() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.phase == Phase::open;
}
std::string status() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.message;
}

namespace {
// The skater's root: the transform that the debug camera reads.
std::optional<Point> skater_position(std::uintptr_t base, std::uintptr_t client) {
    const auto local = multiplayer::capture_local(base, client, false);
    if (local.ready && std::ranges::all_of(local.pose.root.position, [](float v) { return std::isfinite(v); }))
        return local.pose.root.position;
    return std::nullopt;
}
void open_step(State& s, const std::optional<Point>& at, const std::optional<StagePlace>& map, std::uint64_t now) {
    s.want_open = false;
    if (multiplayer_session_active()) return say(s, "your tricks are recorded in solo play only");
    if (!map) return say(s, "objects cannot be placed on this map right now");
    if (!at) return say(s, "there is no skater to put on the stage");
    // A falling skater would come back to a place in the air: the open waits until their height holds for height_hold_ms.
    if (!s.fall_wait_until) s.fall_wait_until = now + fall_wait_ms;
    if (!s.height_held_until || at->at(1) < s.checked_y - 0.3f) {
        if (now > s.fall_wait_until) return say(s, "the skater is falling, so the stage did not open");
        s.height_held_until = now + height_hold_ms;
        s.checked_y = at->at(1);
    }
    if (now < s.height_held_until) {
        s.want_open = true;
        return;
    }
    s.place = *map;
    s.back = *at;
    s.centre = {spot[0], spot[2]};
    if (!place(s)) return say(s, "the stage's floor could not be asked for");
    s.phase = Phase::placing;
    s.since = now;
    s.made = 0;
    s.moved = false;
    say(s, "placing the stage");
}
// The game makes a pad at scale 1 and the move that follows gives it its scale. A move made before the pad's physics body
// exists scales only its model, so it is moved again a moment later.
void placing_step(State& s, std::uint64_t now) {
    if (!stage_object(floor_id)) {
        if (now > s.since + floor_make_ms) finish(s, "the stage's floor was not made");
    } else if (!s.made) {
        s.made = now;
    } else if (!s.moved && now >= s.made + floor_move_ms) {
        s.moved = place(s);
    } else if (s.moved && now >= s.made + floor_ready_ms) {
        s.phase = Phase::arriving;
        s.tries = 0;
        s.since = 0;
    }
}
void arriving_step(State& s, const std::optional<Point>& at, std::uint64_t now) {
    if (at && on_floor(s, *at, 3)) {
        if (!s.steady_since) s.steady_since = now;
        if (now >= s.steady_since + arrive_stand_ms) {
            s.phase = Phase::open;
            if (!s.ui_was_hidden) console::request_debug(overlay::DebugAction::set_game_ui_hidden, true);
            s.hid_ui = !s.ui_was_hidden;
            say(s, "the stage is open");
        }
        return;
    }
    s.steady_since = 0;
    // A teleport that did not happen, or a fall through the floor, sends them again; a teleport takes a moment to show.
    const bool fell = now >= s.since + teleport_show_ms && at && on_floor_column(s, *at) && at->at(1) < spot[1] + floor_top - 2;
    if (!s.since || now >= s.since + teleport_retry_ms || fell) {
        if (++s.tries > max_arrive_tries) return finish(s, "the skater did not stay on the stage");
        (void)place(s);
        if (!teleport_local_skater({s.centre[0], floor_height() + 0.5f, s.centre[1]})) return finish(s, "the skater could not be teleported");
        s.since = now;
        say(s, "taking the skater to the stage");
    }
}
void open_floor_step(State& s, const std::optional<Point>& at) {
    if (!at) return;
    if (at->at(1) < spot[1] + floor_top - 5) {
        s.centre = {at->at(0), at->at(2)};
        (void)place(s);
        (void)teleport_local_skater({s.centre[0], floor_height() + 0.5f, s.centre[1]});
    } else if (ground_distance(*at, centre_point(s)) > follow) {
        s.centre = {at->at(0), at->at(2)};
        (void)place(s);
    }
}
// The ground at the skater's place can stream in after they arrive, so a skater who drops through it is sent again.
void returning_step(State& s, const std::optional<Point>& at, std::uint64_t now) {
    if (at && ground_distance(*at, s.back) < 3 && std::abs(at->at(1) - s.back[1]) < 1) {
        if (!s.steady_since) s.steady_since = now;
        if (now >= s.steady_since + return_stand_ms) s.phase = Phase::closed;
        return;
    }
    s.steady_since = 0;
    if (now >= s.since + return_retry_ms) {
        if (++s.tries > max_return_tries) {
            s.phase = Phase::closed;
            return say(s, "the stage is closed, but the skater could not be put back where they were", logging::Level::warning);
        }
        (void)teleport_local_skater(s.back);
        s.since = now;
    }
}
void step(State& s, std::uintptr_t base, std::uintptr_t client, bool ready) {
    std::lock_guard lock(s.mutex);
    if ((s.phase == Phase::closed && !s.want_open) || !ready) return;
    const auto at = skater_position(base, client);
    const auto now = GetTickCount64();
    const auto map = stage_map();
    if (s.want_close) {
        s.want_close = false;
        finish(s, "the stage is closed");
        return;
    }
    if (s.want_open) return open_step(s, at, map, now);
    // The stage is solo play only: other players' objects replace it.
    if (s.phase != Phase::returning && multiplayer_session_active()) return finish(s, "the stage closed: you are on a multiplayer server");
    // Objects cannot be placed for a moment (a load, or a save that failed): the stage waits. The way back needs no objects.
    if (!map) {
        if (s.phase == Phase::returning) returning_step(s, at, now);
        return;
    }
    // A new map, or the same map loaded again, takes the stage's objects with it.
    if (*map != s.place) return finish(s, "the stage closed: the map changed", false);
    switch (s.phase) {
    case Phase::placing: return placing_step(s, now);
    case Phase::arriving: return arriving_step(s, at, now);
    case Phase::open: return open_floor_step(s, at);
    case Phase::returning: return returning_step(s, at, now);
    case Phase::closed: return;
    }
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto& s = state();
    try {
        step(s, base, client, ready);
    } catch (...) {
        // A failure that repeats would leave the skater far from the map: the stage closes and takes them back.
        std::lock_guard lock(s.mutex);
        s.want_open = s.want_close = false;
        if (s.phase == Phase::returning) s.phase = Phase::closed;
        else if (s.phase != Phase::closed) {
            try {
                finish(s, "the stage closed after an error");
            } catch (...) {
                s.phase = Phase::closed;
            }
        }
    }
}
} // namespace dingosdk::anim_studio::stage
