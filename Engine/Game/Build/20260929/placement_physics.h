#pragma once
#include <array>
#include <cstdint>

// Physics on a plugin's own local placements (Extension/Objects/placement_physics.cpp,
// reskate.objects.physics) and the marker save/return events (reskate.marker.events). Bodies are
// FBPhysics 16-byte handles {FBPhysicsWorld*, u32 index, u32 generation} (remote_collision.h).
// Every function is checked by its first 16 bytes before it is called or hooked; the offsets were
// found by the PR #356 author against this build and are read with memory::peek, never trusted.
namespace dingosdk::game::build::v20260929::placement_physics {
// A body's motion type, as set_motion_type (remote_collision.h) takes it and motion_type returns it.
inline constexpr int motion_fixed = 1, motion_keyframed = 2, motion_dynamic = 3;

// (const Handle* body, float out[8]) -> out: the body's rigid pose, rotation quaternion (x, y, z, w)
// then position (x, y, z, pad).
inline constexpr std::uintptr_t body_pose = 0x25d6720;
// (const Handle* body, bool asleep): puts the body to sleep or wakes it from sleep.
inline constexpr std::uintptr_t sleep = 0x25dc600;
// (const Handle* body) -> int: the body's motion type. Hooked (placement_adapters.cpp motion_hook) so
// the callers in placement_adapters.h moving_calls and material_calls see a claimed dynamic surface
// as keyframed or fixed.
inline constexpr std::uintptr_t motion_type = 0x25d5bf0;
inline constexpr std::array<unsigned char, 16> motion_type_prefix{0x48,0x8b,0x01,0x44,0x8b,0x41,0x08,0x41,0x0f,0xb6,0xc8,0x49,0xc1,0xe8,0x03,0x80};
inline constexpr std::array<unsigned char, 16> body_pose_prefix{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0x01,0x48,0x8b,0xfa};
inline constexpr std::array<unsigned char, 16> sleep_prefix{0x48,0x89,0x5c,0x24,0x10,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x8b,0x49,0x08};

// The skater's marker controller: (controller, const float* transform, bool, bool, bool) saves a
// marker; (controller, const float* transform, label, const void* options) returns to one. Both are
// hooked to tell plugins that the local skater saved or returned.
inline constexpr std::uintptr_t marker_save = 0x544640, marker_return = 0x53e7a0;
inline constexpr std::array<unsigned char, 16> marker_save_prefix{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x80,0xb9,0x90,0x00,0x00,0x00};
inline constexpr std::array<unsigned char, 16> marker_return_prefix{0x40,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x83,0xec,0x58,0x4c,0x8b,0xe2,0x48};
// The controller's owner handle (whose first u64 is the skater's entity), its transition state (0
// before a return, 1 once one starts) and its own saved transform, which a real return passes.
inline constexpr std::uintptr_t marker_owner = 0x18, marker_transition = 0x40, marker_transform = 0x50;

// A physics world's state (world + world_state) holds the per-body buffers: the velocities, paged
// (velocity_page is the page in use, each page velocity_page_stride(velocity_count) bytes of 16-byte
// linear velocities) and the inverse masses (mass_buffer, one float a body; set_body_980 in
// remote_collision.h writes the same array).
inline constexpr std::uintptr_t world_state = 0x28, velocity_buffer = 0x930, velocity_count = 0x938, velocity_page = 0x93c, mass_buffer = 0x980;
constexpr std::uint64_t velocity_page_stride(std::uint32_t count) noexcept { return (std::uint64_t{count} * 16 + 60) & ~std::uint64_t{63}; }

// The client realm's placement store (realm context + client_ready_offset, local_placements.h): ready
// (u8 1) once filled, then an array of record_size records, each with its active flag (u8), drop ID
// (u32) and entity (u64). A claimed object's client replica is found here by its server drop ID.
inline constexpr std::uintptr_t store_ready = 0x100, records_begin = 0x48, records_end = 0x50, record_size = 0x50;
inline constexpr std::uintptr_t record_active = 0x3a, record_id = 0x28, record_entity = 0x10;
}
