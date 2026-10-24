#pragma once
#include "Engine/Vfs/content_catalogs.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The Object Browser's category tree, taken from the game's category service
// records in the content cache and the families mods declare for their own
// objects (content_cache::add_mod_objects). Objects neither places fall back to
// one tile each under their installed family.
namespace dingosdk::objects {
struct InstalledObject {
    std::string key, family, title;
};
struct BrowserTile {
    std::string id, title;
    std::uint32_t priority{};
    std::vector<std::string> items;
};
struct BrowserCategory {
    std::string id, title;
    std::uint32_t priority{};
    std::vector<BrowserTile> tiles;
};
struct Placement {
    const content_cache::ObjectCategory* category{};
    const content_cache::ObjectGroup* group{};
};

// Group id -> its place in the tree. Browser categories win over other build-kit ones.
std::map<std::string, Placement, std::less<>> object_groups(const content_cache::Catalogs& catalogs);
// Where one object sits, or nothing when the cache does not place it.
Placement object_placement(std::string_view key, const content_cache::Catalogs& catalogs,
    const std::map<std::string, Placement, std::less<>>& groups);
// The tile an object belongs to: its group, or the object itself.
std::string browser_tile(std::string_view key, const content_cache::Catalogs& catalogs,
    const std::map<std::string, Placement, std::less<>>& groups);
// Browser categories first, then other build-kit categories, then unplaced families.
std::vector<BrowserCategory> browser_layout(const std::vector<InstalledObject>& objects,
    const content_cache::Catalogs& catalogs);
}
