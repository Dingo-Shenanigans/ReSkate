#include "mod_conflicts.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_merge.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace dingosdk::mods {
using namespace detail;
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

// The assets two mods' versions of can conflict: EBX, and Lua scripts. The rest is content, of
// which the higher mod's version is kept and both mods still work (decide).
bool can_conflict(const fb::BundleAsset& asset) {
    return asset.kind == fb::AssetKind::ebx ||
           (asset.kind == fb::AssetKind::resource && asset.resourceType == luaScriptResourceType);
}

using Rows = std::vector<MergeReport::Conflict>;
void conflict(Rows& rows, const std::string& kept, const std::string& lost, const std::string& asset) {
    if (kept.empty() || lost.empty() || kept == lost) return;
    auto row = std::ranges::find_if(rows, [&](const MergeReport::Conflict& held) { return held.kept == kept && held.lost == lost; });
    if (row == rows.end()) row = rows.insert(rows.end(), {kept, lost, asset, 0});
    ++row->assets;
    if (auto area = asset_area(asset); std::ranges::find(row->areas, area) == row->areas.end()) row->areas.push_back(std::move(area));
}

// One mod's version of an asset in a bundle that is not the game's.
struct Change {
    std::uint64_t key{};
    std::size_t at{};       // the asset's place in the manifest's EBX, for reading it
    const fb::BundleAsset* asset{};
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

} // namespace

// Everything read so far, kept between checks. Map nodes stay where they are, so what points into
// them (bundles into their TOC, changes into their copy's asset list) stays good.
struct ConflictChecker::State {
    fs::path baseRoot, gameRoot, output;
    std::optional<vfs::Layout> layout;
    std::optional<CasStore> store;

    struct Toc {
        bool read{};
        std::optional<fb::TocDocument> document;
        std::map<std::string, const fb::TocBundle*, std::less<>> bundles;   // by lower-case name
    };
    // The game's copy of a bundle: its asset list, and its version of each asset that can
    // conflict (sha1, place among the EBX).
    struct GameBundle {
        bool read{};
        std::optional<Listing> listing;
        std::unordered_map<std::uint64_t, std::pair<fb::Sha1, std::size_t>> original;
    };
    // A mod's copy of a bundle: its asset list, and of that what can conflict and is not the game's.
    struct Copy {
        bool read{};
        std::optional<Listing> listing;
        std::vector<Change> changes;
    };
    struct ModState {
        std::string fingerprint;
        RelativeFiles files;
        std::map<std::string, Toc, std::less<>> tocs;       // by lower-case superbundle
        std::map<std::string, Copy, std::less<>> copies;    // by "superbundle|bundle"
    };
    std::map<std::string, Toc, std::less<>> gameTocs;
    std::map<std::string, GameBundle, std::less<>> gameBundles;   // by "superbundle|bundle"
    std::map<std::wstring, ModState, std::less<>> mods;           // by folder
    // Whether the edits to an EBX, in that order, could not be combined: by the bundle, the asset
    // and each edit's sha1. Read again for each check that meets them otherwise.
    std::mutex verdictMutex;
    std::unordered_map<std::string, bool> verdicts;

    static void read_toc(Toc& toc, const fs::path& path) {
        toc.read = true;
        std::error_code error;
        if (!fs::is_regular_file(path, error)) return;
        toc.document = fb::read_toc(read_file(path));
        for (const auto& bundle : toc.document->bundles) toc.bundles.emplace(lower(bundle.name), &bundle);
    }
};

