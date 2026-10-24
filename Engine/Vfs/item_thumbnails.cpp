#include "item_thumbnails.h"
#include "game_bundles.h"
#include "Engine/Core/Platform/path_text.h"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fb = frostbite;
namespace {
// Where the game keeps its item thumbnails: one texture per owned item.
constexpr std::string_view bundle_name = "win32/characters/customization/configs/cas_main_sharedbundle";
constexpr std::string_view prefix = "thumbnail/tool/";

bool build_kit_item(std::string_view item) {
    return item.starts_with("own_bk") && !item.ends_with("_lrg") &&
        std::all_of(item.begin(), item.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
}

namespace {
struct Opened {
    fb::TocDocument toc;
    std::optional<GameBundle> bundle;
};
// The bundle's superbundle is looked up rather than assumed, so a game update that moves it only
// costs a scan of the TOCs.
Opened open_bundle(const GameData& data, const std::filesystem::path& gameRoot, std::string_view name) {
    Opened opened;
    std::error_code error;
    std::string problems;
    for (std::filesystem::recursive_directory_iterator it(gameRoot / L"Data" / L"Win32", error), end;
         it != end && !error && !opened.bundle; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        try {
            opened.toc = data.read_toc(std::filesystem::relative(it->path(), gameRoot / L"Data").generic_string());
            opened.bundle = data.read_bundle(opened.toc, name);
        } catch (const std::exception& failure) {
            if (problems.size() < 600)
                problems += "; " + path_utf8(it->path().filename()) + ": " + failure.what();
        }
    }
    if (!opened.bundle) throw std::runtime_error("The game's bundle " + std::string(name) + " was not found" + problems);
    return opened;
}
// One texture of the bundle, decoded and scaled.
fb::Image read_texture(const GameData& data, const Opened& opened, std::size_t index, std::uint32_t side) {
    const auto& bundle = *opened.bundle;
    const auto& resource = bundle.manifest.resources[index];
    const auto* payload = bundle.payload(fb::AssetKind::resource, index);
    if (!payload) throw std::runtime_error("missing payload");
    const auto header = fb::read_texture_header(data.read(*payload), resource.resourceMeta);
    // Pixels are usually in the same bundle; otherwise the TOC's own chunks.
    std::vector<std::byte> pixels;
    std::size_t chunk{};
    if (bundle.find_chunk(header.chunk, &chunk)) {
        pixels = data.read(*bundle.payload(fb::AssetKind::chunk, chunk));
    } else {
        const auto found = std::find_if(opened.toc.chunks.begin(), opened.toc.chunks.end(),
            [&](const fb::TocChunk& entry) { return entry.guid == header.chunk; });
        if (found == opened.toc.chunks.end()) throw std::runtime_error("missing pixel chunk");
        pixels = data.read({found->location, found->offset, found->size});
    }
    return fb::resize(fb::decode_texture(header, pixels, side), side, side);
}
}

ThumbnailRead read_build_kit_thumbnails(const std::filesystem::path& gameRoot, std::uint32_t side) {
    const GameData data(gameRoot);
    const auto opened = open_bundle(data, gameRoot, bundle_name);
    ThumbnailRead result;
    const auto& resources = opened.bundle->manifest.resources;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (resource.resourceType != fb::texture_resource_type || !resource.name.starts_with(prefix)) continue;
        const auto item = std::string_view(resource.name).substr(prefix.size());
        if (!build_kit_item(item)) continue;
        try {
            result.thumbnails.push_back({std::string(item), read_texture(data, opened, index, side)});
        } catch (...) {
            ++result.failed;
        }
    }
    return result;
}

ThumbnailRead read_ui_textures(const std::filesystem::path& gameRoot, std::string_view bundle,
                               std::span<const std::string> names, std::uint32_t side) {
    const GameData data(gameRoot);
    const auto opened = open_bundle(data, gameRoot, bundle);
    ThumbnailRead result;
    const auto& resources = opened.bundle->manifest.resources;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (resource.resourceType != fb::texture_resource_type ||
            std::find(names.begin(), names.end(), resource.name) == names.end())
            continue;
        try {
            result.thumbnails.push_back({resource.name, read_texture(data, opened, index, side)});
        } catch (...) {
            ++result.failed;
        }
    }
    return result;
}
}
