#pragma once
#include "skater_body.h"
#include "skater_skeleton.h"
#include "skater_state.h"
#include "ui_textures.h"
#include <array>
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

// The style editor's values.
// The flip trick gestures (Gameplay/Input/Gestures/FlipTricks): in the level root bundle, beside the render skeleton.
inline constexpr std::string_view gestures_toc = skater_skeleton::skeleton_toc, gestures_bundle = skater_skeleton::skeleton_bundle;
inline constexpr std::string_view gesture_prefix = "gameplay/input/gestures/fliptricks/gesture_fliptrick_";
// Board entity -> pose holder.
inline constexpr std::uintptr_t board_pose_holder = 0xf0;
// UI models by schema hash (TypeNameHash of the game's type) and field hash. Found in UI model dumps on 2026-10-04 and 2026-10-10.
// The menu's button bar: the buttons of the page that shows now. Each 16-byte element holds its button model's handle at +8, and
// the model's action has a navigation.
inline constexpr std::uint32_t button_bar = 0xe6ab1489, button_bar_buttons = 0x5fc6738b;
inline constexpr std::uintptr_t button_model_handle = 8;
inline constexpr std::uint32_t button_action = 0xa2b71c93, button_navigation = 0xb1e14cd6;
// UI/Legacy/Flow/State/StateNavigationModel: the menu's current and queued named navigations. The event is
// UI/Legacy/Flow/State/StateNavigationChanged.
inline constexpr std::uint32_t navigation_queue = 0xcc4776b5, navigation_current = 0xf3ba9cf8, navigation_queued = 0xf437b255;
inline constexpr std::uint32_t navigation_changed_event = 0xcb03d3ec;
// UI manager -> model manager.
inline constexpr std::uintptr_t ui_model_manager = 0x140;
// The menu's character room, below the origin of every map: the root of the menu's skater, and how far from it a rig counts as in
// the room. Its floor is at -69.99. Measured 2026-10-10 on Stadium 1 and San Vansterdam.
inline constexpr std::array<float, 3> menu_room{1.0f, -69.04f, -5.25f}, menu_room_reach{2.0f, 2.0f, 2.0f};
// The height of a board on that floor: a board at rest has its root 0.0868 m above the floor. Measured 2026-10-10 in 32 clips.
inline constexpr float menu_room_board_height = -69.903f;
// The editor camera: Skatepedia's camera at the start of its demonstrations, its fov, and its turn from the board's long axis
// (radians). Measured 2026-10-10 in four clips. A skater standing on the board faces along that axis (their root's x axis).
inline constexpr float editor_camera_fov = 46.8f, editor_camera_turn = -1.780f;
}
