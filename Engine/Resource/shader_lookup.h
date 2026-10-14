#pragma once

// The two shader lookup tables a level superbundle carries. A map mod adds a
// row per material it introduces, aliasing a shader program and texture set the
// base already ships, so two mods' tables differ from the base only by the rows
// each added and can be combined exactly.

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

// `resourceMeta` is 16 bytes: program size, relocation size, lookup size. The
// program blob and its relocations are identical across copies because a mod
// only ever points new keys at programs that are already there.
[[nodiscard]] MergedTable merge_program_lookup(const Table& base, std::span<const Table> edits);

[[nodiscard]] MergedTable merge_texture_lookup(const Table& base, std::span<const Table> edits);

} // namespace dingosdk::frostbite::shader
