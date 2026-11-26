#include "mod_merge_verify.h"

#include "mod_merge_internal.h"
#include "native_db.h"
#include "Engine/Resource/ebx_boxed.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/texture.h"
#include "Engine/Vfs/game_bundles.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace dingosdk::mods::detail {
namespace {

constexpr std::uint32_t mesh_set_type = 0x49b156d4;
constexpr std::uint32_t object_id_mask = 0x01ffffffU;

std::string text_of(std::size_t value) { return std::to_string(value); }

void boxed_in(const fb::ebx::Value& value, std::vector<fb::ebx::BoxedReference>& found) {
    if (const auto* boxed = std::get_if<fb::ebx::BoxedReference>(&value.data)) {
        if (boxed->encodedType != 0) found.push_back(*boxed);
    } else if (const auto* nested = std::get_if<std::shared_ptr<fb::ebx::Object>>(&value.data)) {
        if (*nested) for (const auto& field : (*nested)->fields) boxed_in(field.value, found);
    } else if (const auto* entries = std::get_if<fb::ebx::Value::Array>(&value.data)) {
        for (const auto& entry : *entries) boxed_in(entry, found);
    }
}

std::optional<std::uint64_t> whole_number(const fb::ebx::Value& value) {
    if (const auto* number = std::get_if<std::uint64_t>(&value.data)) return *number;
    if (const auto* number = std::get_if<std::int64_t>(&value.data)) return static_cast<std::uint64_t>(*number);
    return std::nullopt;
}

// The records of a chunk list that have neither an owner hash nor a guid, by place.
std::vector<std::size_t> blank_records(const native_db::Node& list) {
    std::vector<std::size_t> blank;
    for (std::size_t index = 0; index < list.children.size(); ++index)
        if (!list.children[index].field("h64") && !list.children[index].field("g")) blank.push_back(index);
    return blank;
}

native_db::Node read_chunk_list(std::span<const std::byte> metadata) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(metadata.data());
    return native_db::read({bytes, metadata.size()}, "chunk metadata", nullptr,
                           {.unique_fields = false, .max_entries = metadata.size()});
}

} // namespace

std::vector<std::string> check_document_format(const fb::ebx::Document& document) {
    std::vector<std::string> problems;
    std::size_t unreadBoxes{};
    std::vector<fb::ebx::BoxedReference> boxes;
    for (const auto& instance : document.instances)
        if (instance.object)
            for (const auto& field : instance.object->fields) boxed_in(field.value, boxes);
    for (const auto& box : boxes) unreadBoxes += !fb::ebx::read_boxed(document, box).has_value();
    if (unreadBoxes) problems.push_back(text_of(unreadBoxes) + " boxed value(s) in it do not decode");

    // An object's id is the low 25 bits of its Flags. Two objects of one document with one id
    // collide (see renumber in ebx_merge.cpp). Studio also holds an exported object's id to its
    // guid's; mods' own maps break that by the hundred thousand and load all the same
    // (2026-11-18, 214 mods), so the merge does not.
    std::map<std::uint32_t, std::size_t> ids;
    for (const auto& instance : document.instances) {
        if (!instance.object) continue;
        const auto* flags = instance.object->find("Flags");
        const auto number = flags ? whole_number(flags->value) : std::nullopt;
        if (!number || *number <= 0xFFFFU) continue;
        const auto id = static_cast<std::uint32_t>(*number) & object_id_mask;
        if (id) ++ids[id];
    }
    std::size_t repeated{};
    for (const auto& [id, count] : ids) repeated += count > 1 ? count - 1 : 0;
    if (repeated) problems.push_back("object ids are used twice, " + text_of(repeated) + " time(s)");
    return problems;
}

std::vector<std::string> check_document_format(std::span<const std::byte> ebx) {
    try {
        return check_document_format(fb::ebx::read_document(ebx));
    } catch (const std::exception& error) {
        return {std::string("the document does not read: ") + error.what()};
    }
}

