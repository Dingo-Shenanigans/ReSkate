#include "object_categories.h"
#include <algorithm>
#include <tuple>

namespace dingosdk::objects {
namespace {
std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
}

std::map<std::string, Placement, std::less<>> object_groups(const content_cache::Catalogs& catalogs) {
    std::map<std::string, Placement, std::less<>> result;
    for (const bool browser : {true, false})
        for (const auto& category : catalogs.object_categories)
            if (category.quick_drop == browser)
                for (const auto& group : category.groups) result.try_emplace(group.id, Placement{&category, &group});
    return result;
}

Placement object_placement(std::string_view key, const content_cache::Catalogs& catalogs,
    const std::map<std::string, Placement, std::less<>>& groups) {
    const auto id = lower(key);
    // The game's own record, or the family a mod gives its object (reskate-objects.json).
    const auto& records = catalogs.items.contains(id) ? catalogs.items : catalogs.mod_objects;
    if (!records.contains(id)) return {};
    const auto& item = records.at(id);
    if (!item.is_object() || !item.contains("group") || !item.at("group").is_string()) return {};
    const auto found = groups.find(item.at("group").string());
    return found == groups.end() ? Placement{} : found->second;
}

std::string browser_tile(std::string_view key, const content_cache::Catalogs& catalogs,
    const std::map<std::string, Placement, std::less<>>& groups) {
    const auto placement = object_placement(key, catalogs, groups);
    return placement.group ? placement.group->id : std::string(key);
}

std::vector<BrowserCategory> browser_layout(const std::vector<InstalledObject>& objects,
    const content_cache::Catalogs& catalogs) {
    const auto groups = object_groups(catalogs);
    struct Entry { int rank; BrowserCategory category; };
    std::map<std::string, Entry> categories;
    for (const auto& object : objects) {
        const auto placement = object_placement(object.key, catalogs, groups);
        std::string category_id, category_title, tile_id, tile_title;
        std::uint32_t category_priority{}, tile_priority{};
        int rank = 2;
        if (placement.group) {
            rank = placement.category->quick_drop ? 0 : 1;
            category_id = placement.category->id; category_title = placement.category->title;
            category_priority = placement.category->priority;
            tile_id = placement.group->id; tile_title = placement.group->title;
            tile_priority = placement.group->priority;
        } else {
            category_id = category_title = object.family;
            tile_id = object.key; tile_title = object.title;
        }
        auto& entry = categories.try_emplace(category_id,
            Entry{rank, {category_id, category_title, category_priority, {}}}).first->second;
        auto& tiles = entry.category.tiles;
        auto tile = std::find_if(tiles.begin(), tiles.end(), [&](const BrowserTile& t) { return t.id == tile_id; });
        if (tile == tiles.end()) tile = tiles.insert(tiles.end(), {tile_id, tile_title, tile_priority, {}});
        tile->items.push_back(object.key);
    }
    std::vector<Entry> ordered;
    for (auto& [id, entry] : categories) ordered.push_back(std::move(entry));
    std::stable_sort(ordered.begin(), ordered.end(), [](const Entry& a, const Entry& b) {
        return std::tie(a.rank, a.category.priority, a.category.title) <
               std::tie(b.rank, b.category.priority, b.category.title);
    });
    std::vector<BrowserCategory> result;
    for (auto& entry : ordered) {
        auto& tiles = entry.category.tiles;
        std::stable_sort(tiles.begin(), tiles.end(), [](const BrowserTile& a, const BrowserTile& b) {
            return std::tie(a.priority, a.title) < std::tie(b.priority, b.title);
        });
        result.push_back(std::move(entry.category));
    }
    return result;
}
}
