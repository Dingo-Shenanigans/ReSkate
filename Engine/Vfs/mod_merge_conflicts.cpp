#include "mod_merge_internal.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/shader_lookup.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace dingosdk::mods::detail {
namespace {

// The merge keys an asset within a bundle by its kind and, for a chunk its guid, for the rest its
// name in any case (asset_key in mod_merge_combine.cpp). The same, hashed: a big bundle has some
// 17,000 assets and a hundred mods' copies of it, too many to make a string for each.
std::uint64_t key_hash(const fb::BundleAsset& asset) {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    if (asset.kind == fb::AssetKind::chunk)
        for (const auto byte : asset.guid.bytes) value = (value ^ static_cast<std::uint8_t>(byte)) * 0x100000001B3ULL;
    else
        value = name_hash(asset.name);
    return value ^ (static_cast<std::uint64_t>(asset.kind) + 1) * 0x9E3779B97F4A7C15ULL;
}

// Assets the merge combines whatever the mods did to them, so two versions are no conflict:
// the shader tables, the preset table and the collision the material grid renumbers.
bool combined_anyway(const fb::BundleAsset& asset) {
    return asset.kind == fb::AssetKind::resource &&
           (asset.resourceType == fb::shader::programLookupType || asset.resourceType == fb::shader::textureLookupType ||
            asset.resourceType == fb::material_grid::physicsResourceType || fb::bundle_ref::is_table(asset.name));
}

using Rows = std::vector<MergeReport::Conflict>;
void conflict(Rows& rows, const std::string& kept, const std::string& lost, const std::string& asset) {
    if (kept.empty() || lost.empty() || kept == lost) return;
    for (auto& row : rows)
        if (row.kept == kept && row.lost == lost) {
            ++row.assets;
            return;
        }
    rows.push_back({kept, lost, asset, 1});
}

// One mod's version of an asset in a bundle that is not the game's.
struct Change {
    std::uint64_t key{};
    std::size_t copy{};     // in Shared::copies
    std::size_t at{};       // the asset's place in the manifest's EBX, for reading it
    const fb::BundleAsset* asset{};
};

// One bundle that two or more mods ship in one superbundle, and what is read of it.
struct Shared {
    const fb::TocBundle* game{};
    std::vector<std::pair<std::size_t, const fb::TocBundle*>> copies;   // (mod, its copy), lowest priority first
    std::optional<Listing> gameListing;
    // The game's version of each asset: its sha1, and its place among the EBX.
    std::unordered_map<std::uint64_t, std::pair<fb::Sha1, std::size_t>> original;
    std::vector<std::optional<Listing>> listings;   // by copy
    std::vector<std::vector<Change>> changes;       // by copy: what it ships that is not the game's
};

// `count` pieces of work on up to `threads` threads beside this one, taken in turn.
void in_parallel(std::size_t count, std::size_t threads, bool background, const std::function<void(std::size_t)>& work) {
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (;;) {
            const auto mine = next.fetch_add(1);
            if (mine >= count) return;
            try {
                work(mine);
            } catch (const std::exception&) {}   // a part that fails finds nothing: the merge still checks for itself
        }
    };
    std::vector<std::jthread> workers;
    try {
        // Threads of their own, not the system's pool (see the superbundles in mod_merge.cpp).
        for (std::size_t index = 0; index + 1 < count && index < threads; ++index)
            workers.emplace_back([&] {
                if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                run();
            });
    } catch (const std::system_error&) {} // fewer threads, or none: this one does the rest
    run();
}

// The game's copy of a bundle, keyed, to tell a mod's change from a copy of the game's.
void read_game(Shared& shared, const CasStore& store, const fs::path& baseRoot, const fs::path& gameRoot) {
    if (!shared.game) return;
    shared.gameListing = list_bundle(store, baseRoot, baseRoot, *shared.game, gameRoot);
    if (!shared.gameListing) return;
    const auto& manifest = shared.gameListing->manifest;
    for (std::size_t index = 0; index < manifest.ebx.size(); ++index)
        shared.original.emplace(key_hash(manifest.ebx[index]), std::pair{manifest.ebx[index].sha1, index});
    for (const auto* list : {&manifest.resources, &manifest.chunks})
        for (const auto& asset : *list) shared.original.emplace(key_hash(asset), std::pair{asset.sha1, std::size_t{}});
}