std::vector<std::string> check_texture_format(std::span<const std::byte> header, std::span<const std::byte> metadata,
                                              const fb::BundleAsset* chunk) {
    if (header.size() != 180) return {"has a texture header of another game's size (" + text_of(header.size()) + " bytes)"};
    fb::TextureHeader texture;
    try {
        texture = fb::read_texture_header(header, metadata);
    } catch (const std::exception& error) {
        return {std::string("its header does not read: ") + error.what()};
    }
    std::vector<std::string> problems;
    if (texture.depth != texture.slices) problems.push_back("its depth and slice count differ");
    if (texture.type == 0 && texture.depth != 1) problems.push_back("is a 2D texture with more than one layer");
    std::uint64_t payload{}, beforeFirst{};
    bool past{};
    for (std::size_t mip = 0; mip < texture.mip_sizes.size(); ++mip) {
        if (mip >= texture.mip_count) {
            past |= texture.mip_sizes[mip] != 0;
            continue;
        }
        payload += texture.mip_sizes[mip];
        if (mip < texture.first_mip) beforeFirst += texture.mip_sizes[mip];
    }
    if (past) problems.push_back("has a mip size past its mip count");
    if (texture.chunk_size != payload) problems.push_back("the total in its header is not the sum of its mips");
    if (texture.first_mip >= texture.mip_count) problems.push_back("keeps no mip with its bundle");
    if (chunk && chunk->logicalSize != 0 && texture.type == 0) {
        if (static_cast<std::uint64_t>(chunk->logicalOffset) + chunk->logicalSize != payload)
            problems.push_back("its chunk's place in the whole (" + text_of(chunk->logicalOffset) + " + " + text_of(chunk->logicalSize) +
                               ") does not end where its mips do (" + text_of(payload) + ")");
        else if (chunk->logicalOffset > beforeFirst)
            problems.push_back("its bundle keeps less than the mips its header says are with it");
    }
    return problems;
}

std::vector<std::string> check_mesh_format(std::span<const std::byte> resource, const ChunkBytes& geometry) {
    std::vector<std::string> problems;
    std::size_t lods{};
    try {
        static_cast<void>(fb::any_mesh_lod_geometry(resource, 0, &lods));
    } catch (const std::exception&) {
        return problems;   // a layout ReSkate does not read; nothing to hold it to
    }
    for (std::size_t lod = 0; lod < lods; ++lod) {
        try {
            const auto level = fb::any_mesh_lod_geometry(resource, lod);
            const auto bytes = geometry(level.chunk);
            if (!bytes) continue;
            // Shorter than its vertices and indices, the GPU reads past it. (Studio also holds
            // it to their length rounded up to 16; mods' maps leave that padding off by the ten
            // thousand and load.)
            if (bytes->size() < static_cast<std::size_t>(level.vertex_bytes) + level.index_bytes)
                problems.push_back("detail level " + text_of(lod) + ": its chunk is " + text_of(bytes->size()) +
                                   " bytes, less than its vertices and indices (" +
                                   text_of(static_cast<std::size_t>(level.vertex_bytes) + level.index_bytes) + ")");
            for (const auto& problem : fb::check_mesh_lod(resource, lod, *bytes))
                problems.push_back("detail level " + text_of(lod) + ": " + problem);
        } catch (const std::exception& error) {
            problems.push_back("detail level " + text_of(lod) + " does not read: " + error.what());
        }
    }
    return problems;
}

std::vector<std::string> check_sound_format(const fb::ebx::Document& document, const ChunkSize& size) {
    std::vector<std::string> problems;
    const auto* root = document.root();
    const auto* chunks = root && root->object ? root->object->find("Chunks") : nullptr;
    const auto* entries = chunks ? std::get_if<fb::ebx::Value::Array>(&chunks->value.data) : nullptr;
    if (!entries) return problems;
    for (const auto& entry : *entries) {
        const auto* object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&entry.data);
        if (!object || !*object) continue;
        const auto* id = (*object)->find("ChunkId");
        const auto* listed = (*object)->find("ChunkSize");
        const auto* guid = id ? std::get_if<fb::Guid>(&id->value.data) : nullptr;
        const auto said = listed ? whole_number(listed->value) : std::nullopt;
        if (!guid || !said) continue;
        const auto actual = size(*guid);
        if (actual && *actual != *said)
            problems.push_back("chunk " + guid->string() + " is " + text_of(*actual) + " bytes and the wave document says " +
                               text_of(static_cast<std::size_t>(*said)));
    }
    return problems;
}

