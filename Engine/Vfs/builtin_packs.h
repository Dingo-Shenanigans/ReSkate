#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Game content ReSkate carries itself: packs of game assets built by ReSkate Studio, zipped into
// ReSkate.dll (the FPV drone's model, tools/models/build_drone_pack.py). Nobody installs or sees
// them. Official mods (below) are the other content ReSkate keeps for every player. Before a merge each is unpacked under Mods/.reskate-builtin/<name> (a dot folder,
// which no mod list shows) when the one embedded differs from what is there, and the catalogue
// merges it after every installed mod, always on (mod_catalog.cpp).
namespace dingosdk::mods {
inline constexpr std::string_view builtin_folder = ".reskate-builtin";
struct BuiltinPack {
    std::string name;                 // as the merge knows it: reskate-builtin-<pack>
    std::filesystem::path directory;  // Mods/.reskate-builtin/<pack>
};
// The packs ReSkate.dll carries, unpacked and current under `mods_root`. ReSkate.dll is the
// module running this when it is loaded (the game); otherwise the copy beside this program (the
// launcher) is opened as data. Never throws; `notes` says what was unpacked or went wrong.
std::vector<BuiltinPack> unpack_builtin_packs(const std::filesystem::path& mods_root,
                                              std::vector<std::string>& notes) noexcept;
// One pack's bytes as ReSkate.dll carries them (builtin_pack_format.h, or a plain zip) unpacked
// into `target`; false with `issue` saying why. For tests and tools: unpack_builtin_packs uses it.
bool unpack_builtin_pack(std::span<const unsigned char> bytes, const std::filesystem::path& target, std::string& issue) noexcept;

// Official mods (Launcher/official_mods.h): Thunderstore packages the ReSkate backend names, which
// the player installs on the launcher's OFFICIAL tab. Each is a mod like any other, in
// Mods/<Namespace-Name> and in mods.json (switched on and off there, and placed in the load
// order), with a stamp file holding its version (written last, so a folder without one is not a
// whole official mod). One switched off does not load, and the feature it is made for does not start.
inline constexpr std::string_view official_stamp = ".official-version";
// Where official mods were kept before they were mods like any other: Mods/.reskate-official, with
// the ones turned off listed in its official-off.txt. move_official_mods takes them out of it; the
// launcher keeps the last list of official mods it read there.
inline constexpr std::string_view official_folder = ".reskate-official";
inline constexpr std::string_view official_off_file = "official-off.txt";
// The official mods the features are made for, by folder name (Namespace-Name).
inline constexpr std::string_view board_break_official = "Fourth_Studio-Board_Break_Pack";
inline constexpr std::string_view road_rash_official = "Fourth_Studio-Road_Rash";
// Moves the official mods kept in Mods/.reskate-official into the Mods folder, each at the bottom
// of the load order (where they loaded from there), switched off when they were turned off there.
// Says what it did, a line each. Does nothing once there is nothing left there. Never throws.
std::vector<std::string> move_official_mods(const std::filesystem::path& mods_root) noexcept;
// Whether the mod folder `directory` is an official mod (it has its stamp). Never throws.
bool official_mod(const std::filesystem::path& directory) noexcept;
// Whether the player switched the official mod `folder` off (in mods.json). Never throws.
bool official_off(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
// Switches it off or back on; false when mods.json could not be written. Never throws.
bool set_official_off(const std::filesystem::path& mods_root, std::string_view folder, bool off) noexcept;
// Whether the official mod `folder` is installed (the player installs them on the launcher's
// OFFICIAL tab) and not switched off: what the feature it is made for needs to start. Never throws.
bool official_in_use(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
// Mods that came before an official one and are retired with it (Fourth Studio's deprecated first
// Board Break), by folder name; the launcher takes them away and nothing loads them.
bool retired_mod(std::string_view folder) noexcept;
// Whether the installed mod `folder` (Mods/<folder>) is left out because ReSkate sees to it: a
// retired one. Never throws.
bool kept_by_reskate(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
} // namespace dingosdk::mods
