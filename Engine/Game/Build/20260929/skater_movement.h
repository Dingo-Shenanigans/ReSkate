#pragma once
#include <cstdint>

// The local skater's movement as reskate.skater.movement reads it (Extension/Skater/skater_movement.cpp).
// Offsets from PR #356's author, read with memory::peek and checked against the vtables in
// skater_state.h and offboard_flight.h before anything is trusted.
namespace dingosdk::game::build::v20260929::skater_movement {
// The skater's physics context velocity (context + context_velocity, vec3): its velocity in the world.
// A grind's friction step (placement_adapters.h friction) reads it too.
inline constexpr std::uintptr_t context_velocity = 0x3d0;
// The skater core's parent state machine and board (core + skater_parent, core + skater_board). On foot
// the parent is the off-board flight machine (offboard_flight.h vtable), holding the context, board and
// rig it runs for (state_context, state_board, state_rig) and its active state (active_state); while
// that is the walking state (offboard_flight_states[0]), its walking velocity (vec3) is at
// walking_velocity.
inline constexpr std::uintptr_t skater_parent = 0x3b0, skater_board = 0x430, state_context = 8, state_board = 0x10, state_rig = 0x18,
                                active_state = 0x48, walking_velocity = 0x10;
} // namespace dingosdk::game::build::v20260929::skater_movement
