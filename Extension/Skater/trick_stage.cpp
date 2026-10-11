#include "trick_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Console/commands.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include <array>
#include <cmath>
#include <mutex>
#include <optional>

namespace dingosdk::trick_stage {
namespace {
// 20 km from the map's origin and 300 m up: no map draws anything there.
constexpr std::array<float, 3> spot{20000, 300, 20000};
constexpr const char *floor_item = "own_bkpads_generic_padlow_00001";
constexpr std::uint64_t floor_id = 1;
// A padlow is 3 x 0.5 x 3 m; scaled by 20 it is a 60 m square with its top 10 m above its origin.
constexpr float floor_scale = 20;
constexpr float floor_top = 0.5f * floor_scale;
// The skater's position is this far above the floor when they stand on it.
constexpr float standing = 0.95f;
// The floor moves under the skater when they are this far from its middle (its edge is 30 m away).
constexpr float follow = 12;

enum class Phase { closed, placing, arriving, open, returning };
struct State {
    std::mutex mutex;
    Phase phase{};
    bool want_open{}, want_close{}, ui_was_hidden{}, hid_ui{};
    std::string map, message{"The stage is closed."};
    std::array<float, 3> back{};
    std::array<float, 2> centre{spot[0], spot[2]};
    std::uint64_t since{}, steady_since{}, made{};
    std::uint64_t checked_at{}, waited_from{}; // an open's look at the skater's height, to see that they are not falling
    float checked_y{};
    bool moved{};
    int tries{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
void say(State &s, std::string message, logging::Level level = logging::Level::info) {
    logging::log(level, logging::Channel::objects, "Trick stage: {}", message);
    s.message = std::move(message);
}
float floor_height() { return spot[1] + floor_top + standing; }
bool place(State &s) {
    const profile::PlacedObject floor{floor_id, floor_item, {s.centre[0], spot[1], s.centre[1]}, {0, 0, 0, 1}, floor_scale};
    return set_stage_objects(s.map, std::span(&floor, 1));
}
// Removes the floor and, once the skater is on it, takes them back. `left` is false when the map went away with it.
void finish(State &s, std::string message, bool left = true) {
    if (left) (void)set_stage_objects(s.map, {});
    const bool sent = left && s.phase != Phase::placing && teleport_local_skater(s.back);
    if (s.hid_ui) console::request_debug(overlay::DebugAction::set_game_ui_hidden, false);
    s.hid_ui = false;
    s.phase = sent ? Phase::returning : Phase::closed;
    s.since = GetTickCount64();
    s.steady_since = 0;
    s.tries = 0;
    say(s, std::move(message));
}
// Above or below the floor's middle: the teleport has happened.
bool on_floor_column(const State &s, const std::array<float, 3> &at) { return std::hypot(at[0] - s.centre[0], at[2] - s.centre[1]) < 3; }
bool on_floor(const State &s, const std::array<float, 3> &at, float reach) {
    return std::hypot(at[0] - s.centre[0], at[2] - s.centre[1]) < reach && std::abs(at[1] - floor_height()) < 0.5f;
}
} // namespace

void open(bool ui_hidden) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.want_open = s.phase == Phase::closed || s.phase == Phase::returning;
    s.want_close = false;
    s.checked_at = s.waited_from = 0;
    s.ui_was_hidden = ui_hidden;
}
void close() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.want_open = false;
    s.want_close = s.phase != Phase::closed;
}
bool active() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.phase != Phase::closed || s.want_open;
}
bool ready() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.phase == Phase::open;
}
std::string status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.message;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if ((s.phase == Phase::closed && !s.want_open) || !ready) return;
        std::optional<std::array<float, 3>> at;
        try {
            overlay::DebugModel skater;
            (void)client_source::detail::debug_skater(base, client, skater);
            if (skater.skater_position_valid) at = skater.skater_position;
        } catch (...) {}
        const auto now = GetTickCount64();
        const auto map = stage_map();
        if (s.want_close) {
            s.want_close = false;
            finish(s, "The stage is closed.");
            return;
        }
        if (s.want_open) {
            s.want_open = false;
            if (multiplayer_session_active()) return say(s, "The stage is offline only.");
            if (!map) return say(s, "Objects cannot be placed on this map right now.");
            if (!at) return say(s, "There is no skater to put on the stage.");
            // A falling skater would come back to a place in the air: the open waits until their height holds for 300 ms.
            if (!s.waited_from) s.waited_from = now;
            if (!s.checked_at || at->at(1) < s.checked_y - 0.3f) {
                if (now > s.waited_from + 10000) return say(s, "The skater is falling, so the stage did not open.");
                s.checked_at = now;
                s.checked_y = at->at(1);
            }
            if (now < s.checked_at + 300) {
                s.want_open = true;
                return;
            }
            s.map = *map;
            s.back = *at;
            s.centre = {spot[0], spot[2]};
            if (!place(s)) return say(s, "The stage's floor could not be asked for.");
            s.phase = Phase::placing;
            s.since = now;
            s.made = 0;
            s.moved = false;
            return say(s, "Placing the stage.");
        }
        // A new map takes the stage's objects with it.
        if (!map || *map != s.map) return finish(s, "The stage closed: the map changed.", false);
        switch (s.phase) {
        case Phase::placing:
            // The game makes a pad at scale 1 and the move that follows gives it its scale. A move made before the
            // pad's physics body exists scales only its model, so it is moved again a moment later.
            if (!stage_object(floor_id)) {
                if (now > s.since + 15000) finish(s, "The stage's floor was not made.");
            } else if (!s.made) {
                s.made = now;
            } else if (!s.moved && now >= s.made + 1500) {
                s.moved = place(s);
            } else if (s.moved && now >= s.made + 2000) {
                s.phase = Phase::arriving;
                s.tries = 0;
                s.since = 0;
            }
            break;
        case Phase::arriving:
            if (at && on_floor(s, *at, 3)) {
                if (!s.steady_since) s.steady_since = now;
                if (now >= s.steady_since + 1000) {
                    s.phase = Phase::open;
                    if (!s.ui_was_hidden) console::request_debug(overlay::DebugAction::set_game_ui_hidden, true);
                    s.hid_ui = !s.ui_was_hidden;
                    say(s, "The stage is open.");
                }
                break;
            }
            s.steady_since = 0;
            // A teleport that did not happen, or a skater who fell through the floor: move the floor and send them again. A
            // teleport takes a moment to show in the skater's position.
            if (!s.since || now >= s.since + 4000 || (now >= s.since + 1000 && at && on_floor_column(s, *at) && at->at(1) < spot[1] + floor_top - 2)) {
                if (++s.tries > 3) return finish(s, "The skater did not stay on the stage.");
                (void)place(s);
                if (!teleport_local_skater({s.centre[0], floor_height() + 0.5f, s.centre[1]}))
                    return finish(s, "The skater could not be teleported.");
                s.since = now;
                say(s, "Taking the skater to the stage.");
            }
            break;
        case Phase::open:
            if (!at) break;
            if (at->at(1) < spot[1] + floor_top - 5) {
                // Off the floor: put the floor and the skater back together.
                s.centre = {at->at(0), at->at(2)};
                (void)place(s);
                (void)teleport_local_skater({s.centre[0], floor_height() + 0.5f, s.centre[1]});
            } else if (std::hypot(at->at(0) - s.centre[0], at->at(2) - s.centre[1]) > follow) {
                s.centre = {at->at(0), at->at(2)};
                (void)place(s);
            }
            break;
        case Phase::returning:
            // The ground at the skater's place can stream in after they arrive, so a skater who drops through it is sent again.
            if (at && std::hypot(at->at(0) - s.back[0], at->at(2) - s.back[2]) < 3 && std::abs(at->at(1) - s.back[1]) < 1) {
                if (!s.steady_since) s.steady_since = now;
                if (now >= s.steady_since + 500) s.phase = Phase::closed;
                break;
            }
            s.steady_since = 0;
            if (now >= s.since + 500) {
                if (++s.tries > 20) {
                    s.phase = Phase::closed;
                    return say(s, "The stage is closed, but the skater could not be put back where they were.", logging::Level::warning);
                }
                (void)teleport_local_skater(s.back);
                s.since = now;
            }
            break;
        case Phase::closed:
            break;
        }
    } catch (...) {}
}
} // namespace dingosdk::trick_stage
