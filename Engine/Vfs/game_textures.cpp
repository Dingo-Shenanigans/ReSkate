#include "game_textures.h"
#include <algorithm>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fb = frostbite;

GameTextures::GameTextures(std::filesystem::path gameRoot) : data_(std::move(gameRoot)) {}

const fb::TocDocument& GameTextures::toc(std::string_view name) {
    auto found = tocs_.find(name);
    if (found == tocs_.end()) found = tocs_.emplace(std::string(name), data_.read_toc(name)).first;
    return found->second;
}

const GameBundle& GameTextures::bundle(std::string_view toc_name, std::string_view name) {
    const auto key = std::make_pair(std::string(toc_name), std::string(name));
    auto found = bundles_.find(key);
    if (found == bundles_.end()) {
        auto read = data_.read_bundle(toc(toc_name), name);
        if (!read) throw std::runtime_error("the bundle " + std::string(name) + " is missing");
        found = bundles_.emplace(key, std::move(*read)).first;
    }
    return found->second;
}

fb::Image GameTextures::read(std::string_view toc_name, std::string_view bundle_name, std::string_view name,
                             std::uint32_t side) {
    if (!side) throw std::runtime_error("a texture needs a size to fit");
    const auto& in = bundle(toc_name, bundle_name);
    std::size_t index{};
    if (!in.find(fb::AssetKind::resource, name, &index) || !in.payload(fb::AssetKind::resource, index))
        throw std::runtime_error("the texture " + std::string(name) + " is missing");
    const auto& resource = in.manifest.resources[index];
    if (resource.resourceType != fb::texture_resource_type)
        throw std::runtime_error(std::string(name) + " is not a texture");
    const auto header = fb::read_texture_header(data_.read(*in.payload(fb::AssetKind::resource, index)), resource.resourceMeta);
    // A streamed texture's bundle holds only its small mips; the TOC's own chunk holds them all.
    std::vector<std::byte> pixels;
    const auto& chunks = toc(toc_name).chunks;
    const auto whole = std::find_if(chunks.begin(), chunks.end(),
        [&](const fb::TocChunk& chunk) { return chunk.guid == header.chunk && !chunk.removed; });
    std::size_t chunk{};
    if (whole != chunks.end()) pixels = data_.read({whole->location, whole->offset, whole->size});
    else if (in.find_chunk(header.chunk, &chunk) && in.payload(fb::AssetKind::chunk, chunk))
        pixels = data_.read(*in.payload(fb::AssetKind::chunk, chunk));
    else throw std::runtime_error("the pixels of " + std::string(name) + " are missing");
    // The smallest mip whose longer side still covers `side`.
    const auto longer = std::max(header.width, header.height), shorter = std::min(header.width, header.height);
    const auto minimum = longer <= side ? shorter : std::max<std::uint32_t>(1, (side * shorter + longer - 1) / longer);
    auto image = fb::decode_texture(header, pixels, minimum);
    const auto fitted = std::max(image.width, image.height);
    if (fitted <= side) return image;
    return fb::resize(image, std::max<std::uint32_t>(1, image.width * side / fitted),
                      std::max<std::uint32_t>(1, image.height * side / fitted));
}
}
