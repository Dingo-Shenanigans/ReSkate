#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// The renderer's texture handles (Extension/World/texture_handle_guard.cpp).
//
// A render texture handle is {u16 id, u8 weak}: an id into the renderer's pool named "Texture"
// (the object at `textures` + 0x10, 0x98 bytes for each id). An entry holds the texture at + 0x70
// while it lives; when its generation ends and a live texture has the same name, the sweep
// (0x3e9833a, through 0x3e90110) gives the new entry that texture and points the old one at it
// by its id at + 0x8c. resolve (0x3e897c0, and the same code inline at 0x3e8f262) reads + 0x70,
// then the entry + 0x8c names through redirect, then the pool's default for the id (0x3e89740):
// it never checks what redirect gives back. redirect answers null when + 0x8c is 0, and a
// material parameter that still holds the id of a texture whose last reference went (a
// ParamDbStateStream built for the render jobs keeps the bare id, 0x3ed5ac0) finds exactly
// that: an entry with no texture and no redirect. Both callers then read through null
// (Skate.exe+0x3e897fb, a third of 2.1.1's crash reports, every one with TheVlixOne-Nike_Pack
// 2.3.x installed; 2026-10-21).
namespace dingosdk::game::build::v20260929::texture_handles {
inline constexpr std::uintptr_t textures = 0x76e0010;
// redirect(pool, id) -> the texture holder of the entry `id` points at, null when it points at none.
inline constexpr Fingerprint redirect{0x3e9b770, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x44,
    0x0f,0xb7,0xc2,0x4c,0x8d,0x89,0xb8,0x00,0x00,0x00,0x41,0x83,0xe8,0x02,0x48,0x8b}};
}
