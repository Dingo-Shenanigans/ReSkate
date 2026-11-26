#pragma once

// The format check of the merged patch: what the merge wrote, read back from Mods/.reskate as the
// game reads it, against the rules ReSkate Studio's `verify-mod` holds a built mod to (Studio's
// src/modding/build/mod_verify.cpp and docs/). A broken asset is named in the merge's notes with
// the mod it came from, where before it was found by the game crashing (the chunk record without
// "h64" of 2026-10-25: Skate.exe+0x15b2c34).
//
// What the merge wrote: the documents it combined, the assets it carried into other bundles, the
// manifests and chunk lists of the bundles it rebuilt. What a mod's own archive holds is as the mod
// ships it and is not read: on a merge of 214 mods (2026-11-18) that was 895,590 documents and
// 190,425 meshes, eight minutes, and what it found was harmless and the mods' own (a boxed debug
// text lost from every map's level root, mesh chunks without their padding).
//
// The rules, each as Studio words it:
//   documents  read at all; every boxed value reads; no object id used twice
//   textures   a header of this game's size that reads; mips past the count empty; the total the
//              sum of the mips; a mip kept with the bundle; depth and slices; where the chunk's
//              place in the whole ends
//   meshes     each detail level's chunk as long as its vertices and indices; no section running
//              past its buffers; no triangle naming a vertex outside its section
//              (Engine/Resource/mesh_set.h, check_mesh_lod)
//   sounds     each chunk a sound wave document lists is as long as the document says
//   chunk lists every record has the owner hash ("h64") or a guid ("g")
// Studio checks more (the type library, a document written again being the pipeline's bytes,
// a sound bank's own layout); ReSkate has no reader for those. It also holds an exported object's
// id to its guid's, which mods' own maps break by the hundred thousand and still load, so that
// rule would only bury the rest (2026-11-18, a merge of 214 mods).
//
// Each payload the patch stores is read once, however many bundles list it.

#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/ebx_document.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::mods::detail {

namespace fb = frostbite;

// What is wrong with an EBX document, in Studio's words.
[[nodiscard]] std::vector<std::string> check_document_format(std::span<const std::byte> ebx);
// The same for a document already read.
[[nodiscard]] std::vector<std::string> check_document_format(const fb::ebx::Document& document);

// What is wrong with a texture resource: its header, its bundle metadata, and the bundle's entry
// for its chunk (null when the bundle has none).
[[nodiscard]] std::vector<std::string> check_texture_format(std::span<const std::byte> header, std::span<const std::byte> metadata,
                                                            const fb::BundleAsset* chunk);

// What is wrong with a MeshSet resource; `geometry` gives a chunk's bytes by its guid, or nothing
// when it cannot be found (that level is then not checked).
using ChunkBytes = std::function<std::optional<std::vector<std::byte>>(const fb::Guid&)>;
[[nodiscard]] std::vector<std::string> check_mesh_format(std::span<const std::byte> resource, const ChunkBytes& geometry);

// What is wrong with a sound wave document's list of chunks; `size` gives a chunk's length.
using ChunkSize = std::function<std::optional<std::size_t>(const fb::Guid&)>;
[[nodiscard]] std::vector<std::string> check_sound_format(const fb::ebx::Document& document, const ChunkSize& size);

// What is wrong with a bundle's chunk metadata.
[[nodiscard]] std::vector<std::string> check_chunk_metadata(std::span<const std::byte> metadata);

struct PatchCheck {
    struct Finding {
        std::string owner;    // the mod whose archive holds it, or "the merge"
        std::string bundle;
        std::string asset;
        std::string what;
    };
    std::vector<Finding> findings;
    std::size_t documents{}, textures{}, meshes{}, sounds{}, chunkLists{};   // each stored payload once
    double seconds{};
};

// Reads back what the merge wrote of the patch at `patchRoot` (Mods/.reskate, layout.toc written),
// in the superbundles `tocs` (as "Win32/....toc"), over the game at `gameRoot`. `owner` names whose
// a payload is: a mod's name for what its own archive holds, "the merge" for the merge's own
// archive, whose assets are then named after the mod that ships them. Never throws: a bundle that
// cannot be read is itself a finding.
[[nodiscard]] PatchCheck check_patch(const std::filesystem::path& gameRoot, const std::filesystem::path& patchRoot,
                                     std::span<const std::string> tocs,
                                     const std::function<std::string(const fb::BundleFileInfo&)>& owner);

} // namespace dingosdk::mods::detail
