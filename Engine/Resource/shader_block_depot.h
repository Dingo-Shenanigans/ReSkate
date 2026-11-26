#pragma once

// A shader block depot (resource type 0x73312045, ShaderBlockDepotResource): what a material key
// means, as a block of parameters (its textures and numbers). A mesh section names a material by
// a 64-bit key, and the depots of its bundle give the key its block (ReSkate Studio,
// docs/shader-block-depot.md and docs/materials.md, held against all 2,475 depots of the game).
//
// The game walks a depot's rows and adds each key to one table for all depots; a key nothing
// has added gets the default block, and the material draws with no parameters of its own. So
// two mods that each add materials to one of the game's depots need both their rows in it: with
// one mod's copy alone, the other mod's keys stop resolving. Blocks are copied as they are and
// keys are looked up, never computed.
//
// The resource, every pointer an offset from its start:
//   u64 16 (where the rows are), u64 0
//   rows, 16 bytes each: u64 material key, u64 where the block's head is
//   the blocks back to back, in the order the rows first use them, then zeros to 16
//   heads, 24 bytes each, in the blocks' order: u64 where the block is, u64 its identity, u64 its size
//   zeros to 16
//   the pointer table: u32 for each pointer, saying where it is (for each row: its head's block
//   pointer the first time a row uses that head, then the row's own; and last a zero)
// and 16 bytes of metadata: u32 where the data ends, u32 the pointer table's size, u32 0x01190003,
// u32 the number of rows.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dingosdk::frostbite::shader_depot {

constexpr std::uint32_t resourceType = 0x73312045;

struct Block {
    std::uint64_t identity{};      // the game shares blocks across depots by this
    std::vector<std::byte> bytes;  // u32 parameter count, then the parameters
    bool operator==(const Block&) const = default;
};

struct Depot {
    struct Row {
        std::uint64_t key{};
        std::size_t block{};       // into `blocks`
    };
    std::vector<Row> rows;
    std::vector<Block> blocks;
    std::uint32_t version{0x01190003};
};

struct Resource {
    std::span<const std::byte> resource;
    std::span<const std::byte> resourceMeta;
};

// Throws when the bytes are not a depot.
[[nodiscard]] Depot read(const Resource& depot);

struct Written {
    std::vector<std::byte> resource;
    std::vector<std::byte> resourceMeta;
};
// The depot in the game's form: written from what `read` gives, every depot of the game is its
// own bytes again.
[[nodiscard]] Written write(const Depot& depot);

struct Merged {
    Written depot;
    std::size_t added{};     // rows of keys the base does not have, taken from the edits
    std::size_t changed{};   // rows of the base's keys an edit gave another block
    // Keys two edits gave different blocks (counted once per key and later edit): the later
    // edit's block is kept, and the earlier mod's material draws with it.
    struct Clash {
        std::uint64_t key{};
        std::size_t earlier{}, later{};   // indices into `edits`
    };
    std::vector<Clash> clashes;
    // Blocks of different content under one identity: the game keeps whichever it loaded first.
    std::size_t identityClashes{};
};

// Every row of the base and of each edit, by key: an edit's row of a key the base has replaces
// the base's when its block differs, and a key the base does not have is added. Edits are given
// lowest priority first, so where two give one key different blocks the later one is kept.
[[nodiscard]] Merged merge(const Resource& base, std::span<const Resource> edits);

} // namespace dingosdk::frostbite::shader_depot
