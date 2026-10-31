#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include "physics_bodies.h"
#include <array>
#include <cstdint>

// Native rigid-body operations and the body/shape layout of the supported game build, for
// Extension/World/owned_bodies.cpp. Callers still qualify their own world and body and compare
// these bytes before any call; a pin here does not qualify an owner, API or command by itself.
namespace dingosdk::game::build::v20260929::rigid_bodies {
inline constexpr Fingerprint linear_force{0x25d14b0, {
    0x44,0x8b,0x41,0x08,0x4c,0x8b,0xc9,0x48,0x8b,0x01,0x41,0x0f,0xb6,0xc8,0x80,0xe1,
    0x07,0x4c,0x8b,0x50,0x28,0x49,0x8b,0x82,0x80,0x09,0x00,0x00,0xc4,0xa2,0x79,0x18}};
inline constexpr Fingerprint angular_torque{0x25d19a0, {
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0xda,0x48,0x8b,0xf9,
    0x48,0x8d,0x54,0x24,0x20,0xe8,0x06,0x59,0x00,0x00,0xc5,0xf8,0x10,0x2b,0x44,0x8b}};
// (body, const vec4*) stores the authored m/I principal-axis ratio in state+0x988.
inline constexpr Fingerprint set_inertia_ratio{0x25dce90, {
    0x48,0x8b,0x01,0x44,0x8b,0x41,0x08,0xc5,0xf8,0x10,0x02,0x4d,0x03,0xc0,0x48,0x8b,
    0x48,0x28,0x48,0x8b,0x81,0x88,0x09,0x00,0x00,0xc4,0xa1,0x78,0x11,0x04,0xc0,0xc3}};
// The game's body synchronizer, which itself produces force and torque inputs through
// 0x25d14b0/0x25d19a0. A matching local wrapper is evidence of that producer phase, not a
// world-wide barrier.
inline constexpr Fingerprint synchronizer{0xfc06b0, {
    0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,
    0xd0,0x00,0x00,0x00,0xf6,0x42,0x50,0x01,0x48,0x8b,0xf2,0xc5,0xf8,0x29,0x70,0xc8}};
// Overwrite a dynamic body's pending force or torque; parking a body writes zero to both.
inline constexpr Fingerprint overwrite_force{0x25da0b0, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x8b,0x01,0x4c,0x8b,0xd1,0x44,0x8b,0x49,0x08,0x4c,
    0x8b,0xda,0x45,0x8b,0xc1,0x41,0x0f,0xb6,0xc9,0x49,0xc1,0xe8,0x03,0x80,0xe1,0x07}};
inline constexpr Fingerprint overwrite_torque{0x25d7fe0, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x8b,0x01,0x4c,0x8b,0xd1,0x44,0x8b,0x49,0x08,0x4c,
    0x8b,0xda,0x45,0x8b,0xc1,0x41,0x0f,0xb6,0xc9,0x49,0xc1,0xe8,0x03,0x80,0xe1,0x07}};
// The physics state layout the owned-body service reads. The state is the world's store
// (physics_bodies::store_offset). Every read reroots the current world and repeats the
// allocation, generation and array checks.
inline constexpr std::uintptr_t position_ring = 0x900, body_capacity = 0x908;
inline constexpr std::uintptr_t local_com = 0x910, rotation_ring = 0x918, local_rotation = 0x928;
inline constexpr std::uintptr_t velocity_ring = 0x930, spin_ring = 0x940;
inline constexpr std::uintptr_t pending_force = 0x970, pending_torque = 0x978;
inline constexpr std::uintptr_t inverse_mass_array = 0x980, inertia_ratio_array = 0x988;
inline constexpr std::uintptr_t allocated_bits = 0x9c8, dynamic_bits = 0xa08;
// The native hit reader returns a shape (what physics_bodies.h calls a body). 0x25d3cb0 maps
// that shape through F98; 0x25d7e00 checks its allocation bit and byte generation in this state.
inline constexpr std::uintptr_t shape_allocated_bits = 0xf58, shape_body_indices = 0xf98, shape_generations = 0xff8;
// 0x25d7b10 compares a body's u32 generation in the world, not its state.
inline constexpr std::uintptr_t world_body_generations = 0x680;
inline constexpr std::uintptr_t shape_categories = 0xfe8, shape_collides = 0xff0;
// A shape's owner slot is physics_bodies::body_owner_offset, indexed by the shape. A slot of
// FFFFFFFF has no owner: set_shape_owner stores the owner's u32 index there, -1 for none, and
// the native ignore lists skip their test for it.
inline constexpr std::uint32_t unassigned_shape_owner = 0xffffffffU;
// 0x25d55d0 takes a shape handle and stores DWORD (World+28)+FE8[index]
// through its second argument, with no index, allocation or generation check.
// The SDK never calls it. Its bytes qualify the store_offset/shape_categories
// layout that guarded reads use for the optional in_categories hit field; a
// mismatch disables that field only.
inline constexpr std::uintptr_t shape_in_categories_rva = 0x25d55d0;
inline constexpr std::array<unsigned char, 32> shape_in_categories_prefix{
    0x48,0x8b,0x01,0x44,0x8b,0x41,0x08,0x48,0x8b,0x48,0x28,0x48,0x8b,0x81,0xe8,0x0f,
    0x00,0x00,0x42,0x8b,0x0c,0x80,0x48,0x8b,0xc2,0x89,0x0a,0xc3,0xcc,0xcc,0xcc,0xcc};
inline constexpr Fingerprint shape_in_categories{shape_in_categories_rva, shape_in_categories_prefix};
// The local client/physics participant qualifies the observed producer phase;
// it does not grant ownership of that participant or its bodies to a plugin.
inline constexpr std::uintptr_t client_context = 8, client_phase = 0xc4;
inline constexpr std::uintptr_t tls_active = 0xb19, tls_context = 0x550;
inline constexpr std::uintptr_t player_begin = 0x4c8, player_end = 0x4d0;
inline constexpr std::uintptr_t player_context = 0x78, player_local = 0x45, player_remote = 0x44;
inline constexpr std::uintptr_t player_entity = 0xb8, player_handle = 0xb0;
inline constexpr std::uintptr_t entity_context = 0x20, entity_player = 0xf8, entity_teleport = 0x7e0;
inline constexpr std::uintptr_t entity_collection = 0x70, entity_component = 0x628;
// The local player's handle points this far into its skater entity, at a secondary subobject.
inline constexpr std::uintptr_t entity_handle_target = 8;
inline constexpr std::uintptr_t component_collection = 0x18, component_core = 0x70;
inline constexpr std::uintptr_t core_context = 0x3c0, core_rig = 0x438, rig_core = 0x4630;
inline constexpr std::uintptr_t context_seconds = 0x17ec;
inline constexpr std::uint32_t safe_categories = 0, safe_world_collisions = 0x101;
} // namespace dingosdk::game::build::v20260929::rigid_bodies
