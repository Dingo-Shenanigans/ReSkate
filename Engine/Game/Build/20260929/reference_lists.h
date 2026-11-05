#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// A level load's asset reference lists (Extension/World/reference_list_guard.cpp).
//
// Two loaders, register(registry, assets, flag) at 0x557c40 (after 0x550500 resolves the list by name)
// and 0x891c80, take an array of assets and walk each asset's references in its array at + 0x28,
// reading each reference's + 0x48 (bit 2 of each pointer is a flag; an array's count is the dword
// before it, its bit 31 a flag). They skip an asset that is null, but not a reference: an empty one
// read 0x48 (Skate.exe+0x557dc3, 59 reports from 17 installs in 2.1.3, and +0x891e07, 170 from 54).
// The level exit walks the same arrays again (0x559190, Skate.exe+0x5591ed). Every report had
// NolanIsHigh-NolanIsHighs_Cosmetics_Pack_1 1.5.2, and switching it off ends the crash: a reference of
// one of its assets resolves to nothing once loaded (none of its arrays holds a null pointer as
// written; Sentry and a test install, 2026-11-02). So the guard takes the empty references out of
// each array (moving the rest down and lowering the count) before either loader walks it, and every
// later walk finds none.
namespace dingosdk::game::build::v20260929::reference_lists {
inline constexpr Fingerprint register_named{0x557c40, {
    0x44,0x88,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x53,0x55,0x56,0x57,0x41,0x54,
    0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x70,0x48,0x8b,0x02,0x48,0x8d,0x79,0x58,0x4c}};
inline constexpr Fingerprint register_assets{0x891c80, {
    0x48,0x8b,0xc4,0x44,0x88,0x40,0x18,0x48,0x89,0x48,0x08,0x53,0x57,0x41,0x55,0x48,
    0x81,0xec,0x80,0x00,0x00,0x00,0x48,0x89,0x68,0xe0,0x4c,0x8b,0xe9,0x48,0x89,0x70}};
inline constexpr std::uint32_t asset_references = 0x28;
inline constexpr std::uint32_t asset_name = 0x18; // Asset.Name, a char*
inline constexpr std::uintptr_t pointer_flag = 4;
inline constexpr std::uint32_t array_count_flag = 0x80000000;
}
