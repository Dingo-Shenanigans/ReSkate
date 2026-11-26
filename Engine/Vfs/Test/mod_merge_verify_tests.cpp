// The format check of the merged patch (Engine/Vfs/mod_merge_verify.h).
//
// Each rule is held to an asset made here that breaks it and one that does not:
//   * a chunk list with a record that has neither "h64" nor "g" (what crashed the game at
//     Skate.exe+0x15b2c34 on 2026-10-25, a mod's grip texture added to a game deck bundle);
//   * a texture header whose total is not the sum of its mips, a mip kept past its count, a chunk
//     whose place in the whole does not end where the mips do;
//   * a document with an object id used twice (and one whose id is not its guid's, which is left
//     alone: mods' maps are full of them and load);
//   * a sound wave document that lists a chunk at another length than the chunk has.
// With the game's folder as argument, the same rules are run over the game's own documents,
// textures, meshes and chunk lists of its five top-level superbundles, where they must find nothing: the
// check names a mod only for what the game itself never does.
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/texture.h"
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Vfs/mod_merge_verify.h"
#include "Engine/Vfs/native_db.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;
namespace native_db = dingosdk::native_db;
using namespace dingosdk::mods::detail;

namespace {
int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
bool says(const std::vector<std::string>& problems, std::string_view part) {
    for (const auto& problem : problems)
        if (problem.find(part) != std::string::npos) return true;
    return false;
}

// A chunk list of `records`, each with an owner hash unless it is in `blank`.
std::vector<std::byte> chunk_list(std::size_t records, std::size_t blank) {
    native_db::Node list;
    list.type = 1;
    list.terminated = true;
    for (std::size_t index = 0; index < records; ++index) {
        native_db::Node record;
        record.type = 2;
        record.terminated = true;
        if (index != blank) {
            native_db::Node owner;
            owner.type = 9;
            owner.named = true;
            owner.name = "h64";
            owner.owned.assign(8, static_cast<unsigned char>(index + 1));
            record.children.push_back(std::move(owner));
        }
        native_db::Node meta;
        meta.type = 2;
        meta.named = true;
        meta.name = "meta";
        meta.terminated = true;
        record.children.push_back(std::move(meta));
        list.children.push_back(std::move(record));
    }
    const auto bytes = native_db::write(list);
    const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    return {data, data + bytes.size()};
}

void chunk_lists() {
    expect(check_chunk_metadata(chunk_list(3, 99)).empty(), "chunk list: every record with an owner hash passes");
    const auto problems = check_chunk_metadata(chunk_list(3, 1));
    expect(problems.size() == 1 && says(problems, "neither an owner hash (h64) nor a guid (g), e.g. record 1"),
           "chunk list: the record with neither is named");
    expect(check_chunk_metadata({}).empty(), "chunk list: a bundle with none passes");
}

// A 2D texture header as the game writes one (180 bytes, metadata version 13), 64 x 64 in BC1.
struct Texture {
    std::vector<std::byte> header = std::vector<std::byte>(180);
    std::vector<std::byte> metadata = std::vector<std::byte>(16);
    template <typename Type> void put(std::size_t at, Type value) { std::memcpy(header.data() + at, &value, sizeof(value)); }
    Texture() {
        const std::uint32_t version = 13;
        std::memcpy(metadata.data(), &version, 4);
        put<std::uint16_t>(22, 64);
        put<std::uint16_t>(24, 64);
        put<std::uint16_t>(26, 1);
        put<std::uint16_t>(28, 1);
        put<std::uint8_t>(30, 3);   // mips
        put<std::uint8_t>(31, 1);   // the first is streamed
        const std::uint32_t mips[3]{2048, 512, 128};
        for (std::size_t mip = 0; mip < 3; ++mip) put<std::uint32_t>(56 + mip * 4, mips[mip]);
        put<std::uint32_t>(116, 2048 + 512 + 128);
    }
};

void textures() {
    const Texture sound;
    fb::BundleAsset chunk;
    chunk.kind = fb::AssetKind::chunk;
    chunk.logicalOffset = 2048;
    chunk.logicalSize = 640;
    expect(check_texture_format(sound.header, sound.metadata, &chunk).empty(), "texture: a sound header and chunk pass");
    expect(check_texture_format(sound.header, sound.metadata, nullptr).empty(), "texture: with its chunk elsewhere too");

    auto total = sound;
    total.put<std::uint32_t>(116, 4096);
    expect(says(check_texture_format(total.header, total.metadata, &chunk), "the total in its header is not the sum of its mips"),
           "texture: a wrong total is named");
    auto past = sound;
    past.put<std::uint32_t>(56 + 5 * 4, 16);
    expect(says(check_texture_format(past.header, past.metadata, nullptr), "a mip size past its mip count"),
           "texture: a mip past the count is named");
    auto placed = chunk;
    placed.logicalSize = 600;
    expect(says(check_texture_format(sound.header, sound.metadata, &placed), "does not end where its mips do (2688)"),
           "texture: a chunk ending elsewhere than the mips is named");
    expect(says(check_texture_format(std::span(sound.header).first(144), sound.metadata, nullptr), "another game's size"),
           "texture: a header of another size is named");
}

template <typename Type> ebx::Value value_of(Type data) {
    ebx::Value value;
    value.data = std::move(data);
    return value;
}
ebx::InstanceRecord instance(std::uint64_t flags, std::uint8_t seed) {
    ebx::InstanceRecord record;
    record.exported = true;
    record.instanceGuid.bytes.fill(static_cast<std::byte>(seed));
    record.object = std::make_shared<ebx::Object>();
    record.object->fields.push_back({0, "Flags", value_of(flags)});
    return record;
}
std::uint64_t id_of(std::uint8_t seed) {
    return (static_cast<std::uint64_t>(seed) * 0x01010101U & 0x01ffffffU) | 0x40000000U;
}

void documents() {
    ebx::Document document;
    document.instances = {instance(0, 0x01), instance(id_of(0x20), 0x20), instance(id_of(0x30), 0x30)};
    expect(check_document_format(document).empty(), "document: ids of their own pass");

    auto twice = document;
    twice.instances.push_back(instance(id_of(0x30), 0x40));
    expect(says(check_document_format(twice), "object ids are used twice, 1 time(s)"), "document: an id used twice is named");

    // An exported object whose id is not its guid's, as mods' own maps have by the thousand.
    auto stray = document;
    stray.instances.push_back(instance(id_of(0x50), 0x60));
    expect(check_document_format(stray).empty(), "document: an id not its guid's is left to the mod");
    expect(says(check_document_format(std::vector<std::byte>(8)), "does not read"), "document: bytes that are no document are named");
}

void sounds() {
    ebx::Document wave;
    fb::Guid chunk;
    chunk.bytes.fill(std::byte{7});
    auto entry = std::make_shared<ebx::Object>();
    entry->fields.push_back({0, "ChunkId", value_of(chunk)});
    entry->fields.push_back({1, "ChunkSize", value_of(std::uint64_t{1000})});
    ebx::InstanceRecord root;
    root.object = std::make_shared<ebx::Object>();
    root.object->fields.push_back({0, "Chunks", value_of(ebx::Value::Array{value_of(entry)})});
    wave.instances.push_back(root);
    const auto sized = [](std::size_t size) { return [size](const fb::Guid&) { return std::optional<std::size_t>(size); }; };
    expect(check_sound_format(wave, sized(1000)).empty(), "sound: a chunk of the length listed passes");
    expect(says(check_sound_format(wave, sized(900)), "is 900 bytes and the wave document says 1000"), "sound: another length is named");
}

// The rules over the game's own data: none may fire.
void the_games_own(const fs::path& root) {
    const dingosdk::vfs::GameData game(root);
    std::size_t documents{}, textures{}, meshes{}, lists{}, found{};
    std::string example;
    const auto note = [&](const std::string& asset, const std::vector<std::string>& problems) {
        if (problems.empty()) return;
        if (!found) example = asset + ": " + problems.front();
        found += problems.size();
    };
    for (const auto* name : {"Win32/customization.toc", "Win32/items.toc", "Win32/globals.toc", "Win32/buildkititems.toc", "Win32/ui.toc"}) {
    const auto toc = game.read_toc(name);
    std::map<fb::Guid, const fb::TocChunk*> tocChunks;
    for (const auto& chunk : toc.chunks) tocChunks.emplace(chunk.guid, &chunk);
    for (const auto& listed : toc.bundles) {
        const auto bundle = game.read_bundle(toc, listed.name);
        if (!bundle) continue;
        for (std::size_t index = 0; index < bundle->manifest.ebx.size(); ++index)
            if (const auto* payload = bundle->payload(fb::AssetKind::ebx, index)) {
                ++documents;
                note(bundle->manifest.ebx[index].name, check_document_format(game.read(*payload)));
            }
        for (std::size_t index = 0; index < bundle->manifest.resources.size(); ++index) {
            const auto& asset = bundle->manifest.resources[index];
            const auto* payload = bundle->payload(fb::AssetKind::resource, index);
            if (!payload) continue;
            if (asset.resourceType == fb::texture_resource_type) {
                ++textures;
                const auto bytes = game.read(*payload);
                const fb::BundleAsset* chunk{};
                try { chunk = bundle->find_chunk(fb::read_texture_header(bytes, asset.resourceMeta).chunk); } catch (const std::exception&) {}
                note(asset.name, check_texture_format(bytes, asset.resourceMeta, chunk));
            } else if (asset.resourceType == 0x49b156d4 && meshes < 2000) {
                ++meshes;
                note(asset.name, check_mesh_format(game.read(*payload), [&](const fb::Guid& guid) -> std::optional<std::vector<std::byte>> {
                    std::size_t at{};
                    if (bundle->find_chunk(guid, &at))
                        if (const auto* chunk = bundle->payload(fb::AssetKind::chunk, at)) return game.read(*chunk);
                    if (const auto known = tocChunks.find(guid); known != tocChunks.end())
                        return game.read({known->second->location, known->second->offset, known->second->size});
                    return std::nullopt;
                }));
            }
        }
        if (!bundle->manifest.chunkMetadata.empty()) {
            ++lists;
            note(listed.name + " chunk metadata", check_chunk_metadata(bundle->manifest.chunkMetadata));
        }
    }
    }
    std::cout << "the game's own: " << documents << " documents, " << textures << " textures, " << meshes << " meshes, "
              << lists << " chunk lists checked\n";
    expect(documents && textures && meshes && lists, "game: there was something of each kind to check");
    expect(found == 0, "game: " + std::to_string(found) + " finding(s) in the game's own data, e.g. " + example);
}
} // namespace

int main(int argc, char** argv) try {
    chunk_lists();
    textures();
    documents();
    sounds();
    if (argc > 1 && fs::exists(fs::path(argv[1]) / L"Data" / L"Win32" / L"customization.toc")) the_games_own(argv[1]);
    else std::cout << "No game given; the game's own data is skipped.\n";
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "merged patch format check: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
