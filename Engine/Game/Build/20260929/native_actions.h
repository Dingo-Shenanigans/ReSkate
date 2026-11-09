#pragma once
#include "rigid_bodies.h"
#include "remote_collision.h"
#include "skater_state.h"

namespace dingosdk::game::build::v20260929::native_actions {
// Existing local-owner fields and the already pinned world getter. The action
// path introduces no new native function or inferred state layout.
inline constexpr auto physics_world = remote_collision::physics_world;
inline constexpr auto client_context = rigid_bodies::client_context;
inline constexpr auto entity_context = rigid_bodies::entity_context;
inline constexpr auto world_state = rigid_bodies::world_state;
inline constexpr auto physics_state = skater_state::physics_state_offset;
inline constexpr auto trick_state = skater_state::trick_state_offset;
inline constexpr auto offboard_state = skater_state::offboard_state_offset;
inline constexpr auto ragdoll = skater_state::ragdoll_offset;
inline constexpr auto in_air = skater_state::in_the_air_offset;
} // namespace dingosdk::game::build::v20260929::native_actions
