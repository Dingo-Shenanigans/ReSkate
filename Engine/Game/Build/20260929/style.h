#pragma once
#include "skater_body.h"
#include "skater_state.h"
#include "ui_textures.h"
#include <cstdint>
#include <string_view>

// Style layer: joint rotations added to the local skater's evaluated pose.
namespace dingosdk::game::build::v20260929::style {
// Animation/Dingo/AnimBase_Default_Skeleton, the rig every body type animates.
inline constexpr std::uint32_t skeleton_joints = 395;
// That skeleton's asset: in the game configuration bundle of globals.toc, the bundle of the scoring icons.
inline constexpr std::string_view skeleton_toc = ui_textures::scoring_toc, skeleton_bundle = ui_textures::scoring_bundle;
inline constexpr std::string_view skeleton_asset = "animation/dingo/animbase_default_skeleton";
// Skater component -> physics core (no_bail::bail_core_vtable) -> context.
inline constexpr std::uintptr_t component_core = 0x70;
inline constexpr std::uintptr_t core_context = 0x3c0;
// The physics state id in the context. Read only. Trusted only while No Bail's contracts match, because No Bail hooks its selector.
inline constexpr std::uintptr_t context_physics_state = skater_state::physics_state_offset;
// State id families: ground and air riding, and grinds. On foot is no_bail::offboard_physics_state.
inline constexpr std::uint32_t riding_states_begin = 100, riding_states_end = 300;
inline constexpr std::uint32_t grind_states_begin = 400, grind_states_end = 500;
// Physics core -> step input, the trick selection's object. No code site is fingerprinted, so readers range-check the value.
inline constexpr std::uintptr_t core_trick_selection = skater_body::core_step_input_offset;
// int32 FlipTrickType in progress, -1 for none. Measured 2026-10-04: ollie 1, kickflip 2, heelflip 3, pop shuvit 4,
// FS pop shuvit 7, nollie 16.
inline constexpr std::uintptr_t trick_selection_flip_trick = 0x28;
// Riding states with the board on the ground: plain riding, and a state seen after some landings. Measured 2026-10-04.
inline constexpr std::uint32_t riding_ground_state = 100, riding_landed_state = 101;
// Skater entity -> skater component -> pose holder.
inline constexpr std::uintptr_t entity_component = 0x628, component_pose_holder = 0xa0;
// The pose holder's animation interface: the pointer that the render-pose listener receives.
inline constexpr std::uintptr_t holder_animation_interface = 0xc0;
// One bone of a pose buffer: scale, then rotation, then position.
inline constexpr std::uintptr_t bone_size = 0x30, bone_rotation = 0x10, bone_position = 0x20;
}
