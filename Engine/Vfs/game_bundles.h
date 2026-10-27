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
    // Offline inspection of an explicit merged patch over Data. CAS patch/base identities
    // remain separate; a patch's TOC can still name manifests in the original Data archives.
    GameData(std::filesystem::path gameRoot, std::filesystem::path patchRoot);

    [[nodiscard]] frostbite::TocDocument read_toc(std::string_view relative) const;
    // The bundle named `name` (lower case) in a superbundle TOC, or nothing.
    [[nodiscard]] std::optional<GameBundle> read_bundle(const frostbite::TocDocument& toc, std::string_view name) const;
    // A payload, decoded.
    [[nodiscard]] std::vector<std::byte> read(const frostbite::BundleFileInfo& payload) const;

private:
    std::filesystem::path gameRoot_;
    GameArchives archives_;
    std::optional<GameArchives> patchArchives_;
};
}