// One mod's copy: its asset list, and of that what is not the game's.
void read_copy(Shared& shared, std::size_t copy, const std::vector<const Mod*>& mods, const CasStore& store,
               const fs::path& baseRoot, const fs::path& gameRoot) {
    auto& listing = shared.listings[copy];
    listing = list_bundle(store, mods[shared.copies[copy].first]->directory, baseRoot, *shared.copies[copy].second, gameRoot);
    if (!listing) return;   // (the merge notes what it cannot read)
    auto& changes = shared.changes[copy];
    const auto take = [&](const std::vector<fb::BundleAsset>& list, bool ebx) {
        for (std::size_t index = 0; index < list.size(); ++index) {
            const auto& asset = list[index];
            if (combined_anyway(asset)) continue;
            const auto key = key_hash(asset);
            if (const auto shipped = shared.original.find(key); shipped != shared.original.end() && shipped->second.first == asset.sha1)
                continue;
            changes.push_back({key, copy, ebx ? index : 0, &asset});
        }
    };
    take(listing->manifest.ebx, true);
    take(listing->manifest.resources, false);
    take(listing->manifest.chunks, false);
}

// The conflicts in one bundle, from the changes its copies were found to ship.
Rows decide(const Shared& shared, const std::vector<const Mod*>& mods, const CasStore& store,
            const fs::path& baseRoot, const fs::path& gameRoot) {
    Rows rows;
    // Each asset's changes, lowest priority first, the assets in the order they were first met.
    std::unordered_map<std::uint64_t, std::vector<const Change*>> byAsset;
    std::vector<std::uint64_t> order;
    for (const auto& copy : shared.changes)
        for (const auto& change : copy) {
            auto& list = byAsset[change.key];
            if (list.empty()) order.push_back(change.key);
            list.push_back(&change);
        }
    const auto mod_of = [&](const Change& change) -> const std::string& { return mods[shared.copies[change.copy].first]->name; };
    for (const auto key : order) {
        const auto& changes = byAsset.at(key);
        const auto& kept = *changes.back();
        // Two mods' different changes, or nothing to decide.
        if (std::ranges::all_of(changes, [&](const Change* change) {
                return change->asset->sha1 == kept.asset->sha1 || shared.copies[change->copy].first == shared.copies[kept.copy].first; }))
            continue;
        const auto shipped = shared.original.find(key);

        if (kept.asset->kind != fb::AssetKind::ebx || shipped == shared.original.end()) {
            // Not combined: of each asset the highest-priority mod's version is the one in the game, and a
            // mod lower down that changed it differently loses its change. (The rule of absorb's own_change.)
            fb::Sha1 last{};
            std::vector<const std::string*> holders;
            for (const auto* change : changes) {
                const auto& name = mod_of(*change);
                if (!holders.empty() && change->asset->sha1 == last) {
                    if (std::ranges::find(holders, &name) == holders.end()) holders.push_back(&name);
                    continue;
                }
                for (const auto* lost : holders) conflict(rows, name, *lost, change->asset->name);
                last = change->asset->sha1;
                holders = {&name};
            }
            continue;
        }

        // EBX the game has is combined, the changes of every mod onto the game's copy: a conflict only
        // when that cannot be done (the kept-whole and could-not-combine cases of the merge's combine).
        const auto lose_to_kept = [&] {
            for (const auto* change : changes)
                if (change->asset->sha1 != kept.asset->sha1) conflict(rows, mod_of(kept), mod_of(*change), kept.asset->name);
        };
        try {
            const auto base = fb::ebx::read_document(
                read_asset(store, baseRoot, baseRoot, *shared.gameListing, shipped->second.second, gameRoot));
            // The combined material grid is worked out separately (mod_merge_grid.cpp).
            if (lower(base.rootType).find("materialgrid") != std::string::npos) continue;
            std::map<fb::Sha1, fb::ebx::Document> documents;
            std::vector<const fb::ebx::Document*> edits;
            for (const auto* change : changes) {
                auto [at, fresh] = documents.try_emplace(change->asset->sha1);
                if (fresh)
                    at->second = fb::ebx::read_document(read_asset(store, mods[shared.copies[change->copy].first]->directory,
                                                                   baseRoot, *shared.listings[change->copy], change->at, gameRoot));
                edits.push_back(&at->second);
            }
            fb::ebx::MergeSummary summary;
            (void)fb::ebx::merge_documents(base, edits, &summary);
            if (summary.dropped.empty() && !summary.instances && !summary.arrayEntries && (!summary.values || !summary.exact()))
                lose_to_kept();
        } catch (const std::exception&) {
            lose_to_kept();
        }
    }
    return rows;
}

} // namespace

