#pragma once
// Reading individual assets out of the installed game's bundles, without
// loading the whole asset database.
#include "game_archives.h"
#include "Engine/Resource/toc.h"
#include <filesystem>
#include <optional>
#include <string_view>

namespace dingosdk::vfs {
struct GameBundle {
    frostbite::BinaryBundle manifest;
    // Where each asset's payload lives: the manifest's ebx, then resources,
    // then chunks, in order.
    std::vector<frostbite::BundleFileInfo> payloads;

    [[nodiscard]] const frostbite::BundleFileInfo* payload(frostbite::AssetKind kind, std::size_t index) const noexcept;
    [[nodiscard]] const frostbite::BundleAsset* find(frostbite::AssetKind kind, std::string_view name,
                                                     std::size_t* index = nullptr) const noexcept;
    [[nodiscard]] const frostbite::BundleAsset* find_chunk(const frostbite::Guid& id, std::size_t* index = nullptr) const noexcept;
};

class GameData {
public:
    // `gameRoot` is the folder with Skate.exe; its Data folder is read.
    explicit GameData(std::filesystem::path gameRoot);
    // The same with a patch layer over it: `patchRoot` is a folder laid out as Data is (its
    // layout.toc and Win32), as the merged mods' is (Mods/.reskate). A superbundle TOC the layer
    // has is read from it, and what a TOC marks as the patch's is read from the layer's archives:
    // so a level a mod adds, and what a mod changed of the game's, are read as the game reads them.
    // A layer that is not there (no mods) leaves the game's own data.
    GameData(std::filesystem::path gameRoot, const std::filesystem::path& patchRoot);

    [[nodiscard]] frostbite::TocDocument read_toc(std::string_view relative) const;
    // The bundle named `name` (lower case) in a superbundle TOC, or nothing.
    [[nodiscard]] std::optional<GameBundle> read_bundle(const frostbite::TocDocument& toc, std::string_view name) const;
    // A payload, decoded.
    [[nodiscard]] std::vector<std::byte> read(const frostbite::BundleFileInfo& payload) const;

private:
    // The archives a payload is in, and their root: the patch layer's for what is marked as its.
    [[nodiscard]] const GameArchives& archives_of(const frostbite::CasIdentifier& location) const noexcept;

    std::filesystem::path gameRoot_;
    GameArchives archives_;
    std::filesystem::path patchRoot_; // empty: none
    std::optional<GameArchives> patch_;
};
}
