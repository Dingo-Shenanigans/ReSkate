#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
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
// What plugins save (reskate.files), a folder each: Mods/.reskate_plugin_data/<plugin>/. Not in the
// generated folder, which every full merge deletes, and not in a mod's folder, which its updates
// replace.
inline constexpr char plugin_data_folder[] = ".reskate_plugin_data";
inline constexpr char mods_folder[] = "Mods";
inline constexpr char order_file[] = "mods.json";
// The mod's own description, written by its author or by the park editor:
// {"name":"Display name","author":"...","version_number":"1.0.0",
// "description":"..."}. Read loosely; other fields are kept when rewritten.
inline constexpr char manifest_file[] = "manifest.json";
// The older name for the same information ("version" instead of
// "version_number"), still read when a mod has no manifest.json.
inline constexpr char info_file[] = "reskate-mod.json";
// A plugin mod's manifest (Extension/Plugins/plugin_manifest.h). Besides what the plugin host reads,
// the merge reads its "resident" list (ResidentAssets) from here.
inline constexpr char plugin_manifest_file[] = "reskate-plugin.json";
// ReSkate Studio's stamp in every mod it builds: "ReSkate Studio native Patch v1", then
// key=value lines, of which skate_sha256=<the Skate.exe it was built for> decides whether the
// mod loads: game data built for another Skate.exe points into that build's archives.
inline constexpr char studio_marker_file[] = ".reskate-studio-patch";

// Assets of one of the mod's bundles that the game selects at run time without an import naming them
// (a plugin's world objects, the HUD widgets it picks by size): kept with the mod's additions when
// another mod (a map) replaces that bundle, as imported additions are. reskate-plugin.json:
//   "resident": [{"bundle": "win32/levels/...", "roots": ["world/...", "ui/.../"],
//                 "shaders": [{"name": "world/.../shaderblockdepot_...", "roots": ["world/...", ...]}]}]
// Names are lower-case asset names; a root ending in '/' keeps every asset under it. A shader block
// depot (a resource no EBX imports) goes with its roots when all of them come from the mod.
struct ResidentAssets {
    std::string bundle;
    std::vector<std::string> roots;
    struct Shader {
        std::string name;
        std::vector<std::string> roots;
    };
    std::vector<Shader> shaders;
};

// A mod that brings several takes on the same game data, of which one is in the game at a time (a deck
// pack's decks: the game has one deck model). reskate-plugin.json:
//   "variants": {"folder": "decks", "order": ["standard", "wide"], "displaces": ["..._bundlereftable"]}
// Each folder <folder>/<name>/ with a layout.toc is the variant <name> (lower-case letters, digits and
// '_'), laid out like a mod folder. "order" is the variants the menus list first, in that order; the
// first of them loads when none was chosen. "displaces" names the bundles a variant replaces: while one
// loads, every other mod whose layout lists one of them (the declaring mod's own layout too) is set
// aside for the run (mod_catalog.h).
struct Variants {
    std::string folder;
    std::vector<std::string> order;
    std::vector<std::string> displaces;
};

struct Mod {
    std::string name;                 // folder name directly under Mods/
    std::filesystem::path directory;
    bool provides_layout{};           // has layout.toc, so it becomes a layout layer
    bool provides_levels{};           // has reskate-levels.json
    bool provides_plugin{};           // its reskate-plugin.json names Luau code to run (an "entry")
    bool provides_objects{};          // has reskate-objects.json (Quick Drop object names and families)
    bool provides_music{};            // has reskate-music.json (playlists for its songs)
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
    // From reskate-plugin.json: the assets its bundles keep resident (ResidentAssets).
    std::vector<ResidentAssets> resident;
    // From reskate-plugin.json: the variants it holds (Variants), if it declares any.
    std::optional<Variants> variants;
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
    // Left out for a conflict with this mod, higher in the load order, as its files were then
    // (its mod_fingerprint()). It stays out only while that mod still loads above it unchanged.
    std::string conflictWith;
    std::string conflictFingerprint;
};
// Names, sizes and write times of every file in the mod folder.
std::string mod_fingerprint(const std::filesystem::path& directory);
// Plugins' files from before plugin_data_folder (Mods/.reskate/data/<plugin>/, inside the generated
// folder a full merge deletes) move to Mods/.reskate_plugin_data/<plugin>/: a plugin's folder whole
// when it has none there yet, else the files it lacks. Run before anything deletes the generated
// folder. Never throws; returns what it moved, a line each, for the log.
std::vector<std::string> keep_plugin_data(const std::filesystem::path& mods_root) noexcept;
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

// Whether `name` (an asset of `bundle`, either lower-case) is one the mod keeps resident, and the
// shader block depot entry `name` is in that bundle, or null.
bool resident_root(const Mod& mod, std::string_view bundle, std::string_view name) noexcept;
const ResidentAssets::Shader* resident_shader(const Mod& mod, std::string_view bundle, std::string_view name) noexcept;

// What a mod holds, for a player, what says most about it first: "Map" or "3 maps", "Park" or
// "Parks", "Plugin", "Game data", "Objects", "Music". Empty when it holds none of these. A plugin
// that also ships game data reads as a plugin first, since its code is what it is for.
std::vector<std::string> mod_contents(const Mod& mod);

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
// One mod folder as scan_mods reads it: its layout, levels, parks, info files and build stamp.
// `name` is how it is known (for a folder under Mods/, the folder's name). Throws on I/O errors.
Mod describe_mod(const std::filesystem::path& directory, std::string name, std::vector<std::string>& notes);

// Writes mods.json for `entries` in order, replacing the file atomically.
// Throws with a readable message on failure.
void save_mod_order(const std::filesystem::path& mods_root, const std::vector<ModEntry>& entries);

// Whether mods.json lists the mod folder `name` switched off. A folder it does not list loads.
// Reads mods.json alone, not the mods. Never throws: false when it cannot be read.
bool mod_disabled(const std::filesystem::path& mods_root, std::string_view name) noexcept;
// Switches the mod folder `name` on or off in mods.json, every other row as it was; one it does
// not list yet is added at its end, the bottom of the load order. False when mods.json could not
// be read (malformed: left for the player to see) or written. Never throws.
bool set_mod_enabled(const std::filesystem::path& mods_root, std::string_view name, bool enabled) noexcept;

} // namespace dingosdk::mods
