#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dingosdk::mods {

struct Catalog;

struct MergeReport {
    bool built{};                       // Mods/.reskate holds a usable patch
    bool reused{};                      // it is the previous launch's, nothing having changed
    std::size_t superbundles{};         // superbundle TOCs written
    std::size_t mergedBundles{};        // bundles combined from two or more mods
    std::size_t mergedAssets{};         // EBX assets two or more mods had both edited
    std::size_t unchangedCopies{};      // mods' asset copies equal to the game's, not kept for combining
    std::size_t archives{};             // cas files linked in
    std::string issue;                  // non-empty when nothing was produced
    std::vector<std::string> notes;
    // What the merge could not use from each mod, keyed by mod folder name:
    // a bundle pointing past the end of an archive, a manifest that could not
    // be decoded, a copy that had to be left out. Each mod gets a summary line
    // from these, and a level that never finishes loading can name its mod.
    std::map<std::string, std::vector<std::string>> problems;
    // Two mods changing one asset in ways that cannot both be in the game: the copy of the mod
    // higher in the load order is kept and the other's changes are not in it. One row per pair of
    // mods (folder names), with one of the assets and how many. The catalog leaves the losing mod
    // out whole and merges again, so no mod loads half its changes.
    struct Conflict {
        std::string kept;
        std::string lost;
        std::string asset;
        std::size_t assets{};
        std::vector<std::string> areas;   // what those assets are, in a player's words (asset_area)
    };
    std::vector<Conflict> conflicts;
    // Nothing was built: the check before the merge found `conflicts` (MergeOptions::conflicts_first).
    bool stopped_for_conflicts{};
    // Every superbundle the merged mods' layouts assign to an install chunk,
    // as (superbundle, install chunk name). A live merge registers the ones
    // the game did not know at launch.
    std::vector<std::pair<std::string, std::string>> superbundle_chunks;
    // Superbundle TOCs a live merge removed because no enabled mod ships them now.
    std::vector<std::string> removed_tocs;
    // The loading screens enabled mods add for their levels (a live merge only).
    // The game reads its loading-screen configuration once, at launch, so a
    // level added since then is given its screen in memory.
    struct LoadScreen {
        std::string mod;
        std::string level;                  // the LevelOverrides row's LevelName
        std::string bundle;                 // the widget's bundle, as authored
        std::array<std::byte, 16> widget{}; // WidgetAssetGuid
    };
    std::vector<LoadScreen> load_screens;
    // Each custom map's own mesh geometry, every level of detail of every mesh in its level
    // bundles, by level folder (dingolevel_reskate_*). What its mesh streaming pool is sized from.
    std::map<std::string, std::uint64_t> map_meshes;
};

// Mods/.reskate/reskate-map-meshes.json: map_meshes as the patch was last built, a level folder to
// its bytes. A live merge updates the maps it saw and keeps the rest.
inline constexpr char map_meshes_file[] = "reskate-map-meshes.json";
// Empty when the file is missing or cannot be read.
[[nodiscard]] std::map<std::string, std::uint64_t> read_map_meshes(const std::filesystem::path& file);
void write_map_meshes(const std::filesystem::path& file, const std::map<std::string, std::uint64_t>& meshes);

// How far a merge has got. Only a merge that is really rebuilding reports; an
// unchanged one is reused without a word.
struct MergeProgress {
    std::size_t done{};
    std::size_t total{};
    std::size_t mods{};                 // how many mods are being merged
    std::string step;                   // what is being merged now
};
using MergeObserver = std::function<void(const MergeProgress&)>;

struct MergeOptions {
    // A merge while the game runs, between level loads. The engine keeps
    // archives it has read open, so nothing already in Mods/.reskate is
    // replaced or deleted: archives stay where the launch's merge put them,
    // new bundle manifests are appended to the end of archive 1, and the
    // superbundle TOCs (read afresh at each mount) are rewritten. layout.toc is
    // only read at startup and stays as it is; the stamp goes, so the next
    // launch builds a clean patch.
    bool live{};
    // Mods (folder names) already loaded in the running game, by the launch's merge or an
    // earlier live one: they passed the store-copies check then, so a live merge holds only
    // the others against it. Reading every mod's items and meshes again is most of what
    // adding one map would otherwise wait on.
    std::vector<std::string> checked;
    // Look for mods that conflict (ConflictChecker, mod_conflicts.h) before building anything, and when some do,
    // stop there (MergeReport::stopped_for_conflicts) so the caller merges once without them.
    bool conflicts_first{};
};

// Combines every enabled mod into one patch under Mods/.reskate: superbundle
// TOCs whose bundles carry the base assets plus each mod's additions, and each
// mod's cas archives re-indexed so they cannot collide. The engine composes
// exactly one patch per superbundle, so a single merged layer is the only shape
// that lets two mods touching the same bundle both load.
//
// The patch is kept between launches: when the SDK, every enabled mod's files
// and the base files they merge onto are all as they were, the previous build
// is reused instead of being rebuilt.
[[nodiscard]] MergeReport merge_mods(const Catalog& catalog, const MergeObserver& observe = {},
                                     const MergeOptions& options = {}) noexcept;

} // namespace dingosdk::mods
