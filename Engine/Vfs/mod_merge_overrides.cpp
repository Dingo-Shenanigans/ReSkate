#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/ebx_writer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {

namespace {

// The game's side of what every mod is held against, read once for all of them: forty
// cosmetic mods each patch the same few bundles, the largest with some 17,000 assets, and
// each used to list that bundle again for itself.
struct GameBundle {
    std::optional<Listing> listing;
    // The game's copy of each asset: its sha1, and where in the listing it is.
    std::map<std::string, std::pair<fb::Sha1, std::size_t>, std::less<>> original;
    std::map<std::string, const fb::BundleAsset*, std::less<>> scripts;
    std::set<std::string, std::less<>> resources;
    std::set<fb::Guid> chunks;
};
struct GameToc {
    fb::TocDocument document;
    std::map<std::string, const fb::TocBundle*, std::less<>> bundles;
    std::set<fb::Guid> chunks;
};
// Made on first use by whichever thread asks first; the others wait for it. One that could
// not be read throws for each asker, as reading it for each did.
template <class Value> class Shared {
public:
    template <class Make> const Value& get(const std::string& key, Make&& make) {
        Entry* entry{};
        {
            std::lock_guard lock(mutex_);
            entry = &entries_[key];
        }
        std::call_once(entry->once, [&] { make(entry->value); });
        return entry->value;
    }

private:
    struct Entry {
        std::once_flag once;
        Value value;
    };
    std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> entries_;   // nodes stay where they are
};

// The game's assets a mod changed. They wait until its additions are settled: one that
// another mod's asset of the same name stands in for is not where a copy can find it, and a
// change must not name it there.
struct Change {
    std::string bundle;                         // lower case: where the mod changed it
    std::string name;                           // lower case
    std::string asset;                          // as the bundle spells it
    fb::Sha1 game;                              // the game's copy it replaces
    fb::Sha1 sha1;
    std::uint64_t originalSize{};
    std::vector<std::byte> encoded;
    std::optional<fb::BundleFileInfo> gameFile; // where the game's copy is
};
struct Script {
    std::string name;   // lower case
    fb::Sha1 game;      // the game's copy it replaces
    std::optional<AssetOverride> value;   // none: it could not be read, and `note` says so
    std::size_t note{};                   // its place in the mod's notes
};
// What one mod's files say, read without regard to any other mod: the part of the work that
// is reading and decoding, done for several mods at once. What it means beside the mods
// above it is settled afterwards, one mod at a time in priority order.
struct ModRead {
    // This mod's new assets, and the partitions its recorded changes import.
    // An added asset only follows a change that refers to it: copied on its
    // own, it could arrive in another mod's bundle without the resources
    // and chunks it was added with.
    std::vector<std::pair<std::string, AssetAddition>> candidates, companions;
    struct BundleChunk { std::string bundle; AssetAddition addition; fb::BundleFileInfo file; bool added{}; };
    std::vector<BundleChunk> bundleChunks;
    std::map<std::string,AssetOverrides::ChunkDefaults,std::less<>> chunkDefaults;
    std::vector<bool> unreadable;                                    // per candidate: never follows
    std::map<std::string, std::set<fb::Guid>, std::less<>> addedIn;  // the documents it added to each bundle, by guid
    std::vector<fb::TocChunk> newChunks;   // TOC chunks the game's TOCs do not have
    std::set<fb::Guid> imported;
    std::vector<Change> changes;
    std::vector<Script> scripts;
    std::vector<std::string> notes;
    std::exception_ptr failure;
    // For the items a player left out of a mod (leave_out), read only when some mod has any: what
    // each candidate refers to, by its place among them, and what every other document of the mod
    // that could name one refers to (its changes to the game's assets, its own bundles).
    std::vector<AssetLinks> links, referrers;
    // Something of the mod's could not be read for that, so what it names is not known.
    bool unsure{};
    // The documents left out of the mod: its changed item lists do not name them in other mods' copies.
    std::set<fb::Guid> gone;
};

// `linked`: some mod has items left out (Mod::items_off), so what the documents refer to is kept too.
ModRead read_mod(const Mod& mod, const RelativeFiles& files, const CasStore& store, const fs::path& baseRoot,
                 const fs::path& gameRoot, Shared<GameToc>& gameTocs, Shared<GameBundle>& gameBundles, bool linked) {
    ModRead read;
    auto& [candidates, companions, bundleChunks, chunkDefaults, unreadable, addedIn, newChunks, imported, changes, scripts, notes, failure,
           links, referrers, unsure, gone] = read;
    const auto decoded = [&](std::span<const std::byte> encoded) {
        return fb::ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
    };
    // A bundle of the mod's own (the game ships none of that name): nothing in it is left out, but a
    // document in it may name what is. Read only for a mod with items left out.
    const auto refer = [&](const fb::TocBundle& bundle) {
        try {
            const auto region = fb::read_bundle_region(bundle.region);
            if (std::none_of(region.files.begin(), region.files.end(), [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                return;
            const auto listing = list_bundle(store, mod.directory, baseRoot, bundle, gameRoot);
            if (!listing) { unsure = true; return; }
            for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                const auto at = listing->first + index;
                if (at >= listing->files.size() || !listing->files[at].location.patch) continue;
                referrers.push_back(asset_links(fb::ebx::read_document(read_asset(store, mod.directory, baseRoot, *listing, index, gameRoot))));
            }
        } catch (const std::exception&) { unsure = true; }
    };
    const bool choosing = linked && !mod.items_off.empty();
    for (const auto& relative : files.tocs) {
        std::error_code error;
        const auto baseToc = baseRoot / fs::path(relative);
        if (!fs::is_regular_file(baseToc, error)) {
            if (choosing) try {
                for (const auto& bundle : fb::read_toc(read_file(mod.directory / fs::path(relative))).bundles) refer(bundle);
            } catch (const std::exception&) { unsure = true; }
            continue;
        }
        try {
            const auto own = fb::read_toc(read_file(mod.directory / fs::path(relative)));
            const auto tocKey = ascii_lower(relative);
            const auto& game = gameTocs.get(tocKey, [&](GameToc& toc) {
                toc.document = fb::read_toc(read_file(baseToc));
                for (const auto& bundle : toc.document.bundles) toc.bundles.emplace(ascii_lower(bundle.name), &bundle);
                for (const auto& chunk : toc.document.chunks) toc.chunks.insert(chunk.guid);
            });
            for (const auto& chunk : own.chunks)
                if (!chunk.removed && chunk.location.patch && !game.chunks.contains(chunk.guid)) newChunks.push_back(chunk);
            for (const auto& bundle : own.bundles) {
                const auto bundleName = ascii_lower(bundle.name);
                const auto shipped = game.bundles.find(bundleName);
                if (shipped == game.bundles.end()) {
                    if (choosing) refer(bundle);
                    continue;
                }
                const auto modListing = list_bundle(store, mod.directory, baseRoot, bundle, gameRoot);
                const auto& known = gameBundles.get(tocKey + '|' + bundleName, [&](GameBundle& made) {
                    made.listing = list_bundle(store, baseRoot, baseRoot, *shipped->second, gameRoot);
                    if (!made.listing) return;
                    const auto& manifest = made.listing->manifest;
                    for (std::size_t index = 0; index < manifest.ebx.size(); ++index)
                        made.original.emplace(ascii_lower(manifest.ebx[index].name), std::pair{manifest.ebx[index].sha1, index});
                    // Maps carry stock SkaterLoader too. Replacing it only in the game's
                    // bundles loses custom cosmetic material targets on a map transition.
                    // Other resource types retain their own table/dependency merge rules.
                    for (const auto& asset : manifest.resources) {
                        if (asset.resourceType == luaScriptResourceType) made.scripts.emplace(ascii_lower(asset.name), &asset);
                        made.resources.insert(ascii_lower(asset.name));
                    }
                    for (const auto& asset : manifest.chunks) made.chunks.insert(asset.guid);
                });
                if (!modListing || !known.listing) {
                    // (A copy that cannot be listed passes through whole or is left out whole.)
                    if (choosing && !modListing) unsure = true;
                    continue;
                }
                if (!known.listing->manifest.chunkMetadata.empty() && !chunkDefaults.contains(bundleName)) {
                    AssetOverrides::ChunkDefaults defaults;
                    defaults.metadata=known.listing->manifest.chunkMetadata;
                    for (const auto& entry:known.listing->manifest.chunks) defaults.chunks.push_back(entry.guid);
                    chunkDefaults.emplace(bundleName,std::move(defaults));
                }
                const auto& gameListing = known.listing;
                const auto& original = known.original;
                for (std::size_t index = 0; index < modListing->manifest.ebx.size(); ++index) {
                    const auto& asset = modListing->manifest.ebx[index];
                    const auto name = ascii_lower(asset.name);
                    const auto game_copy = original.find(name);
                    if (game_copy != original.end() && game_copy->second.first == asset.sha1) continue;
                    const auto at = modListing->first + index;
                    if (at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    // As the merge places it in the mod's own bundle: repaired when it would
                    // crash the game's load (repair_ebx_payload).
                    auto repairedAsset = asset;
                    const auto payload = [&] {
                        auto encoded = store.read(file.location.patch ? mod.directory : baseRoot,
                                                  file.location, file.offset, file.size);
                        if (file.location.patch) repair_ebx_payload(encoded, repairedAsset, gameRoot);
                        return encoded;
                    };
                    if (game_copy == original.end()) {
                        auto encodedPayload = payload();
                        candidates.push_back({bundleName, {mod.name, repairedAsset, std::move(encodedPayload), tocKey}});
                        continue;
                    }
                    auto encodedPayload = payload();
                    try {
                        const auto document = decoded(encodedPayload);
                        for (const auto& reference : document.imports)
                            imported.insert(reference.fileGuid);
                        if (linked) {
                            referrers.push_back(asset_links(document));
                            referrers.back().item_list = item_list_name(name);
                        }
                    } catch (const std::exception&) {
                        if (choosing) unsure = true;
                    }
                    std::optional<fb::BundleFileInfo> gameFile;
                    if (const auto gameAt = gameListing->first + game_copy->second.second;
                        gameAt < gameListing->files.size())
                        gameFile = gameListing->files[gameAt];
                    changes.push_back({bundleName, name, asset.name, game_copy->second.first, repairedAsset.sha1,
                                       repairedAsset.originalSize, std::move(encodedPayload), gameFile});
                }
                for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                    const auto& asset = modListing->manifest.resources[index];
                    if (asset.resourceType != luaScriptResourceType) continue;
                    const auto name = ascii_lower(asset.name);
                    const auto originalScript = known.scripts.find(name);
                    if (originalScript == known.scripts.end() || originalScript->second->sha1 == asset.sha1 ||
                        originalScript->second->resourceId != asset.resourceId) continue;
                    // (A higher mod's version of the script, or this mod's own from another
                    // bundle, wins when the mods are settled: read here either way.)
                    if (std::ranges::any_of(scripts, [&](const Script& have) {
                            return have.name == name && have.game == originalScript->second->sha1 && have.value; })) continue;
                    const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                    if (at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    try {
                        scripts.push_back({name, originalScript->second->sha1,
                            AssetOverride{mod.name, asset.sha1, asset.originalSize,
                                store.read(file.location.patch ? mod.directory : baseRoot,
                                           file.location, file.offset, file.size), asset}});
                    } catch (const std::exception& failed) {
                        // Read whether or not it will be wanted, so a failure is only this
                        // script's: where a higher mod's version stands, it is nobody's and
                        // the note goes (settled below). The rest of the TOC is read on.
                        notes.push_back(mod.name + ": " + relative +
                            ": its changes could not be read for other mods' copies (" + failed.what() + ")");
                        scripts.push_back({name, originalScript->second->sha1, std::nullopt, notes.size() - 1});
                    }
                }
                // Keep chunk locations, not their potentially large payloads. Only
                // data named by a retained mesh/texture is read and carried later.
                const auto& chunks = modListing->manifest.chunks;
                const auto chunkMetadata = chunks.empty() ? nullptr :
                    std::make_shared<const std::vector<std::byte>>(modListing->manifest.chunkMetadata);
                for (std::size_t index = 0; index < chunks.size(); ++index) {
                    const auto at = modListing->first + modListing->manifest.ebx.size() +
                        modListing->manifest.resources.size() + index;
                    if (at >= modListing->files.size()) continue;
                    AssetAddition addition{mod.name, chunks[index], {}, tocKey};
                    addition.asset.name = addition.asset.guid.string();
                    addition.chunk_metadata = chunkMetadata;
                    addition.chunk_index = index;
                    const bool added = !known.chunks.contains(chunks[index].guid);
                    bundleChunks.push_back({bundleName, std::move(addition), modListing->files[at], added});
                }
                // Resources the mod adds, kept to go with an added EBX of the same name
                // (a wave's sound-bank resource registers the wave with the audio system).
                for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                    const auto& asset = modListing->manifest.resources[index];
                    const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                    if (known.resources.contains(ascii_lower(asset.name)) || at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    companions.push_back({bundleName, {mod.name, asset,
                        store.read(file.location.patch ? mod.directory : baseRoot, file.location, file.offset, file.size), tocKey}});
                }
            }
        } catch (const std::exception& failed) {
            notes.push_back(mod.name + ": " + relative +
                ": its changes could not be read for other mods' copies (" + failed.what() + ")");
            if (choosing) unsure = true;
        }
    }
    // Each added document's guid, and the guids of the documents it refers to in turn.
    unreadable.assign(candidates.size(), false);
    if (linked) links.resize(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto& addition = candidates[index].second;
        try {
            const auto document = decoded(addition.encoded);
            addition.file = document.fileGuid;
            for (const auto& reference : document.imports) addition.names.push_back(reference.fileGuid);
            addedIn[candidates[index].first].insert(addition.file);
            // Assets the mod's plugin selects at run time (its world objects, HUD variants picked
            // by size) have no import naming them: its resident list keeps them, as imported
            // additions are kept, when a map replaces their bundle.
            const bool resident = resident_root(mod, candidates[index].first, addition.asset.name);
            if (resident) imported.insert(addition.file);
            if (linked) {
                links[index] = asset_links(document);
                links[index].name = ascii_lower(addition.asset.name);
                links[index].resident = resident;
            }
        } catch (const std::exception&) { unreadable[index] = true; }
    }
    return read;
}

// A mod's added chunks, found by their ids in an asset's bytes: as stored, or with the first three
// fields the other way round (a mesh names its chunks that way).
class ChunkNames final {
public:
    void add(const fb::Guid& id) {
        alias(id, id);
        auto swapped = id;
        std::reverse(swapped.bytes.begin(), swapped.bytes.begin() + 4);
        std::reverse(swapped.bytes.begin() + 4, swapped.bytes.begin() + 6);
        std::reverse(swapped.bytes.begin() + 6, swapped.bytes.begin() + 8);
        alias(swapped, id);
    }
    [[nodiscard]] bool empty() const noexcept { return aliases_.empty(); }
    void find(std::span<const std::byte> bytes, std::set<fb::Guid>& into) const {
        for (std::size_t at = 0; at + sizeof(fb::Guid) <= bytes.size(); ++at) {
            if (!starts_[std::to_integer<std::size_t>(bytes[at]) << 8 | std::to_integer<std::size_t>(bytes[at + 1])]) continue;
            fb::Guid key;
            std::memcpy(key.bytes.data(), bytes.data() + at, key.bytes.size());
            if (const auto found = aliases_.find(key); found != aliases_.end()) into.insert(found->second);
        }
    }

private:
    void alias(const fb::Guid& bytes, const fb::Guid& id) {
        aliases_.emplace(bytes, id);
        starts_[std::to_integer<std::size_t>(bytes.bytes[0]) << 8 | std::to_integer<std::size_t>(bytes.bytes[1])] = true;
    }
    std::map<fb::Guid, fb::Guid> aliases_;
    std::vector<bool> starts_ = std::vector<bool>(0x10000);
};

// The items the player left out of `mod` (Mod::items_off): they, and what only they use, are taken
// out of what was read of the mod, so nothing of them follows into other mods' copies, and are
// listed in `out` for the merge of the mod's own bundles (combine), which then reads each copy
// without them. What goes is decided by plan_left_out; a resource goes with the added document of
// its name unless something that stays names it by id, and an added chunk when an asset that goes
// names it and none that stays does. `others`: what every other document refers to. Anything that
// cannot be read leaves the mod whole: an item too many is better than an asset too few.
void leave_out(const Mod& mod, ModRead& read, std::span<const AssetLinks* const> others, const fs::path& gameRoot,
               AssetOverrides& out, MergeReport& report) {
    auto& [candidates, companions, bundleChunks, chunkDefaults, unreadable, addedIn, newChunks, imported, changes, scripts, notes, failure,
           links, referrers, unsure, gone] = read;
    const auto whole = [&](const std::string& why) {
        report.notes.push_back(mod.name + ": the items left out of it are loaded all the same: " + why);
    };
    if (unsure || std::find(unreadable.begin(), unreadable.end(), true) != unreadable.end())
        return whole("not all of the mod could be read to tell what they use");
    const auto plan = plan_left_out(links, others, mod.items_off);
    for (const auto& key : plan.stayed)
        report.notes.push_back(mod.name + ": " + key + " was left out of the mod but stays: another asset names it");
    if (std::find(plan.gone.begin(), plan.gone.end(), true) == plan.gone.end()) return;

    // The resources named by what stays, and the names of the documents that go.
    std::set<std::uint64_t> wanted;
    for (const auto* other : others)
        if (other) wanted.insert(other->resources.begin(), other->resources.end());
    std::set<std::pair<std::string, std::string>> goneNames;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (plan.gone[index]) goneNames.emplace(candidates[index].first, links[index].name);
        else wanted.insert(links[index].resources.begin(), links[index].resources.end());
    }
    std::vector<bool> goneResource(companions.size());
    for (std::size_t index = 0; index < companions.size(); ++index) {
        const auto& [bundle, companion] = companions[index];
        goneResource[index] = goneNames.contains({bundle, ascii_lower(companion.asset.name)}) && !wanted.contains(companion.asset.resourceId);
    }
    // The chunks the mod added that an asset that goes names, less those one that stays names.
    ChunkNames ids;
    for (const auto& chunk : bundleChunks)
        if (chunk.added) ids.add(chunk.addition.asset.guid);
    std::set<fb::Guid> namedGone, namedKept;
    if (!ids.empty()) try {
        for (std::size_t index = 0; index < candidates.size(); ++index)
            ids.find(fb::decode_cas(candidates[index].second.encoded, {gameRoot}), plan.gone[index] ? namedGone : namedKept);
        for (std::size_t index = 0; index < companions.size(); ++index)
            ids.find(fb::decode_cas(companions[index].second.encoded, {gameRoot}), goneResource[index] ? namedGone : namedKept);
        for (const auto& change : changes) ids.find(fb::decode_cas(change.encoded, {gameRoot}), namedKept);
    } catch (const std::exception& failed) {
        return whole(std::string("an asset could not be read to tell which chunks they use (") + failed.what() + ")");
    }

    // Decided; now taken out.
    auto& skipped = out.left_out[mod.name];
    auto& keys = out.left_out_items[mod.name];
    std::size_t documents{}, resources{}, chunks{};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!plan.gone[index]) continue;
        const auto& [bundle, addition] = candidates[index];
        skipped[bundle].insert(asset_key_text(addition.asset));
        unreadable[index] = true;   // never follows into another mod's copy
        if (const auto beside = addedIn.find(bundle); beside != addedIn.end()) beside->second.erase(addition.file);
        gone.insert(addition.file);
        out.left_out_documents.insert(addition.file);
        if (!links[index].item.empty()) keys.insert(links[index].item);
        ++documents;
    }
    for (std::size_t index = companions.size(); index-- > 0;) {
        if (!goneResource[index]) continue;
        skipped[companions[index].first].insert(asset_key_text(companions[index].second.asset));
        companions.erase(companions.begin() + static_cast<std::ptrdiff_t>(index));
        ++resources;
    }
    std::erase_if(bundleChunks, [&](const ModRead::BundleChunk& chunk) {
        const auto& id = chunk.addition.asset.guid;
        if (!chunk.added || !namedGone.contains(id) || namedKept.contains(id)) return false;
        auto listed = chunk.addition.asset;
        listed.kind = fb::AssetKind::chunk;
        skipped[chunk.bundle].insert(asset_key_text(listed));
        ++chunks;
        return true;
    });
    report.notes.push_back(mod.name + ": " + std::to_string(plan.items) + " item(s) left out of it, as the player chose, with what only they use: " +
        std::to_string(documents) + " document(s), " + std::to_string(resources) + " resource(s) and " + std::to_string(chunks) +
        " chunk(s) are not merged");
}

} // namespace

