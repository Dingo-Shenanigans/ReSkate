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

// Which mods a mod pack brought in: the dependencies a Thunderstore install of it downloaded that
// were not installed before (theirs too), kept by the launcher in Mods/brought-in.json, pack folder
// to dependency folders. A mod the player had already is never in it.
inline constexpr char brought_in_file[] = "brought-in.json";
std::vector<std::string> brought_in(const std::filesystem::path& mods_root, const std::string& pack);
// Adds to `pack`'s list. Throws a user-facing message when the file cannot be written.
void record_brought_in(const std::filesystem::path& mods_root, const std::string& pack,
    const std::vector<std::string>& dependencies);
// Drops the lists of mods that are gone; nothing when there were none.
void forget_brought_in(const std::filesystem::path& mods_root, const std::vector<std::string>& removed);

// The installed mods a pack brought in that nothing else needs, to offer with its uninstall: what
// the mods in `removing` brought in (brought_in), without the ones `removing` already has and without
// any that a mod staying installed (the rest of `installed`) lists as a dependency in its
// manifest.json ("Namespace-Name-1.2.3" is the folder Namespace-Name), or that one's dependencies in
// turn. In `installed`'s order. A pack installed before this was kept brought nothing in.
std::vector<std::string> orphaned_dependencies(const std::filesystem::path& mods_root,
    const std::vector<std::string>& removing, const std::vector<std::string>& installed);

// Changes the game cannot make while it runs, because it holds the mod's files open (an update or
// uninstall of a mod it has loaded), wait in Mods/.reskate-pending until the next start: the new
// copy of a mod as a folder there, and the mods to uninstall in its remove.txt, a folder name a
// line. apply_pending makes them; the launcher calls it before each launch's merge.
inline constexpr char pending_folder[] = ".reskate-pending";
// Installs `source` as install() does, into the pending folder, and returns the mod's name. A
// removal waiting for the same mod is dropped: the update is what was asked for last.
std::string install_later(const std::filesystem::path& mods_root, const std::filesystem::path& source,
                          const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options = {});
// Marks Mods/<name> to be uninstalled at the next start, dropping an update waiting for it.
// Throws a user-facing message.
void remove_later(const std::filesystem::path& mods_root, const std::string& name);
struct Pending {
    std::vector<std::string> updates, removals;
};
// What waits. A missing or unreadable folder is nothing; never throws.
Pending pending(const std::filesystem::path& mods_root) noexcept;
// Moves each waiting copy into place (the old copy deleted for good, as an update's is) and moves
// each mod to remove to the Recycle Bin. What could not be done stays for the next try. Never
// throws; returns what happened, a line each, for the log.
std::vector<std::string> apply_pending(const std::filesystem::path& mods_root) noexcept;

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
