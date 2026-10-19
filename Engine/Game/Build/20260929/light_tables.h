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
}
