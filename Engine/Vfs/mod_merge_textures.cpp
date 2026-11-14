#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/texture.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace dingosdk::mods::detail {
namespace {
constexpr std::uint16_t texture_streams = 0x1;   // the flag the game's own streamed world textures carry
constexpr std::uint32_t texture_2d = 0;

// The hash a chunk record names its texture by: of the texture's lower-case name.
std::uint64_t owner_hash(std::string_view name) {
    std::uint64_t value = 5381;
    for (const auto c : name) value = value * 33U ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Where, in the encoded chunk, the block that decodes to `logical` onwards starts; nothing when
// no block starts there. Each block: a byte, its decoded size (24 bits, big-endian), a byte of
// compression, its encoded size (the low 20 bits), then that many bytes.
std::optional<std::uint32_t> block_at(std::span<const std::byte> encoded, std::uint64_t logical) {
    std::uint64_t decoded{};
    std::size_t at{};
    const auto byte = [&](std::size_t i) { return std::to_integer<std::uint32_t>(encoded[at + i]); };
    while (at + 8 <= encoded.size()) {
        if (decoded == logical) return static_cast<std::uint32_t>(at);
        if (decoded > logical) return std::nullopt;
        const auto decodedSize = (byte(1) << 16) | (byte(2) << 8) | byte(3);
        const auto encodedSize = ((byte(5) & 15) << 16) | (byte(6) << 8) | byte(7);
        if (!decodedSize || encodedSize > encoded.size() - at - 8) return std::nullopt;
        at += 8 + encodedSize;
        decoded += decodedSize;
    }
    return std::nullopt;
}
} // namespace

StreamedTextures stream_resident_textures(const CasStore& store, const fs::path& root, fb::BinaryBundle& manifest, std::vector<fb::BundleFileInfo>& files, std::size_t first,
                                          const std::set<fb::Guid>& tocChunks, const fs::path& gameRoot,
                                          const std::vector<std::uint32_t>* only) {
    StreamedTextures result;
    if (manifest.chunks.empty() || manifest.chunkMetadata.empty() || (only && only->empty())) return result;
    // The bundle's chunk records, read when the first texture that would stream needs its own: a
    // level bundle has thousands, and reading them for each bundle that turned out to have no
    // such texture was most of a merge of a map already read once (2.4 of 3.2 s; 2026-11-14).
    // A list that cannot be read changes nothing.
    std::optional<native_db::Node> records;
    bool unreadable{};
    const auto chunk_records = [&]() -> native_db::Node* {
        if (!records && !unreadable) try {
            const std::span<const unsigned char> list(reinterpret_cast<const unsigned char*>(manifest.chunkMetadata.data()),
                                                      manifest.chunkMetadata.size());
            records = native_db::read(list, "chunk metadata", nullptr, {.unique_fields = false, .max_entries = list.size()});
        } catch (const std::exception&) {
            unreadable = true;
        }
        return records ? &*records : nullptr;
    };
    // A texture's record is found by its name's hash and its pixels by their chunk's guid: looked
    // up, where each texture used to go through every record and every chunk for its own (the
    // first of each, as going through them found). Made when the first texture asks.
    std::optional<std::unordered_map<std::uint64_t, std::size_t>> recordAt;
    const auto record_of = [&](std::string_view name) -> native_db::Node* {
        auto* list = chunk_records();
        if (!list) return nullptr;
        if (!recordAt) {
            recordAt.emplace();
            for (std::size_t row = 0; row < list->children.size(); ++row)
                if (const auto* hash = list->children[row].field("h64"); hash && hash->payload().size() == 8) {
                    std::uint64_t value{};
                    std::memcpy(&value, hash->payload().data(), 8);
                    recordAt->try_emplace(value, row);
                }
        }
        const auto found = recordAt->find(owner_hash(name));
        return found == recordAt->end() ? nullptr : &list->children[found->second];
    };
    std::optional<std::map<fb::Guid, std::size_t>> chunkAt;
    const auto chunk_of = [&](const fb::Guid& guid) {
        if (!chunkAt) {
            chunkAt.emplace();
            for (std::size_t at = 0; at < manifest.chunks.size(); ++at) chunkAt->try_emplace(manifest.chunks[at].guid, at);
        }
        const auto found = chunkAt->find(guid);
        return found == chunkAt->end() ? manifest.chunks.end() : manifest.chunks.begin() + static_cast<std::ptrdiff_t>(found->second);
    };
    const std::unordered_set<std::uint32_t> wanted = only ? std::unordered_set<std::uint32_t>(only->begin(), only->end())
                                                          : std::unordered_set<std::uint32_t>{};
    const auto resourcesAt = first + manifest.ebx.size();
    const auto chunksAt = resourcesAt + manifest.resources.size();
    for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
        auto& asset = manifest.resources[index];
        if (asset.resourceType != fb::texture_resource_type || resourcesAt + index >= files.size()) continue;
        if (only && !wanted.contains(static_cast<std::uint32_t>(resourcesAt + index))) continue;
        const auto& file = files[resourcesAt + index];
        if (!file.location.patch) continue;   // the game's own texture
        try {
            auto header = fb::decode_cas(store.read(root, file.location, file.offset, file.size), {gameRoot});
            const auto texture = fb::read_texture_header(header, asset.resourceMeta);
            // Only a map's world textures: Studio keeps cosmetics and UI whole on purpose, and a
            // costume's or a face's texture made to stream left a load waiting forever at
            // Starting server (2026-10-17).
            if (header.size() != 180) continue;
            constexpr std::size_t group = 0x80;
            constexpr std::string_view worlds = "Worlds_";
            if (std::memcmp(header.data() + group, worlds.data(), worlds.size()) != 0) continue;
            if (texture.type != texture_2d || !(texture.flags & texture_streams) || texture.mip_count < 2)
                continue;
            // The streamer reads a chunk as compressed blocks only when its guid says so (the low
            // bit of its last byte), and as raw pixels otherwise. Kept whole, nothing ever streams
            // it; streamed, a compressed chunk under a guid without the bit was copied as pixels
            // past what was read (a crash in memcpy, 70 of PQS Cordoba's, 2026-10-17).
            if ((std::to_integer<unsigned>(texture.chunk.bytes[15]) & 1u) == 0) continue;
            // The mips the game would stream, as Studio's map compiler picks them: up to three,
            // while what stays is over 64 KB, and only where the tail starts on a 64 KB block.
            std::uint64_t total{};
            for (std::size_t mip = 0; mip < texture.mip_count; ++mip) total += texture.mip_sizes[mip];
            if (texture.first_mip != 0) {
                // Early Studio builds wrote streamed world textures with the bundle chunk already
                // sliced to the resident tail, leaving logicalOffset at 0 instead of the streamed
                // mips' byte count (864 textures of ZennyAU-LES_Skatepark, 646 of Mw3_Resistance).
                if (texture.first_mip >= texture.mip_count) continue;
                std::uint64_t streamedBytes{};
                for (std::size_t mip = 0; mip < texture.first_mip; ++mip) streamedBytes += texture.mip_sizes[mip];
                if (!streamedBytes || (streamedBytes & 0xffffu)) continue;
                const auto chunk = chunk_of(texture.chunk);
                if (chunk == manifest.chunks.end() || !tocChunks.contains(texture.chunk)) continue;
                if (chunk->logicalOffset != 0 || chunk->logicalSize != total - streamedBytes || !chunk_records()) continue;
                chunk->logicalOffset = static_cast<std::uint32_t>(streamedBytes);
                chunk->originalSize = (chunk->logicalOffset & 0xFFFFU) | chunk->logicalSize;
                result.offsets.push_back(resourcesAt + index);
                if (result.offset_example.empty()) result.offset_example = asset.name;
                continue;
            }
            std::size_t firstMip{};
            std::uint64_t resident = total, start{};
            while (resident > 0x10000 && firstMip < 3 && firstMip + 1 < texture.mip_count) {
                resident -= texture.mip_sizes[firstMip];
                start += texture.mip_sizes[firstMip++];
            }
            while (firstMip && (start & 0xffffu)) start -= texture.mip_sizes[--firstMip];
            if (!firstMip) continue;
            // Its pixels: a chunk of this bundle that holds the whole chain, which the TOC lists
            // in full for the mips that stream.
            const auto chunk = chunk_of(texture.chunk);
            if (chunk == manifest.chunks.end() || !tocChunks.contains(texture.chunk)) continue;
            const auto chunkIndex = chunksAt + static_cast<std::size_t>(chunk - manifest.chunks.begin());
            if (chunkIndex >= files.size()) continue;
            auto& chunkFile = files[chunkIndex];
            if (!chunkFile.location.patch || chunk->logicalOffset != 0 || chunk->logicalSize != total) continue;
            const auto encoded = store.read(root, chunkFile.location, chunkFile.offset, chunkFile.size);
            const auto range = block_at(encoded, start);
            if (!range) continue;
            // Its record, by its name's hash, says the same first mip.
            auto* record = record_of(asset.name);
            auto* meta = record ? record->field("meta") : nullptr;
            auto* mip = meta ? meta->field("firstMip") : nullptr;
            if (!mip || mip->payload().size() != 4) continue;

            // The first mip is in the header twice in this layout, and the game's own streamed
            // textures (and Studio's) have both the same: with 0x20 left at 0 the game took the
            // bundle's tail for the whole chain and read past it (a crash in memcpy as Cordoba
            // finished loading, 2026-10-17).
            constexpr std::size_t firstMipAt = 0x1f, residentMipAt = 0x20;
            if (header[firstMipAt] != std::byte{0} || header[residentMipAt] != std::byte{0}) continue;
            header[firstMipAt] = static_cast<std::byte>(firstMip);
            header[residentMipAt] = static_cast<std::byte>(firstMip);
            if (fb::read_texture_header(header, asset.resourceMeta).first_mip != firstMip)
                throw std::runtime_error("the first mip did not read back");
            mip->owned.assign(4, 0);
            mip->owned[0] = static_cast<unsigned char>(firstMip);
            chunk->logicalOffset = static_cast<std::uint32_t>(start);
            chunk->logicalSize = static_cast<std::uint32_t>(total - start);
            chunkFile.offset += *range;
            chunkFile.size -= *range;
            asset.sha1 = sha1_of(header);
            asset.originalSize = header.size();
            result.headers.emplace(resourcesAt + index, fb::encode_cas(header, {gameRoot}));
            result.bytes += start;
            if (result.example.empty()) result.example = asset.name;
        } catch (const std::exception&) {
            continue;
        }
    }
    if (!result.headers.empty()) {
        const auto bytes = native_db::write(*records);
        const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
        manifest.chunkMetadata.assign(data, data + bytes.size());
    }
    return result;
}

} // namespace dingosdk::mods::detail
