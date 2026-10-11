#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/ebx_writer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

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
    AssetOverride value;
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
    std::vector<bool> unreadable;                                    // per candidate: never follows
    std::map<std::string, std::set<fb::Guid>, std::less<>> addedIn;  // the documents it added to each bundle, by guid
    std::vector<fb::TocChunk> newChunks;   // TOC chunks the game's TOCs do not have
    std::set<fb::Guid> imported;
    std::vector<Change> changes;
    std::vector<Script> scripts;
    std::vector<std::string> notes;
    std::exception_ptr failure;
};

ModRead read_mod(const Mod& mod, const RelativeFiles& files, const CasStore& store, const fs::path& baseRoot,
                 const fs::path& gameRoot, Shared<GameToc>& gameTocs, Shared<GameBundle>& gameBundles) {
    ModRead read;
    auto& [candidates, companions, unreadable, addedIn, newChunks, imported, changes, scripts, notes, failure] = read;
    const auto decoded = [&](std::span<const std::byte> encoded) {
        return fb::ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
    };
    for (const auto& relative : files.tocs) {
        std::error_code error;
        const auto baseToc = baseRoot / fs::path(relative);
        if (!fs::is_regular_file(baseToc, error)) continue;
        try {
            const auto own = fb::read_toc(read_file(mod.directory / fs::path(relative)));
            const auto tocKey = lower(relative);
            const auto& game = gameTocs.get(tocKey, [&](GameToc& toc) {
                toc.document = fb::read_toc(read_file(baseToc));
                for (const auto& bundle : toc.document.bundles) toc.bundles.emplace(lower(bundle.name), &bundle);
                for (const auto& chunk : toc.document.chunks) toc.chunks.insert(chunk.guid);
            });
            for (const auto& chunk : own.chunks)
                if (!chunk.removed && chunk.location.patch && !game.chunks.contains(chunk.guid)) newChunks.push_back(chunk);
            for (const auto& bundle : own.bundles) {
                const auto bundleName = lower(bundle.name);
                const auto shipped = game.bundles.find(bundleName);
                if (shipped == game.bundles.end()) continue;
                const auto modListing = list_bundle(store, mod.directory, baseRoot, bundle, gameRoot);
                const auto& known = gameBundles.get(tocKey + '|' + bundleName, [&](GameBundle& made) {
                    made.listing = list_bundle(store, baseRoot, baseRoot, *shipped->second, gameRoot);
                    if (!made.listing) return;
                    const auto& manifest = made.listing->manifest;
                    for (std::size_t index = 0; index < manifest.ebx.size(); ++index)
                        made.original.emplace(lower(manifest.ebx[index].name), std::pair{manifest.ebx[index].sha1, index});
                    // Maps carry stock SkaterLoader too. Replacing it only in the game's
                    // bundles loses custom cosmetic material targets on a map transition.
                    // Other resource types retain their own table/dependency merge rules.
                    for (const auto& asset : manifest.resources) {
                        if (asset.resourceType == luaScriptResourceType) made.scripts.emplace(lower(asset.name), &asset);
                        made.resources.insert(lower(asset.name));
                    }
                });
                if (!modListing || !known.listing) continue;
                const auto& gameListing = known.listing;
                const auto& original = known.original;
                for (std::size_t index = 0; index < modListing->manifest.ebx.size(); ++index) {
                    const auto& asset = modListing->manifest.ebx[index];
                    const auto name = lower(asset.name);
                    const auto game_copy = original.find(name);
                    if (game_copy != original.end() && game_copy->second.first == asset.sha1) continue;
                    const auto at = modListing->first + index;
                    if (at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    const auto payload = [&] {
                        return store.read(file.location.patch ? mod.directory : baseRoot,
                                          file.location, file.offset, file.size);
                    };
                    if (game_copy == original.end()) {
                        candidates.push_back({bundleName, {mod.name, asset, payload(), tocKey}});
                        continue;
                    }
                    auto encodedPayload = payload();
                    try {
                        for (const auto& reference : decoded(encodedPayload).imports)
                            imported.insert(reference.fileGuid);
                    } catch (const std::exception&) {}
                    std::optional<fb::BundleFileInfo> gameFile;
                    if (const auto gameAt = gameListing->first + game_copy->second.second;
                        gameAt < gameListing->files.size())
                        gameFile = gameListing->files[gameAt];
                    changes.push_back({bundleName, name, asset.name, game_copy->second.first, asset.sha1,
                                       asset.originalSize, std::move(encodedPayload), gameFile});
                }
                for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                    const auto& asset = modListing->manifest.resources[index];
                    if (asset.resourceType != luaScriptResourceType) continue;
                    const auto name = lower(asset.name);
                    const auto originalScript = known.scripts.find(name);
                    if (originalScript == known.scripts.end() || originalScript->second->sha1 == asset.sha1 ||
                        originalScript->second->resourceId != asset.resourceId) continue;
                    // (A higher mod's version of the script, or this mod's own from another
                    // bundle, wins when the mods are settled: read here either way.)
                    if (std::ranges::any_of(scripts, [&](const Script& have) {
                            return have.name == name && have.game == originalScript->second->sha1; })) continue;
                    const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                    if (at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    scripts.push_back({name, originalScript->second->sha1,
                        AssetOverride{mod.name, asset.sha1, asset.originalSize,
                            store.read(file.location.patch ? mod.directory : baseRoot,
                                       file.location, file.offset, file.size), asset}});
                }
                // Resources the mod adds, kept to go with an added EBX of the same name
                // (a wave's sound-bank resource registers the wave with the audio system).
                for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                    const auto& asset = modListing->manifest.resources[index];
                    const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                    if (known.resources.contains(lower(asset.name)) || at >= modListing->files.size()) continue;
                    const auto& file = modListing->files[at];
                    companions.push_back({bundleName, {mod.name, asset,
                        store.read(file.location.patch ? mod.directory : baseRoot, file.location, file.offset, file.size), tocKey}});
                }
            }
        } catch (const std::exception& failed) {
            notes.push_back(mod.name + ": " + relative +
                ": its changes could not be read for other mods' copies (" + failed.what() + ")");
        }
    }
    // Each added document's guid, and the guids of the documents it refers to in turn.
    unreadable.assign(candidates.size(), false);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto& addition = candidates[index].second;
        try {
            const auto document = decoded(addition.encoded);
            addition.file = document.fileGuid;
            for (const auto& reference : document.imports) addition.names.push_back(reference.fileGuid);
            addedIn[candidates[index].first].insert(addition.file);
        } catch (const std::exception&) { unreadable[index] = true; }
    }
    return read;
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
    {
        std::atomic<std::size_t> next{0};
        const auto work = [&] {
            for (;;) {
                const auto mine = next.fetch_add(1);
                if (mine >= asked.size()) return;
                try {
                    reads[mine] = read_mod(*asked[mine].first, *asked[mine].second, store, baseRoot, gameRoot, gameTocs, gameBundles);
                } catch (...) {
                    reads[mine].failure = std::current_exception();
                }
            }
        };
        std::vector<std::jthread> workers;
        try {
            // Threads of their own, not the system's pool (see the superbundles in mod_merge.cpp).
            for (std::size_t index = 0; index < std::min(threads, asked.size()) && asked.size() > 1; ++index)
                workers.emplace_back([&] {
                    if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    work();
                });
        } catch (const std::system_error&) {} // fewer threads, or none: this one does the rest
        work();
    }

    const auto decoded = [&](std::span<const std::byte> encoded) {
        return fb::ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
    };
    // Settle: highest priority first, an asset a higher mod already changed keeps that change.
    for (std::size_t slot = 0; slot < asked.size(); ++slot) {
        const auto* mod = asked[slot].first;
        auto& read = reads[slot];
        if (read.failure) std::rethrow_exception(read.failure);
        auto& [candidates, companions, unreadable, addedIn, newChunks, imported, changes, scripts, notes, failure] = read;
        std::size_t changed{};
        report.notes.insert(report.notes.end(), std::make_move_iterator(notes.begin()), std::make_move_iterator(notes.end()));
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
            if (versions.contains(script.game)) continue;
            versions.emplace(script.game, std::move(script.value));
            ++changed;
        }
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
            const auto [place, fresh] = index.try_emplace({addition.asset.kind, lower(addition.asset.name)}, list.size());
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
            const auto held = index.find({fb::AssetKind::ebx, lower(companion.asset.name)});
            if (held != index.end() && found->second[held->second].mod == mod->name) keep(bundle, std::move(companion));
        }
        for (auto& change : changes) {
            // What the change names of the assets the mod added beside it. The mod
            // put them in one bundle with what names them, so they stay together.
            std::set<fb::Guid> names;
            const auto beside = addedIn.find(change.bundle);
            try {
                auto document = decoded(change.encoded);
                const auto stays = [&](const fb::ebx::ImportReference& reference) {
                    return !lost.contains(reference.fileGuid); };
                if (!std::ranges::all_of(document.imports, stays))
                    if (const auto gone = fb::ebx::drop_root_references(document, stays)) {
                        const auto rebuilt = fb::ebx::write_document(document);
                        change.sha1 = sha1_of(rebuilt);
                        change.originalSize = rebuilt.size();
                        change.encoded = fb::encode_cas(rebuilt, {gameRoot});
                        report.notes.push_back(mod->name + ": " + change.asset + ": " + std::to_string(gone) +
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
        // The new TOC chunks this mod's carried EBX name (as raw GUID bytes, the way an
        // EBX stores a ChunkId), so they can follow those assets into other superbundles.
        if (!newChunks.empty()) {
            std::vector<std::vector<std::byte>> carried;
            for (const auto& [bundle, list] : out.added)
                for (const auto& addition : list)
                    if (addition.mod == mod->name && addition.asset.kind == fb::AssetKind::ebx) try {
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
