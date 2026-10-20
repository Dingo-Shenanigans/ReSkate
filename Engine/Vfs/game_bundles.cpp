#include "game_bundles.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Core/Platform/path_text.h"
#include <fstream>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fs = std::filesystem;
namespace fb = frostbite;
namespace {
std::size_t first_index(const fb::BinaryBundle& manifest, fb::AssetKind kind) {
    switch (kind) {
    case fb::AssetKind::ebx: return 0;
    case fb::AssetKind::resource: return manifest.ebx.size();
    default: return manifest.ebx.size() + manifest.resources.size();
    }
}
const std::vector<fb::BundleAsset>& list(const fb::BinaryBundle& manifest, fb::AssetKind kind) {
    return kind == fb::AssetKind::ebx ? manifest.ebx : kind == fb::AssetKind::resource ? manifest.resources : manifest.chunks;
}
}

const fb::BundleFileInfo* GameBundle::payload(fb::AssetKind kind, std::size_t index) const noexcept {
    if (index >= list(manifest, kind).size()) return nullptr;
    const auto at = first_index(manifest, kind) + index;
    return at < payloads.size() ? &payloads[at] : nullptr;
}

const fb::BundleAsset* GameBundle::find(fb::AssetKind kind, std::string_view name, std::size_t* index) const noexcept {
    const auto& assets = list(manifest, kind);
    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (assets[i].name != name) continue;
        if (index) *index = i;
        return &assets[i];
    }
    return nullptr;
}

const fb::BundleAsset* GameBundle::find_chunk(const fb::Guid& id, std::size_t* index) const noexcept {
    for (std::size_t i = 0; i < manifest.chunks.size(); ++i) {
        if (manifest.chunks[i].guid != id) continue;
        if (index) *index = i;
        return &manifest.chunks[i];
    }
    return nullptr;
}

GameData::GameData(fs::path gameRoot)
    : gameRoot_(std::move(gameRoot)), archives_(gameRoot_ / "Data", read_layout(gameRoot_ / "Data" / "layout.toc").root) {}
GameData::GameData(fs::path gameRoot, fs::path patchRoot) : GameData(std::move(gameRoot)) {
    patchArchives_.emplace(patchRoot, read_layout(patchRoot / "layout.toc").root);
}

fb::TocDocument GameData::read_toc(std::string_view relative) const {
    auto path = gameRoot_ / "Data" / fs::path(relative);
    if (patchArchives_) {
        const auto candidate = patchArchives_->root() / fs::path(relative);
        if (fs::is_regular_file(candidate)) path = candidate;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(fs::file_size(path)));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read " + path_utf8(path));
    return fb::read_toc(bytes);
}

std::optional<GameBundle> GameData::read_bundle(const fb::TocDocument& toc, std::string_view name) const {
    for (const auto& bundle : toc.bundles) {
        if (bundle.name != name) continue;
        auto region = fb::read_bundle_region(bundle.region);
        GameBundle result;
        if (!region.inlineManifest.empty()) {
            result.manifest = fb::read_binary_bundle(region.inlineManifest);
            result.payloads = std::move(region.files);
        } else {
            // The manifest is the region's first file, in cas like any payload.
            if (region.files.empty()) throw std::runtime_error("Bundle " + std::string(name) + " has no manifest");
            const auto& first = region.files.front();
            const auto& source = first.location.patch && patchArchives_ ? *patchArchives_ : archives_;
            result.manifest = source.read_manifest(source.root(), first.location, first.offset, first.size, gameRoot_);
            result.payloads.assign(region.files.begin() + 1, region.files.end());
        }
        return result;
    }
    return std::nullopt;
}

std::vector<std::byte> GameData::read(const fb::BundleFileInfo& payload) const {
    const auto& source = payload.location.patch && patchArchives_ ? *patchArchives_ : archives_;
    const auto raw = source.read(source.root(), payload.location, payload.offset, payload.size);
    return fb::decode_cas(raw, {gameRoot_});
}
}