namespace {
// One bundle that two or more of the checked mods ship in one superbundle.
struct Shared {
    std::string key;                                                // "superbundle|bundle"
    ConflictChecker::State* state{};
    const ConflictChecker::State::GameBundle* game{};
    std::vector<std::pair<std::size_t, const ConflictChecker::State::Copy*>> copies;   // (mod, its copy), lowest priority first
};

// The conflicts in one bundle, from the changes its copies were found to ship.
Rows decide(const Shared& shared, const std::vector<const Mod*>& mods, ConflictChecker::State& state) {
    Rows rows;
    if (!shared.game) return rows;
    struct Seen {
        std::size_t copy{};
        const Change* change{};
    };
    // Each asset's changes, lowest priority first, the assets in the order they were first met.
    std::unordered_map<std::uint64_t, std::vector<Seen>> byAsset;
    std::vector<std::uint64_t> order;
    for (std::size_t copy = 0; copy < shared.copies.size(); ++copy)
        for (const auto& change : shared.copies[copy].second->changes) {
            auto& list = byAsset[change.key];
            if (list.empty()) order.push_back(change.key);
            list.push_back({copy, &change});
        }
    const auto mod_of = [&](const Seen& seen) -> const std::string& { return mods[shared.copies[seen.copy].first]->name; };
    for (const auto key : order) {
        const auto& changes = byAsset.at(key);
        const auto& kept = changes.back();
        // Two mods' different changes, or nothing to decide.
        if (std::ranges::all_of(changes, [&](const Seen& seen) {
                return seen.change->asset->sha1 == kept.change->asset->sha1 || mod_of(seen) == mod_of(kept); }))
            continue;
        // What two mods add under one name is content: the higher mod's is kept and both load.
        const auto shipped = shared.game->original.find(key);
        if (shipped == shared.game->original.end()) continue;

        if (kept.change->asset->kind != fb::AssetKind::ebx) {
            // One of the game's Lua scripts: only the highest-priority mod's version runs, so every
            // mod lower down that changed it differently loses to each mod above it with another
            // version (which of those it loses to depends on which of them load). (The rule of
            // absorb's own_change in mod_merge_combine.cpp.)
            std::vector<const Seen*> earlier;
            for (const auto& seen : changes) {
                for (const auto* lower : earlier)
                    if (lower->change->asset->sha1 != seen.change->asset->sha1)
                        conflict(rows, mod_of(seen), mod_of(*lower), seen.change->asset->name);
                earlier.push_back(&seen);
            }
            continue;
        }

        // EBX the game has is combined, the changes of every mod onto the game's copy: a conflict only
        // when that cannot be done (the kept-whole and could-not-combine cases of the merge's combine).
        std::string verdictKey = shared.key + '|' + std::to_string(key);
        for (const auto& seen : changes)
            verdictKey.append(reinterpret_cast<const char*>(seen.change->asset->sha1.bytes.data()), seen.change->asset->sha1.bytes.size());
        std::optional<bool> uncombinable;
        {
            std::lock_guard lock(state.verdictMutex);
            if (const auto known = state.verdicts.find(verdictKey); known != state.verdicts.end()) uncombinable = known->second;
        }
        if (!uncombinable) {
            try {
                const auto base = fb::ebx::read_document(
                    read_asset(*state.store, state.baseRoot, state.baseRoot, *shared.game->listing, shipped->second.second, state.gameRoot));
                // The combined material grid is worked out separately (mod_merge_grid.cpp), and how an item
                // looks is content: the higher mod's is kept.
                if (lower(base.rootType).find("materialgrid") != std::string::npos || content_type(base.rootType)) {
                    uncombinable = false;
                } else {
                    std::map<fb::Sha1, fb::ebx::Document> documents;
                    std::vector<const fb::ebx::Document*> edits;
                    for (const auto& seen : changes) {
                        auto [at, fresh] = documents.try_emplace(seen.change->asset->sha1);
                        if (fresh)
                            at->second = fb::ebx::read_document(read_asset(*state.store, mods[shared.copies[seen.copy].first]->directory,
                                state.baseRoot, *shared.copies[seen.copy].second->listing, seen.change->at, state.gameRoot));
                        edits.push_back(&at->second);
                    }
                    fb::ebx::MergeSummary summary;
                    (void)fb::ebx::merge_documents(base, edits, &summary);
                    uncombinable = summary.dropped.empty() && !summary.instances && !summary.arrayEntries &&
                                   (!summary.values || !summary.exact());
                }
            } catch (const std::exception&) {
                uncombinable = true;
            }
            std::lock_guard lock(state.verdictMutex);
            state.verdicts.emplace(std::move(verdictKey), *uncombinable);
        }
        if (*uncombinable)
            for (const auto& seen : changes)
                if (seen.change->asset->sha1 != kept.change->asset->sha1)
                    conflict(rows, mod_of(kept), mod_of(seen), kept.change->asset->name);
    }
    return rows;
}
} // namespace

ConflictChecker::ConflictChecker(std::filesystem::path data_root) : state_(std::make_unique<State>()) {
    state_->gameRoot = data_root;
    state_->baseRoot = data_root / L"Data";
    state_->output = data_root / mods_folder / generated_folder;
}

ConflictChecker::~ConflictChecker() = default;