AssetOverrides collect_asset_overrides(const std::vector<const Mod*>& mods,
                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                       const CasStore& store, const fs::path& baseRoot,
                                       const fs::path& gameRoot, MergeReport& report,
                                       std::size_t threads, bool background) {
    AssetOverrides out;
    // Where in `out.added` each bundle's addition of a kind and (lower-case) name is: looked up
    // for every addition and companion, which a search of the list made quadratic.
    std::map<std::string, std::map<std::pair<fb::AssetKind, std::string>, std::size_t>, std::less<>> addedAt;

    // A map's edits to the game's assets serve its own levels; asset mods
    // (cameras, tuning, cosmetics) are the ones meant to apply everywhere.
    std::vector<std::pair<const Mod*, const RelativeFiles*>> asked;
    for (const auto* mod : mods) {
        if (mod->provides_levels) continue;
        if (const auto files = modFiles.find(mod); files != modFiles.end()) asked.emplace_back(mod, &files->second);
    }
    // Read: every mod's files, several mods at a time.
    Shared<GameToc> gameTocs;
    Shared<GameBundle> gameBundles;
    std::vector<ModRead> reads(asked.size());
    // Items a player left out of a mod (Mod::items_off): what every document refers to is read too.
    const bool linked = std::ranges::any_of(mods, [](const Mod* mod) { return !mod->items_off.empty(); });
    for_each_index(asked.size(), threads, background, [&](std::size_t mine) {
        try {
            reads[mine] = read_mod(*asked[mine].first, *asked[mine].second, store, baseRoot, gameRoot, gameTocs, gameBundles, linked);
        } catch (...) {
            reads[mine].failure = std::current_exception();
        }
    });
    if (linked) {
        for (const auto* mod : mods)
            if (!mod->items_off.empty() && std::ranges::none_of(asked, [&](const auto& one) { return one.first == mod; }))
                report.notes.push_back(mod->name + ": the items left out of it are loaded all the same: a mod that adds levels is merged whole");
        // Each such mod in turn, before any is settled: what a mod leaves out depends on what the
        // other mods' documents name, not on what follows whom.
        for (std::size_t slot = 0; slot < asked.size(); ++slot) {
            if (asked[slot].first->items_off.empty() || reads[slot].failure) continue;
            std::vector<const AssetLinks*> others;
            for (std::size_t other = 0; other < reads.size(); ++other) {
                if (reads[other].failure) continue;
                for (const auto& links : reads[other].referrers) others.push_back(&links);
                if (other != slot)
                    for (const auto& links : reads[other].links) others.push_back(&links);
            }
            leave_out(*asked[slot].first, reads[slot], others, gameRoot, out, report);
        }
    }

    const auto decoded = [&](std::span<const std::byte> encoded) {
        return fb::ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
    };
    // Settle: highest priority first, an asset a higher mod already changed keeps that change.
    for (std::size_t slot = 0; slot < asked.size(); ++slot) {
        const auto* mod = asked[slot].first;
        auto& read = reads[slot];
        if (read.failure) std::rethrow_exception(read.failure);
        auto& [candidates, companions, bundleChunks, chunkDefaults, unreadable, addedIn, newChunks, imported, changes, scripts, notes, failure,
               links, referrers, unsure, gone] = read;
        for (auto& [bundle,defaults]:chunkDefaults) out.bundle_chunk_defaults.try_emplace(bundle,std::move(defaults));
        std::size_t changed{};
        // A higher mod's version of an asset, which this mod's change is combined with
        // below, may name what this mod adds.
        for (const auto& change : changes)
            if (const auto versions = out.changed.find(change.name); versions != out.changed.end())
                if (const auto existing = versions->second.find(change.game); existing != versions->second.end()) try {
                    for (const auto& reference : decoded(existing->second.encoded).imports)
                        imported.insert(reference.fileGuid);
                } catch (const std::exception&) {}
        for (auto& script : scripts) {
            auto& versions = out.scripts[script.name];
            if (versions.contains(script.game)) {
                if (!script.value) notes[script.note].clear();   // not wanted: as if never read
                continue;
            }
            if (!script.value) continue;   // wanted, and noted
            versions.emplace(script.game, std::move(*script.value));
            ++changed;
        }
        for (auto& note : notes)
            if (!note.empty()) report.notes.push_back(std::move(note));
        // One addition per kind and name in a bundle, the highest-priority mod's. Two
        // mods adding the same asset is no clash; two different assets under one name
        // is, and only one of them can be what a copy gets: say whose, so a song or an
        // item that goes missing on a map can be traced to the mod that took its name.
        std::map<std::string, std::pair<std::size_t, std::string>, std::less<>> shadowed;   // by the mod kept
        // This mod's added EBX that another mod's different document stands in for: what
        // names one by its guid finds nothing in a copy that was given the other's.
        std::set<fb::Guid> lost;
        const auto keep = [&](const std::string& bundle, AssetAddition addition) {
            auto& list = out.added[bundle];
            auto& index = addedAt[bundle];
            const auto [place, fresh] = index.try_emplace({addition.asset.kind, ascii_lower(addition.asset.name)}, list.size());
            if (!fresh) {
                const auto holder = list.begin() + static_cast<std::ptrdiff_t>(place->second);
                if (holder->mod != addition.mod && holder->asset.sha1 != addition.asset.sha1) {
                    auto& [count, example] = shadowed[holder->mod];
                    if (!count++) example = addition.asset.name;
                    if (addition.asset.kind == fb::AssetKind::ebx && holder->file != addition.file)
                        lost.insert(addition.file);
                }
                return false;
            }
            list.push_back(std::move(addition));
            return true;
        };
        // Transitively: an addition a following addition imports follows too (a new song
        // imports its new wave, and only the song is named by the changed playlist).
        std::vector<bool> followed = unreadable;   // (an unreadable one never follows)
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                auto& [bundle, addition] = candidates[index];
                if (followed[index] || !imported.contains(addition.file)) continue;
                followed[index] = grew = true;
                imported.insert(addition.names.begin(), addition.names.end());
                keep(bundle, std::move(addition));
            }
        }
        for (auto& [bundle, companion] : companions) {
            // Looked up, not made: a bundle with nothing carried must stay out of `added`.
            const auto found = out.added.find(bundle);
            if (found == out.added.end()) continue;
            // One addition per kind and name in a bundle (keep): this mod's, or not.
            const auto& index = addedAt[bundle];
            const auto held = index.find({fb::AssetKind::ebx, ascii_lower(companion.asset.name)});
            // Native mesh material keys do not import their shader depot by EBX GUID/name. A depot
            // the mod's resident list names goes with its own runtime roots, once all of them come
            // from this mod (on a replacement map's copy of the bundle too).
            constexpr std::uint32_t shader_block_depot = 0x73312045;
            const auto* shader = companion.asset.resourceType == shader_block_depot ? resident_shader(*mod, bundle, companion.asset.name) : nullptr;
            const bool runtimeShader = shader &&
                std::all_of(shader->roots.begin(),shader->roots.end(),[&](const auto& root) {
                    return std::any_of(found->second.begin(),found->second.end(),[&](const auto& addition) {
                        return addition.mod==mod->name && addition.asset.kind==fb::AssetKind::ebx &&
                            ascii_lower(addition.asset.name)==root;
                    });
                });
            if ((held != index.end() && found->second[held->second].mod == mod->name) || runtimeShader)
                keep(bundle, std::move(companion));
        }
        // A native mesh RES can refer to geometry in its bundle, rather than
        // the superbundle TOC. Carrying only EBX/RES leaves the engine probing
        // its chunk hash table forever on replacement-map bundles.
        if (!bundleChunks.empty()) {
            std::map<fb::Guid, fb::Guid> aliases;
            std::vector<bool> starts(0x10000);
            const auto add_alias = [&](const fb::Guid& bytes, const fb::Guid& id) {
                aliases.emplace(bytes,id);
                starts[std::to_integer<std::size_t>(bytes.bytes[0])*256 +
                       std::to_integer<std::size_t>(bytes.bytes[1])] = true;
            };
            for (const auto& chunk : bundleChunks) {
                const auto& id=chunk.addition.asset.guid;
                add_alias(id,id);
                auto swapped=id;
                std::reverse(swapped.bytes.begin(),swapped.bytes.begin()+4);
                std::reverse(swapped.bytes.begin()+4,swapped.bytes.begin()+6);
                std::reverse(swapped.bytes.begin()+6,swapped.bytes.begin()+8);
                add_alias(swapped,id);
            }
            std::map<std::string,std::set<fb::Guid>,std::less<>> referenced;
            for (auto& [bundle,list] : out.added)
                for (auto& addition : list) {
                    if (addition.mod!=mod->name || addition.asset.kind==fb::AssetKind::chunk) continue;
                    std::vector<std::byte> bytes;
                    try { bytes=fb::decode_cas(addition.encoded,{gameRoot}); }
                    catch (const std::exception&) { continue; }
                    std::set<fb::Guid> named;
                    for (std::size_t at=0;at+16<=bytes.size();++at) {
                        if (!starts[std::to_integer<std::size_t>(bytes[at])*256+
                                    std::to_integer<std::size_t>(bytes[at+1])]) continue;
                        fb::Guid key;std::memcpy(key.bytes.data(),bytes.data()+at,16);
                        if (const auto found=aliases.find(key);found!=aliases.end()) named.insert(found->second);
                    }
                    addition.chunk_names.assign(named.begin(),named.end());
                    referenced[bundle].insert(named.begin(),named.end());
                }
            std::size_t carried{};
            for (auto& chunk : bundleChunks) {
                const auto found=referenced.find(chunk.bundle);
                if (found==referenced.end() || !found->second.contains(chunk.addition.asset.guid)) continue;
                const auto& file=chunk.file;
                chunk.addition.encoded=store.read(file.location.patch?mod->directory:baseRoot,
                    file.location,file.offset,file.size);
                if (keep(chunk.bundle,std::move(chunk.addition))) ++carried;
            }
            if (carried) report.notes.push_back(mod->name+": "+std::to_string(carried)+
                " referenced bundle chunk(s) accompany mesh/texture additions into map copies");
        }
        for (auto& change : changes) {
            if (item_list_name(change.name)) out.item_list_bundles.insert(change.bundle);
            // What the change names of the assets the mod added beside it. The mod
            // put them in one bundle with what names them, so they stay together.
            std::set<fb::Guid> names;
            const auto beside = addedIn.find(change.bundle);
            try {
                auto document = decoded(change.encoded);
                // (The items the player left out of this mod are not in those copies either.)
                const auto chosen = [&](const fb::ebx::ImportReference& reference) {
                    return !gone.contains(reference.fileGuid); };
                const auto found = [&](const fb::ebx::ImportReference& reference) {
                    return !lost.contains(reference.fileGuid); };
                const auto stays = [&](const fb::ebx::ImportReference& reference) { return chosen(reference) && found(reference); };
                if (!std::ranges::all_of(document.imports, stays)) {
                    const auto left = std::ranges::all_of(document.imports, chosen) ? std::size_t{} : fb::ebx::drop_root_references(document, chosen);
                    const auto missing = std::ranges::all_of(document.imports, found) ? std::size_t{} : fb::ebx::drop_root_references(document, found);
                    if (left || missing) {
                        const auto rebuilt = fb::ebx::write_document(document);
                        change.sha1 = sha1_of(rebuilt);
                        change.originalSize = rebuilt.size();
                        change.encoded = fb::encode_cas(rebuilt, {gameRoot});
                    }
                    if (missing)
                        report.notes.push_back(mod->name + ": " + change.asset + ": " + std::to_string(missing) +
                            " entry(ies) left out of the copies other mods carry, where another mod's asset of "
                            "the same name stands in for what they name");
                }
                if (beside != addedIn.end())
                    for (const auto& reference : document.imports)
                        if (beside->second.contains(reference.fileGuid) && stays(reference))
                            names.insert(reference.fileGuid);
            } catch (const std::exception&) {}
            auto& versions = out.changed[change.name];
            const auto existing = versions.find(change.game);
            if (existing == versions.end()) {
                versions.emplace(change.game, AssetOverride{mod->name, change.sha1, change.originalSize,
                                                            std::move(change.encoded), std::nullopt, std::move(names)});
                ++changed;
                continue;
            }
            // The same change again, from another of the mod's bundles or another mod.
            if (existing->second.sha1 == change.sha1) {
                existing->second.names.insert(names.begin(), names.end());
                continue;
            }
            if (!change.gameFile) continue;
            try {
                const auto baseDoc = decoded(store.read(baseRoot, change.gameFile->location,
                                                        change.gameFile->offset, change.gameFile->size));
                const auto existingDoc = decoded(existing->second.encoded);
                const auto thisDoc = decoded(change.encoded);
                // Lowest priority first, the order a bundle's own copies are combined
                // in: where two mods changed one value, the higher mod's is kept.
                const std::array<const fb::ebx::Document*, 2> editDocs{&thisDoc, &existingDoc};
                fb::ebx::MergeSummary summary;
                auto combined = fb::ebx::merge_documents(baseDoc, editDocs, &summary);
                // Changed values alone are only worth a copy that has all of them.
                if (!summary.instances && !summary.arrayEntries && !(summary.values && summary.exact())) continue;
                const auto rebuilt = fb::ebx::write_document(combined);
                existing->second.sha1 = sha1_of(rebuilt);
                existing->second.originalSize = rebuilt.size();
                existing->second.encoded = fb::encode_cas(rebuilt, {gameRoot});
                if (existing->second.mod != mod->name && !existing->second.mod.ends_with(" + " + mod->name))
                    existing->second.mod += " + " + mod->name;
                existing->second.names.insert(names.begin(), names.end());
            } catch (const std::exception&) {}
        }
        for (const auto& [other, clash] : shadowed)
            report.notes.push_back(mod->name + ": " + std::to_string(clash.first) +
                " added asset(s) share a name with ones " + other + " adds, e.g. " + clash.second +
                "; other mods' copies of the bundle get " + other + "'s");
        // The new TOC chunks this mod's carried EBX or companion RES name, as raw
        // GUID bytes. Texture pixel ChunkIds live in RES headers, not their EBX.
        // Scan only retained dependencies so unrelated chunks do not follow them.
        if (!newChunks.empty()) {
            std::vector<std::vector<std::byte>> carried;
            for (const auto& [bundle, list] : out.added)
                for (const auto& addition : list)
                    if (addition.mod == mod->name && (addition.asset.kind == fb::AssetKind::ebx ||
                                                     addition.asset.kind == fb::AssetKind::resource)) try {
                        carried.push_back(fb::decode_cas(addition.encoded, {gameRoot}));
                    } catch (const std::exception&) {}
            // Each carried asset is gone through once, for all the chunks together: looking for
            // every chunk in every asset in turn took most of a merge with many of either.
            std::set<fb::Guid> wanted, named, taken;
            std::vector<bool> starts(0x10000); // the two bytes a wanted chunk's id starts with
            for (const auto& chunk : newChunks) {
                wanted.insert(chunk.guid);
                starts[std::to_integer<std::size_t>(chunk.guid.bytes[0]) << 8 |
                       std::to_integer<std::size_t>(chunk.guid.bytes[1])] = true;
            }
            for (const auto& bytes : carried)
                for (std::size_t at = 0; at + sizeof(fb::Guid) <= bytes.size(); ++at) {
                    if (!starts[std::to_integer<std::size_t>(bytes[at]) << 8 | std::to_integer<std::size_t>(bytes[at + 1])])
                        continue;
                    fb::Guid id;
                    std::memcpy(id.bytes.data(), bytes.data() + at, id.bytes.size());
                    if (wanted.contains(id)) named.insert(id);
                }
            for (const auto& chunk : newChunks)
                if (named.contains(chunk.guid) && taken.insert(chunk.guid).second) out.chunks[mod->name].push_back(chunk);
            if (!taken.empty())
                report.notes.push_back(mod->name + ": " + std::to_string(taken.size()) +
                    " added chunk(s) follow its added assets into other mods' superbundles");
        }
        if (changed)
            report.notes.push_back(mod->name + ": " + std::to_string(changed) +
                " changed asset(s) also apply to the copies other mods carry");
    }
    return out;
}

} // namespace dingosdk::mods::detail
