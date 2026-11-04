#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::game::build::v20260929::live_mods {
template<std::size_t N> consteval auto hex_bytes(const char (&hex)[N]) {
    static_assert(N % 2 == 1);
    std::array<std::uint8_t, (N - 1) / 2> result{};
    const auto nibble = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        throw "Invalid native contract hex";
    };
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((nibble(hex[index * 2]) << 4) | nibble(hex[index * 2 + 1]));
    return result;
}

// Superbundle registry (the layer set's +0x58). register(registry, name)
// creates and indexes a superbundle the way layout.toc's superBundles list
// does at launch; find(registry, name, prefixed) looks one up by name; attach
// (superbundle, 0, install chunk, 1) lists it under an install chunk, which is
// what lets its bundles be located. All run under the layout manager's lock.
inline constexpr std::uintptr_t register_superbundle = 0x41de0e0;
inline constexpr auto register_superbundle_prefix = hex_bytes("48895c241048897424185557415441564157488bec4883ec");
inline constexpr std::uintptr_t find_superbundle = 0x41e5cb0;
inline constexpr auto find_superbundle_prefix = hex_bytes("48895c24184889742420574881ecd0000000");
inline constexpr std::uintptr_t attach_superbundle = 0x41dfb70;
inline constexpr auto attach_superbundle_prefix = hex_bytes("48895c242056574155415641574883ec30488b4120488bf1");

// Object layout.
inline constexpr std::uintptr_t manager_lock = 0x130;     // CRITICAL_SECTION on the layout manager
inline constexpr std::uintptr_t layers_registry = 0x58;   // layer set -> superbundle registry
inline constexpr std::uintptr_t registry_manifest = 0x28; // registry -> install manifest
inline constexpr std::uintptr_t manifest_chunks = 0x28;   // install manifest -> chunk pointer vector (begin, end)
inline constexpr std::uintptr_t chunk_name = 0x100;       // install chunk -> name string

// A registered superbundle's mounts (vector of pointers at +0x30), each holding
// the TOC every data layer supplied for it (vector of 0x28-byte entries at
// +0x50, top layer first). An entry keeps the TOC file object at +0x10 and the
// start of its data (past the 0x22c-byte file header) at +0x18 and +0x20; the
// file object owns the whole file at +0x28, its size at +0x30 and +0x38.
// Bundles are looked up in that data when they load, so a TOC rewritten on
// disk can be handed to the game by swapping these pointers.
inline constexpr std::uintptr_t superbundle_mounts = 0x30;
inline constexpr std::uintptr_t mount_tocs = 0x50;
inline constexpr std::uintptr_t toc_entry_file = 0x10;
inline constexpr std::uintptr_t toc_entry_data = 0x18;
inline constexpr std::uintptr_t toc_file_bytes = 0x28;
inline constexpr std::uintptr_t toc_file_size = 0x30;
inline constexpr std::size_t toc_header = 0x22c;
// Converts a TOC's data from the file's big-endian words to native order, in
// place (data = file + toc_header); the loader runs it on every TOC it mounts.
inline constexpr std::uintptr_t prepare_toc = 0x41f8ed0;
inline constexpr auto prepare_toc_prefix = hex_bytes("4883ec28c5fa6f01c5fa6f15");

// The root level's LevelDescriptionAsset, loaded from globals at launch only. Its
// OnDemandBundles array ({char* SuperBundle; u32 BundleHash; pad} per entry,
// element count in the u32 before the first element) is how the root finds a
// map's bundle; a map added while the game runs is appended to it in memory.
inline constexpr char root_description[] = "levels/game/dingolevel_root/dingolevel_root/description";
inline constexpr std::uintptr_t description_on_demand = 0x40;
inline constexpr std::size_t on_demand_entry_size = 0x10;
}
