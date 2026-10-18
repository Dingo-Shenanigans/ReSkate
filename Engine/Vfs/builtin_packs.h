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
// the player installs on the launcher's OFFICIAL tab into Mods/.reskate-official/<Namespace-Name>,
// each with a stamp file holding its version (written last, so a folder without one is not a whole
// mod). Merged after every installed mod, like the packs above, unless turned off. Not in
// mods.json, and no mod list shows them.
inline constexpr std::string_view official_folder = ".reskate-official";
inline constexpr std::string_view official_stamp = ".official-version";
// The official mods the player turned off (the launcher's OFFICIAL tab): their folder names, one a
// line, in Mods/.reskate-official/official-off.txt. One that is off stays installed, but nothing
// merges or lists it, and the feature it is made for does not start.
inline constexpr std::string_view official_off_file = "official-off.txt";
// The official mods the features are made for, by folder name (Namespace-Name).
inline constexpr std::string_view board_break_official = "Fourth_Studio-Board_Break_Pack";
inline constexpr std::string_view road_rash_official = "Fourth_Studio-Road_Rash";
// The official mods in place under `mods_root`, by folder name; without those turned off unless
// `with_off`. Never throws.
std::vector<BuiltinPack> official_packs(const std::filesystem::path& mods_root, bool with_off = false) noexcept;
// Whether the player turned the official mod `folder` off. Never throws.
bool official_off(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
// Turns it off or back on; false when the list could not be written. Never throws.
bool set_official_off(const std::filesystem::path& mods_root, std::string_view folder, bool off) noexcept;
// Whether the official mod `folder` is installed (the player installs them on the launcher's
// OFFICIAL tab) and not turned off: what the feature it is made for needs to start. Never throws.
bool official_in_use(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
// Mods that came before an official one and are retired with it (Fourth Studio's deprecated first
// Board Break), by folder name; the launcher takes them away and nothing loads them.
bool retired_mod(std::string_view folder) noexcept;
// Whether the installed mod `folder` (Mods/<folder>) is left out because ReSkate keeps it itself:
// an official mod in place under `mods_root`, or a retired one. Never throws.
bool kept_by_reskate(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
} // namespace dingosdk::mods
