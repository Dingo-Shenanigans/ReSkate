#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// The light renderer (Extension/World/light_table_guard.cpp). Each kind of light has a table of 64
// pages of 128 and an ID allocator at manager+0xb8; nothing checks the IDs against the table.
namespace dingosdk::game::build::v20260929::light_tables {
inline constexpr std::uintptr_t renderer = 0x77ef140; // the light renderer, null until made
// The allocator: the free list head at +0xe8 (low 32 bits 0xffffffff when empty), fresh IDs given at +0xf0.
inline constexpr std::uint32_t allocator = 0xb8, free_head = 0xe8, issued = 0xf0;
inline constexpr std::uint32_t capacity = 64 * 128;
inline constexpr Fingerprint take_id{0x3e844f0, {
    0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xb9,0xe8,0x00,
    0x00,0x00,0x48,0x8b,0xf1,0x90,0x8b,0xdf,0x83,0xff,0xff,0x74,0x45,0x48,0xbd,0x00}};

// factory(creator, creation info): the entity, or null. The six begin alike.
inline constexpr std::array<unsigned char, 32> factory_head{
    0x40,0x53,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x58,0x48,0x8b,0x9a,0x40,0x01,0x00,
    0x00,0x48,0x8b,0xfa,0x4c,0x8b,0xf1,0x48,0x8d,0x54,0x24,0x30,0x48,0x8b,0x4f,0x10};
struct Kind {
    const char* name;
    std::uint32_t manager; // in the renderer
    Fingerprint factory;
};
inline constexpr Kind kinds[]{
    {"OldPointLight", 0x60, {0x52d3f50, factory_head}},
    {"OldSpotLight", 0x570, {0x52d4480, factory_head}},
    {"PbrSphereLight", 0xa80, {0x52d0e00, factory_head}},
    {"PbrSpotLight", 0xf90, {0x52d1740, factory_head}},
    {"PbrTubeLight", 0x14a0, {0x52d1d40, factory_head}},
    {"PbrRectangularLight", 0x19b0, {0x52d2680, factory_head}},
};

// ---- A spot light made through the renderer, as its PbrSpotLight entity makes one (Extension/World/plugin_lights.h) ----
// Read 2026-11-19 (Skate.exe, read in place): the entity makes its light through four of the renderer's slots. The light's
// state (its "info", 0x120 bytes) starts from the renderer's own defaults (info_init, which only stores constants: the
// same as a PbrSpotLightEntityData's) with every field marked set, as the entity marks them when it makes one; a change
// later sends only what changed, marked so. The renderer copies what it is sent and keeps nothing of the caller's.
inline constexpr std::uintptr_t renderer_vtable = 0x666fc90;
// The renderer's slots: take a light away (renderer, id), make one (renderer, its description: the light's id back, its
// kind in the top three bits), move one (renderer, id, its transform: rows right, up, back and position, as a camera's;
// it shines along -back), change a PBR spot light (renderer, id, the info: only the fields marked). Each slot holds the
// function beside it (checked through the renderer's class before any is called).
inline constexpr std::uintptr_t slot_destroy = 0x08, slot_create = 0x10, slot_transform = 0x20, slot_update = 0x70;
inline constexpr std::uintptr_t fn_destroy = 0x4ed3280, fn_create = 0x4ec5ee0, fn_transform = 0x4ee95b0, fn_update = 0x4eebe40;
// The info's defaults (info_init); and what the entity lets go of in an info once it has been sent (two handles, at
// +0x10c and +0x104, and a reference at +0x30, none of which an info made here holds: let go all the same, as the game does).
inline constexpr std::uintptr_t fn_info_init = 0x5f21db0, fn_release_handle = 0x3df3bc0, fn_release_ref = 0x3dc5e90;
inline constexpr std::array<unsigned char, 19> info_init_prefix{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xc7,0x01,0x00,
                                                                0x00,0x80,0x3f};
inline constexpr std::array<unsigned char, 12> update_prefix{0x81,0xe2,0xff,0xff,0xff,0x1f,0x48,0x81,0xc1,0x90,0x0f,0x00};
inline constexpr std::uint8_t kind_pbr_spot = 3;
// The description's +0x51, which its spatial node is made with: 2 for a light placed in the world itself under no parent
// (as the game's own world lights are made: watched 2026-11-19), its transform the world's. 0 (an entity's light under its
// bus's node) is in no world without a parent: never drawn.
inline constexpr std::uint8_t world_light = 2;
// The info's fields (offsets, as the renderer reads them) and their marks: three words, A, B and C.
inline constexpr std::size_t info_size = 0x120;
inline constexpr std::size_t info_colour = 0x10, info_intensity = 0xe0, info_unit = 0x74, info_radius = 0xdc, info_inner = 0x80, info_outer = 0x5c,
                             info_cast_shadows = 0xa4, info_shadows_on = 0xf5, info_shadow_near = 0xd4, info_enabled = 0xf6, info_mobility = 0xec;
inline constexpr std::size_t info_marks_a = 0xa8, info_marks_b = 0xd0, info_marks_c = 0xf9;
inline constexpr std::size_t info_handle_a = 0x10c, info_handle_b = 0x104, info_reference = 0x30;
inline constexpr std::uint32_t mark_colour = 0x1000, mark_intensity = 0x2000, mark_radius = 0x4000, mark_inner = 0x2, mark_outer = 0x4,
                               mark_cast_shadows = 0x200000, mark_unit = 0x20000;
inline constexpr std::uint32_t mark_b_shadows_on = 0x40, mark_b_shadow_near = 0x8;
// Made: the fields the entity marks when it makes its light, exactly (its data's: all of A and C and most of B, with its
// enabled switch, shadow cache count and mobility); not the four it leaves to the renderer (two handles, a float at +0x8c
// and a byte at +0xfe).
inline constexpr std::uint32_t made_marks_a = 0xffffffffu, made_marks_b = 0xce5fffffu;
inline constexpr std::uint8_t made_marks_c = 0x1f;
// QualityScalableEnabled (info_cast_shadows): from which of the game's quality levels on it is on (Low 0 ... Ultra 3), or
// never (4).
inline constexpr std::uint32_t shadows_from_low = 0, shadows_never = 4;
}