std::vector<MergeReport::Conflict> find_conflicts(const std::vector<const Mod*>& mods,
                                                  const std::map<const Mod*, RelativeFiles>& modFiles,
                                                  const fs::path& dataRoot, const fs::path& output,
                                                  std::size_t threads, bool background) {
    const auto baseRoot = dataRoot / L"Data";
    const auto gameRoot = dataRoot;
    // The superbundles two or more mods ship: only there can two mods' versions of an asset meet.
    std::map<std::string, std::vector<std::size_t>> shippedBy;   // lower case relative path, mods highest priority first
    std::map<std::string, std::string> spelled;
    for (std::size_t mod = 0; mod < mods.size(); ++mod)
        if (const auto files = modFiles.find(mods[mod]); files != modFiles.end())
            for (const auto& relative : files->second.tocs) {
                auto key = lower(relative);
                spelled.try_emplace(key, relative);
                shippedBy[std::move(key)].push_back(mod);
            }
    std::erase_if(shippedBy, [](const auto& entry) { return entry.second.size() < 2; });
    if (shippedBy.empty()) return {};

    // Their TOCs, the game's and the mods', kept while the bundles they list are read.
    std::vector<fb::TocDocument> tocs;
    tocs.reserve([&] {
        std::size_t count{};
        for (const auto& [relative, by] : shippedBy) count += by.size() + 1;
        return count;
    }());
    std::vector<Shared> bundles;
    for (const auto& [relative, by] : shippedBy) {
        std::map<std::string, Shared> named;
        for (auto mod = by.rbegin(); mod != by.rend(); ++mod) {
            try {
                const auto& toc = tocs.emplace_back(fb::read_toc(read_file(mods[*mod]->directory / fs::path(spelled.at(relative)))));
                for (const auto& bundle : toc.bundles) named[lower(bundle.name)].copies.emplace_back(*mod, &bundle);
            } catch (const std::exception&) {}   // (the merge leaves a mod out whose TOC cannot be read)
        }
        std::erase_if(named, [](const auto& entry) { return entry.second.copies.size() < 2; });
        if (named.empty()) continue;
        std::error_code error;
        if (const auto game = baseRoot / fs::path(spelled.at(relative)); fs::is_regular_file(game, error)) try {
            const auto& toc = tocs.emplace_back(fb::read_toc(read_file(game)));
            for (const auto& bundle : toc.bundles)
                if (const auto found = named.find(lower(bundle.name)); found != named.end()) found->second.game = &bundle;
        } catch (const std::exception&) {}
        for (auto& [name, shared] : named) {
            shared.listings.resize(shared.copies.size());
            shared.changes.resize(shared.copies.size());
            bundles.push_back(std::move(shared));
        }
    }
    if (bundles.empty()) return {};

    // On several threads, in three steps, so that one bundle a hundred mods ship is not all one
    // thread's: the game's copy of each bundle, then each mod's copy of each by itself, then what
    // the copies' changes mean in each bundle. The rows are then put together in bundle order.
    const auto layout = vfs::read_layout(baseRoot / L"layout.toc");
    const CasStore store(baseRoot, output, layout.root);
    in_parallel(bundles.size(), threads, background, [&](std::size_t index) { read_game(bundles[index], store, baseRoot, gameRoot); });
    std::vector<std::pair<std::size_t, std::size_t>> copies;   // (bundle, copy)
    for (std::size_t index = 0; index < bundles.size(); ++index)
        for (std::size_t copy = 0; copy < bundles[index].copies.size(); ++copy) copies.emplace_back(index, copy);
    in_parallel(copies.size(), threads, background, [&](std::size_t index) {
        read_copy(bundles[copies[index].first], copies[index].second, mods, store, baseRoot, gameRoot);
    });
    std::vector<Rows> found(bundles.size());
    in_parallel(bundles.size(), threads, background, [&](std::size_t index) {
        found[index] = decide(bundles[index], mods, store, baseRoot, gameRoot);
    });
    Rows rows;
    for (const auto& bundle : found)
        for (const auto& row : bundle) {
            const auto same = std::ranges::find_if(rows, [&](const MergeReport::Conflict& held) {
                return held.kept == row.kept && held.lost == row.lost; });
            if (same != rows.end()) same->assets += row.assets;
            else rows.push_back(row);
        }
    return rows;
}

} // namespace dingosdk::mods::detail
