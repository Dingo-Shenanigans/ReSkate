#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// The game's mesh variation databases (Extension/World/mesh_unload_guard.cpp).
namespace dingosdk::game::build::v20260929::mesh_variations {
// remove_databases(registry, ?, list): run on a job worker as a level's bundles unload. For
// each MeshVariationDatabase in the list it takes every entry out of the registry
// (0x3f5a200), keyed by the entry's mesh (its name hash, mesh + 0xc8) and variation hashes
// (entry + 0x20, + 0x24). It holds the registry's lock (0x158f2c0 / 0x158f430) throughout.
inline constexpr Fingerprint remove_databases{0x3f5fbc0, {
    0x48,0x89,0x54,0x24,0x10,0x53,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,
    0x83,0xec,0x28,0x48,0x8b,0x79,0x10,0x4c,0x8b,0xe2,0x4c,0x8b,0xe9,0x49,0x8b,0xd8}};
// The read of the mesh's name hash (mov edx, [rax + 0xc8]), with no check that the entry still
// has its mesh: an access violation reading 0xc8 when the mesh was unloaded before its
// database (crash reports of 2026-10-08 to 2026-10-11, always on leaving one custom map).
inline constexpr std::uintptr_t mesh_hash_read = 0x3f5fc52;
inline constexpr std::array<unsigned char, 6> mesh_hash_read_bytes{0x8b, 0x90, 0xc8, 0x00, 0x00, 0x00};
// The step to the next entry (add rbx, 0x30), after the call that takes one out: where an entry
// without a mesh goes on from.
inline constexpr std::uintptr_t next_entry = 0x3f5fc67;
inline constexpr std::array<unsigned char, 4> next_entry_bytes{0x48, 0x83, 0xc3, 0x30};
}
