#pragma once
#include <array>
#include <cstdint>

// Style layer: joint rotations added to the local skater's evaluated pose.
namespace dingosdk::game::build::v20260929::style {
// Animation/Dingo/AnimBase_Default_Skeleton, the rig every body type animates.
inline constexpr std::uint32_t skeleton_joints = 395;
// Skater component -> physics core (no_bail::bail_core_vtable) -> context.
inline constexpr std::uintptr_t component_core = 0x70;
inline constexpr std::uintptr_t core_context = 0x3c0;
// The physics state id in the context. Read only. Trusted only while No Bail's contracts match, because No Bail hooks its selector.
inline constexpr std::uintptr_t context_physics_state = 0x1414;
// State id families: ground and air riding, grinds, and on foot.
inline constexpr std::uint32_t riding_states_begin = 100, riding_states_end = 300;
inline constexpr std::uint32_t grind_states_begin = 400, grind_states_end = 500;
inline constexpr std::uint32_t offboard_state = 504;
// Physics core -> trick selection object. No code site is fingerprinted, so readers range-check the value.
inline constexpr std::uintptr_t core_trick_selection = 0x3c8;
// int32 FlipTrickType in progress, -1 for none. Measured 2026-10-04: ollie 1, kickflip 2, heelflip 3, pop shuvit 4, FS pop shuvit 7, nollie 16.
inline constexpr std::uintptr_t trick_selection_flip_trick = 0x28;
// Riding states with the board on the ground: plain riding, and a state seen after some landings. Measured 2026-10-04.
inline constexpr std::uint32_t riding_ground_state = 100, riding_landed_state = 101;
// Skatepedia's UI models by schema hash (TypeNameHash of the game's type) and field hash. Found in a UI model dump on 2026-10-04.
// UI/Features/Skatepedia/SkatepediaTrickEntryDataModel: one for each row, and one for the highlighted entry that its skater performs.
inline constexpr std::uint32_t skatepedia_entry = 0xbf835c8e, skatepedia_entry_name = 0x53bac52d;
// UI/Foundations/Components/InfoCard/InfoCardViewModel: its title label names the highlighted entry.
inline constexpr std::uint32_t info_card = 0xa8080640, info_card_title = 0xe08a935d, label_text = 0x4d8e01b9;
// UI/Legacy/Flow/State/StateNavigationModel: the menu's current and queued named navigations. The event is UI/Legacy/Flow/State/StateNavigationChanged.
inline constexpr std::uint32_t navigation_queue = 0xcc4776b5, navigation_current = 0xf3ba9cf8, navigation_queued = 0xf437b255;
inline constexpr std::uint32_t navigation_changed_event = 0xcb03d3ec;
// Skater entity -> skater component -> pose holder. The board entity has its pose holder at its own offset.
inline constexpr std::uintptr_t entity_component = 0x628, component_pose_holder = 0xa0, board_pose_holder = 0xf0;
// The pose holder's animation interface: the pointer that the render-pose listener receives.
inline constexpr std::uintptr_t holder_animation_interface = 0xc0;
// One bone of a pose buffer: scale, then rotation, then position.
inline constexpr std::uintptr_t bone_size = 0x30, bone_rotation = 0x10, bone_position = 0x20;
// UI manager -> model manager. The event dispatcher's byte that says it accepts events.
inline constexpr std::uintptr_t ui_model_manager = 0x140, dispatcher_live = 0x28;
// A camera's world transform: 4x4 floats, rows right, up, backward, position.
inline constexpr std::uintptr_t camera_transform = 0x50;
// Skatepedia's stage in the world, and how far its skater goes from that point. Measured 2026-10-04.
inline constexpr std::array<float, 3> skatepedia_stage{0.0f, -168.22f, -2.0f}, skatepedia_stage_reach{4.0f, 4.0f, 9.0f};
}
