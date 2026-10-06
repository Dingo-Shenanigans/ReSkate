#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/ebx_writer.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {

AssetOverrides collect_asset_overrides(const std::vector<const Mod*>& mods,
                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                       const CasStore& store, const fs::path& baseRoot,
                                       const fs::path& gameRoot, MergeReport& report) {
    AssetOverrides out;
    // Highest priority first: an asset a higher mod already changed keeps that change.
    for (const auto* mod : mods) {
        // A map's edits to the game's assets serve its own levels; asset mods
        // (cameras, tuning, cosmetics) are the ones meant to apply everywhere.
        if (mod->provides_levels) continue;
        const auto files = modFiles.find(mod);
        if (files == modFiles.end()) continue;
        std::size_t changed{};
        // This mod's new assets, and the partitions its recorded changes import.
        // An added asset only follows a change that refers to it: copied on its
        // own, it could arrive in another mod's bundle without the resources
        // and chunks it was added with.
        std::vector<std::pair<std::string, AssetAddition>> candidates, companions;
        std::vector<fb::TocChunk> newChunks;   // TOC chunks the game's TOCs do not have
        std::set<fb::Guid> imported;
        // The game's assets this mod changed. They wait until its additions are
        // settled: one that another mod's asset of the same name stands in for
        // is not where a copy can find it, and a change must not name it there.
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
        std::vector<Change> changes;
        const auto decoded = [&](std::span<const std::byte> encoded) {
            return fb::ebx::read_document(fb::decode_cas(encoded, {gameRoot}));
        };
        for (const auto& relative : files->second.tocs) {
            std::error_code error;
            const auto baseToc = baseRoot / fs::path(relative);
            if (!fs::is_regular_file(baseToc, error)) continue;
            try {
                const auto own = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                const auto game = fb::read_toc(read_file(baseToc));
                std::map<std::string, const fb::TocBundle*, std::less<>> gameBundles;
                for (const auto& bundle : game.bundles) gameBundles.emplace(lower(bundle.name), &bundle);
                std::set<fb::Guid> gameChunks;
                for (const auto& chunk : game.chunks) gameChunks.insert(chunk.guid);
                for (const auto& chunk : own.chunks)
                    if (!chunk.removed && chunk.location.patch && !gameChunks.contains(chunk.guid)) newChunks.push_back(chunk);
                for (const auto& bundle : own.bundles) {
                    const auto shipped = gameBundles.find(lower(bundle.name));
                    if (shipped == gameBundles.end()) continue;
                    const auto modListing = list_bundle(store, mod->directory, baseRoot, bundle, gameRoot);
                    const auto gameListing = list_bundle(store, baseRoot, baseRoot, *shipped->second, gameRoot);
                    if (!modListing || !gameListing) continue;
                    // The game's copy of each asset: its sha1, and where in the listing it is.
                    std::map<std::string, std::pair<fb::Sha1, std::size_t>, std::less<>> original;
                    for (std::size_t index = 0; index < gameListing->manifest.ebx.size(); ++index) {
                        const auto& asset = gameListing->manifest.ebx[index];
                        original.emplace(lower(asset.name), std::pair{asset.sha1, index});
                    }
                    for (std::size_t index = 0; index < modListing->manifest.ebx.size(); ++index) {
                        const auto& asset = modListing->manifest.ebx[index];
                        const auto name = lower(asset.name);
                        const auto game_copy = original.find(name);
                        if (game_copy != original.end() && game_copy->second.first == asset.sha1) continue;
                        const auto at = modListing->first + index;
                        if (at >= modListing->files.size()) continue;
                        const auto& file = modListing->files[at];
                        const auto payload = [&] {
                            return store.read(file.location.patch ? mod->directory : baseRoot,
                                              file.location, file.offset, file.size);
                        };
                        if (game_copy == original.end()) {
                            candidates.push_back({lower(bundle.name), {mod->name, asset, payload(), lower(relative)}});
                            continue;
                        }
                        auto encodedPayload = payload();
                        try {
                            for (const auto& reference : decoded(encodedPayload).imports)
                                imported.insert(reference.fileGuid);
                        } catch (const std::exception&) {}
                        // A higher mod's version of the asset, which this change is
                        // combined with below, may name what this mod adds.
                        if (const auto versions = out.changed.find(name); versions != out.changed.end())
                            if (const auto existing = versions->second.find(game_copy->second.first);
                                existing != versions->second.end()) try {
                                for (const auto& reference : decoded(existing->second.encoded).imports)
                                    imported.insert(reference.fileGuid);
                            } catch (const std::exception&) {}
                        std::optional<fb::BundleFileInfo> gameFile;
                        if (const auto gameAt = gameListing->first + game_copy->second.second;
                            gameAt < gameListing->files.size())
                            gameFile = gameListing->files[gameAt];
                        changes.push_back({lower(bundle.name), name, asset.name, game_copy->second.first, asset.sha1,
                                           asset.originalSize, std::move(encodedPayload), gameFile});
                    }
                    // Maps carry stock SkaterLoader too. Replacing it only in the game's
                    // bundles loses custom cosmetic material targets on a map transition.
                    // Other resource types retain their own table/dependency merge rules.
                    std::map<std::string, const fb::BundleAsset*, std::less<>> gameScripts;
                    for (const auto& asset : gameListing->manifest.resources)
                        if (asset.resourceType == luaScriptResourceType) gameScripts.emplace(lower(asset.name), &asset);
                    for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                        const auto& asset = modListing->manifest.resources[index];
                        if (asset.resourceType != luaScriptResourceType) continue;
                        const auto name = lower(asset.name);
                        const auto originalScript = gameScripts.find(name);
                        if (originalScript == gameScripts.end() || originalScript->second->sha1 == asset.sha1 ||
                            originalScript->second->resourceId != asset.resourceId) continue;
                        auto& versions = out.scripts[name];
                        if (versions.contains(originalScript->second->sha1)) continue;
                        const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                        if (at >= modListing->files.size()) continue;
                        const auto& file = modListing->files[at];
                        versions.emplace(originalScript->second->sha1,
                            AssetOverride{mod->name, asset.sha1, asset.originalSize,
                                store.read(file.location.patch ? mod->directory : baseRoot,
                                           file.location, file.offset, file.size), asset});
                        ++changed;
                    }
                    // Resources the mod adds, kept to go with an added EBX of the same name
                    // (a wave's sound-bank resource registers the wave with the audio system).
                    std::set<std::string, std::less<>> shippedResources;
                    for (const auto& asset : gameListing->manifest.resources) shippedResources.insert(lower(asset.name));
                    for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                        const auto& asset = modListing->manifest.resources[index];
                        const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                        if (shippedResources.contains(lower(asset.name)) || at >= modListing->files.size()) continue;
                        const auto& file = modListing->files[at];
                        companions.push_back({lower(bundle.name), {mod->name, asset,
                            store.read(file.location.patch ? mod->directory : baseRoot, file.location, file.offset, file.size), lower(relative)}});
                    }
                }
            } catch (const std::exception& failure) {
                report.notes.push_back(mod->name + ": " + relative +
                    ": its changes could not be read for other mods' copies (" + failure.what() + ")");
            }
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
            const auto name = lower(addition.asset.name);
            const auto holder = std::ranges::find_if(list, [&](const AssetAddition& other) {
                return other.asset.kind == addition.asset.kind && lower(other.asset.name) == name; });
            if (holder != list.end()) {
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
        std::vector<bool> followed(candidates.size());
        // The documents this mod added to each bundle, by guid.
        std::map<std::string, std::set<fb::Guid>, std::less<>> addedIn;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            auto& addition = candidates[index].second;
            try {
                const auto document = decoded(addition.encoded);
                addition.file = document.fileGuid;
                for (const auto& reference : document.imports) addition.names.push_back(reference.fileGuid);
                addedIn[candidates[index].first].insert(addition.file);
            } catch (const std::exception&) { followed[index] = true; }   // unreadable: never follows
        }
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
            const auto name = lower(companion.asset.name);
            if (std::ranges::any_of(found->second, [&](const AssetAddition& addition) {
                    return addition.mod == mod->name && addition.asset.kind == fb::AssetKind::ebx &&
                           lower(addition.asset.name) == name; }))
                keep(bundle, std::move(companion));
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
            std::set<fb::Guid> taken;
            for (const auto& chunk : newChunks) {
                const auto& id = chunk.guid.bytes;
                if (!taken.contains(chunk.guid) && std::ranges::any_of(carried, [&](const std::vector<std::byte>& bytes) {
                        return std::search(bytes.begin(), bytes.end(), id.begin(), id.end()) != bytes.end(); })) {
                    out.chunks[mod->name].push_back(chunk);
                    taken.insert(chunk.guid);
                }
            }
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
