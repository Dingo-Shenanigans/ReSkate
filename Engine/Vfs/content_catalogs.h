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
    bool reserved(const std::string& key) const;
};
// Read once, on first use. Empty (available == false) when no pack is installed.
const Catalogs& catalogs();
Catalogs read_catalogs(const std::filesystem::path& folder);
}
