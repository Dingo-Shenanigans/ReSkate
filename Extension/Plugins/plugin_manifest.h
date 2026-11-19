#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <string_view>

// reskate-plugin.json, beside a mod's manifest.json, is what makes a mod a plugin:
//   {"api": 1, "entry": "plugin/main.lua", "side": "client", "permissions": ["console", "storage"]}
// Thunderstore's manifest.json keeps its own rules; this file is ReSkate's alone.
namespace dingosdk::plugins {
inline constexpr char manifest_file[] = "reskate-plugin.json";
// The API this ReSkate gives plugins. A plugin written for a newer one is refused.
inline constexpr int api_version = 1;
inline constexpr std::size_t maximum_manifest_bytes = 64 * 1024;
// Source files the host may load with `require`, captured with the manifest it approved.
using PluginSources = std::map<std::string, std::string, std::less<>>;
inline std::string plugin_source_key(std::string_view path) {
    std::string key(path);
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return key;
}

enum class Side { client, server, both };
struct Manifest {
    int api{};
    std::string entry; // inside the package, as safe_relative_path leaves it
    Side side{Side::client};
    std::set<std::string, std::less<>> permissions;
    // net.http: the exact host names its web requests may go to ("api.example.com"), lowercase.
    std::vector<std::string> domains;
    std::set<std::string, std::less<>> effects; // exact private particle/material asset names
    // files.folders: folders beside the game it keeps files in ("My Routes": <game>/My Routes/...), as
    // granted_folder_name allows them. Its file calls name one as "<folder>:<path>".
    std::vector<std::string> folders;
    // mods.read: the folders it reads in every enabled mod ("parks": <mod>/parks/...), lowercase
    // (reskate.mods.content and read_content). Other mods ship data for it there, with no code.
    std::vector<std::string> content;
    // Font files of its own package ("fonts/Display-Regular.ttf": .ttf or .otf, as safe_relative_path
    // leaves them) that its drawn texts may name as style.font. Windows alone reads them.
    std::vector<std::string> fonts;
};
// What a manifest's "fonts" may be: so many files, each no larger than this.
inline constexpr std::size_t maximum_fonts = 8, maximum_font_bytes = 8u << 20;
// Every permission a manifest may name, including those whose APIs a later ReSkate adds, so a
// manifest written for the whole plan stays valid. The base set needs none.
bool known_permission(std::string_view name) noexcept;
// All of them.
std::vector<std::string_view> known_permissions();
// Whether a folder beside the game may be granted to a plugin by this name: one plain name (letters,
// digits, space, . _ -), not hidden, not a name Windows keeps, and none of the game's or ReSkate's own
// folders (Data, Mods, logs ...), in any case.
bool granted_folder_name(std::string_view name) noexcept;
// Throws std::runtime_error with a message a modder can act on.
Manifest parse_manifest(std::string_view text);
// A path a plugin names (its entry, a require, a package read) in generic form, or empty when
// it could leave the package: absolute, a drive or a stream (':'), a backslash, an empty, "." or
// ".." segment, or a character outside printable ASCII.
std::string safe_relative_path(std::string_view path);
// Whether a folder or a file of the path (as safe_relative_path leaves one) has a name Windows keeps
// for a device (con, prn, aux, nul, com1 ... lpt9, with or without an extension, in any case): a path
// in a plugin's data folder has none (plugin_api.h's data_path, and the files a zip unpacks to).
bool names_a_device(std::string_view path) noexcept;
// Why a package may not load, from its files alone, or empty. A plugin never ships a program: no
// Windows executable (by its first bytes, whatever its name) and no script Windows runs.
std::string check_package_files(const std::filesystem::path& directory);
// Why the files a manifest names are not there to use, or empty: each of its "fonts" is a file inside
// the package (links and junctions followed) of at most maximum_font_bytes.
std::string check_manifest_files(const Manifest& manifest, const std::filesystem::path& directory);
const char* side_name(Side side) noexcept;
}