std::vector<std::string> check_chunk_metadata(std::span<const std::byte> metadata) {
    if (metadata.empty()) return {};
    try {
        const auto blank = blank_records(read_chunk_list(metadata));
        if (blank.empty()) return {};
        return {text_of(blank.size()) + " chunk record(s) have neither an owner hash (h64) nor a guid (g), e.g. record " +
                text_of(blank.front()) + ": the game reads such a record's guid through null"};
    } catch (const std::exception& error) {
        return {std::string("the chunk metadata does not read: ") + error.what()};
    }
}

PatchCheck check_patch(const std::filesystem::path& gameRoot, const std::filesystem::path& patchRoot,
                       std::span<const std::string> tocs, const std::function<std::string(const fb::BundleFileInfo&)>& owner) {
    const auto started = std::chrono::steady_clock::now();
    PatchCheck check;
    std::optional<vfs::GameData> data;
    try {
        data.emplace(gameRoot, patchRoot);
    } catch (const std::exception& error) {
        check.findings.push_back({"the merge", "", "layout.toc", std::string("the patch does not read: ") + error.what()});
        return check;
    }
    const auto add = [&](const std::string& who, const std::string& bundle, const std::string& asset, std::vector<std::string> problems) {
        for (auto& problem : problems) check.findings.push_back({who, bundle, asset, std::move(problem)});
    };
    // The patch's superbundles list the same payload in bundle after bundle (each map's TOC
    // carries the shared bundles again): each stored payload is read once.
    using Stored = std::tuple<bool, std::uint32_t, std::uint16_t, std::uint32_t, std::uint32_t>;
    const auto stored = [](const fb::BundleFileInfo& payload) {
        return Stored{payload.location.patch, payload.location.installChunk, payload.location.archive, payload.offset, payload.size};
    };
    std::set<Stored> seen;
    std::set<fb::Sha1> lists;
    // Whose an asset is that the merge wrote into its own archive: the mod that ships it.
    std::map<std::string, std::string, std::less<>> shippedBy;
    for (const auto& relative : tocs) {
        fb::TocDocument toc;
        try {
            toc = data->read_toc(relative);
        } catch (const std::exception& error) {
            add("the merge", relative, "", {std::string("the superbundle does not read: ") + error.what()});
            continue;
        }
        std::map<fb::Guid, const fb::TocChunk*> tocChunks;
        for (const auto& chunk : toc.chunks)
            if (!chunk.removed) tocChunks.emplace(chunk.guid, &chunk);
        for (const auto& listed : toc.bundles) {
            std::optional<vfs::GameBundle> bundle;
            try {
                bundle = data->read_bundle(toc, ascii_lower(listed.name));
            } catch (const std::exception& error) {
                add("the merge", listed.name, "", {std::string("the bundle does not read: ") + error.what()});
                continue;
            }
            if (!bundle) continue;
            const auto& manifest = bundle->manifest;
            // A chunk's bytes: the bundle's own, else the superbundle's.
            const auto where = [&](const fb::Guid& guid) -> std::optional<fb::BundleFileInfo> {
                std::size_t index{};
                if (bundle->find_chunk(guid, &index))
                    if (const auto* payload = bundle->payload(fb::AssetKind::chunk, index)) return *payload;
                if (const auto found = tocChunks.find(guid); found != tocChunks.end())
                    return fb::BundleFileInfo{found->second->location, found->second->offset, found->second->size};
                return std::nullopt;
            };
            const ChunkBytes chunkBytes = [&](const fb::Guid& guid) -> std::optional<std::vector<std::byte>> {
                const auto payload = where(guid);
                if (!payload) return std::nullopt;
                try {
                    return data->read(*payload);
                } catch (const std::exception&) {
                    return std::nullopt;
                }
            };
            bool fromPatch{};
            for (std::size_t index = 0; index < manifest.ebx.size(); ++index) {
                const auto* payload = bundle->payload(fb::AssetKind::ebx, index);
                if (!payload || !payload->location.patch) continue;
                fromPatch = true;
                if (!seen.insert(stored(*payload)).second) continue;
                // What a mod's own archive holds is as that mod ships it: what the merge wrote is
                // what is read back.
                const auto who = owner(*payload);
                if (who != "the merge") {
                    shippedBy.try_emplace(manifest.ebx[index].name, who);
                    continue;
                }
                ++check.documents;
                try {
                    const auto document = fb::ebx::read_document(data->read(*payload));
                    add(who, listed.name, manifest.ebx[index].name, check_document_format(document));
                    if (ascii_lower(document.rootType).find("soundwave") != std::string::npos) {
                        ++check.sounds;
                        add(who, listed.name, manifest.ebx[index].name, check_sound_format(document, [&](const fb::Guid& guid) {
                            const auto bytes = chunkBytes(guid);
                            return bytes ? std::optional<std::size_t>(bytes->size()) : std::nullopt;
                        }));
                    }
                } catch (const std::exception& error) {
                    add(who, listed.name, manifest.ebx[index].name, {std::string("the document does not read: ") + error.what()});
                }
            }
            for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                const auto& asset = manifest.resources[index];
                const auto* payload = bundle->payload(fb::AssetKind::resource, index);
                if (!payload || !payload->location.patch) continue;
                fromPatch = true;
                if (asset.resourceType != fb::texture_resource_type && asset.resourceType != mesh_set_type) continue;
                if (!seen.insert(stored(*payload)).second) continue;
                const auto who = owner(*payload);
                if (who != "the merge") {
                    shippedBy.try_emplace(asset.name, who);
                    continue;
                }
                try {
                    const auto bytes = data->read(*payload);
                    if (asset.resourceType == fb::texture_resource_type) {
                        ++check.textures;
                        const fb::BundleAsset* chunk{};
                        try {
                            chunk = bundle->find_chunk(fb::read_texture_header(bytes, asset.resourceMeta).chunk);
                        } catch (const std::exception&) {}
                        add(who, listed.name, asset.name, check_texture_format(bytes, asset.resourceMeta, chunk));
                    } else {
                        ++check.meshes;
                        add(who, listed.name, asset.name, check_mesh_format(bytes, chunkBytes));
                    }
                } catch (const std::exception& error) {
                    add(who, listed.name, asset.name, {std::string("the resource does not read: ") + error.what()});
                }
            }
            for (std::size_t index = 0; !fromPatch && index < manifest.chunks.size(); ++index)
                if (const auto* payload = bundle->payload(fb::AssetKind::chunk, index)) fromPatch = payload->location.patch;
            // The game's own bundle, as it ships it, is the game's to answer for; a list met
            // before is not read again.
            if (!fromPatch || manifest.chunkMetadata.empty() || !lists.insert(sha1_of(manifest.chunkMetadata)).second) continue;
            ++check.chunkLists;
            try {
                const auto blank = blank_records(read_chunk_list(manifest.chunkMetadata));
                if (blank.empty()) continue;
                // In the game's form a list has a record for each chunk, by the chunks' guids: so
                // the first blank record's chunk is whose the list's mistake most likely is.
                std::vector<std::size_t> order(manifest.chunks.size());
                for (std::size_t at = 0; at < order.size(); ++at) order[at] = at;
                std::ranges::stable_sort(order, [&](std::size_t left, std::size_t right) {
                    return manifest.chunks[left].guid < manifest.chunks[right].guid; });
                std::string who = "the merge", asset = "chunk metadata";
                if (blank.front() < order.size()) {
                    const auto chunk = order[blank.front()];
                    asset = "chunk metadata, the record of chunk " + manifest.chunks[chunk].guid.string();
                    if (const auto* payload = bundle->payload(fb::AssetKind::chunk, chunk); payload && payload->location.patch)
                        who = owner(*payload);
                }
                add(who, listed.name, asset, check_chunk_metadata(manifest.chunkMetadata));
            } catch (const std::exception& error) {
                add("the merge", listed.name, "chunk metadata", {std::string("the chunk metadata does not read: ") + error.what()});
            }
        }
    }
    // A mod's asset the merge wrote into its own archive (into another bundle, or combined with
    // another mod's copy) is still that mod's to answer for.
    for (auto& finding : check.findings)
        if (finding.owner == "the merge")
            if (const auto found = shippedBy.find(finding.asset); found != shippedBy.end())
                finding.owner = found->second + " (as the merge wrote it)";
    check.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return check;
}

} // namespace dingosdk::mods::detail