std::vector<MergeReport::Conflict> ConflictChecker::check(const std::vector<const Mod*>& mods, std::size_t threads,
                                                          bool background) noexcept {
    try {
        auto& state = *state_;
        if (!state.store) {
            state.layout = vfs::read_layout(state.baseRoot / L"layout.toc");
            state.store.emplace(state.baseRoot, state.output, state.layout->root);
        }
        // What was read is kept, the files are not: between checks a mod's archives must be free
        // to be replaced (an update, an uninstall).
        struct Close {
            const CasStore& store;
            ~Close() { store.close_all(); }
        } close{*state.store};
        // Each mod as read before, unless its files changed since. (Looking at every mod's files
        // is most of a check that finds nothing new: on several threads too.)
        std::vector<State::ModState*> held(mods.size());
        for (std::size_t mod = 0; mod < mods.size(); ++mod) held[mod] = &state.mods[mods[mod]->directory.wstring()];
        in_parallel(mods.size(), threads, background, [&](std::size_t mod) {
            auto& known = *held[mod];
            const auto fingerprint = mod_fingerprint(mods[mod]->directory);
            if (!fingerprint.empty() && known.fingerprint == fingerprint) return;
            known = {};
            known.fingerprint = fingerprint;
            known.files = scan(mods[mod]->directory);
        });
        // The superbundles two or more mods ship: only there can two mods' versions of an asset meet.
        std::map<std::string, std::vector<std::size_t>> shippedBy;   // lower case, mods highest priority first
        std::map<std::string, std::string> spelled;
        for (std::size_t mod = 0; mod < mods.size(); ++mod)
            for (const auto& relative : held[mod]->files.tocs) {
                auto key = lower(relative);
                spelled.try_emplace(key, relative);
                shippedBy[std::move(key)].push_back(mod);
            }
        std::erase_if(shippedBy, [](const auto& entry) { return entry.second.size() < 2; });

        // The bundles two or more of them ship there, with their copies and the game's.
        std::vector<Shared> bundles;
        std::vector<std::pair<State::GameBundle*, const fb::TocBundle*>> gameUnread;
        struct CopyUnread {
            State::Copy* copy;
            const fs::path* folder;
            const fb::TocBundle* bundle;
            const State::GameBundle* game;
        };
        std::vector<CopyUnread> copyUnread;
        for (const auto& [relative, by] : shippedBy) {
            std::map<std::string, std::vector<std::pair<std::size_t, const fb::TocBundle*>>> named;
            for (auto mod = by.rbegin(); mod != by.rend(); ++mod) {
                auto& toc = held[*mod]->tocs[relative];
                if (!toc.read) try {
                    State::read_toc(toc, mods[*mod]->directory / fs::path(spelled.at(relative)));
                } catch (const std::exception&) {}   // (the merge leaves a mod out whose TOC cannot be read)
                for (const auto& [name, bundle] : toc.bundles) named[name].emplace_back(*mod, bundle);
            }
            std::erase_if(named, [](const auto& entry) { return entry.second.size() < 2; });
            if (named.empty()) continue;
            auto& gameToc = state.gameTocs[relative];
            if (!gameToc.read) try {
                State::read_toc(gameToc, state.baseRoot / fs::path(spelled.at(relative)));
            } catch (const std::exception&) {}
            for (const auto& [name, copies] : named) {
                Shared shared{relative + '|' + name, &state};
                const auto gameBundle = gameToc.bundles.find(name);
                // Without the game's copy, nothing in it can conflict: what mods add is content.
                if (gameBundle == gameToc.bundles.end()) continue;
                auto& game = state.gameBundles[shared.key];
                if (!game.read) gameUnread.emplace_back(&game, gameBundle->second);
                shared.game = &game;
                for (const auto& [mod, bundle] : copies) {
                    auto& copy = held[mod]->copies[shared.key];
                    if (!copy.read) copyUnread.push_back({&copy, &mods[mod]->directory, bundle, &game});
                    shared.copies.emplace_back(mod, &copy);
                }
                bundles.push_back(std::move(shared));
            }
        }
        if (bundles.empty()) return {};

        // On several threads, in three steps, so that one bundle a hundred mods ship is not all one
        // thread's: the game's copies not read before, then each mod's copy not read before, then what
        // the copies' changes mean in each bundle. The rows are then put together in bundle order.
        in_parallel(gameUnread.size(), threads, background, [&](std::size_t index) {
            auto& [game, bundle] = gameUnread[index];
            game->read = true;
            game->listing = list_bundle(*state.store, state.baseRoot, state.baseRoot, *bundle, state.gameRoot);
            if (!game->listing) return;
            const auto& manifest = game->listing->manifest;
            for (std::size_t at = 0; at < manifest.ebx.size(); ++at)
                game->original.emplace(key_hash(manifest.ebx[at]), std::pair{manifest.ebx[at].sha1, at});
            for (const auto& asset : manifest.resources)
                if (can_conflict(asset)) game->original.emplace(key_hash(asset), std::pair{asset.sha1, std::size_t{}});
        });
        in_parallel(copyUnread.size(), threads, background, [&](std::size_t index) {
            const auto& unread = copyUnread[index];
            auto& copy = *unread.copy;
            copy.read = true;
            copy.listing = list_bundle(*state.store, *unread.folder, state.baseRoot, *unread.bundle, state.gameRoot);
            if (!copy.listing) return;   // (the merge notes what it cannot read)
            const auto take = [&](const std::vector<fb::BundleAsset>& list, bool ebx) {
                for (std::size_t at = 0; at < list.size(); ++at) {
                    const auto& asset = list[at];
                    if (!can_conflict(asset)) continue;
                    const auto key = key_hash(asset);
                    const auto shipped = unread.game->original.find(key);
                    if (shipped == unread.game->original.end() || shipped->second.first == asset.sha1) continue;
                    copy.changes.push_back({key, ebx ? at : 0, &asset});
                }
            };
            take(copy.listing->manifest.ebx, true);
            take(copy.listing->manifest.resources, false);
        });
        std::vector<Rows> found(bundles.size());
        in_parallel(bundles.size(), threads, background, [&](std::size_t index) {
            found[index] = decide(bundles[index], mods, state);
        });
        Rows rows;
        for (const auto& bundle : found)
            for (const auto& row : bundle) {
                const auto same = std::ranges::find_if(rows, [&](const MergeReport::Conflict& held) {
                    return held.kept == row.kept && held.lost == row.lost; });
                if (same == rows.end()) {
                    rows.push_back(row);
                    continue;
                }
                same->assets += row.assets;
                for (const auto& area : row.areas)
                    if (std::ranges::find(same->areas, area) == same->areas.end()) same->areas.push_back(area);
            }
        return rows;
    } catch (...) {
        return {};
    }
}

