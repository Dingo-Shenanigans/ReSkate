#pragma once
#include "client_source_spawn.h"
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::skater_state {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The skater's live state, read-only (Extension/HallOfMeat/hall_of_meat_skater.h).

// The physics state the core's selector chose this step (no_bail.h: choose_physics_state; its
// offboard_physics_state is the one on foot and through a whole bail).
inline constexpr std::uintptr_t physics_state_offset = 0x1414; // uint32, context
inline constexpr std::array<std::uint32_t, 2> board_air_states{200, 201}; // PHYSICS_AIR, KNOWN_AIR

// The core's trick state (its vtable slot +0xb0 returns core+0x3b8) holds the offboard state
// at +0x78: the skater on foot and in a ragdoll (the script native GetOffboardScoring copies
// it out). Each offboard substate class raises exactly one substate flag of it from its vtable
// (0x1465e8c28 on, each beside its name and id); the three ragdoll ones, FollowRagdoll (3)
// 0x14482d970, FollowAnimatedRagdoll (4) 0x14482d3b0 and FollowSimulatedRagdoll (5) 0x14482db00,
// the same one. In the air is the substate's own +0x150, copied over. Flags are one byte, 0 or 1.
inline constexpr std::uintptr_t trick_state_offset = 0x3b8;   // core
inline constexpr std::uintptr_t offboard_state_offset = 0x78; // trick state
inline constexpr std::uintptr_t ragdoll_offset = 0x18c;       // offboard state
inline constexpr std::uintptr_t in_the_air_offset = 0x180;    // offboard state

// How fast the skater moves: the linear velocity of its physics bodies, read as the SDK's noclip
// reads and writes them (Extension/Skater/client_noclip.cpp). The board's physics: core+0x430 ->
// +0x18 (vtable board_physics_vtable); the skeleton's: the rig (core+0x438) -> +0x2f10 (vtable
// rig_physics_vtable). Each keeps its bodies at +0x20: their count (uint32) at the start, body n at
// n * 0x130, each pointing back to its owner at +0x10 with its velocity (float[3], m/s) at +0x70 and
// its spin (angular velocity, float[3], radians per second) at +0x90. 0x140fc06b0 syncs a body with
// the physics world: by its pending flags (+0x60) it sets the velocity (8, 0x1425da600) from +0x70
// and the spin (0x20, 0x1425d8530) from +0x90.
// The board has 9, body 0 its root; the skeleton 26, body n the ragdoll's body n (skater_body.h).
inline constexpr std::uintptr_t board_holder_offset = 0x430;  // core
inline constexpr std::uintptr_t board_physics_offset = 0x18;  // board holder
inline constexpr std::uintptr_t rig_physics_offset = 0x2f10;  // rig
inline constexpr std::uintptr_t physics_bodies_offset = 0x20; // board or rig physics
inline constexpr std::uintptr_t physics_body_size = 0x130;
inline constexpr std::uintptr_t body_owner_offset = 0x10;
inline constexpr std::uintptr_t body_velocity_offset = 0x70;
inline constexpr std::uintptr_t body_spin_offset = 0x90;
inline constexpr std::uint32_t board_body_count = 9, rig_body_count = 26;
inline constexpr std::size_t board_root_body = 0;
inline constexpr std::uintptr_t board_physics_vtable = client_source_spawn::board_physics_vtable;
inline constexpr std::uintptr_t rig_physics_vtable = client_source_spawn::rig_physics_vtable;
}
