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
    // Board Break: the name of its deck that is the one in the game ("standard", "wide" and so on),
    // empty when none is and a board cannot break; and the enabled mods set aside for it, its
    // other decks and other mods that bring a deck model. Those are not loaded and nothing about
    // them waits to be applied.
    std::string board_break_deck;
    std::vector<std::string> set_aside;
};

// Board Break's decks (Extension/BoardBreak/board_break.h). The game has one deck model for every
// board, so each deck that can break is a copy of it in a content mod, and the deck the player
// chose is the one that loads. A deck comes in one of two ways:
//   - a mod whose folder is named Board_Break_<Name> (also behind a team name, as a mod manager
//     installs it: Team-Board_Break_<Name>) holds the deck <name>;
//   - a deck pack, a mod of any name with a folder decks/<name>/ for each deck it brings, laid out
//     like a mod folder (layout.toc, Win32/...). One package can bring every deck that way, where
//     the launcher installs one mod folder per package. For the run each deck inside is a mod of
//     its own, listed as <pack's folder>#<name>. The pack's own layout is not one of its decks,
//     whatever the folder is named, and is set aside like any other deck model. It is what an
//     older ReSkate loads from the pack: in a folder named Board_Break_<Name> the deck 2.0.6
//     breaks, which knows a deck by its folder's name only; under another name the game's own.
// A deck goes by its name in small letters. These are the decks the feature was made with, in the
// order the menus list them, and the console also knows them by their place here (deck 1 is the
// first). Any other deck comes after them, by its name: a new shape needs no change to ReSkate.
inline constexpr std::array<std::string_view, 10> board_break_decks{"standard", "wide", "concave", "powell", "slim",
                                                                    "egg", "square", "shaped", "oldschool", "pig"};
// The name of the deck a mod holds going by the mod's name (a folder's, or a pack's deck's), or
// nothing; and the folder name of the pack that one of a pack's decks is listed under, or nothing.
std::string board_break_deck_of(std::string_view mod);
std::string board_break_pack_of(std::string_view mod);
// The deck the player chose ("standard" when nothing was chosen): read whenever the mods are put
// together, at launch and at every live apply. Saving a choice makes it this run's at once.
std::string wanted_board_break_deck() noexcept;
bool save_board_break_deck(std::string_view deck) noexcept;
// The decks that are in the Mods folder, whatever mods.json says of their mods, in the menus'
// order; and the folders of the mods that bring them (a pack's once), for what a pack carries
// beside its decks.
std::vector<std::string> installed_board_break_decks() noexcept;
std::vector<std::filesystem::path> board_break_folders() noexcept;
// The folder with a deck's own files (a pack's decks/<name>, or the Board_Break_<Name> folder): what
// it says of itself (deck.ini) is there. Empty when that deck is not installed.
std::filesystem::path board_break_deck_folder(std::string_view deck) noexcept;
// What the mods in effect give the game: the launch's catalogue, or the one the last live apply
// read (live_mods tells with board_break_applied).
std::string board_break_in_play() noexcept;
std::vector<std::string> set_aside_for_board_break();
void board_break_applied(const Catalog& catalog) noexcept;

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