std::map<std::string, ConflictLoss, std::less<>> conflict_losses(const std::vector<std::string>& order,
    const std::vector<MergeReport::Conflict>& conflicts, const std::set<std::string, std::less<>>& leaving,
    const std::function<bool(const std::string&)>& exempt) {
    std::map<std::string, ConflictLoss, std::less<>> losses;
    auto out = leaving;
    for (const auto& name : order) {
        if (out.contains(name) || (exempt && exempt(name))) continue;
        ConflictLoss loss;
        for (const auto& row : conflicts) {
            if (row.lost != name || out.contains(row.kept)) continue;
            if (loss.winner.empty()) loss.winner = row.kept;
            loss.rows.push_back(&row);
        }
        if (loss.rows.empty()) continue;
        out.insert(name);
        losses.emplace(name, std::move(loss));
    }
    return losses;
}

std::string conflict_winner(const Mod& mod) {
    constexpr std::string_view prefix = "conflicts with ";
    if (mod.problems.empty() || !mod.problems.front().starts_with(prefix)) return {};
    const auto& problem = mod.problems.front();
    return problem.substr(prefix.size(), problem.find(':') - prefix.size());
}

bool content_type(std::string_view root_type) {
    const auto type = lower(root_type);
    return type.find("appearance") != std::string::npos || type.ends_with("textureasset") || type.ends_with("meshasset");
}

std::string asset_area(std::string_view asset) {
    const auto name = lower(asset);
    if (name.find("lua") != std::string::npos) return "scripts";
    if (name.starts_with("gameplay/characters/data/recipe") || name.starts_with("characters/")) return "skater setup";
    static constexpr std::pair<std::string_view, std::string_view> areas[]{
        {"ui", "menus"}, {"items", "items"}, {"levels", "maps"}, {"gameplay", "gameplay"}, {"audio", "sounds"},
        {"sound", "sounds"}, {"cameras", "cameras"}, {"camera", "cameras"}, {"physics", "physics"},
        {"thumbnail", "thumbnails"}, {"fx", "effects"}, {"vfx", "effects"}};
    const auto first = std::string_view(name).substr(0, name.find('/'));
    for (const auto& [prefix, area] : areas)
        if (first == prefix) return std::string(area);
    return "game files";
}

} // namespace dingosdk::mods
