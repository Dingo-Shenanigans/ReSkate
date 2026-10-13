#pragma once

// Shader lookup tables carry material rows and compiled program records.
// Mods can alias stock programs or add private programs with their own
// permutations and backing arrays; both must survive a merge.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dingosdk::frostbite::shader {

inline constexpr std::uint32_t programLookupType = 0xE2E6955B;
inline constexpr std::uint32_t textureLookupType = 0x2D254A89;

struct Table {
    std::span<const std::byte> resource;
    std::span<const std::byte> resourceMeta;
};

struct MergedTable {
    std::vector<std::byte> resource;
    std::vector<std::byte> resourceMeta;
    std::size_t added{};       // rows taken from the edits
    std::size_t conflicts{};   // rows two edits disagreed on; the first won
};

// `resourceMeta` is 16 bytes: program size, relocation size, lookup size.
// Private program records and their relocated backing data are copied from
// each edit, and material indices are remapped into the combined table.
[[nodiscard]] MergedTable merge_program_lookup(const Table& base, std::span<const Table> edits);

[[nodiscard]] MergedTable merge_texture_lookup(const Table& base, std::span<const Table> edits);

} // namespace dingosdk::frostbite::shader
