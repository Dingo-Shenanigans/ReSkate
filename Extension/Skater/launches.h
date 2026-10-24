#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The local skater's launches into the air, for plugins: each launch as the game made it (the
// `launch` event), and a launch's speed across, along or up from the direction of travel set by a
// plugin (reskate.skater.shape_launch). What a plugin makes of them (the grind-exits mod picks the
// side of a pop out of a grind with LT / RT) is the plugin's; this is what has to happen in the
// game's own threads.
//
// Every launch calls the game's jump-trajectory setter with the launch velocity; the trainer hooks
// that call (Extension/Trainer/trainer_jump.cpp) and hands each one here first. Its call site and
// the physics state before it say what it launched out of (Engine/Game/Build/20260929/launches.h).
// The direction of travel is the board's horizontal velocity over the last few physics steps.
//
// The skater's facing (its root's right axis, sampled on the client tick) and whether it rides
// switch (the game's stance flag) are read here too, for reskate.skater.get.
namespace dingosdk::launches {
// Requires the validated build and No Bail started (its physics-step listeners).
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;

enum class From : std::uint8_t { grind, ollie, no_comply, boneless, other };
const char* name(From from) noexcept;
std::optional<From> from_name(std::string_view name) noexcept;

// m/s, each one set if given: across the direction of travel (+ its left), along it, and up.
struct Shape {
    std::optional<float> across, along, up;
};
// Client thread: the launches out of `from` from now on (empty: the game's own). One plugin shapes
// each kind at a time: empty when taken, else why not.
std::string shape(std::string_view owner, From from, std::optional<Shape> shape);

struct Launch {
    From from{From::other};
    std::array<float, 3> game{}, velocity{}; // as the game made it, as it left
    std::array<float, 2> travel{};           // the horizontal direction of travel (x, z), unit; 0 without one
    float travel_speed{};                    // m/s
    std::string reason; // "applied", "none" (no shape), "slow" (too slow to tell the travel), "not_ours"
    std::string by;     // the plugin whose shape applied
};
// Client thread: the launches since the last call. Launches are watched from the first call on.
std::vector<Launch> take();

struct Facing {
    std::array<float, 3> right{}; // the skater root's right axis
    bool switch_stance{};
};
// Client thread: the facing, when it was sampled in the last quarter of a second.
std::optional<Facing> facing() noexcept;
// The skater is grinding now (as the physics steps last said). Watching starts with the first call.
bool grinding() noexcept;
// Client thread, every tick, with the local client: the launch hook made ready while used, the
// skater's facing sampled.
void tick(std::uintptr_t client) noexcept;
// Whatever the plugin shaped.
void release(std::string_view owner);
}
