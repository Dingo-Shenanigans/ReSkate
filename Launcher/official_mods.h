#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Official mods: Thunderstore packages made for ReSkate's own features, listed on the launcher's
// OFFICIAL tab, where the player installs, updates and turns them off. The ReSkate backend names
// them (GET /api/v1/official-mods, beside the verified mods), so one is added or dropped without a
// release; the launcher keeps the last list it read, and has one of its own for a first launch with
// no network. Installed, they live in Mods/.reskate-official/<Namespace-Name>
// (Engine/Vfs/builtin_packs.h), which the game merges after every installed mod.
//
// As the launcher starts it reads the list and, on a thread of its own, looks up each one's latest
// version: a copy the player installed from Thunderstore is moved over, and copies left in the
// Mods folder (and mods retired with an official one) go to the Recycle Bin and out of mods.json.
// Nothing is downloaded until the player asks (install()). Play waits for that and for downloads
// under way.
namespace dingosdk::official_mods {
struct Mod {
    std::string full_name;   // Namespace-Name
    std::string title, description, icon;
    std::string version;     // the latest the list or Thunderstore gave; empty when not known
    std::string download_url;
    std::string installed;   // the version in place; empty when none is
    std::string status;      // what the launcher did with it, for the OFFICIAL tab
    bool working{};          // being downloaded or put in place now
};
struct State {
    std::vector<Mod> mods;
    std::string source;      // where the list came from: "ReSkate", "the last list read" or "ReSkate's own list"
    bool syncing{}, synced{};
};

// Puts the official mods in place under `mods_root`, once a launch (later calls do nothing).
void start(const std::filesystem::path& mods_root);
// Waits for that and any install() under way, at most `timeout`. False while one still runs (or it
// never started).
bool wait(std::chrono::milliseconds timeout);
// Downloads the latest version of the official mod `full_name` and puts it in place under
// `mods_root`, on a thread of its own; snapshot() shows how it goes. Does nothing while the list is
// still being read or the mod is already being downloaded.
void install(const std::filesystem::path& mods_root, const std::string& full_name);
// Takes the installed official mod `full_name` away (its folder, and whether it was turned off), on
// a thread of its own; it can be installed again. Does nothing while it is being downloaded.
void uninstall(const std::filesystem::path& mods_root, const std::string& full_name);
State snapshot();
// Whether the Thunderstore package `full_name` is official (or retired with one): the browser
// neither lists nor installs it.
bool official(std::string_view full_name);

// The pieces, for tests.
// The backend's answer: its mods, or none when it is not that answer.
std::vector<Mod> parse_list(std::string_view json);
// The list ReSkate falls back on when it has never read one: Fourth Studio's Board Break pack and
// Road Rash.
std::vector<Mod> fallback_list();
// For each official mod not in place yet, a copy in the Mods folder is moved in (stamped with its
// manifest's version); the copies left in the Mods folder and retired mods go to the Recycle Bin;
// mods.json forgets what went. Returns what was done, a line each. Never throws.
std::vector<std::string> adopt_and_retire(const std::filesystem::path& mods_root, const std::vector<Mod>& official);
}
