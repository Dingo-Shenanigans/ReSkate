#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstddef>
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
//
// What remained in 2.1.3 (Sentry, 2026-11-02: Skate.exe+0x3e897fb, +0x3e89803, +0x3e89809 and,
// through the view cache, +0x469a020 / +0x469a035; 1,900 reports from 1,650 installs, nearly all with
// TheVlixOne-Nike_Pack): resolve finds a holder at + 0x70, or through redirect, that is not one.
// The faults read 0x23c4, 0x203e, 0x3f800000 (1.0f), 0xbf5db3de and non-canonical addresses: an id
// whose entry is free (its memory reused by the pool) or never held a texture, as when a parameter
// that is not a texture is read as an id. Every texture's vtable lies in the image, and resolve calls
// its slots + 0x10 and + 0x18 before answering, so the guard checks the holder, the texture and
// those two slots before resolve reads through them, and answers no texture otherwise. Every caller
// of resolve takes null (0x3f86270 and 0x469a000 as above; the others store it or check it).
namespace dingosdk::game::build::v20260929::texture_handles {
inline constexpr std::uintptr_t textures = 0x76e0010;
// The pool within the object `textures` points at.
inline constexpr std::uint32_t pool_offset = 0x10;
// resolve(handle) -> the texture a {u16 id, u8 weak} handle names, retained; the entry's own
// texture (null for a dead entry) when its holder is empty.
inline constexpr Fingerprint resolve{0x3e897c0, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,
    0x8b,0x35,0x3a,0x68,0x85,0x03,0x48,0x8b,0xf9,0x0f,0xb7,0x11,0x48,0x83,0xc6,0x10}};
// lookup(pool, id) -> the entry's holder slot (entry + 0x70); makes the id's bucket if it has none.
inline constexpr Fingerprint lookup{0x351a080, {
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x44,0x0f,0xb7,0xc2,0x4c,0x8d,
    0x89,0xb8,0x00,0x00,0x00,0x41,0x83,0xe8,0x02,0xb9,0x3f,0x00,0x00,0x00,0x49,0x83}};
// The texture's vtable slots resolve calls on what it found, one after the other.
inline constexpr std::uint32_t resolve_calls_offset = 0x10;
inline constexpr std::size_t resolve_calls = 2;

// A material's texture parameter value in the render parameter database: a kind byte, a flags byte,
// then `count` 4-byte texture handles (its size, slot 3 of the value type's vtable 0x64ff638, is
// count * 4 + 2). The type (built by 0x3f877e0) has two handlers, one for each kind: 0 (the table at
// 0x64ff6e8, whose slot 17 is 0x3f86270 above) and 1 (0x64ff7a0); both read `count` 16-byte entries.
// copy(type, value, source, count) (slot 6) takes the kind from source + 9 and calls that kind's
// handler from the type's own short table, so a kind past 1 reads past it and calls through whatever
// lies there (Skate.exe+0x3f87028 and +0x3f87035, 590 reports from 500 installs in 2.1.3; Sentry,
// 2026-11-02). The kinds seen were 0x68, 0xa2, 0xb0 and 0xf0, bytes of a pointer: a value of another
// type read as a texture's. They came with ReSkate Studio's "textureedits/<GUID>_ap" presets, which the
// preset guard leaves out of the script call; the render database still copies them in through the
// reflection copy (0x3dfb090 -> 0x3df7d40 -> here). clear(type, value, count) (slot 5) is the type's own
// empty value: all zeroes, kind 0 and no handles, as for a parameter nothing set.
inline constexpr Fingerprint value_copy{0x3f86fe0, {
    0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x41,0x8b,
    0xe9,0x49,0x8b,0xc8,0x4d,0x8b,0xf0,0x48,0x8b,0xf2,0xe8,0x51,0x07,0xe7,0xff,0x49}};
inline constexpr Fingerprint value_clear{0x3f86860, {
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xda,0x41,0x8b,0xd0,0xff,
    0x50,0x18,0x44,0x8b,0xc0,0x33,0xd2,0x48,0x8b,0xcb,0x48,0x83,0xc4,0x20,0x5b,0xe9}};
inline constexpr std::uint32_t value_kind_offset = 9;
inline constexpr std::uint8_t value_kinds = 2;
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
