#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// The launcher's side of the Mods folder: installing mods from a .zip or a
// folder, and removing them. Order and enabled state live in mods.json
// (Engine/Vfs/mod_list.h), which the runtime reads when the game starts.
namespace dingosdk::launcher_mods {

// The Mods folder the game will read: <game>/ModData/Default/Mods when the
// game is started with that data path (a ModData folder exists), else <game>/Mods.
std::filesystem::path mods_root(const std::filesystem::path& game_directory);

// Thrown when a mod with the same folder name is already installed and the
// caller did not ask to replace it.
struct AlreadyInstalled : std::runtime_error {
    explicit AlreadyInstalled(std::string folder)
        : std::runtime_error("A mod named \"" + folder + "\" is already installed."), name(std::move(folder)) {}
    std::string name;
};

using Progress = std::function<void(float fraction)>;

struct InstallOptions {
    // Install under this folder name instead of one taken from the source
    // (Thunderstore packages: Namespace-Name).
    std::string folder;
    // Written into manifest.json as "author" when it names none (Thunderstore
    // manifests have no author; the team is the author).
    std::string author;
    // Refuse a source with nothing the game loads, unless its manifest.json
    // lists dependencies (a mod pack).
    bool require_content{};
};

// Installs `source` (a .zip, or a folder) as Mods/<name> and returns <name>.
// The mod is the shallowest folder holding layout.toc, reskate-levels.json,
// reskate-plugin.json or parks/<map>.park.json, else the shallowest holding manifest.json or
// reskate-mod.json. Thunderstore's manifest.json, icon.png, README.md and
// CHANGELOG.md at the top of a package are copied in when the mod sits one
// folder deeper. A top-level mod takes the archive's name without a trailing
// version ("Team-Map-1.0.0.zip" installs as Team-Map). With `replace`, an
// installed mod of the same name is deleted for good once the new copy is in. Throws a
// user-facing message; a failed or cancelled install leaves the Mods folder as it was.
std::string install(const std::filesystem::path& mods_root, const std::filesystem::path& source, bool replace,
                    const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options = {});

// Moves Mods/<name> to the Recycle Bin. Throws a user-facing message.
void remove(const std::filesystem::path& mods_root, const std::string& name);

// What the mods give the game's data, as one line: each mod that ships a layout, in load order, with
// its folder name and its files' fingerprint (Engine/Vfs/mod_list.h mod_fingerprint), then the
// variants in play. A mod that only holds a plugin, or loose files the game does not draw with, is not
// in it. When it changes between launches, the launcher empties the game's shader cache: shaders built
// for the last set of mods, kept and reused, were found to break the picture after many changes.
struct DataMod {
    std::string name;
    std::filesystem::path directory;
};
std::string game_data_signature(const std::vector<DataMod>& mods, const std::map<std::string, std::string, std::less<>>& variants);
// The game's own shader cache, kept apart from the normal game's (Extension/Boot/user_data_redirect.cpp):
// %LOCALAPPDATA%\ReSkate\Game\Skate\cache. Not the shader_cache folder the game ships with.
std::filesystem::path game_shader_cache();
// Removes everything in `folder`, never the folder itself. What could not be removed, a line each.
std::vector<std::string> empty_folder(const std::filesystem::path& folder);


} // namespace dingosdk::launcher_mods
