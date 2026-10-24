#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Game content ReSkate carries itself: packs of game assets built by ReSkate Studio, zipped into
// ReSkate.dll (the Throwdowns logo grid, tools/builtin/build_throwdown_menu_pack.py). Nobody installs or sees
// them. Before a merge each is unpacked under Mods/.reskate-builtin/<name> (a dot folder,
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

// Mods ReSkate retired (Fourth Studio's deprecated first Board Break), by folder name: nothing loads them.
bool retired_mod(std::string_view folder) noexcept;
// Whether the installed mod `folder` (Mods/<folder>) is left out because ReSkate sees to it: a
// retired one. Never throws.
bool kept_by_reskate(const std::filesystem::path& mods_root, std::string_view folder) noexcept;
} // namespace dingosdk::mods
