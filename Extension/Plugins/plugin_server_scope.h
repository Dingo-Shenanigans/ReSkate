#pragma once
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Plugins a dedicated server brought here. A server can require plugins of its players
// (Extension/Assets/map_download.h): one that was not on here before the server asked for it, not
// installed or switched off, is the server's and not the player's own. It stays in Mods, switched
// on in mods.json, and runs only while the player is on a server that requires it; everywhere
// else the plugin host leaves it unloaded. The player can make it theirs (and one of their own
// the server's) in the Plugin Manager.
//
// Which plugins these are is kept by mod folder in Mods/.reskate_server_plugins.json, a JSON
// array of folder names. Safe from any thread; each call reads or replaces the file.
namespace dingosdk::plugins::server_scope {
inline constexpr char file_name[] = ".reskate_server_plugins.json";
// Mod folder names, lower case.
using Folders = std::set<std::string, std::less<>>;

// Nothing when there is no file, or it cannot be read.
Folders read(const std::filesystem::path& mods_root) noexcept;
// Marks the mod folder as a server's, or as the player's own. False when it could not be saved.
bool set(const std::filesystem::path& mods_root, std::string_view folder, bool server_only) noexcept;
// Forgets the folders that are not in `present` (mods taken out since). Nothing is written when
// all of them are there.
void keep_only(const std::filesystem::path& mods_root, const std::vector<std::string>& present) noexcept;

// Whether a server's required plugin ("Owner-Name" or "Owner-Name-1.2.0", protocol.h:
// valid_required_plugins) is this mod: its folder is the package's, or its name and author are.
bool names(std::string_view package, std::string_view folder, std::string_view title, std::string_view author);
// Whether the plugin in this mod folder may run now: always when it is the player's own, else
// only while the session's server requires it (`required`: empty outside a dedicated server).
bool runs(const Folders& server_only, const std::vector<std::string>& required, std::string_view folder, std::string_view title,
          std::string_view author);
}
