#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// The physics entity system's wake pass (Extension/World/physics_wake_guard.cpp).
//
// wake(system), a per-frame callback of the Physics entity system (its vtable at 0x64c6ff8,
// called from the entity world's update at 0x3cd6055), collects the bodies changed this step
// (0x269c270 on the world at system + 0x20), finds each one's physics owner (0x25d5f90, by the
// state's body-to-owner list at + 0xb30) and calls each owner once (owner + 0x18 -> vtbl + 0xf0).
// It keeps "once" in a bitset on the heap with a bit for each of `owner_limit` owners
// (system + 0xf8, a Physics setting: 20000), and sets the bit of an owner's index (0x3d2f846)
// without checking it against that. An index is good while it is below the world's owner
// count (its generation list, world + 0x810 .. + 0x818, four bytes each; 0x25d7d90), and the
// owner pool hands indices out first in, first out, so after a long session in which many
// bodies come and go (multiplayer's remote skaters, made and removed as players cross the
// build radius) indices pass 20000. The bit then lands in the next heap block's header, and a
// later free crashes in the allocator (Skate.exe+0x3d99e2f, +0x3d9ee43, +0x3d9ee47; indices
// 20199, 20203 and 20215 in three dumps, the next block's size off by exactly that bit;
// 2026-10-21).
namespace dingosdk::game::build::v20260929::physics_wake {
inline constexpr Fingerprint wake{0x3d2f780, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,
    0x48,0x81,0xec,0xa0,0x00,0x00,0x00,0x48,0x8b,0xd9,0x48,0x8d,0x55,0x0f,0x48,0x8b}};
inline constexpr std::ptrdiff_t world_offset = 0x20;       // system + 0x20: the world
inline constexpr std::ptrdiff_t owner_limit_offset = 0xf8; // system + 0xf8: u32, the bitset's size in owners
inline constexpr std::ptrdiff_t owner_generations_begin = 0x810, owner_generations_end = 0x818; // world: u32 each
}
