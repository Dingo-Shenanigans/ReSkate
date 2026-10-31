#pragma once
#include "plugin_manifest.h"
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the player has decided about each plugin. A plugin runs only when they turned it on, and an
// answer holds for the plugin's own files as they were then: change one and it asks again.
namespace dingosdk::plugins {
// asking: no answer for these files yet. on / off: the player's answer for them.
enum class Approval { asking, on, off };

// Changes when a file of the plugin itself does: reskate-plugin.json and everything under the top
// folder of its entry ("plugin/main.lua" gives plugin/). A mod's other files (cosmetics, maps,
// images) do not count. An entry at the mod's top folder has no folder of its own, so the whole mod
// counts. It reads the files' bytes; run it off the game thread.
std::string plugin_fingerprint(const std::filesystem::path& directory);
struct PluginSnapshot {
    std::string fingerprint;
    std::shared_ptr<const PluginSources> sources; // manifest and Lua modules from the same bytes as the fingerprint
    std::string error; // a source that could not be captured within the host's limits
};
PluginSnapshot plugin_snapshot(const std::filesystem::path& directory);

// The mod's reskate-plugin.json, or nothing when it cannot be read.
std::optional<Manifest> read_manifest(const std::filesystem::path& directory);
std::optional<Manifest> read_manifest(const PluginSources& sources);

// How much a permission matters to the player. Red: other players see it, it changes your game settings
// or it reaches the internet. Amber: it controls your own game or input, or touches your files. Grey: it
// only shows things to you or reads.
enum class Risk { grey, amber, red };
// A category groups the requested permissions and their full descriptions for Plugin Manager.
struct PermissionGroup {
    std::string category;
    Risk risk{};
    std::vector<std::string> phrases; // the first `loud` are at the category's own risk, then the lesser ones
    std::vector<std::string> permissions; // exact manifest permissions, for honest prompt summaries
    std::size_t loud{};
};
std::vector<PermissionGroup> permission_groups(const Manifest& manifest);
// The category a permission is under, or nothing for one that has no words yet.
std::string_view permission_category(std::string_view name);
// A concise summary from the requested permissions; full descriptions and domains stay in Plugin Manager.
std::string approval_prompt(const std::vector<PermissionGroup>& groups);

// Mod folder name: the plugin fingerprint the player answered for, and the answer. Kept in a JSON
// file, and safe to use from any thread.
class Approvals {
public:
    explicit Approvals(std::filesystem::path file);
    // In developer mode a plugin with an answer keeps it when its files change, and the answer moves
    // to the new files. One with no answer at all always asks.
    Approval state(std::string_view id, std::string_view fingerprint);
    void set_developer(bool on) { developer_ = on; }
    bool developer() const { return developer_; }
    void set(std::string_view id, std::string_view fingerprint, bool on);

private:
    struct Record {
        std::string fingerprint;
        bool on{};
    };
    std::mutex mutex_;
    std::atomic<bool> developer_{};
    std::filesystem::path file_;
    std::map<std::string, Record, std::less<>> records_;
};
}
