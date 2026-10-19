#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// A mesh's render settings (Extension/World/mesh_settings_guard.cpp).
namespace dingosdk::game::build::v20260929::mesh_settings {
// apply_settings(mesh, settings): as a level's meshes load (0x3f59610, for each mesh whose flag
// at + 0x3b0 is clear), it builds the settings object of the mesh's asset in place at mesh + 0x340
// (0x70 bytes, a reflected instance of 0x76f26f0) when the asset has one; settings is that
// object's source, null when it has none. Its caller sets the flag either way.
inline constexpr Fingerprint apply_settings{0x3f5beb0, {
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x60,0x80,0xb9,0xb0,0x03,0x00,0x00,
    0x00,0x48,0x8b,0xfa,0x48,0x8b,0xd9,0x74,0x20,0x48,0x81,0xc1,0x40,0x03,0x00,0x00}};
// The mesh (0x3c0 bytes, made at 0x3f33770) leaves + 0x340 to + 0x3af as the allocation had it.
inline constexpr std::ptrdiff_t settings_offset = 0x340;
// The settings object's string the mesh frees as it unloads (0x3f5fa20, through 0x1584c50,
// whenever the flag is set): with no settings object it is whatever the allocation left there.
inline constexpr std::ptrdiff_t unload_string_offset = 0x360;
inline constexpr std::ptrdiff_t applied_flag_offset = 0x3b0;
// The game's empty string: a string that holds it frees nothing (0x1584c50 compares with it).
inline constexpr std::uintptr_t empty_string = 0x72fe704;
}
