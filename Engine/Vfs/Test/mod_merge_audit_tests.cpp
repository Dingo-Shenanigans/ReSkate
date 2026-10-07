// Runs the whole mod merge on a data root and audits the patch it wrote, against the game and
// against every mod, so a Mods folder of real mods can be checked without starting the game.
//
// What has to hold of any merge:
//   * every superbundle TOC reads back, and every placement lies inside an archive that is there
//     and that the merged layout declares;
//   * every payload the patch stores decodes to the size its asset list gives, and every EBX in
//     it parses;
//   * every bundle, asset and TOC chunk a mod ships is in the patch;
//   * a document two mods' assets fought over by name is named by nothing that is left;
//   * a document the merge wrote keeps its instances in the order the engine searches them, has
//     every reference a mod appended to one of its lists, and has every value a mod changed in
//     place changed: no value in it is the merge's own invention;
//   * a list whose entries the game keeps beside it (an item list, the music playlist) names
//     nothing its own bundle does not hold, in every copy of that bundle the patch carries;
//   * a bundle's chunk metadata has one record for each of its chunks, in the order of the
//     chunks' guids as the game writes it: every texture's own record, with the first mip its
//     header gives, is at its chunk's place.
// What is only reported: assets two mods add under one name, and assets of the game two mods
// change where the patch has one mod's copy whole.
//
// Arguments: <data root> [<listing file>], the root being a folder holding the game's Data/ and
// a Mods/ folder; skipped when none is given. The merge runs there as the game would run it and
// writes Mods/.reskate. With a second argument, every asset the patch itself stores is written
// to that file, one line each, to compare two merges of the same mods.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk;
using namespace dingosdk::mods::detail;

namespace {
using Bytes = std::vector<std::byte>;

std::map<std::string, std::size_t> failures;
// The first of each kind in full, the rest counted: one broken mod can fail ten thousand times.
void fail(const std::string& kind, const std::string& text) {
    const auto count = ++failures[kind];
    if (count <= 25) std::cout << "FAIL [" << kind << "] " << text << '\n';
    else if (count == 26) std::cout << "FAIL [" << kind << "] ... more of these, counted below\n";
}
void section(const std::string& title) { std::cout << "\n=== " << title << '\n'; }

std::string hex(const fb::Sha1& sha) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto value : sha.bytes) {
        text += digits[static_cast<unsigned>(value) >> 4];
        text += digits[static_cast<unsigned>(value) & 15];
    }
    return text;
}
std::string key_of(const fb::BundleAsset& asset) {
    return std::to_string(static_cast<int>(asset.kind)) + ':' +
        (asset.kind == fb::AssetKind::chunk ? asset.guid.string() : lower(asset.name));
}
const char* kind_name(fb::AssetKind kind) {
    return kind == fb::AssetKind::ebx ? "ebx" : kind == fb::AssetKind::resource ? "res" : "chunk";
}
std::string name_of(const fb::BundleAsset& asset) {
    return std::string(kind_name(asset.kind)) + " " + (asset.kind == fb::AssetKind::chunk ? asset.guid.string() : asset.name);
}

struct Entry {
    const fb::BundleAsset* asset{};
    fb::BundleFileInfo file;
};
// One copy of a bundle: its asset list, and the assets in region order (ebx, resources, chunks).
struct Bundle {
    std::string name;
    std::optional<Listing> listing;
    std::vector<Entry> assets;
    std::map<std::string, std::size_t> at;   // asset key -> index into assets
};
struct Toc {
    std::string relative;   // Win32/..., generic separators
    fb::TocDocument document;
    std::vector<Bundle> bundles;
    std::map<std::string, std::size_t> bundleAt;   // lower-case name
    std::set<fb::Guid> chunks;
};

// Where a payload really lives: 0 the game, 1 the merged patch, 2 and up a mod.
struct Place {
    int layer{};
    std::uint32_t chunk{};
    std::uint16_t archive{};
    std::uint32_t offset{}, size{};
    auto operator<=>(const Place&) const = default;
};

struct EbxFacts {
    bool parsed{};
    fb::Guid file;
    std::string rootType;
    std::vector<fb::ebx::ImportReference> imports;
};

// One mod's own copy of an asset that is not the game's copy of it.
struct Copy {
    std::size_t mod{};
    fb::Sha1 sha1;
    fb::BundleFileInfo file;
};
// An asset at least one mod ships a copy of its own of, and what the patch has for it.
struct Contested {
    std::string bundle, name;
    fb::AssetKind kind{};
    bool shipped{};   // the game has this asset in this bundle
    fb::Sha1 game, merged;
    Place gamePlace, mergedPlace;
    std::vector<Copy> copies;
};

void walk(const fb::ebx::Value& value, const std::function<void(const fb::ebx::PointerReference&)>& visit) {
    if (const auto* pointer = std::get_if<fb::ebx::PointerReference>(&value.data)) { visit(*pointer); return; }
    if (const auto* object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&value.data)) {
        if (*object) for (const auto& field : (*object)->fields) walk(field.value, visit);
        return;
    }
    if (const auto* array = std::get_if<fb::ebx::Value::Array>(&value.data))
        for (const auto& element : *array) walk(element, visit);
}

// What a list entry refers to, so the same entry can be found in another copy of the document;
// empty for an entry that is not a reference to something with a guid.
std::string identity(const fb::ebx::Document& document, const fb::ebx::Value& value) {
    const auto* pointer = std::get_if<fb::ebx::PointerReference>(&value.data);
    if (!pointer || pointer->kind == fb::ebx::PointerKind::null || pointer->index < 0) return {};
    const auto index = static_cast<std::size_t>(pointer->index);
    if (pointer->kind == fb::ebx::PointerKind::external)
        return index < document.imports.size()
            ? "x:" + document.imports[index].fileGuid.string() + "/" + document.imports[index].classGuid.string() : std::string{};
    return index < document.instances.size() && document.instances[index].exported
        ? "i:" + document.instances[index].instanceGuid.string() : std::string{};
}
// The document a list entry names, when it names one in another file.
const fb::Guid* named(const fb::ebx::Document& document, const fb::ebx::Value& value) {
    const auto* pointer = std::get_if<fb::ebx::PointerReference>(&value.data);
    if (!pointer || pointer->kind != fb::ebx::PointerKind::external || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.imports.size()) return nullptr;
    return &document.imports[static_cast<std::size_t>(pointer->index)].fileGuid;
}

