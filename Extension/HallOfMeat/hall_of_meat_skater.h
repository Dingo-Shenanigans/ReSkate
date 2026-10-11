#pragma once
#include "hall_of_meat_model.h"
#include "Extension/Skater/no_bail.h"
#include <cstdint>

// The local skater as Hall of Meat reads it, every physics step No Bail hands on (no_bail.h): what
// each body touched (Engine/Game/Build/20260929/skater_body.h) and how the skater moves, on the board
// or off it and whether in a ragdoll (Engine/Game/Build/20260929/skater_state.h). Read-only, but for
// the physics step length the slow motion gives the skater.
namespace dingosdk::hall_of_meat {
// Requires the validated build and No Bail started: verifies the layout once.
bool start_skater(std::uintptr_t image_base) noexcept;
bool skater_available() noexcept;
// Physics thread, in the step: the step as hall_of_meat_model.h takes it. Exact here; read outside
// the step, the contacts may be half written.
Step read_step(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
// The local skater's physics step length (seconds of game time): the game sets it only when it builds
// the skater's core (hall_of_meat_slow_motion.h). 0 while there is no local skater.
float step_length() noexcept;
// Game update thread: gives the local skater this physics step length. False while there is no local
// skater.
bool set_step_length(float seconds) noexcept;
}
