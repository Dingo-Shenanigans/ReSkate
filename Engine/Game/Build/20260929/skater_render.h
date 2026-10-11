#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::game::build::v20260929::skater_render {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The local skater as the renderer draws it: the skinning matrices of its mesh and the camera of
// the same picture (Extension/HallOfMeat/hall_of_meat_render.h). Read 2026-10-06; the draw packet,
// the render view and the handle chain were first used by gBGYo's ReSkate fork
// (github.com/gBGYo/ReSkate).
//
// A skinned render object hands its draw packet out through 0x48db8e0(object, packet), packing
// the bones first when they changed (0x1448d2a30): each bone's skinning matrix (vertex in the
// mesh's model space to the world, the skeleton's inverse bind already in it) placed by the
// object's world matrix, three rows of four floats, transposed. The placement leaves the world
// position's whole metres out: the packet's root (float[4]) adds them back, and a relative packet
// is in its actor's frame (the packet's first matrix) besides. Bone 0 keeps its own position.
inline constexpr Fingerprint draw_packet_contract{0x48db8e0, {
    0x4c,0x8b,0xdc,0x55,0x56,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x48,0x8b,0x41,
    0x28,0x48,0x8b,0xf2,0x48,0x8b,0xf9,0x48,0x8b,0x68,0x40,0x48,0x85,0xed,0x0f,0x84}};
inline constexpr std::uintptr_t render_object_vtable = 0x65f2910;
inline constexpr std::uintptr_t render_index_offset = 0x108; // render object, uint32
inline constexpr std::uintptr_t render_object_lock_offset = 0x104; // render object, uint32: set while packing
inline constexpr std::uintptr_t packet_actor_offset = 0x00;     // float[16], copied from object+0x50
inline constexpr std::uintptr_t packet_bones_offset = 0x80;     // pointer to float[12] per bone (object+0xe0)
inline constexpr std::uintptr_t packet_bone_count_offset = 0x88; // uint32 (object+0xf8)
inline constexpr std::uintptr_t packet_root_offset = 0x90;      // pointer to float[4] (object+0xd8)
inline constexpr std::uintptr_t packet_relative_offset = 0x98;  // byte (object+0xfe)
inline constexpr std::uintptr_t packet_visible_offset = 0x99;   // byte (object+0xff)

// Which render object is the local skater's: the skater entity keeps a render handle for riding
// and one for walking (entity+0x4d0, 8 bytes apart; each mode its own render object). A handle
// names a record in a manager's paged pool (handle - 2 is its index; index + 16 has its highest
// bit at page + 4, the pages from pool+8 on), checked in each manager's own lookup: the first
// record keeps the second manager's handle at +0x40, the second the render index + 2 at +0x0c.
inline constexpr std::uintptr_t entity_render_handles_offset = 0x4d0; // skater entity, uint32 x 2
inline constexpr std::size_t render_modes = 2;
inline constexpr std::uintptr_t entity_render_handle_stride = 8;
inline constexpr std::uint32_t first_handle = 2;
inline constexpr std::uintptr_t manager_pool_offset = 0xc8;
struct RenderManager {
    std::uintptr_t global; // the manager's pointer
    std::uintptr_t vtable;
    std::size_t record_size;
    std::uintptr_t handle_offset; // in its record: the next handle
};
inline constexpr std::array<RenderManager, 2> handle_chain{{
    {0x71b78f8, 0x6671138, 0x98, 0x40},
    {0x77ef158, 0x666d4c0, 0x68, 0x0c},
}};
// The managers' own lookups: the first's (0x144f0dcf0, its vtable slot 65, passes this+0x10)
// finds its 0x98-byte records in the pool at +0x10+0xb8; the second's (0x144ee23c0, reached from a
// secondary vtable) its 0x68-byte ones. That the second's pool is at +0xc8 as well, gBGYo measured.
inline constexpr Fingerprint first_lookup_contract{0x4f0dd80, {
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x28,0x48,0x8d,0xb1,0xb8,0x00,0x00,0x00,
    0x48,0x8b,0xd9,0x83,0xc2,0xfe,0xb9,0x3f,0x00,0x00,0x00,0x48,0x83,0xc2,0x10,0x48}};
inline constexpr Fingerprint second_lookup_contract{0x4ee23c0, {
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x4c,0x8d,0x89,0xb8,0xfb,0xff,
    0xff,0x48,0xbb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,0x44,0x8d,0x42,0xfe,0xb9}};

// The camera of each rendered view: 0x3f8b2b0(blackboard, current view, previous view, jitter)
// copies the 320-byte view (0x143f7e700) for the shaders. In it the camera's world matrix at
// +0x40 (rows right, up, back, position; their fourth lanes not the matrix's) and the vertical
// field of view in radians at +0x104. The main picture's view has kind 0 (uint32), 0x38 bytes
// before it.
inline constexpr Fingerprint render_view_contract{0x3f8b2b0, {
    0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x88,
    0x0c,0x00,0x00,0xc5,0xf8,0x29,0x70,0xb8,0xc5,0xf8,0x29,0x78,0xa8,0xc5,0x78,0x29}};
inline constexpr std::size_t view_size = 0x140;
inline constexpr std::uintptr_t view_camera_offset = 0x40;  // float[16]
inline constexpr std::uintptr_t view_fov_offset = 0x104;    // float, radians
inline constexpr std::uintptr_t view_kind_back = 0x38;      // uint32, before the view
inline constexpr std::uint32_t main_view_kind = 0;

inline constexpr std::array<Fingerprint, 4> contracts{draw_packet_contract, first_lookup_contract,
    second_lookup_contract, render_view_contract};
}
