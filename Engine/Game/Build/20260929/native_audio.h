#pragma once
#include <array>
#include <cstdint>

// Remote skater sound.
namespace dingosdk::game::build::v20260929::native_audio {
// The physics material slots the root level's material grid has (MaterialIndexMap, slots 0 to 115: the
// same on every install; Engine/Resource/material_grid.h). A packed material word (bits 0-5 flags, 6-18 the
// material slot, 19-31 the property slot) with a slot past them is read from whatever memory follows them,
// and the game crashes on contact, so a plugin's surface sound (reskate.skater.surface_sound) may name only
// these. A Studio map's own surfaces are appended after them (slot 116 on) and are not offered.
inline constexpr std::uint32_t material_slots = 116;
// Skater sound component submit (handle, frame). Hooked to capture local sound.
inline constexpr std::uintptr_t submit = 0x44a370;
inline constexpr std::array<unsigned char, 17> submit_prefix{
    0x4c,0x8b,0xc9,0x4c,0x8b,0xc2,0x48,0x8b,0x0d,0xfb,0x95,0xd9,0x06,0x41,0x0f,0xb7,
    0x11};
// Merges a partial sound frame into owned storage (dst, src, masks). Primitive fields only.
inline constexpr std::uintptr_t frame_copy = 0x58d6540;
inline constexpr std::array<unsigned char, 15> frame_copy_prefix{
    0x40,0x53,0x56,0x57,0x41,0xf6,0x00,0x01,0x4c,0x8d,0x91,0x64,0x01,0x00,0x00};
// Native sound frame difference function (audited with frame_copy). Reference only.
inline constexpr std::uintptr_t frame_difference = 0x58d53a0;
// Resets a sound frame to its defaults.
inline constexpr std::uintptr_t frame_reset = 0x58ced60;
inline constexpr std::array<unsigned char, 14> frame_reset_prefix{
    0xc5,0xfc,0x10,0x05,0x68,0x49,0xb0,0x00,0x33,0xc0,0xc5,0xfc,0x11,0x01};
// Component stop operation; clears its handle (component + 0x74).
inline constexpr std::uintptr_t stop_sound = 0x43e540;
inline constexpr std::array<unsigned char, 15> stop_sound_prefix{
    0x40,0x53,0x48,0x83,0xec,0x20,0x0f,0xb7,0x11,0x48,0x8b,0xd9,0x66,0x85,0xd2};
}
