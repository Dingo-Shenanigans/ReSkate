#pragma once
#include "Engine/Vfs/mod_list.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Live mods: applying Mods/ and mods.json while the game runs, so a change
// takes effect at the next level load instead of the next launch.
//
// The engine re-reads the root level, customization, buildkititems and each
// destination's superbundle TOC at every load, and opens an archive only when
// it first reads from it (sharing it with writers). The launch's merge places
// enabled mods' archives, so a live merge
// only rewrites TOCs and appends manifests; a superbundle the game did not
// know at launch is registered with it and listed under its install chunk.
// Globals and items are read once at launch; the merged patch always carries
// its own copy of both, which a live apply hands to the game in memory, so
// maps, loading screens and cosmetics (the ownables system reloads its bundle
// at every level load) change too. The UI and language packs still need a
// restart. Maps that change shared root shaders/materials also require a restart
// when enabled or disabled; a live merge rejects that change before writing.
namespace dingosdk::live_mods {
// Re-reads mods.json and starts a merge of the enabled mods on a worker
// thread. Returns what happened to the request; the outcome is logged.
// With reload_level, the level being played is loaded again once the merge
// finishes (see take_reload), so the change shows straight away; it is not
// when only maps changed, which show up in the level list instead.
std::string apply(bool reload_level = false);
// True once per apply that asked to reload the level and needed to, after it
// succeeded. Handed out together with take_level_manifests.
bool take_reload();
// What applying the pending changes in `list` will do to the level being
// played, for the overlay's apply button.
enum class ApplyEffect {
    maps_only,    // only maps added or taken away; nothing to reload
    reload_level, // something the level shows changed (cosmetics, settings)
    leave_level,  // the map being played goes away, so San Van is loaded
};
ApplyEffect preview(const mods::ModList& list);
// The apply button's label for an effect, shared by the overlay and native menu.
inline const char* apply_label(ApplyEffect effect) {
    return effect == ApplyEffect::maps_only ? "Apply"
         : effect == ApplyEffect::leave_level ? "Apply and go to San Van" : "Apply and reload level";
}
// The level being played (its level-manager destination, else the level),
// kept current by the client tick.
void set_current_level(std::string level);
// True while a live merge runs.
bool busy() noexcept;
// The last live apply's outcome for the overlay, or empty before the first.
std::string status();
// Folder names of the mods in effect now: the launch's, or the last live apply's.
std::vector<std::string> applied_mods();
// True when the launch's merge placed this enabled mod's archives.
bool placed_at_launch(const std::string& name);
// reskate-levels.json of every mod a live apply left enabled, highest
// priority first; handed out once per successful apply.
std::optional<std::vector<std::filesystem::path>> take_level_manifests();
}
