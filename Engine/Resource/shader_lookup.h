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
    std::size_t outside{};     // rows left out: they point past the base's programs
};

// `resourceMeta` is 16 bytes: program size, relocation size, lookup size. The
// program blob and its relocations are identical across copies because a mod
// only ever points new keys at programs that are already there. The blob starts
// with its record count (u32), and every value of a lookup row is a record's
// index: the game takes it without a bounds check (an index past the end puts
// a pointer outside the database into its shader registry, Skate.exe 0x3f72280),
// so an added row with an index the base's blob does not have is left out
// (`outside`): a mod that brought programs of its own cannot have them merged
// onto the base's.
[[nodiscard]] MergedTable merge_program_lookup(const Table& base, std::span<const Table> edits);

[[nodiscard]] MergedTable merge_texture_lookup(const Table& base, std::span<const Table> edits);

} // namespace dingosdk::frostbite::shader
