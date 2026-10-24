#pragma once
#include "Engine/Game/Skater/skater_body.h"
#include "Extension/Skater/no_bail.h"
#include <cstdint>
#include <optional>

// The local skater as its physics simulates it, every physics step No Bail hands on (no_bail.h): what
// each body touched (Engine/Game/Build/20260929/skater_body.h) and how the skater moves, on the board
// or off it and whether in a ragdoll (Engine/Game/Build/20260929/skater_state.h). Plugins hear it as
// the physics_step event (skater_events.h). Read-only, but for the physics step length the game speed
// (game_speed.h) gives the skater.
namespace dingosdk::skater_physics {
// One physics step of the local skater.
struct Step {
    float seconds{}; // how long the step simulated, in the game's time
    bool wipeout{};  // the step asks for a wipeout
    bool airborne{}; // the skater is in the air, on the board or off it
    // The body is a ragdoll; empty when the skater state could not be read.
    std::optional<bool> ragdoll;
    // How the skater moves (with the board while on it, else as its pelvis), metres per second;
    // empty when unknown.
    std::optional<game::Vec3> velocity;
    // How fast the pelvis turns, any way round, radians per second; empty when unknown.
    std::optional<float> spin;
    skater_body::Contacts body; // what each body touched in the step
};

// Requires the validated build and No Bail started: verifies the layout once.
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;
// Physics thread, in the step. Exact here; read outside the step, the contacts may be half written.
Step read_step(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
// The local skater's physics step length (seconds of game time): the game sets it only when it builds
// the skater's core (game_speed.h). 0 while there is no local skater.
float step_length() noexcept;
// Game update thread: gives the local skater this physics step length. False while there is no local
// skater.
bool set_step_length(float seconds) noexcept;
}
