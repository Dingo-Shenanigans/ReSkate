#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
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
//
// The "default for the id" (0x3e89740) is the entry itself, so resolve then answers the entry's
// own texture at + 0, which for a dead entry is null too: with an empty holder from redirect,
// resolve gives null instead of crashing. Its caller for a material's texture parameter (0x3f86270,
// slot 17 of the table at 0x64ff6e8) passes that through 0x3e61260 to the texture view cache
// (0x3e6dbb0, keyed by the texture at DAT 0x76e9548 + 0xb10). On a miss the cache builds the view
// state without the texture (0x469a000 checks it for null and makes the same empty view the game
// makes for an empty slot), then, under its write lock from 0x3e6dcf1, asks the texture for its key
// (texture vtbl + 0xf8) to keep the state: through null (Skate.exe+0x3e6ddc1, every report with the
// guard's line just before it and TheVlixOne-Nike_Pack installed; 2026-10-24). For a null texture
// the cache insert is skipped (`view_cache_insert`): no lock is held there, rsi is saved on every
// path to it, and no branch lands inside its two instructions.
namespace dingosdk::game::build::v20260929::texture_handles {
inline constexpr std::uintptr_t textures = 0x76e0010;
// redirect(pool, id) -> the texture holder of the entry `id` points at, null when it points at none.
inline constexpr Fingerprint redirect{0x3e9b770, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x44,
    0x0f,0xb7,0xc2,0x4c,0x8d,0x89,0xb8,0x00,0x00,0x00,0x41,0x83,0xe8,0x02,0x48,0x8b}};
// In the view cache (0x3e6dbb0), the texture in r15, the built state in r14:
//   mov rbx, [rip + 0x0387b850]   ; DAT 0x76e9548, the cache
//   add rbx, 0xae8                ; its write lock
// the two complete instructions moved into the stub, which goes on at `view_cache_resume`, or for a
// null texture to `view_cache_return` (mov rsi, [rsp + 0xe8]; mov rax, r14; the cookie check; ret).
inline constexpr std::uintptr_t view_cache_insert_rva = 0x3e6dcf1;
inline constexpr std::array<unsigned char, 14> view_cache_insert{
    0x48,0x8b,0x1d,0x50,0xb8,0x87,0x03,0x48,0x81,0xc3,0xe8,0x0a,0x00,0x00};
inline constexpr std::uintptr_t view_cache_resume_rva = 0x3e6dcff;
inline constexpr std::uintptr_t view_cache_return_rva = 0x3e6df03;
inline constexpr std::uintptr_t view_cache = 0x76e9548;
inline constexpr std::uint32_t view_cache_lock_offset = 0xae8;
}
