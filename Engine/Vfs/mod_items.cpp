#include "mod_items.h"
#include "mod_merge_internal.h"
#include "mod_store_copies.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace dingosdk::mods {

namespace {
using namespace detail;

// One copy of a bundle the mod ships, kept while its items' thumbnails are read from it.
struct Copy {
    Listing listing;
    std::map<std::string, std::size_t, std::less<>> textures;   // lower-case name -> its place among the resources
};
// A document the mod added: what an item's thumbnail is found through.
struct Added {
    std::string name, type, item;
    std::vector<fb::Guid> imports;
};

// An item names its thumbnails by import: a cdn texture each (the grid's and a large one), which
// imports the texture itself. The smallest that is at least `side` across is the one decoded; the
// pixels are in the texture's bundle or among the chunks the mod's superbundle lists.
fb::Image thumbnail(const CasStore& store, const fs::path& directory, const fs::path& baseRoot, const fs::path& gameRoot,
                    const Added& item, const std::map<fb::Guid, Added>& added, const std::vector<std::unique_ptr<Copy>>& copies,
                    const std::map<fb::Guid, fb::TocChunk>& chunks, std::uint32_t side) {
    struct Candidate {
        const Copy* copy{};
        std::size_t resource{};
        fb::TextureHeader header;
    };
    std::vector<Candidate> candidates;
    const auto texture = [&](const std::string& name) {
        for (const auto& copy : copies) {
            const auto found = copy->textures.find(name);
            if (found == copy->textures.end()) continue;
            try {
                const auto& manifest = copy->listing.manifest;
                const auto bytes = read_asset(store, directory, baseRoot, copy->listing, manifest.ebx.size() + found->second, gameRoot);
                candidates.push_back({copy.get(), found->second, fb::read_texture_header(bytes, manifest.resources[found->second].resourceMeta)});
            } catch (const std::exception&) {}
            return;
        }
    };
    for (const auto& first : item.imports) {
        const auto cdn = added.find(first);
        if (cdn == added.end()) continue;
        if (cdn->second.type == "textureasset") texture(cdn->second.name);
        for (const auto& second : cdn->second.imports)
            if (const auto held = added.find(second); held != added.end() && held->second.type == "textureasset") texture(held->second.name);
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        const bool enoughA = a.header.width >= side, enoughB = b.header.width >= side;
        if (enoughA != enoughB) return enoughA;
        return enoughA ? a.header.width < b.header.width : a.header.width > b.header.width;
    });
    for (const auto& candidate : candidates) try {
        const auto& manifest = candidate.copy->listing.manifest;
        std::vector<std::byte> pixels;
        const auto inBundle = std::find_if(manifest.chunks.begin(), manifest.chunks.end(),
            [&](const fb::BundleAsset& chunk) { return chunk.guid == candidate.header.chunk; });
        if (inBundle != manifest.chunks.end()) {
            pixels = read_asset(store, directory, baseRoot, candidate.copy->listing,
                                manifest.ebx.size() + manifest.resources.size() + static_cast<std::size_t>(inBundle - manifest.chunks.begin()), gameRoot);
        } else {
            const auto listed = chunks.find(candidate.header.chunk);
            if (listed == chunks.end()) continue;
            const auto& chunk = listed->second;
            pixels = fb::decode_cas(store.read(chunk.location.patch ? directory : baseRoot, chunk.location, chunk.offset, chunk.size), {gameRoot});
        }
        return fb::resize(fb::decode_texture(candidate.header, pixels, side), side, side);
    } catch (const std::exception&) {}
    return {};
}

} // namespace

