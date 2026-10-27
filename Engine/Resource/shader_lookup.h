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
// Native ShaderProgram bytecode resource type, also used by Studio's native
// shader resource writer; confirmed on the pinned build's shaders/bytecode/*.
inline constexpr std::uint32_t bytecodeType = 0x0E2B02F7;

struct Table {
    std::span<const std::byte> resource;
    std::span<const std::byte> resourceMeta;
};

struct MergedTable {
    std::vector<std::byte> resource;
    std::vector<std::byte> resourceMeta;
    std::size_t added{};       // rows taken from the edits
    std::size_t conflicts{};   // disagreements: retained graph wins, then first edit
    std::size_t outside{};     // rows left out: they point past the base's programs
    std::size_t removed{};     // invalid rows removed from the retained table itself
};

// The selected bundle contributor owns the retained program graph. Merge only
// private aliases whose program heap and relocation bytes match it exactly. No program
// records or relocations are copied or rebased. An optional stock table allows
// stock aliases from other graphs that also preserve the stock graph. Retained
// graph rows win clashes, even against higher-priority alias contributors; other
// fresh keys use edit order. `outside` includes
// rows from incompatible graphs as well as out-of-range indices.
// Selection helpers for contributors to the SAME bundle. A caller selecting a
// different contributor must also retain that contributor's compiled resources.
[[nodiscard]] bool preserves_stock_programs(const Table& candidate, const Table& stock);
[[nodiscard]] bool same_program_graph(const Table& candidate, const Table& other);
[[nodiscard]] bool only_stock_aliases(const Table& candidate, const Table& stock);
[[nodiscard]] MergedTable merge_program_lookup(const Table& base, std::span<const Table> edits, const Table& stock = {});

[[nodiscard]] MergedTable merge_texture_lookup(const Table& base, std::span<const Table> edits);

} // namespace dingosdk::frostbite::shader
