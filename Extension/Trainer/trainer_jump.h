#pragma once
// No comply and boneless heights. The game's trick scripts launch these two through its
// jump-trajectory setter with a fixed upward speed, and no tuning value is read for it:
// the trainer multiplies that speed as the trajectory is set.
#include <cstdint>

namespace dingosdk::trainer {
enum class Trick : std::uint8_t { none, no_comply, boneless };
struct TrickLaunch {
    Trick trick{};
    float up_speed{}, factor{1}; // the game's own upward speed, and what it was multiplied by
};
// Installs the hook once the game's code is recognised. Safe to call every tick.
bool start_trick_heights(std::uintptr_t base) noexcept;
// Height multipliers (1: the game's own height). The launch speed is scaled by their roots.
void set_trick_heights(float no_comply, float boneless) noexcept;
// The pop out of a manual: a height multiplier, and Enum.Intent.Manual as read this tick (above 0: in a manual).
void set_manual_pop(float height, std::int32_t intent) noexcept;
// Gravity on foot as a multiple of the game's own (1 puts the game's back). False: this build's code is not the known
// one, or it could not be written. (In the air on the board: set_air_gravity, client_source_spawn.h.)
bool set_foot_gravity(float factor) noexcept;
float foot_gravity_in_force() noexcept;
// The fastest the game lets a skater on foot move along the ground (10 m/s its own). False while this game build's
// number is not where it is known to be.
bool set_foot_speed_cap(float metres_per_second) noexcept;

// The pop out of a grind or slide: the part of its launch that goes sideways off the edge is multiplied by `side`
// (1: the game's own). `travel_x/z` is the skater's velocity along the ground this tick and `grinding` whether it is on
// a grind now: the last direction of travel on the grind is what "sideways" is measured against.
// `push`: Skate 3's rule on top, a sideways speed (m/s) at full stick, added to the right of the travel for a stick to
// the right (0: none); `nudge` is the stick's sideways push on the grind this tick (-1..1).
void set_grind_side(float side, float travel_x, float travel_z, bool grinding, float push = 0, float nudge = 0) noexcept;
// The newest grind pop not yet taken: the game's own speed along the grind and sideways off it (m/s), and the stick's
// push it was popped with.
bool take_grind_pop(float &along, float &side, float &nudge) noexcept;
// EnumGS.Anim.CurrentFlipTrick as read this tick (0xff: unknown), for launches that depend on the trick.
void set_launch_trick(std::uint32_t trick, std::uintptr_t at = 0) noexcept;
// The least upward speed (m/s) a pop into that trick launches with (0: the game's own), for flip tricks set to turn
// slowly: the ollie, the pop out of a manual and the pop out of a grind or slide.
void set_flip_launch(std::uint32_t trick, float up) noexcept;
// A trick chosen after the pop (the launch could not know it): the flight just begun is put on the arc of that upward
// launch speed, from where it is. False when there is no pop within the last 0.4 s, or it already has that much.
bool raise_flight(float up, float &was, float &time) noexcept;
// What came of the last one: 1 launched again, -1 not, 0 nothing new.
int take_flight_raise() noexcept;
// The scripted launches since the last call, oldest first; false when there are none.
bool take_trick_launch(TrickLaunch &out) noexcept;
// Plugins' launches (Extension/Skater/launches.h): may change the launch velocity
// (x, y, z at parameters + 0x80) of any call of the game's jump-trajectory setter before the game
// uses it, given the caller's address relative to the game image (the return address minus 5) and
// the trajectory object it fills. Runs before the trainer's own trick multipliers. Physics thread;
// nullptr for none. Requires start_trick_heights.
using TrajectoryModifier = void (*)(std::uintptr_t caller_rva, std::uintptr_t trajectory, float (&velocity)[3]) noexcept;
void set_trajectory_modifier(TrajectoryModifier modifier) noexcept;
} // namespace dingosdk::trainer