ModItems read_mod_items(const std::filesystem::path& data_root, const std::filesystem::path& directory, std::uint32_t side) noexcept {
    ModItems result;
    try {
        const auto baseRoot = data_root / L"Data";
        const auto& gameRoot = data_root;
        const auto layout = vfs::read_layout(baseRoot / L"layout.toc");
        // (Nothing is written: the store only reads here.)
        const CasStore store(baseRoot, fs::temp_directory_path() / L"reskate-mod-items-unused", layout.root);
        std::set<std::string, std::less<>> seen;
        for (const auto& relative : scan(directory).tocs) {
            std::error_code error;
            const auto baseToc = baseRoot / fs::path(relative);
            // An item only loads from a bundle the game loads (mod_store_copies.cpp read_mod).
            if (!fs::is_regular_file(baseToc, error)) continue;
            const auto own = fb::read_toc(read_file(directory / fs::path(relative)));
            const auto game = fb::read_toc(read_file(baseToc));
            std::map<std::string, const fb::TocBundle*, std::less<>> shipped;
            for (const auto& bundle : game.bundles) shipped.emplace(ascii_lower(bundle.name), &bundle);
            std::map<fb::Guid, fb::TocChunk> chunks;
            for (const auto& chunk : own.chunks)
                if (!chunk.removed) chunks.emplace(chunk.guid, chunk);

            std::vector<std::unique_ptr<Copy>> copies;
            std::map<fb::Guid, Added> added;
            std::vector<fb::Guid> items;
            for (const auto& bundle : own.bundles) try {
                const auto original = shipped.find(ascii_lower(bundle.name));
                if (original == shipped.end()) continue;
                const auto region = fb::read_bundle_region(bundle.region);
                if (std::none_of(region.files.begin(), region.files.end(), [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                    continue;
                auto listing = list_bundle(store, directory, baseRoot, bundle, gameRoot);
                const auto gameListing = list_bundle(store, baseRoot, baseRoot, *original->second, gameRoot);
                if (!listing || !gameListing) continue;
                std::set<std::string, std::less<>> known;
                for (const auto& asset : gameListing->manifest.ebx) known.insert(ascii_lower(asset.name));
                auto copy = std::make_unique<Copy>();
                copy->listing = std::move(*listing);
                const auto& manifest = copy->listing.manifest;
                for (std::size_t index = 0; index < manifest.resources.size(); ++index)
                    if (manifest.resources[index].resourceType == fb::texture_resource_type)
                        copy->textures.emplace(ascii_lower(manifest.resources[index].name), index);
                for (std::size_t index = 0; index < manifest.ebx.size(); ++index) {
                    const auto name = ascii_lower(manifest.ebx[index].name);
                    const auto at = copy->listing.first + index;
                    if (known.contains(name) || at >= copy->listing.files.size() || !copy->listing.files[at].location.patch) continue;
                    try {
                        const auto document = fb::ebx::read_document(read_asset(store, directory, baseRoot, copy->listing, index, gameRoot));
                        Added entry;
                        entry.name = name;
                        entry.type = ascii_lower(document.rootType);
                        for (const auto& reference : document.imports) entry.imports.push_back(reference.fileGuid);
                        if (const auto item = item_content(document)) {
                            entry.item = ascii_lower(item->key);
                            items.push_back(document.fileGuid);
                        }
                        added.emplace(document.fileGuid, std::move(entry));
                    } catch (const std::exception&) {}   // not an asset this reads
                }
                copies.push_back(std::move(copy));
            } catch (const std::exception&) {}   // a bundle that cannot be read lists no items; the merge says why
            for (const auto& guid : items) {
                const auto& entry = added.at(guid);
                if (entry.item.empty() || !seen.insert(entry.item).second) continue;
                ModItem item;
                item.key = entry.item;
                item.asset = entry.name;
                // "items/<category>/<name>"
                if (const auto last = entry.name.rfind('/'); last != std::string::npos && last > 0) {
                    const auto before = entry.name.rfind('/', last - 1);
                    item.category = entry.name.substr(before == std::string::npos ? 0 : before + 1,
                                                      last - (before == std::string::npos ? 0 : before + 1));
                }
                if (side) item.thumbnail = thumbnail(store, directory, baseRoot, gameRoot, entry, added, copies, chunks, side);
                result.items.push_back(std::move(item));
            }
        }
        std::sort(result.items.begin(), result.items.end(), [](const ModItem& a, const ModItem& b) {
            return a.category != b.category ? a.category < b.category : a.key < b.key;
        });
    } catch (const std::exception& failure) {
        result.problem = failure.what();
    } catch (...) {
        result.problem = "the mod could not be read";
    }
    return result;
}

} // namespace dingosdk::mods
