#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The Mods folder as a list: which mod folders exist, what they say about
// themselves, and the order and enabled state mods.json gives them. No merge
// happens here, so the launcher's mod manager and the runtime share it.
namespace dingosdk::mods {

// There is no limit on how many mods are listed or loaded: nothing in the merge needs one
// (archives are numbered with 16 bits). There was one of 64 once, which players reached: past
// it the launcher could not save the list at all, and the game skipped the later mods.
inline constexpr std::size_t maximum_mod_name = 64;
// Generated state the SDK owns. Folders beginning with a dot are never mods.
inline constexpr char generated_folder[] = ".reskate";
inline constexpr char mods_folder[] = "Mods";
inline constexpr char order_file[] = "mods.json";
// The mod's own description, written by its author or by the park editor:
// {"name":"Display name","author":"...","version_number":"1.0.0",
// "description":"..."}. Read loosely; other fields are kept when rewritten.
inline constexpr char manifest_file[] = "manifest.json";
// The older name for the same information ("version" instead of
// "version_number"), still read when a mod has no manifest.json.
inline constexpr char info_file[] = "reskate-mod.json";
// ReSkate Studio's stamp in every mod it builds: "ReSkate Studio native Patch v1", then
// key=value lines, of which skate_sha256=<the Skate.exe it was built for> decides whether the
// mod loads: game data built for another Skate.exe points into that build's archives.
inline constexpr char studio_marker_file[] = ".reskate-studio-patch";

struct Mod {
    std::string name;                 // folder name directly under Mods/
    std::filesystem::path directory;
    bool provides_layout{};           // has layout.toc, so it becomes a layout layer
    bool provides_levels{};           // has reskate-levels.json
    // Base maps it ships a park for: parks/<map>.park.json (park editor mods).
    std::vector<std::string> park_maps;
    // Presentation, from manifest.json (or reskate-mod.json). `title` falls back to the folder
    // name and `version` to the one reskate-build.json records.
    std::string title, author, version, description;
    // What built the mod and when, from reskate-build.json; empty when the mod
    // predates that file or was made by another tool.
    std::string tool, built;
    // The level assets its reskate-levels.json registers, read loosely for
    // logging; the strict reading that feeds the level list happens later.
    std::vector<std::string> levels;
    // What the merge could not use from this mod. Empty once it merged cleanly.
    std::vector<std::string> problems;
    // From .reskate-studio-patch: whether the mod has one, and the Skate.exe SHA-256 it records.
    bool studio_marker{};
    std::string skate_sha256;
    // Why the mod cannot load on this game build (it must be updated), or empty. Such a mod is left
    // out of every merge whatever mods.json says: a mod with no build stamp that ships game data,
    // or a stamp for another Skate.exe.
    std::string outdated;
};

// Mods left out of the merge because they could not be merged cleanly, kept
// in <Mods>/.reskate-excluded.json so later launches skip them without merging
// twice. An entry holds only while the mod's files are unchanged.
inline constexpr char exclusions_file[] = ".reskate-excluded.json";
struct Exclusion {
    std::string fingerprint;           // mod_fingerprint() when it was left out
    std::string sdk;                   // the ReSkate.dll that left it out; a new one retries
    std::vector<std::string> problems; // why, most important first
};
// A mod can also be left out with nothing wrong with its files: it adds copies
// of items the game's store sells, which ReSkate does not unlock, so it is not
// loaded at all (Engine/Vfs/mod_store_copies.h). Its first problem then begins
// with this, and reinstalling or rebuilding it changes nothing.
inline constexpr std::string_view store_copies_problem = "adds copies of store items";
[[nodiscard]] inline bool copies_store_items(const std::vector<std::string>& problems) noexcept {
    return !problems.empty() && problems.front().starts_with(store_copies_problem);
}
// Names, sizes and write times of every file in the mod folder.
std::string mod_fingerprint(const std::filesystem::path& directory);
std::map<std::string, Exclusion, std::less<>> read_exclusions(const std::filesystem::path& mods_root) noexcept;
// Replaces the file; an empty map removes it.
void write_exclusions(const std::filesystem::path& mods_root,
                      const std::map<std::string, Exclusion, std::less<>>& exclusions) noexcept;

struct ModEntry {
    Mod mod;
    bool enabled{true};
};

struct ModList {
    std::filesystem::path root;       // <data_root>/Mods
    bool present{};                   // the Mods folder exists
    // Every mod folder on disk, highest priority first: mods.json's rows in
    // their order (enabled or not), then unlisted folders by name, enabled.
    std::vector<ModEntry> entries;
    // mods.json rows naming folders that are not present.
    std::vector<std::string> missing;
    // Mods the game currently leaves out because they could not be merged,
    // with the reasons (only while their files are unchanged).
    std::map<std::string, std::vector<std::string>, std::less<>> excluded;
    // Non-empty when mods.json is malformed; the runtime then loads no mod.
    std::string issue;
    // Advisory messages worth logging, such as unreadable info files.
    std::vector<std::string> notes;
};

// Folder names travel into engine paths, so only plain ASCII is accepted:
// letters, digits, space, '_', '-' and '.', not starting with '.'.
bool valid_mod_name(std::string_view name);

// `path` in generic form ('/' separators) as ASCII text, or empty when it holds any other
// character. Every name the game or a mod list uses is ASCII, and path::string() throws on
// characters outside the ANSI code page, so one oddly named file must not stop a scan.
std::string ascii_path(const std::filesystem::path& path);

// Reads <data_root>/Mods and its mods.json. Never throws. When mods.json is
// malformed, `issue` says why and `entries` still lists the folders (all
// shown as enabled) so a manager can offer to rewrite it.
ModList scan_mods(const std::filesystem::path& data_root) noexcept;

// Writes mods.json for `entries` in order, replacing the file atomically.
// Throws with a readable message on failure.
void save_mod_order(const std::filesystem::path& mods_root, const std::vector<ModEntry>& entries);

} // namespace dingosdk::mods
