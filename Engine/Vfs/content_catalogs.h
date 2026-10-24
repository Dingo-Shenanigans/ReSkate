#pragma once
#include "Engine/Core/Json/json.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// Game catalogues read from the installed content cache (content_cache.h).
namespace dingosdk::content_cache {
// One tile of the native Object Browser: a family of object variants.
struct ObjectGroup {
    std::string id, title;
    std::uint32_t priority{};
};
// A build-kit category of the game's category service.
struct ObjectCategory {
    std::string id, title;
    std::string icon;       // a cdn:/ link, which cannot load offline
    std::uint32_t priority{};
    bool quick_drop{};      // listed by the Object Browser ("qdbuildkit")
    std::vector<ObjectGroup> groups;
};
// A travel destination (location_*): the level it is (field 3), its title,
// description, travel medium ("Water", "Door") and cdn:/ artwork.
struct TravelLocation {
    std::string id, level, name, description, medium, white_icon, black_icon, image;
};
struct Catalogs {
    bool available{};  // the pack is installed and was read
    // Lower-case owned asset id -> {"title", "description", "rarity_id", "group",
    // "object_type"} (each optional). Objects name their ObjectGroup in "group".
    Json items = Json::object();
    // Challenge id -> {"type", "asset", "available", "title_key", "goals", "neighborhood"}.
    Json challenges = Json::object();
    std::vector<std::string> entitlements;
    std::set<std::string> open_items;
    // Items some record grants (an offer, bundle, pack or reward), free or not,
    // and the ones tagged as a brand collab.
    std::set<std::string> granted_items, collab_items;
    // Build-kit categories in record order.
    std::vector<ObjectCategory> object_categories;
    std::vector<TravelLocation> travel_locations;
    // Access point id (accesspoint_*) -> the location ids it offers.
    std::vector<std::pair<std::string, std::vector<std::string>>> travel_access_points;
    // Music playlist id -> {display name, artwork id, track ids} from the music
    // chunk. A music record carries a field-10 message (10.10 name, 10.11
    // artwork) and repeated field-2 tracks; owned items use a varint field 10.
    struct MusicPlaylistEntry {
        std::string name, artwork;
        std::vector<std::string> tracks;
    };
    std::map<std::string, MusicPlaylistEntry, std::less<>> music_playlists;
    // Song ("Artist - Title") -> its cdn:/ cover art: a song record carries a field-10 message
    // (10.10 artist, 10.11 title, 10.12 artwork).
    std::map<std::string, std::string, std::less<>> music_song_artwork;
    // Objects mods add, as their reskate-objects.json describes them (add_mod_objects): lower-case
    // owned asset id -> {"title", "description", "group", "object_type"}, read like `items`. Kept
    // apart from `items`, which stays the game's own catalogue (store checks, the mod badge).
    Json mod_objects = Json::object();
    bool reserved(const std::string& key) const;
};
// Read once, on first use. Empty (available == false) when no pack is installed.
const Catalogs& catalogs();
Catalogs read_catalogs(const std::filesystem::path& folder);

// A mod's names and Quick Drop families for the objects it adds, in its folder:
//   {"schema": 1,
//    "categories": [{"id": "category_mine", "title": "My Objects", "priority": 900}],
//    "families": [{"id": "qdparent_mine_box", "title": "Box", "category": "category_mine"}],
//    "objects": [{"key": "own_bkprops_mine_box_00001", "title": "Box, Red", "family": "qdparent_mine_box"}]}
// A family is one Object Browser tile: its objects are the choices X / Square offers there, as with
// the game's own families. A family may go in one of the game's categories, and an object in one of
// the game's families. Every list and field but the ids and keys is optional; an entry without a
// priority goes after the ones already there.
inline constexpr char mod_objects_file[] = "reskate-objects.json";
// Adds one mod's reskate-objects.json to `out`. Only adds: a category or family id that is already
// taken, or an object the game's catalogue (or a mod read earlier) already has, is refused, so a
// mod can name and group its own objects but never the game's. Returns what was refused, a line
// each. Throws std::exception on a document that is not this shape, so the caller can skip it.
std::vector<std::string> add_mod_objects(Catalogs& out, const std::string& json);
// The process-wide catalogues plus each mod's file (mod name, path), highest priority first.
// Called once while the runtime starts, before the game runs. Missing files are skipped.
struct ModObjectsReport {
    std::size_t mods{}, objects{};
    std::vector<std::string> notes;  // a line each for every file or entry left out
};
ModObjectsReport install_mod_objects(const std::vector<std::pair<std::string, std::filesystem::path>>& files);
}
