#pragma once
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Vfs/game_archives.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Skate Synchros' item cutter (contrib/outfit-sharing.md): the files one cosmetic item needs that
// the player's enabled mods provide, and nothing the game itself ships.
//
// An item (an EBX asset with a Key and ItemData) is followed to what it is made of:
//   - the EBX it imports, and the EBX it names by path in a field (its looks: "nabi_ap",
//     "<guid>_ap"; mods name looks by path, not by import);
//   - each EBX's resources (by reference, and the resource of the same name: a texture's data);
//   - the chunks resources name (texture and mesh data), found by their guids in the bytes.
// Across every enabled mod, not only the item's own: a deck whose shaders a shader library mod
// provides takes that mod's files too. Where two mods ship the same asset the higher one wins, as
// in the merge. Plugin code is never part of an item.
//
// The items of enabled mods have passed the merge's store-copy check already (a mod with a copy
// of a store item is not merged at all); the viewer checks again on what it receives.
namespace dingosdk::synchros {

struct Source {
    std::filesystem::path directory;  // the mod's folder
    std::string name;                 // its folder name, for messages
};

// One stored file an item needs.
struct Piece {
    enum Kind : std::uint8_t { ebx = 0, resource = 1, chunk = 2, toc_chunk = 3 } kind{};
    std::size_t source{};             // index into the sources
    frostbite::BundleFileInfo file{}; // where it is in that mod's archives (encoded size)
    std::string name;                 // ebx and resource
    std::string bundle;               // the bundle it is in; empty for a toc chunk
    std::string superbundle;          // "win32/items", "win32/globals"
    frostbite::BundleAsset asset;     // what the manifest says of it (resource type, meta, chunk range)
};

struct Cut {
    std::string item;                 // the item's asset name
    std::vector<Piece> pieces;
    std::uint64_t bytes{};            // encoded, every piece once
};

class Index {
public:
    // Reads the cosmetics superbundles (items, globals) of `sources`, highest priority first, and
    // every EBX in them (items are told by their contents). `game` is the folder with Skate.exe.
    // Resources are read only when a cut reaches them.
    Index(std::vector<Source> sources, std::filesystem::path game);
    ~Index();
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

    // The asset names of every item the mods provide.
    [[nodiscard]] std::vector<std::string> items() const;
    // An item's pieces, by its asset name ("items/board_bottomart/own_x") or its Key. Nothing
    // when no enabled mod provides that item (the game's own item, or one this PC does not have).
    [[nodiscard]] std::optional<Cut> cut(std::string_view item);
    // A piece's stored bytes, as they are sent.
    [[nodiscard]] std::vector<std::byte> read(const Piece& piece) const;

    // Files that could not be read while indexing, for the log.
    [[nodiscard]] std::size_t unreadable() const noexcept;
    [[nodiscard]] const std::vector<Source>& sources() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace dingosdk::synchros