// Every value of a document by where it is: its instance (the root, an exported one's guid, an
// internal one's place among the internal ones), then the fields and list positions down to it.
// A plain value is spelled with a lower-case letter in front; a reference (what it leads to) and
// a list's length are spelled with an upper-case one, and are the document's shape.
struct Flat {
    std::map<std::string, std::string> values;
    std::size_t internal{};   // internal instances: a place is all one is known by
};
bool plain(const std::string& spelled) { return !spelled.empty() && spelled.front() >= 'a' && spelled.front() <= 'z'; }
void flatten(const fb::ebx::Document& document, const std::vector<std::string>& instances,
             const fb::ebx::Value& value, const std::string& path, Flat& out) {
    const auto& data = value.data;
    if (const auto* flag = std::get_if<bool>(&data)) out.values[path] = *flag ? "b1" : "b0";
    else if (const auto* number = std::get_if<std::int64_t>(&data)) out.values[path] = "i" + std::to_string(*number);
    else if (const auto* count = std::get_if<std::uint64_t>(&data)) out.values[path] = "u" + std::to_string(*count);
    else if (const auto* real = std::get_if<double>(&data)) out.values[path] = "d" + std::to_string(std::bit_cast<std::uint64_t>(*real));
    else if (const auto* text = std::get_if<std::string>(&data)) out.values[path] = "s" + *text;
    else if (const auto* guid = std::get_if<fb::Guid>(&data)) out.values[path] = "g" + guid->string();
    else if (const auto* sha = std::get_if<fb::Sha1>(&data)) out.values[path] = "h" + hex(*sha);
    else if (const auto* resource = std::get_if<fb::ebx::ResourceReference>(&data)) out.values[path] = "r" + std::to_string(resource->id);
    else if (const auto* pointer = std::get_if<fb::ebx::PointerReference>(&data)) {
        const auto index = pointer->index < 0 ? std::size_t{} : static_cast<std::size_t>(pointer->index);
        if (pointer->kind == fb::ebx::PointerKind::null || pointer->index < 0) out.values[path] = "P";
        else if (pointer->kind == fb::ebx::PointerKind::external)
            out.values[path] = index < document.imports.size()
                ? "P" + document.imports[index].fileGuid.string() + "/" + document.imports[index].classGuid.string() : std::string("P?");
        else out.values[path] = index < instances.size() ? "P" + instances[index] : std::string("P?");
    } else if (const auto* object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&data)) {
        if (*object) for (const auto& field : (*object)->fields) flatten(document, instances, field.value, path + "/" + field.name, out);
    } else if (const auto* array = std::get_if<fb::ebx::Value::Array>(&data)) {
        out.values[path + "/#"] = "N" + std::to_string(array->size());
        for (std::size_t index = 0; index < array->size(); ++index)
            flatten(document, instances, (*array)[index], path + "/" + std::to_string(index), out);
    }
}
Flat flatten(const fb::ebx::Document& document) {
    Flat out;
    std::vector<std::string> instances;
    for (std::size_t index = 0; index < document.instances.size(); ++index) {
        const auto& instance = document.instances[index];
        instances.push_back(index == 0 ? std::string("root")
            : instance.exported ? "x:" + instance.instanceGuid.string() : "n:" + std::to_string(out.internal++));
    }
    for (std::size_t index = 0; index < document.instances.size(); ++index)
        if (const auto& object = document.instances[index].object)
            for (const auto& field : object->fields) flatten(document, instances, field.value, instances[index] + "/" + field.name, out);
    return out;
}

struct Audit {
    fs::path root, base, out;
    mods::Catalog catalog;
    vfs::Layout baseLayout, mergedLayout;
    std::unique_ptr<CasStore> store;
    // A deque: bundles hold pointers into their own asset lists, which must not move.
    std::deque<Toc> tocs;
    std::map<std::string, std::size_t> tocAt;   // lower-case relative path
    // The game's bundles by name, wherever they are, to tell a mod's own content from the game's
    // copy of something it carries.
    std::map<std::string, fb::TocBundle> gameBundles;
    std::map<std::string, std::optional<Bundle>> gameListings;
    std::map<Place, EbxFacts> facts;
    std::map<Place, fb::Guid> guids;
    std::vector<Contested> contested;
    // The patch's bundles (by TOC and bundle) that a mod ships a readable copy of: the ones whose
    // chunk metadata the merge may have written.
    std::set<std::pair<std::size_t, std::size_t>> chunkBundles;
    // Those of them the patch has exactly as one mod ships them, chunks and list: the merge wrote
    // nothing there.
    std::set<std::pair<std::size_t, std::size_t>> chunkBundlesAsShipped;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    void progress(const std::string& what) const {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "[" << seconds << "s] " << what << std::endl;
    }
    const fs::path& layer(int index) const {
        return index == 0 ? base : index == 1 ? out : catalog.mods.at(static_cast<std::size_t>(index - 2)).directory;
    }
    // A placement that is not a patch one is in the game, whichever layer's bundle names it.
    Place place(int from, const fb::BundleFileInfo& file) const {
        return {file.location.patch ? from : 0, file.location.installChunk, file.location.archive, file.offset, file.size};
    }
    Bytes payload(const Place& where) const {
        fb::CasDecodeOptions options{root};
        options.maximumOutputSize = 2048ULL * 1024ULL * 1024ULL;
        return fb::decode_cas(store->read(layer(where.layer), {where.layer != 0, where.chunk, where.archive}, where.offset, where.size), options);
    }
    static EbxFacts facts_of(const Bytes& bytes) {
        EbxFacts result;
        try {
            const auto document = fb::ebx::read_document(bytes);
            result.parsed = true;
            result.file = document.fileGuid;
            result.rootType = document.rootType;
            result.imports = document.imports;
        } catch (const std::exception&) {}
        return result;
    }
    const EbxFacts& ebx(const Place& where) {
        if (const auto found = facts.find(where); found != facts.end()) return found->second;
        EbxFacts result;
        try { result = facts_of(payload(where)); } catch (const std::exception&) {}
        return facts.emplace(where, std::move(result)).first->second;
    }
    const fb::Guid* guid(const Place& where) {
        if (const auto found = guids.find(where); found != guids.end()) return &found->second;
        if (const auto known = facts.find(where); known != facts.end() && known->second.parsed)
            return &guids.emplace(where, known->second.file).first->second;
        try { return &guids.emplace(where, fb::ebx::read_file_guid(payload(where))).first->second; }
        catch (const std::exception&) { return nullptr; }
    }

