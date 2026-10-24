#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The map the game starts in (Settings > GAME > Start map). It goes to the game as the launcher's
// -map does (launch.cpp): a "load" at the front of the startup commands, which the game's first level
// load from the splash takes in San Vansterdam's place (Runtime/level_requests.cpp, queue_menu_bam),
// so the game opens straight in it. San Vansterdam is chosen by asking for nothing.
namespace dingosdk::launcher_start_maps {

inline constexpr std::string_view san_vansterdam = "Levels/Game/BAM_LevelRoot/BAM_LevelRoot";

struct StartMap {
    std::string asset;          // the level, as reskate-levels.json or the game names it
    std::string name;           // as the game's menus show it
    std::string mod;            // the mod folder that adds it; empty for one of the game's own
    std::string start_point;    // its first, for a custom map with several; empty: the map's own choice
};

// San Vansterdam first, the game's other maps, then the custom maps of the mods in
// `game_directory`\Mods that load (switched on, merged, made for this game build), in load order.
std::vector<StartMap> start_maps(const std::filesystem::path& game_directory);
// The map with this asset (ASCII case ignored), or null.
const StartMap* find_start_map(const std::vector<StartMap>& maps, std::string_view asset);
// What follows "load " for the map: its asset, and its start point when it has several and names
// none to start at. Empty for San Vansterdam, which the game is in already.
std::wstring load_arguments(const StartMap& map);

} // namespace dingosdk::launcher_start_maps
