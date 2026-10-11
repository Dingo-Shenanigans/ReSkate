#include "mod_scoring.h"

#include "mod_catalog.h"
#include "mod_merge_internal.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>

namespace dingosdk::mods {
using namespace detail;
namespace {
// Per-trick points, the scoring managers' multipliers and graphs, and the
// throwdowns that add tricks up into a score and rank the players.
// Then how the skater handles: the core physics configs (wipeouts, grinds, input),
// the board's, the friction tuning and the gestures that pick each trick. Their
// animation and IK configs are left out (looks only), and SkatePhysicsTuning too:
// multiplayer already skates with the host's or the game's own.
constexpr std::array<std::string_view, 18> scoring_prefixes{
    "gameplay/scorables/",
    "ecs/scoringmanager/",
    "ecs/legacyscoringmanager/",
    "animation/dingo/skatercorephysicsscoringconfig",
    "gameplay/activity/components/throwdowns/",
    "gameplay/activity/prefabs/throwdowns/",
    "ecs/activities/throwdowns/",
    "activities/leaderboard/",
    "animation/dingo/skatercorephysicsconfig",
    "animation/dingo/skatercorephysicsfeatureflagconfig",
    "animation/dingo/skatercorephysicswipeoutconfig",
    "animation/dingo/skatercorephysicsinputconfig",
    "animation/dingo/skatercorephysicsgrindinputconfig",
    "animation/dingo/skatercorephysicsgestureselector",
    "animation/dingo/skatercorephysicsmetricconfig",
    "animation/dingo/skateboardcorephysics_config",
    "gameplay/skater/tuning/",
    "gameplay/input/gestures/",
};
// The core level carries the game's own copy of every scoring asset; each
// district's level bundle carries copies of the same.
constexpr std::string_view reference_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";

std::string key_of(const fb::BundleAsset& asset) {
    return (asset.kind == fb::AssetKind::ebx ? "ebx " : "res ") + lower(asset.name);
}

std::string hex(const fb::Sha1& sha1) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto value : sha1.bytes) {
        const auto byte = static_cast<unsigned>(value);
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

std::uint64_t fnv(std::uint64_t hash, std::string_view text) {
    for (const auto c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
    return hash;
}

std::mutex state_mutex;
ScoringState state;

class Scan {
public:
    Scan(const Catalog& catalog, std::vector<std::string>* notes)
        : baseRoot_(catalog.data_root / L"Data"), gameRoot_(catalog.data_root),
          layout_(vfs::read_layout(baseRoot_ / L"layout.toc")),
          store_(baseRoot_, catalog.root / generated_folder, layout_.root), notes_(notes) {}

    // Every scoring asset the game ships in the bundles of one of its TOCs.
    void read_game(std::string_view relative) {
        const auto* toc = game_toc(relative);
        if (!toc) return;
        for (const auto& [name, bundle] : *toc) read_game_bundle(relative, *bundle);
    }

    // A mod's copies of scoring assets that differ from every copy the game ships.
    void read_mod(const Mod& mod, std::map<std::string, std::set<fb::Sha1>>& changes) {
        for (const auto& relative : scan(mod.directory).tocs) {
            fb::TocDocument own;
            try {
                own = fb::read_toc(read_file(mod.directory / fs::path(relative)));
            } catch (const std::exception& failure) {
                note(mod.name + ": " + relative + " could not be read for the scoring check (" + failure.what() + ")");
                continue;
            }
            const auto* game = game_toc(relative);
            for (const auto& bundle : own.bundles) {
                try {
                    // A bundle the mod only passes through reads every file from the game.
                    const auto region = fb::read_bundle_region(bundle.region);
                    if (std::none_of(region.files.begin(), region.files.end(),
                                     [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                        continue;
                    const auto listing = list_bundle(store_, mod.directory, baseRoot_, bundle, gameRoot_);
                    if (!listing) continue;
                    std::vector<const fb::BundleAsset*> scoring;
                    for (const auto* list : {&listing->manifest.ebx, &listing->manifest.resources})
                        for (const auto& asset : *list)
                            if (scoring_asset(lower(asset.name))) scoring.push_back(&asset);
                    if (scoring.empty()) continue;
                    if (game)
                        if (const auto shipped = game->find(lower(bundle.name)); shipped != game->end())
                            read_game_bundle(relative, *shipped->second);
                    for (const auto* asset : scoring) {
                        const auto known = game_.find(key_of(*asset));
                        // Not one of the game's assets: it changes nothing the game scores with.
                        if (known == game_.end() || known->second.contains(asset->sha1)) continue;
                        changes[key_of(*asset)].insert(asset->sha1);
                    }
                } catch (const std::exception& failure) {
                    note(mod.name + ": " + bundle.name + " could not be read for the scoring check (" + failure.what() + ")");
                }
            }
        }
    }

private:
    using Bundles = std::map<std::string, const fb::TocBundle*, std::less<>>;

    const Bundles* game_toc(std::string_view relative) {
        const auto key = lower(relative);
        if (const auto found = tocs_.find(key); found != tocs_.end())
            return found->second.index.empty() ? nullptr : &found->second.index;
        auto& entry = tocs_[key];
        std::error_code error;
        const auto path = baseRoot_ / fs::path(std::string(relative));
        if (!fs::is_regular_file(path, error)) return nullptr;
        try {
            entry.document = fb::read_toc(read_file(path));
        } catch (const std::exception& failure) {
            note("The game's " + std::string(relative) + " could not be read for the scoring check (" + failure.what() + ")");
            return nullptr;
        }
        for (const auto& bundle : entry.document.bundles) entry.index.emplace(lower(bundle.name), &bundle);
        return &entry.index;
    }

    void read_game_bundle(std::string_view relative, const fb::TocBundle& bundle) {
        if (!read_.insert(lower(relative) + '|' + lower(bundle.name)).second) return;
        try {
            const auto listing = list_bundle(store_, baseRoot_, baseRoot_, bundle, gameRoot_);
            if (!listing) return;
            for (const auto* list : {&listing->manifest.ebx, &listing->manifest.resources})
                for (const auto& asset : *list)
                    if (scoring_asset(lower(asset.name))) game_[key_of(asset)].insert(asset.sha1);
        } catch (const std::exception& failure) {
            note("The game's " + bundle.name + " could not be read for the scoring check (" + failure.what() + ")");
        }
    }

    void note(std::string text) {
        if (notes_) notes_->push_back(std::move(text));
    }

    struct GameToc {
        fb::TocDocument document;
        Bundles index;
    };
    fs::path baseRoot_, gameRoot_;
    vfs::Layout layout_;
    CasStore store_;
    std::vector<std::string>* notes_;
    std::map<std::string, GameToc, std::less<>> tocs_;
    std::set<std::string> read_;                              // game bundles already read
    std::map<std::string, std::set<fb::Sha1>> game_;          // every copy the game ships, by asset
};
} // namespace

bool scoring_asset(std::string_view name) noexcept {
    return std::any_of(scoring_prefixes.begin(), scoring_prefixes.end(),
                       [&](std::string_view prefix) { return name.starts_with(prefix); });
}

ScoringCheck check_scoring(const Catalog& catalog, std::vector<std::string>* notes) noexcept {
    ScoringCheck result;
    try {
        std::vector<const Mod*> mods;
        for (const auto& mod : catalog.mods)
            if (mod.provides_layout) mods.push_back(&mod);
        if (mods.empty()) return result;
        Scan scan(catalog, notes);
        scan.read_game(reference_toc);
        std::map<std::string, std::set<fb::Sha1>> all;
        for (const auto* mod : mods) {
            std::map<std::string, std::set<fb::Sha1>> changes;
            scan.read_mod(*mod, changes);
            if (changes.empty()) continue;
            result.mods.push_back(mod->name);
            for (auto& [key, versions] : changes) all[key].insert(versions.begin(), versions.end());
        }
        if (all.empty()) return result;
        // FNV-1a over every changed asset and version, in order: the same mods give the same value.
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (const auto& [key, versions] : all) {
            result.assets.push_back(key.substr(4));
            for (const auto& sha1 : versions) hash = fnv(hash, key + ' ' + hex(sha1) + '\n');
        }
        std::ranges::sort(result.assets); // "ebx "/"res " keys sort apart once the prefix is gone
        result.assets.erase(std::unique(result.assets.begin(), result.assets.end()), result.assets.end());
        result.fingerprint = hash ? hash : 1;
    } catch (const std::exception& failure) {
        if (notes) notes->push_back(std::string("The scoring check could not run: ") + failure.what());
        result = {};
    } catch (...) {
        result = {};
    }
    return result;
}

void publish_scoring(const ScoringCheck& check) noexcept {
    try {
        std::lock_guard lock(state_mutex);
        if (!state.known || !state.fingerprint) {
            state = {true, check.fingerprint, check.mods};
            return;
        }
        if (!check.fingerprint || check.fingerprint == state.fingerprint) return;
        auto combined = fnv(state.fingerprint, std::to_string(check.fingerprint));
        state.fingerprint = combined ? combined : 1;
        for (const auto& mod : check.mods)
            if (std::find(state.mods.begin(), state.mods.end(), mod) == state.mods.end()) state.mods.push_back(mod);
    } catch (...) {}
}

ScoringState scoring_state() noexcept {
    try {
        std::lock_guard lock(state_mutex);
        return state;
    } catch (...) {
        return {};
    }
}

} // namespace dingosdk::mods
