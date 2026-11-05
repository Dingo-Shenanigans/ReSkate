#pragma once
#include <array>
#include <cstdint>

// Remote multiplayer skater and skateboard actors.
namespace dingosdk::game::build::v20260929::native_skater {
// Skater animation component update (component, update). Hooked to apply remote poses.
inline constexpr std::uintptr_t animation_update = 0xf96b80;
inline constexpr std::array<unsigned char, 18> animation_update_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x81,0xec,0xc0,0x00,
    0x00,0x00};
// Expected code of engine::board_render_pose.
inline constexpr std::array<unsigned char, 15> board_render_pose_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x60};
// The evaluated render interface is embedded at this offset in its animation holder.
inline constexpr std::uintptr_t render_animation_interface_offset = 0xc0;
// Render pose interface vtable; slot +0x10 is engine::board_render_pose.
inline constexpr std::uintptr_t board_render_pose_interface_vtable = 0x63db790;
// Hands a skeleton's render data to the renderer (slot, render_data): its caller (0x2c20d99) calls it
// right after engine::board_render_pose, with the same render data, and the bones are copied for the
// render inside it (2026-10-25, a guard page on the local skater's bone buffer). A thunk into the
// renderer's interface (+0xd8).
inline constexpr std::uintptr_t submit_render_pose = 0x4c7e7a0;
inline constexpr std::array<unsigned char, 26> submit_render_pose_prefix{
    0x4c,0x8b,0xc9,0x4c,0x8b,0xc2,0x48,0x8b,0x0d,0x4b,0x91,0x53,0x02,0x41,0x8b,0x11,
    0x48,0x8b,0x01,0x48,0xff,0xa0,0xd8,0x00,0x00,0x00};
// Entity helper verified together with the creation functions; not called directly.
inline constexpr std::uintptr_t entity_helper = 0x2b761d0;
inline constexpr std::array<unsigned char, 17> entity_helper_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x28,0x48,0x8b,
    0xfa};

// Skateboard placement. set_blueprint_transform(transform, matrix) updates the board
// blueprint's world transform (blueprint_transform_vtable); place_board(entity, matrix) is the
// native source setter that places the entity and then updates the rig input (place_board_rig_call).
inline constexpr std::uintptr_t set_blueprint_transform = 0x2b69dd0;
inline constexpr std::array<unsigned char, 16> set_blueprint_transform_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x56,0x48,0x81,0xec,0xe0,0x00,0x00,0x00,0x80,0xb9,0xa5};
inline constexpr std::uintptr_t blueprint_transform_vtable = 0x6384e18;
inline constexpr std::uintptr_t place_board = 0xf94c50;
inline constexpr std::array<unsigned char, 25> place_board_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xfa,
    0x48,0x8b,0xd9,0xff,0x90,0x58,0x01,0x00,0x00};
inline constexpr std::uintptr_t place_board_rig_call = 0xf94c75;
inline constexpr std::array<unsigned char, 18> place_board_rig_call_prefix{
    0x48,0x8b,0x89,0xb8,0x00,0x00,0x00,0x45,0x33,0xc0,0x48,0x8b,0xd7,0xe8,0x99,0x30,
    0xeb,0x01};
// ClientSkateboardEntity's world-transform event: updates the blueprint transform before
// notifying entity placement. Reference only.
inline constexpr std::uintptr_t board_world_transform_event = 0xf97eb0;
// Placement callback that publishes inverse(parentWorld) * entityWorld to the renderer. Reference only.
inline constexpr std::uintptr_t board_placement_callback = 0xfa3140;

// Skateboard resources.
// Enables a board animation holder's render resources (holder, 1); the same call native board
// startup (board_startup) makes. Board holder vtable slot +0xf8.
inline constexpr std::uintptr_t enable_board_resources = 0x2e4c430;
inline constexpr std::array<unsigned char, 16> enable_board_resources_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x0f,0xb6,0xfa,0x48,0x8b,0xd9};
// Native board startup. Reference only.
inline constexpr std::uintptr_t board_startup = 0xf97880;
// engine::board_holder_vtable slots +0x88 and +0xc8 (resource lifecycle).
inline constexpr std::array<std::uintptr_t, 2> board_holder_resource_methods{0x2e4a410, 0x1f9630};
// Checks board component +0x7d before creating skateboard physics. Reference only.
inline constexpr std::uintptr_t board_physics_create = 0xf96e70;
// Initializes every entity of a blueprint creation list, as ClientSkateboardSource does.
inline constexpr std::uintptr_t initialize_creation_list = 0x2e93f50;
inline constexpr std::array<unsigned char, 13> initialize_creation_list_prefix{
    0x40,0x57,0x48,0x83,0xec,0x60,0x48,0x8b,0x79,0x08,0x48,0x85,0xff};
// Allocator dispatch that frees a creation list page (list, page, size).
inline constexpr std::uintptr_t free_creation_page = 0x2715360;
inline constexpr std::array<unsigned char, 7> free_creation_page_prefix{
    0x48,0xff,0x25,0x51,0x4c,0xa2,0x04};

// Pose layout (native_pose_layout.h). Reference only.
// Post-construction callback that caches the skater component at entity+0x628.
inline constexpr std::uintptr_t component_cache_callback = 0x53fe80;
// Follows the skater component's +0x60 backlink to its separate ClientSkateboardEntity.
inline constexpr std::uintptr_t board_from_component = 0x53f4e0;
// Returns the board entity's animation holder (+0xf0).
inline constexpr std::uintptr_t board_animation_holder = 0xf95310;
// Resolves the current output pose with a signed table index; the live 395-bone skater uses -1,
// selecting table word 18.
inline constexpr std::uintptr_t pose_output_resolve = 0x12e2e50;
}
