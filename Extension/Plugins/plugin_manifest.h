#pragma once
#include <filesystem>
#include <set>
#include <vector>
#include <string>
#include <string_view>

// reskate-plugin.json, beside a mod's manifest.json, is what makes a mod a plugin:
//   {"api": 1, "entry": "plugin/main.lua", "side": "client", "permissions": ["console", "storage"]}
// Thunderstore's manifest.json keeps its own rules; this file is ReSkate's alone.
namespace dingosdk::plugins {
inline constexpr char manifest_file[] = "reskate-plugin.json";
// The API this ReSkate gives plugins. A plugin written for a newer one is refused.
inline constexpr int api_version = 1;
inline constexpr std::size_t maximum_manifest_bytes = 64 * 1024;

enum class Side { client, server, both };
struct Manifest {
    int api{};
    std::string entry; // inside the package, as safe_relative_path leaves it
    Side side{Side::client};
    std::set<std::string, std::less<>> permissions;
    // net.http: the exact host names its web requests may go to ("api.example.com"), lowercase.
    std::vector<std::string> domains;
    // mods.read: the folders it reads in every enabled mod ("parks": <mod>/parks/...), lowercase
    // (reskate.mods.content and read_content). Other mods ship data for it there, with no code.
    std::vector<std::string> content;
};
// Every permission a manifest may name, including those whose APIs a later ReSkate adds, so a
// manifest written for the whole plan stays valid. The base set needs none.
bool known_permission(std::string_view name) noexcept;
// Throws std::runtime_error with a message a modder can act on.
Manifest parse_manifest(std::string_view text);
// A path a plugin names (its entry, a require, a package read) in generic form, or empty when
// it could leave the package: absolute, a drive or a stream (':'), a backslash, an empty, "." or
// ".." segment, or a character outside printable ASCII.
std::string safe_relative_path(std::string_view path);
// Why a package may not load, from its files alone, or empty. A plugin never ships a program: no
// Windows executable (by its first bytes, whatever its name) and no script Windows runs.
std::string check_package_files(const std::filesystem::path& directory);
const char* side_name(Side side) noexcept;
}
