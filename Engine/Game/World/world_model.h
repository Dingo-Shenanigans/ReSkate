#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk {
struct StartPointInfo { std::string name; bool is_default{}; };
struct LevelInfo {
    std::string asset;
    std::string display_name;
    std::vector<StartPointInfo> start_points;
    std::string manifest_start_point;
    std::uint8_t level_flag68{};
    // Manifest rows are detached-level destinations, never base/root levels.
    bool manifest_only{};
    // Declared by a mod's reskate-levels.json (a custom map).
    bool custom{};
    // What the map's manifest row says its world layers are when the player has left them at
    // "default": a layer key and "on" or "off" (reskate-levels.json, worldLayers).
    std::vector<std::pair<std::string, std::string>> world_layers;
};
struct WorldCatalog {
    bool available{};
    std::string issue;
    std::vector<LevelInfo> levels;
};
struct SublevelInfo {
    std::string asset;
    std::uint32_t state{};
};
struct SublevelCatalog {
    bool available{};
    bool active{};
    std::string issue;
    std::vector<SublevelInfo> levels;
};
struct WorldDescription {
    bool available{};
    std::string level, start_point, lm_level, lm_start_point;
    std::string game_mode, hosted_mode;
    bool attributes_available{};
    std::uint32_t field28{};
    std::array<std::uint8_t, 4> flags{};
};
// Read the inspected build's asset metadata only. Call with a validated image
// and preferably from a paused main-thread boundary for a consistent snapshot.
WorldCatalog read_world_catalog(HANDLE process, std::uintptr_t image_base);
// Server detached-sublevel metadata only; active also requires a current root.
// Header revalidation detects observed changes, but is not a synchronization lock.
SublevelCatalog read_server_sublevels(HANDLE process, std::uintptr_t image_base);
WorldDescription read_world_description(HANDLE process, std::uintptr_t descriptor);
std::string world_catalog_json(const WorldCatalog& catalog);
std::string sublevel_catalog_json(const SublevelCatalog& catalog);
std::string world_description_json(const WorldDescription& description);
}