    Bundle read_bundle(int from, const fb::TocBundle& bundle) const {
        Bundle result;
        result.name = bundle.name;
        try {
            result.listing = list_bundle(*store, layer(from), base, bundle, root);
            if (!result.listing) return result;
            std::size_t at = result.listing->first;
            for (const auto* list : {&result.listing->manifest.ebx, &result.listing->manifest.resources, &result.listing->manifest.chunks})
                for (const auto& asset : *list) {
                    if (at >= result.listing->files.size()) throw std::runtime_error("more assets than files");
                    result.at.emplace(key_of(asset), result.assets.size());
                    result.assets.push_back({&asset, result.listing->files[at++]});
                }
        } catch (const std::exception&) {
            result.listing.reset();
            result.assets.clear();
            result.at.clear();
        }
        return result;
    }
    const Bundle* game_bundle(const std::string& name) {
        const auto key = lower(name);
        if (const auto found = gameListings.find(key); found != gameListings.end()) return found->second ? &*found->second : nullptr;
        auto& slot = gameListings[key];
        if (const auto shipped = gameBundles.find(key); shipped != gameBundles.end())
            if (auto bundle = read_bundle(0, shipped->second); bundle.listing) slot = std::move(bundle);
        return slot ? &*slot : nullptr;
    }
    // The guid of every document a copy of a bundle holds.
    std::set<fb::Guid> held(int from, const Bundle& bundle, bool& unsure) {
        std::set<fb::Guid> result;
        for (const auto& entry : bundle.assets) {
            if (entry.asset->kind != fb::AssetKind::ebx) continue;
            if (const auto* id = guid(place(from, entry.file))) result.insert(*id); else unsure = true;
        }
        return result;
    }
    // One record of a bundle's chunk metadata: whose the chunk is, and a texture's first mip.
    struct ChunkMeta {
        std::optional<std::uint64_t> owner;
        std::optional<std::int32_t> firstMip;
    };
    // A copy's chunk metadata, record by record; nothing when the list cannot be read.
    static std::optional<std::vector<ChunkMeta>> records_of(const Bundle& bundle) {
        std::vector<ChunkMeta> records;
        const auto& list = bundle.listing->manifest.chunkMetadata;
        if (list.empty()) return records;
        try {
            const std::span<const unsigned char> bytes(reinterpret_cast<const unsigned char*>(list.data()), list.size());
            const auto root = native_db::read(bytes, "chunk metadata", nullptr, {.unique_fields = false, .max_entries = bytes.size()});
            for (const auto& row : root.children) {
                ChunkMeta record;
                if (const auto* hash = row.field("h64"); hash && hash->payload().size() == 8)
                    std::memcpy(&record.owner.emplace(), hash->payload().data(), 8);
                if (const auto* meta = row.field("meta"))
                    if (const auto* mip = meta->field("firstMip"); mip && mip->payload().size() == 4)
                        std::memcpy(&record.firstMip.emplace(), mip->payload().data(), 4);
                records.push_back(record);
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        return records;
    }
    // The hash a record names a resource by: of its lower-case name.
    static std::uint64_t owner_hash(std::string_view name) {
        std::uint64_t value = 5381;
        for (const auto c : name) value = value * 33U ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }
    // The textures of a copy of a bundle whose pixels are one of the bundle's chunks, each against
    // the record at its chunk's place when the chunks are in the order of their guids: how many
    // there are, how many have another resource's record (or none) there, how many their own with
    // another first mip than the texture's header gives, and one of those that are wrong.
    struct TextureRecords {
        std::size_t textures{}, misplaced{}, mips{};
        std::string example;
    };
    TextureRecords texture_records(int from, const Bundle& bundle, const std::vector<ChunkMeta>& records) const {
        constexpr std::uint32_t textureType = 0x6BDE20BA;
        const auto& manifest = bundle.listing->manifest;
        std::vector<std::size_t> order(manifest.chunks.size());
        for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
        std::ranges::sort(order, [&](std::size_t left, std::size_t right) { return manifest.chunks[left].guid < manifest.chunks[right].guid; });
        std::map<fb::Guid, std::size_t> guidPlace;
        for (std::size_t at = 0; at < order.size(); ++at) guidPlace.emplace(manifest.chunks[order[at]].guid, at);
        TextureRecords result;
        for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
            const auto& resource = manifest.resources[index];
            if (resource.resourceType != textureType || resource.resourceMeta.size() < 4) continue;
            Bytes header;
            try { header = payload(place(from, bundle.assets[manifest.ebx.size() + index].file)); } catch (const std::exception&) { continue; }
            if (header.size() != 144 && header.size() != 180) continue;
            // The header: type and format, a version's extra field, then flags and sizes, the mip
            // count and first mip, padding, and the chunk the pixels are in.
            std::uint32_t version{};
            std::memcpy(&version, resource.resourceMeta.data(), 4);
            const std::size_t mips = 8 + 4 + 4 + (version >= 12 ? 4 : 0) + 10;
            const auto firstMip = static_cast<std::int32_t>(header[mips + 1]);
            fb::Guid chunk;
            std::memcpy(chunk.bytes.data(), header.data() + mips + 2 + (header.size() == 180 ? 8 : 4), 16);
            const auto at = guidPlace.find(chunk);
            if (at == guidPlace.end()) continue;   // its pixels are not in this bundle
            ++result.textures;
            const auto* record = at->second < records.size() ? &records[at->second] : nullptr;
            if (!record || record->owner != owner_hash(resource.name)) {
                if (!result.misplaced++ && result.example.empty()) result.example = resource.name + " has " + (record ? "another resource's record" : "no record") + " at its chunk's place";
            } else if (record->firstMip != firstMip) {
                if (!result.mips++ && result.example.empty())
                    result.example = resource.name + " is told first mip " + (record->firstMip ? std::to_string(*record->firstMip) : std::string("none")) +
                                     ", its header says " + std::to_string(firstMip);
            }
        }
        return result;
    }
    std::string mod_names(const std::vector<std::size_t>& list) const {
        std::set<std::string> unique;
        for (const auto index : list) unique.insert(catalog.mods[index].name);
        std::string text;
        for (const auto& name : unique) text += (text.empty() ? "" : ", ") + name;
        return text;
    }

    // ---------------------------------------------------------------- the merge, and reading what it wrote
    bool merge() {
        section("Merge");
        catalog = mods::load_catalog(root);
        std::cout << catalog.mods.size() << " mod(s) merged, " << catalog.excluded.size() << " left out, "
                  << catalog.disabled.size() << " disabled; " << catalog.notes.size() << " note(s)\n";
        if (!catalog.issue.empty()) fail("merge", catalog.issue);
        for (const auto& warning : catalog.warnings) fail("left out", warning);
        if (!catalog.merged) fail("merge", "no merged patch was produced");
        return catalog.merged;
    }

    void load() {
        base = root / L"Data";
        out = root / L"Mods" / mods::generated_folder;
        baseLayout = vfs::read_layout(base / L"layout.toc");
        mergedLayout = vfs::read_layout(out / L"layout.toc");
        store = std::make_unique<CasStore>(base, fs::temp_directory_path() / L"reskate-merge-audit-unused", mergedLayout.root);

        section("Superbundles");
        const auto toc_files = [](const fs::path& under) {
            std::vector<fs::path> files;
            std::error_code error;
            for (fs::recursive_directory_iterator it(under / L"Win32", error), end; it != end && !error; it.increment(error))
                if (it->is_regular_file(error) && lower(it->path().extension().string()) == ".toc") files.push_back(it->path());
            std::ranges::sort(files);
            return files;
        };
        std::error_code error;
        std::size_t bundles{}, chunks{}, unreadable{};
        for (const auto& file : toc_files(out)) {
            Toc toc;
            toc.relative = fs::relative(file, out, error).generic_string();
            try {
                toc.document = fb::read_toc(read_file(file));
                fb::verify_toc(toc.document);
            } catch (const std::exception& failure) {
                fail("toc", toc.relative + ": " + failure.what());
                continue;
            }
            toc.bundles.reserve(toc.document.bundles.size());
            for (const auto& bundle : toc.document.bundles) {
                if (!toc.bundleAt.emplace(lower(bundle.name), toc.bundles.size()).second)
                    fail("toc", toc.relative + " lists " + bundle.name + " twice");
                toc.bundles.push_back(read_bundle(1, bundle));
                unreadable += !toc.bundles.back().listing;
                ++bundles;
            }
            for (const auto& chunk : toc.document.chunks)
                if (!chunk.removed) toc.chunks.insert(chunk.guid);
            chunks += toc.document.chunks.size();
            tocAt.emplace(lower(toc.relative), tocs.size());
            tocs.push_back(std::move(toc));
        }
        std::cout << "the patch: " << tocs.size() << " superbundle TOC(s), " << bundles << " bundle(s) (" << unreadable
                  << " without a readable asset list), " << chunks << " TOC chunk(s)\n";
        std::size_t shipped{};
        for (const auto& file : toc_files(base)) {
            ++shipped;
            try {
                for (auto& bundle : fb::read_toc(read_file(file)).bundles) gameBundles.try_emplace(lower(bundle.name), std::move(bundle));
            } catch (const std::exception&) {}
        }
        std::cout << "the game: " << shipped << " superbundle TOC(s), " << gameBundles.size() << " bundle name(s)\n";
    }

    // ---------------------------------------------------------------- placements and the layout
    void placements() {
        section("Placements and layout");
        using Archive = std::pair<std::uint32_t, std::uint16_t>;
        const auto declared_in = [](const native_db::Node& layout) {
            std::set<Archive> result;
            for (const auto* field : {"layeredInstallChunkFiles", "unlayeredInstallChunkFiles"})
                if (const auto* node = layout.field(field); node && node->type == 19)
                    vfs::for_each_install_chunk_file(node->payload(), [&](std::uint32_t id, std::uint16_t archive) { result.emplace(id, archive); });
            return result;
        };
        const auto declared = declared_in(mergedLayout.root), shipped = declared_in(baseLayout.root);
        std::set<Archive> used;
        std::size_t checked{};
        const auto check = [&](const std::string& what, const fb::CasIdentifier& location, std::uint32_t offset, std::uint32_t size) {
            ++checked;
            const auto on_disk = store->archive_size(location.patch ? out : base, location);
            if (!on_disk) {
                fail("placement", what + " is in " + store->describe(location) + (location.patch ? " of the patch" : " of the game") + ", which is not there");
                return;
            }
            if (static_cast<std::uint64_t>(offset) + size > *on_disk)
                fail("placement", what + ": " + std::to_string(size) + " bytes at " + std::to_string(offset) + " run past the end of " +
                     store->describe(location) + " (" + std::to_string(*on_disk) + " bytes)");
            if (location.patch) used.emplace(location.installChunk, location.archive);
        };
        for (const auto& toc : tocs) {
            for (const auto& bundle : toc.document.bundles) {
                const auto region = fb::read_bundle_region(bundle.region);
                for (std::size_t index = 0; index < region.files.size(); ++index)
                    check(toc.relative + ": " + bundle.name + " file " + std::to_string(index), region.files[index].location,
                          region.files[index].offset, region.files[index].size);
            }
            for (const auto& chunk : toc.document.chunks)
                if (!chunk.removed) check(toc.relative + ": chunk " + chunk.guid.string(), chunk.location, chunk.offset, chunk.size);
        }
        // An archive the layout does not declare is never opened; one it declares and the patch
        // lacks sends the engine looking for a file that is absent.
        for (const auto& archive : used)
            if (!declared.contains(archive))
                fail("layout", "the patch reads archive " + std::to_string(archive.second) + " of install chunk " +
                     std::to_string(archive.first) + ", which the merged layout does not declare");
        std::size_t added{};
        for (const auto& archive : declared) {
            if (shipped.contains(archive)) continue;
            ++added;
            if (!store->archive_size(out, {true, archive.first, archive.second}))
                fail("layout", "the merged layout declares archive " + std::to_string(archive.second) + " of install chunk " +
                     std::to_string(archive.first) + ", and the patch has no such file");
        }
        std::cout << checked << " placement(s) checked; " << used.size() << " patch archive(s) in use, " << added << " declared beyond the game's\n";

        // The superbundle list is searched by name, so it stays in order; a mod's own superbundles
        // have to be in it, in an install chunk, and have a TOC.
        const auto names = [](const native_db::Node& layout) {
            std::vector<std::string> result;
            if (const auto* list = layout.field("superBundles"))
                for (const auto& row : list->children)
                    if (const auto* name = row.field("name"); name && name->type == 7) result.push_back(lower(name->text));
            return result;
        };
        const auto merged = names(mergedLayout.root), game = names(baseLayout.root);
        const std::set<std::string> known(merged.begin(), merged.end()), stock(game.begin(), game.end());
        if (!std::ranges::is_sorted(merged)) fail("layout", "the merged superbundle list is not in name order");
        if (known.size() != merged.size()) fail("layout", "the merged superbundle list names one twice");
        std::set<std::string> placed;
        if (const auto* manifest = mergedLayout.root.field("installManifest"))
            if (const auto* chunks = manifest->field("installChunks"))
                for (const auto& chunk : chunks->children)
                    if (const auto* list = chunk.field("superbundles"))
                        for (const auto& entry : list->children)
                            if (entry.type == 7) placed.insert(lower(entry.text));
        std::size_t own{};
        for (const auto& mod : catalog.mods) {
            if (!mod.provides_layout) continue;
            try {
                const auto layout = vfs::read_layout(mod.directory / L"layout.toc");
                for (const auto& name : names(layout.root)) {
                    if (stock.contains(name)) continue;
                    ++own;
                    if (!known.contains(name)) fail("layout", mod.name + ": its superbundle " + name + " is not in the merged layout");
                    if (!placed.contains(name)) fail("layout", mod.name + ": its superbundle " + name + " is in no install chunk of the merged layout");
                    if (!tocAt.contains(name + ".toc")) fail("layout", mod.name + ": its superbundle " + name + " has no TOC in the patch");
                }
            } catch (const std::exception& failure) {
                fail("layout", mod.name + ": layout.toc could not be read (" + failure.what() + ")");
            }
        }
        std::cout << merged.size() << " superbundle(s) in the merged layout, " << own << " listed by mods beyond the game's\n";
    }

    // ---------------------------------------------------------------- asset lists and payloads
    void payloads(const fs::path& listing) {
        section("Asset lists and payloads");
        // What a listed size means is learnt from the game's own assets first: only a kind whose
        // every sample agrees is held to it.
        std::array<std::size_t, 3> sampled{}, agreed{};
        std::set<Place> seen;
        for (const auto& toc : tocs)
            for (const auto& bundle : toc.bundles)
                for (const auto& entry : bundle.assets) {
                    const auto kind = static_cast<std::size_t>(entry.asset->kind);
                    const auto where = place(1, entry.file);
                    if (where.layer != 0 || sampled[kind] >= 150 || entry.file.size > (8u << 20) || !seen.insert(where).second) continue;
                    ++sampled[kind];
                    try { agreed[kind] += payload(where).size() == entry.asset->originalSize; } catch (const std::exception&) {}
                }
        std::array<bool, 3> sized{};
        for (std::size_t kind = 0; kind < 3; ++kind) sized[kind] = sampled[kind] >= 20 && agreed[kind] == sampled[kind];

        std::map<Place, std::pair<const Entry*, std::string>> stored;
        std::size_t listed{};
        for (const auto& toc : tocs)
            for (const auto& bundle : toc.bundles) {
                if (bundle.at.size() != bundle.assets.size()) {
                    std::set<std::string> names;
                    std::string example;
                    for (const auto& entry : bundle.assets)
                        if (!names.insert(key_of(*entry.asset)).second && example.empty()) example = name_of(*entry.asset);
                    fail("asset list", toc.relative + ": " + bundle.name + " lists an asset twice, e.g. " + example);
                }
                for (const auto& entry : bundle.assets) {
                    ++listed;
                    if (const auto where = place(1, entry.file); where.layer == 1) stored.try_emplace(where, &entry, toc.relative + ": " + bundle.name);
                }
            }
        std::cout << listed << " asset(s) listed across the patch; " << stored.size() << " payload(s) are stored in the patch's own archives\n";
        progress("decoding " + std::to_string(stored.size()) + " payloads");
        std::ofstream written;
        if (!listing.empty()) written.open(listing, std::ios::binary);
        std::array<std::size_t, 3> decoded{};
        std::size_t bytesIn{}, bytesOut{}, unparsed{};
        for (const auto& [where, what] : stored) {
            const auto& [entry, from] = what;
            const auto kind = static_cast<std::size_t>(entry->asset->kind);
            try {
                const auto bytes = payload(where);
                ++decoded[kind];
                bytesIn += where.size;
                bytesOut += bytes.size();
                if (sized[kind] && bytes.size() != entry->asset->originalSize)
                    fail("payload", from + ": " + name_of(*entry->asset) + " decodes to " + std::to_string(bytes.size()) +
                         " bytes, its asset list says " + std::to_string(entry->asset->originalSize));
                if (entry->asset->kind == fb::AssetKind::ebx) {
                    auto result = facts_of(bytes);
                    if (!result.parsed) { ++unparsed; fail("payload", from + ": " + name_of(*entry->asset) + " is not a document the merge can read"); }
                    facts.emplace(where, std::move(result));
                }
                if (written.is_open()) written << from << '\t' << name_of(*entry->asset) << '\t' << bytes.size() << '\t' << hex(sha1_of(bytes)) << '\n';
            } catch (const std::exception& failure) {
                fail("payload", from + ": " + name_of(*entry->asset) + " at " + store->describe({true, where.chunk, where.archive}) + "+" +
                     std::to_string(where.offset) + " cannot be decoded (" + failure.what() + ")");
            }
        }
        std::cout << decoded[0] << " ebx, " << decoded[1] << " res and " << decoded[2] << " chunk payload(s) decoded: " << bytesIn / (1024 * 1024)
                  << " MB stored, " << bytesOut / (1024 * 1024) << " MB decoded; " << unparsed << " EBX do not parse\n";
    }

    // ---------------------------------------------------------------- every mod against the patch
    void mods_against_patch() {
        section("Every mod against the patch");
        std::map<std::string, Contested> found;   // by bundle name, asset and what the patch has
        std::size_t bundles{}, assets{}, chunks{}, byName{};
        for (std::size_t index = 0; index < catalog.mods.size(); ++index) {
            const auto& mod = catalog.mods[index];
            if (!mod.provides_layout) continue;
            if (index % 20 == 0) progress("mod " + std::to_string(index + 1) + " of " + std::to_string(catalog.mods.size()));
            for (const auto& relative : scan(mod.directory).tocs) {
                const auto merged = tocAt.find(lower(relative));
                if (merged == tocAt.end()) { fail("mod content", mod.name + ": the patch has no " + relative); continue; }
                const auto& toc = tocs[merged->second];
                fb::TocDocument own;
                try { own = fb::read_toc(read_file(mod.directory / fs::path(relative))); }
                catch (const std::exception& failure) { fail("mod content", mod.name + ": " + relative + " cannot be read (" + failure.what() + ")"); continue; }
                for (const auto& chunk : own.chunks) {
                    if (chunk.removed) continue;
                    ++chunks;
                    if (!toc.chunks.contains(chunk.guid))
                        fail("mod content", mod.name + ": " + relative + ": its chunk " + chunk.guid.string() + " is not in the patch's TOC");
                }
                for (const auto& bundle : own.bundles) {
                    ++bundles;
                    const auto at = toc.bundleAt.find(lower(bundle.name));
                    if (at == toc.bundleAt.end()) { fail("mod content", mod.name + ": " + relative + ": its bundle " + bundle.name + " is not in the patch"); continue; }
                    const auto& patched = toc.bundles[at->second];
                    const auto mine = read_bundle(static_cast<int>(index + 2), bundle);
                    if (!mine.listing || !patched.listing) { ++byName; continue; }
                    const auto* game = game_bundle(bundle.name);
                    chunkBundles.emplace(merged->second, at->second);
                    if (mine.listing->manifest.chunkMetadata == patched.listing->manifest.chunkMetadata &&
                        std::ranges::equal(mine.listing->manifest.chunks, patched.listing->manifest.chunks, {},
                                           &fb::BundleAsset::guid, &fb::BundleAsset::guid))
                        chunkBundlesAsShipped.emplace(merged->second, at->second);
                    for (const auto& entry : mine.assets) {
                        ++assets;
                        const auto key = key_of(*entry.asset);
                        const auto there = patched.at.find(key);
                        if (there == patched.at.end()) {
                            fail("mod content", mod.name + ": " + relative + ": " + bundle.name + ": its " + name_of(*entry.asset) + " is not in the patch's copy");
                            continue;
                        }
                        const auto& kept = patched.assets[there->second];
                        const Entry* shipped{};
                        if (game) if (const auto it = game->at.find(key); it != game->at.end()) shipped = &game->assets[it->second];
                        // The game's copy carried along is not this mod's content.
                        if (shipped && shipped->asset->sha1 == entry.asset->sha1) continue;
                        auto& record = found[lower(bundle.name) + '|' + key + '|' + hex(kept.asset->sha1)];
                        if (record.copies.empty()) {
                            record.bundle = bundle.name;
                            record.name = entry.asset->name;
                            record.kind = entry.asset->kind;
                            record.shipped = shipped != nullptr;
                            if (shipped) { record.game = shipped->asset->sha1; record.gamePlace = place(0, shipped->file); }
                            record.merged = kept.asset->sha1;
                            record.mergedPlace = place(1, kept.file);
                        }
                        if (std::ranges::none_of(record.copies, [&](const Copy& copy) { return copy.mod == index && copy.sha1 == entry.asset->sha1; }))
                            record.copies.push_back({index, entry.asset->sha1, entry.file});
                    }
                }
            }
        }
        std::cout << bundles << " mod bundle(s), " << assets << " listed asset(s) and " << chunks << " TOC chunk(s) looked up in the patch; "
                  << byName << " bundle(s) could only be checked by name\n";
        for (auto& [key, record] : found) contested.push_back(std::move(record));
    }

    // ---------------------------------------------------------------- what became of each mod's own content
    void outcomes() {
        section("What became of each mod's own content");
        struct Clash { const Contested* asset; std::vector<std::size_t> kept, lost; };
        std::vector<Clash> clashes, overridden;
        std::vector<const Contested*> written;
        std::size_t asShipped{}, rewritten{};
        for (const auto& record : contested) {
            std::set<fb::Sha1> versions;
            for (const auto& copy : record.copies) versions.insert(copy.sha1);
            const bool one = versions.contains(record.merged);
            if (one && versions.size() == 1) { ++asShipped; continue; }
            if (record.shipped && record.merged == record.game) {
                fail("lost edit", record.bundle + ": " + record.name + ": " + std::to_string(record.copies.size()) + " mod(s) change it (e.g. " +
                     catalog.mods[record.copies.front().mod].name + ") and the patch has the game's own copy");
                continue;
            }
            if (!one) {
                // No one mod's copy: the merge wrote it (edits combined, a change carried in).
                if (record.kind == fb::AssetKind::ebx) written.push_back(&record); else ++rewritten;
                continue;
            }
            Clash clash{&record, {}, {}};
            for (const auto& copy : record.copies) (copy.sha1 == record.merged ? clash.kept : clash.lost).push_back(copy.mod);
            (record.shipped ? overridden : clashes).push_back(std::move(clash));
        }
        std::cout << asShipped << " asset(s) are in the patch as their one mod ships them; the merge wrote " << written.size()
                  << " EBX document(s) and " << rewritten << " other asset(s) itself\n";

        std::cout << "\nassets two mods add under one name, with different content: " << clashes.size() << '\n';
        std::map<std::string, std::vector<const Clash*>> pairs;
        for (const auto& clash : clashes) pairs[mod_names(clash.lost) + " lost them to " + mod_names(clash.kept)].push_back(&clash);
        for (const auto& [pair, list] : pairs)
            std::cout << "   " << pair << ": " << list.size() << ", e.g. " << list.front()->asset->name << " in " << list.front()->asset->bundle << '\n';

        std::cout << "assets of the game two mods change, where the patch has one mod's copy whole: " << overridden.size() << '\n';
        std::size_t printed{};
        for (const auto& clash : overridden) {
            if (++printed > 40) { std::cout << "   ... and " << overridden.size() - 40 << " more\n"; break; }
            std::cout << "   " << kind_name(clash.asset->kind) << ' ' << clash.asset->name << " (" << clash.asset->bundle << "): kept "
                      << mod_names(clash.kept) << "; not in it: " << mod_names(clash.lost) << '\n';
        }

        // A reference is by the guid of the document it names. Where two mods' assets shared a
        // name and one went, whatever still names the one that went finds nothing.
        section("References to documents that lost their name to another mod");
        struct Gone { std::string mod, name, keptBy; };
        std::map<fb::Guid, Gone> gone;
        for (const auto& clash : clashes) {
            if (clash.asset->kind != fb::AssetKind::ebx) continue;
            const auto& kept = ebx(clash.asset->mergedPlace);
            for (const auto& copy : clash.asset->copies) {
                if (copy.sha1 == clash.asset->merged) continue;
                const auto& lost = ebx(place(static_cast<int>(copy.mod + 2), copy.file));
                // The same guid under both copies still resolves, to the copy that stayed.
                if (lost.parsed && kept.parsed && lost.file != kept.file)
                    gone.emplace(lost.file, Gone{catalog.mods[copy.mod].name, clash.asset->name, mod_names(clash.kept)});
            }
        }
        std::map<std::string, std::pair<std::size_t, std::string>> holders;
        std::size_t references{};
        for (const auto& toc : tocs)
            for (const auto& bundle : toc.bundles)
                for (const auto& entry : bundle.assets) {
                    if (entry.asset->kind != fb::AssetKind::ebx) continue;
                    const auto where = place(1, entry.file);
                    if (where.layer != 1) continue;
                    for (const auto& import : ebx(where).imports) {
                        const auto lost = gone.find(import.fileGuid);
                        if (lost == gone.end()) continue;
                        ++references;
                        auto& [count, example] = holders[bundle.name + ": " + entry.asset->name + " names " + lost->second.mod + "'s " +
                                                         lost->second.name + ", which " + lost->second.keptBy + "'s replaced"];
                        if (!count++) example = toc.relative;
                    }
                }
        for (const auto& [text, where] : holders)
            fail("dangling reference", text + " (" + std::to_string(where.first) + " cop" + (where.first == 1 ? "y" : "ies") + ", e.g. in " + where.second + ")");
        std::cout << gone.size() << " document(s) went with a guid nothing in the patch has; " << references << " reference(s) in the patch still name one\n";

        section("Documents the merge wrote");
        std::size_t checked{}, lists{}, kept{}, inPlace{}, valueEdits{}, reshaped{};
        for (const auto* record : written) {
            fb::ebx::Document merged;
            try { merged = fb::ebx::read_document(payload(record->mergedPlace)); }
            catch (const std::exception& failure) { fail("merged document", record->bundle + ": " + record->name + " cannot be read back (" + failure.what() + ")"); continue; }
            ++checked;
            const auto label = record->bundle + ": " + record->name + " (" + merged.rootType + ")";
            // The engine binary-searches the exported instances after the root by guid, and
            // internal ones have to follow them.
            bool internal{};
            const fb::Guid* previous{};
            for (std::size_t index = 1; index < merged.instances.size(); ++index) {
                const auto& instance = merged.instances[index];
                if (!instance.exported) { internal = true; continue; }
                if (internal) { fail("merged document", label + ": an exported instance follows an internal one"); break; }
                if (previous && !(*previous < instance.instanceGuid)) { fail("merged document", label + ": its exported instances are not in guid order"); break; }
                previous = &instance.instanceGuid;
            }
            std::size_t stray{};
            for (const auto& instance : merged.instances)
                if (instance.object)
                    for (const auto& field : instance.object->fields)
                        walk(field.value, [&](const fb::ebx::PointerReference& pointer) {
                            const auto limit = pointer.kind == fb::ebx::PointerKind::internal ? merged.instances.size()
                                : pointer.kind == fb::ebx::PointerKind::external ? merged.imports.size() : std::size_t{};
                            stray += pointer.kind != fb::ebx::PointerKind::null && (pointer.index < 0 || static_cast<std::size_t>(pointer.index) >= limit);
                        });
            if (stray) fail("merged document", label + ": " + std::to_string(stray) + " reference(s) index past the instance or import table");
            if (!record->shipped) continue;
            fb::ebx::Document game;
            try { game = fb::ebx::read_document(payload(record->gamePlace)); } catch (const std::exception&) { continue; }
            if (!merged.root() || !merged.root()->object || !game.root() || !game.root()->object) continue;
            // Every reference a mod appended to one of the root's lists is in the merged list,
            // unless what it names lost its name to another mod's asset.
            std::set<fb::Sha1> done;
            for (const auto& copy : record->copies) {
                if (!done.insert(copy.sha1).second) continue;
                fb::ebx::Document edit;
                try { edit = fb::ebx::read_document(payload(place(static_cast<int>(copy.mod + 2), copy.file))); } catch (const std::exception&) { continue; }
                if (!edit.root() || !edit.root()->object) continue;
                for (const auto& field : edit.root()->object->fields) {
                    const auto* added = std::get_if<fb::ebx::Value::Array>(&field.value.data);
                    const auto* before = game.root()->object->find(field.name);
                    const auto* after = merged.root()->object->find(field.name);
                    const auto* beforeList = before ? std::get_if<fb::ebx::Value::Array>(&before->value.data) : nullptr;
                    const auto* afterList = after ? std::get_if<fb::ebx::Value::Array>(&after->value.data) : nullptr;
                    if (!added || !beforeList || !afterList || added->size() <= beforeList->size()) continue;
                    ++lists;
                    std::set<std::string> present;
                    for (const auto& value : *afterList) present.insert(identity(merged, value));
                    for (auto at = beforeList->size(); at < added->size(); ++at) {
                        const auto id = identity(edit, (*added)[at]);
                        if (id.empty()) continue;
                        if (present.contains(id)) { ++kept; continue; }
                        if (const auto* target = named(edit, (*added)[at]); target && gone.contains(*target)) continue;
                        fail("merged list", label + ": " + field.name + " lacks an entry " + catalog.mods[copy.mod].name + " added (" + id + ")");
                    }
                }
            }

            // Values changed in place. What the merged document has that the game's does not is
            // some mod's value, and a mod that changed values and nothing else has every one of
            // them changed in the merged document: to its own, or to another mod's that changed
            // the same one.
            const auto before = flatten(game), after = flatten(merged);
            // An internal instance is known by its place alone, which says nothing once one was
            // added or taken away.
            const auto told = [&](const Flat& other, const std::string& key) {
                return !key.starts_with("n:") || other.internal == before.internal;
            };
            std::vector<std::pair<std::size_t, Flat>> edits;
            done.clear();
            for (const auto& copy : record->copies) {
                if (!done.insert(copy.sha1).second) continue;
                try { edits.emplace_back(copy.mod, flatten(fb::ebx::read_document(payload(place(static_cast<int>(copy.mod + 2), copy.file))))); }
                catch (const std::exception&) {}
            }
            std::size_t invented{};
            std::string example;
            for (const auto& [key, value] : before.values) {
                if (!plain(value) || !told(after, key)) continue;
                const auto now = after.values.find(key);
                if (now == after.values.end() || now->second == value) continue;
                ++inPlace;
                if (std::ranges::none_of(edits, [&](const std::pair<std::size_t, Flat>& edit) {
                        const auto theirs = edit.second.values.find(key);
                        return told(edit.second, key) && theirs != edit.second.values.end() && theirs->second == now->second; }))
                    if (!invented++) example = key;
            }
            if (invented)
                fail("merged value", label + ": " + std::to_string(invented) + " value(s) are neither the game's nor any mod's, e.g. " + example);
            for (const auto& [mod, edit] : edits) {
                bool shape{};   // a reference moved, a list resized, something of the game's gone
                std::vector<const std::string*> changed;
                for (const auto& [key, value] : before.values) {
                    if (!told(edit, key)) continue;
                    const auto theirs = edit.values.find(key);
                    if (theirs == edit.values.end()) { shape = true; continue; }
                    if (theirs->second == value) continue;
                    if (plain(value) && plain(theirs->second)) { changed.push_back(&key); continue; }
                    // A list on the root that grew at its end is the one change of shape that
                    // is combined, and is checked above.
                    const bool rootList = key.starts_with("root/") && key.ends_with("/#") && std::ranges::count(key, '/') == 2;
                    if (!(rootList && std::stoull(theirs->second.substr(1)) > std::stoull(value.substr(1)))) shape = true;
                }
                if (changed.empty()) continue;
                if (shape) { ++reshaped; continue; }
                std::size_t lost{};
                for (const auto* key : changed) {
                    const auto now = after.values.find(*key);
                    if (told(after, *key) && (now == after.values.end() || now->second == before.values.at(*key)) && !lost++) example = *key;
                }
                if (lost)
                    fail("lost value", label + ": " + std::to_string(lost) + " of the " + std::to_string(changed.size()) + " value(s) " +
                         catalog.mods[mod].name + " changed are the game's again, e.g. " + example);
                ++valueEdits;
            }
        }
        std::cout << checked << " document(s) read back; " << lists << " list(s) a mod appended to, " << kept << " appended reference(s) found in them\n";
        std::cout << inPlace << " value(s) in them differ from the game's, each one some mod's; " << valueEdits
                  << " mod copy(ies) that changed values alone have them all in; " << reshaped
                  << " that also moved a reference or resized a list are not combined that way\n";
    }

    // ---------------------------------------------------------------- lists and what they name
    // The game keeps what some lists name in the list's own bundle (an item list and its items,
    // the music playlist and its songs), and reads such a list without checking an entry. So in
    // every copy of that bundle the patch carries, the entries mods added have to be there too.
    void lists() {
        section("Lists whose entries the game keeps beside them");
        struct Shipped { fb::ebx::Document document; std::set<fb::Guid> held; bool unsure{}; };
        std::map<std::string, std::optional<Shipped>> shipped;   // by bundle and asset key
        std::map<std::string, std::pair<std::size_t, std::string>> missing, twice;
        std::size_t copies{}, entries{};
        for (const auto& toc : tocs)
            for (const auto& bundle : toc.bundles) {
                const auto* game = game_bundle(bundle.name);
                if (!game) continue;
                std::optional<std::set<fb::Guid>> here;
                bool unsure{};
                for (const auto& entry : bundle.assets) {
                    if (entry.asset->kind != fb::AssetKind::ebx) continue;
                    const auto where = place(1, entry.file);
                    const auto key = key_of(*entry.asset);
                    const auto original = game->at.find(key);
                    if (where.layer != 1 || original == game->at.end()) continue;
                    auto& known = shipped[lower(bundle.name) + '|' + key];
                    if (!known) {
                        Shipped value;
                        try { value.document = fb::ebx::read_document(payload(place(0, game->assets[original->second].file))); } catch (const std::exception&) {}
                        known = std::move(value);
                    }
                    const auto* gameRoot = known->document.root();
                    if (!gameRoot || !gameRoot->object) continue;
                    // Only a document with a list of references to other documents is read at all.
                    const auto lists_references = std::ranges::any_of(gameRoot->object->fields, [&](const fb::ebx::FieldValue& field) {
                        const auto* list = std::get_if<fb::ebx::Value::Array>(&field.value.data);
                        return list && !list->empty() && named(known->document, list->front());
                    });
                    if (!lists_references) continue;
                    fb::ebx::Document merged;
                    try { merged = fb::ebx::read_document(payload(where)); } catch (const std::exception&) { continue; }
                    if (!merged.root() || !merged.root()->object) continue;
                    for (const auto& field : merged.root()->object->fields) {
                        const auto* list = std::get_if<fb::ebx::Value::Array>(&field.value.data);
                        const auto* before = gameRoot->object->find(field.name);
                        const auto* beforeList = before ? std::get_if<fb::ebx::Value::Array>(&before->value.data) : nullptr;
                        if (!list || !beforeList || beforeList->empty() || list->size() <= beforeList->size()) continue;
                        // Does the game keep every one of its own entries in this bundle?
                        if (known->held.empty() && !known->unsure) known->held = held(0, *game, known->unsure);
                        if (known->unsure || !std::ranges::all_of(*beforeList, [&](const fb::ebx::Value& value) {
                                const auto* target = named(known->document, value);
                                return target && known->held.contains(*target); })) continue;
                        if (!here) here = held(1, bundle, unsure);
                        if (unsure) continue;
                        ++copies;
                        std::set<fb::Guid> seen;
                        std::size_t absent{}, repeated{};
                        for (std::size_t index = 0; index < list->size(); ++index) {
                            const auto* target = named(merged, (*list)[index]);
                            if (!target) { if (index >= beforeList->size()) ++absent; continue; }
                            if (index < beforeList->size()) continue;
                            ++entries;
                            absent += !here->contains(*target);
                            repeated += !seen.insert(*target).second;
                        }
                        const auto what = bundle.name + ": " + field.name + " of " + entry.asset->name;
                        if (absent) { auto& [count, example] = missing[what + ": " + std::to_string(absent) + " of " + std::to_string(list->size() - beforeList->size()) +
                                                                       " added entr" + (absent == 1 ? "y names" : "ies name") + " a document that bundle does not hold"];
                                      if (!count++) example = toc.relative; }
                        if (repeated) { auto& [count, example] = twice[what + ": " + std::to_string(repeated) + " added entr" + (repeated == 1 ? "y repeats" : "ies repeat") + " one a mod already added"];
                                        if (!count++) example = toc.relative; }
                    }
                }
            }
        for (const auto& [text, where] : missing)
            fail("list entry", text + " (" + std::to_string(where.first) + " cop" + (where.first == 1 ? "y" : "ies") + ", e.g. in " + where.second + ")");
        for (const auto& [text, where] : twice)
            fail("list entry", text + " (" + std::to_string(where.first) + " cop" + (where.first == 1 ? "y" : "ies") + ", e.g. in " + where.second + ")");
        std::cout << copies << " such list(s) with entries mods added, across every copy in the patch; " << entries << " added entries checked\n";
    }

    // ---------------------------------------------------------------- chunk metadata
    // The engine reads a chunk's record before it streams the chunk. The game writes a bundle's
    // list with a record for each chunk, in the order of the chunks' guids, so a texture's record
    // (the hash of its name, and its first mip) is at its chunk's place in that order. A bundle
    // the patch has from more than the game's copy (two mods' costumes built on one game costume,
    // each with chunks of its own) has to be in that form as well: with fewer records than
    // chunks, or with records where the mods' own lists have them, textures and meshes get
    // another's record or none and never appear. Checked wherever the game's own copy of the
    // bundle is in that form, which is everywhere it has been looked at.
    void chunk_metadata() {
        section("Chunk metadata");
        std::size_t bundles{}, untouched{}, asShipped{}, checked{}, textures{}, unknownForm{};
        for (const auto& where : chunkBundles) {
            const auto& toc = tocs[where.first];
            const auto& patched = toc.bundles[where.second];
            const auto& kept = patched.listing->manifest.chunks;
            if (kept.empty()) continue;
            ++bundles;
            const auto what = toc.relative + ": " + patched.name;
            const auto* game = game_bundle(patched.name);
            // The game's own list on the game's own chunks is what the game shipped.
            if (game && game->listing->manifest.chunkMetadata == patched.listing->manifest.chunkMetadata &&
                game->listing->manifest.chunks.size() == kept.size() &&
                std::ranges::equal(game->listing->manifest.chunks, kept, {}, &fb::BundleAsset::guid, &fb::BundleAsset::guid)) {
                ++untouched;
                continue;
            }
            // One mod's copy passed through whole is that mod's to have right: its tool's list is
            // not in the game's form, and the merge does not rewrite what only one mod ships.
            if (chunkBundlesAsShipped.contains(where)) { ++asShipped; continue; }
            const auto records = records_of(patched);
            if (!records) { fail("chunk metadata", what + ": the patch's list cannot be read"); continue; }
            if (records->size() != kept.size() && !(records->empty() && (!game || game->listing->manifest.chunkMetadata.empty()))) {
                fail("chunk metadata", what + ": " + std::to_string(records->size()) + " record(s) for " +
                    std::to_string(kept.size()) + " chunk(s)");
                continue;
            }
            // Only a bundle whose own copy in the game is in guid order says what the patch's has to be.
            if (!game) { ++unknownForm; continue; }
            const auto shipped = records_of(*game);
            if (!shipped) { ++unknownForm; continue; }
            const auto original = texture_records(0, *game, *shipped);
            if (original.misplaced || original.mips) { ++unknownForm; continue; }
            ++checked;
            const auto merged = texture_records(1, patched, *records);
            textures += merged.textures;
            if (merged.misplaced || merged.mips)
                fail("chunk metadata", what + ": of " + std::to_string(merged.textures) + " texture(s) with a chunk in the bundle, " +
                    std::to_string(merged.misplaced) + " do not have their own record at their chunk's place and " +
                    std::to_string(merged.mips) + " have another first mip than their header, e.g. " + merged.example);
        }
        std::cout << bundles << " bundle(s) mods ship that hold chunks: " << untouched << " with the game's own list on the game's own chunks, "
                  << asShipped << " exactly as one mod ships them, " << checked << " others checked, " << textures
                  << " texture record(s) in them; " << unknownForm << " whose own copy in the game gives nothing to check against\n";
    }

    int summary() const {
        section("Summary");
        std::size_t total{};
        for (const auto& [kind, count] : failures) { std::cout << count << " x " << kind << '\n'; total += count; }
        std::cout << (total ? "mod merge audit: " + std::to_string(total) + " problem(s)" : std::string("mod merge audit: ok")) << '\n';
        return total ? 1 : 0;
    }
};
} // namespace

int main(int argc, char** argv) try {
    if (argc < 2 || !fs::exists(fs::path(argv[1]) / "Data") || !fs::exists(fs::path(argv[1]) / "Mods")) {
        std::cout << "No data root with Data and Mods given; skipped.\n";
        return 0;
    }
    Audit audit;
    audit.root = argv[1];
    if (!audit.merge()) return audit.summary();
    audit.load();
    audit.placements();
    audit.payloads(argc > 2 ? fs::path(argv[2]) : fs::path{});
    audit.mods_against_patch();
    audit.outcomes();
    audit.lists();
    audit.chunk_metadata();
    return audit.summary();
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
