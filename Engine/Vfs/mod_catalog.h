#pragma once

#include "mod_list.h"
#include "mod_merge.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::mods {

struct Catalog {
    std::filesystem::path data_root;  // what the engine mounts as /native_data
    std::filesystem::path root;       // <data_root>/Mods
    bool present{};                   // the Mods folder exists
    // Enabled mods, highest priority first. A later mod never shadows an
    // earlier one.
    std::vector<Mod> mods;
    // Folders present on disk that mods.json disables, in folder-name order.
    std::vector<std::string> disabled;
    // Disabled mods that ship a layout. Their archives are placed in the
    // launch's merge too, so one can be enabled while the game runs.
    std::vector<Mod> inactive;
    // True once Mods/.reskate holds the merged patch every mod was combined
    // into. Empty catalogues never generate one.
    bool merged{};
    // Non-empty when mods.json was rejected; nothing is loaded in that case.
    std::string issue;
    // Advisory messages worth logging, such as skipped or missing folders.
    std::vector<std::string> notes;
    // Enabled mods left out entirely because they could not be merged cleanly
    // (their problems say why). A partly merged mod would list maps that never
    // finish loading, so a mod either merges whole or not at all.
    std::vector<Mod> excluded;
    // Mods left out, one line each, for the warning log.
    std::vector<std::string> warnings;
    // Mods left out because they were built for another game version (Mod::outdated), enabled or
    // not: they need updating. Also in `excluded` when they were enabled.
    std::vector<Mod> outdated;
    // Mods with variants (mod_list.h Variants): the variant of each that is the one in the game, by
    // the declaring mod's folder name (none listed: none of its variants loads); and the enabled
    // mods set aside for them, their other variants and other mods that bring what a variant
    // displaces. Those are not loaded and nothing about them waits to be applied.
    std::map<std::string, std::string, std::less<>> variants_in_play;
    std::vector<std::string> set_aside;
};

// Variants (mod_list.h). For the run each variant is a mod of its own, listed after the mod that
// holds it as <mod's folder>#<variant>, with that mod's build stamp; mods.json and the mod list only
// know the mod. One variant of each such mod loads, the one the player chose (else the first of its
// "order", else the first there is), first in the load order whatever mods.json says: what would
// fight it is set aside, whole, and its archives are placed like a disabled mod's, so another variant
// can be chosen while the game runs. The variant a mod's entry stands for, and the mod that holds
// it, or nothing (no folder has a # in its name):
std::string variant_of(std::string_view mod);
std::string variant_owner(std::string_view mod);
// The variants a mod folder holds, its "order" first, then the others by name; and the folder with a
// variant's own files (empty when it has no such variant).
std::vector<std::string> installed_variants(const Mod& mod);
std::filesystem::path variant_folder(const Mod& mod, std::string_view variant);
// The variant chosen for the mod with this folder name, empty when none was: read whenever the mods
// are put together, at launch and at every live apply. Saving a choice makes it this run's at once.
std::string chosen_variant(std::string_view mod) noexcept;
bool save_variant(std::string_view mod, std::string_view variant) noexcept;
// What the mods in effect give the game: the launch's catalogue, or the one the last live apply
// read (live_mods tells with variants_applied). Empty: none of that mod's variants is in the game.
std::string variant_in_play(std::string_view mod) noexcept;
std::vector<std::string> set_aside_for_variants();
void variants_applied(const Catalog& catalog) noexcept;

// The engine's own data root: the -dataPath argument when present, otherwise
// the executable's directory. Mirrors the game so mod paths resolve the same.
std::filesystem::path engine_data_root() noexcept;

// Reads <data_root>/Mods plus its mods.json and, when any mod adds a
// superbundle, writes the merged manifest. Never throws; problems land in
// `issue` and `notes`. `observe` hears how a merge that is rebuilding goes.
// `run_merge` false only reads the folder and mods.json, for a merge the caller runs itself (a live one).
Catalog load_catalog(const std::filesystem::path& data_root, const MergeObserver& observe = {},
                     bool run_merge = true) noexcept;

// Process-wide catalogue, loaded once from engine_data_root() on first use.
// Only that first call's `observe` is ever used.
const Catalog& catalog(const MergeObserver& observe = {}) noexcept;

// reskate-levels.json for every enabled mod, highest priority first.
std::vector<std::filesystem::path> level_manifest_paths(const Catalog& catalog);

// The enabled mod whose reskate-levels.json registers a level named inside
// `destination` (a level path or a loader's "root + level @ start" line),
// ignoring ASCII case; null when no mod does.
const Mod* mod_registering(const Catalog& catalog, std::string_view destination) noexcept;

// One line describing a mod for the log: what built it, what it ships, and any
// problem the merge had with it.
std::string describe(const Mod& mod);

} // namespace dingosdk::mods
