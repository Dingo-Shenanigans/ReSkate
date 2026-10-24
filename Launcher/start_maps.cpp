#include "start_maps.h"

#include "Engine/Game/World/custom_level_manifest.h"
#include "Engine/Game/World/world_names.h"
#include "Engine/Vfs/mod_list.h"

#include <algorithm>
#include <array>

namespace dingosdk::launcher_start_maps {
namespace {

bool same(std::string_view a, std::string_view b) {
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return fold(x) == fold(y); });
}

// The retail maps, as a dedicated server lists them (server_config.cpp).
constexpr std::array<std::string_view, 6> retail{
    san_vansterdam,
    "Levels/Game/DingoLevel_Isle_of_Grom/DingoLevel_Isle_of_Grom",
    "Levels/Game/DingoLevel_MPR/DingoLevel_MPR",
    "Levels/Game/DingoLevel_FTUE_Island/DingoLevel_FTUE_Island",
    "Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_001/DingoLevel_SDM_Int_001",
    "Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_002/DingoLevel_SDM_Int_002"};

} // namespace

std::vector<StartMap> start_maps(const std::filesystem::path& game_directory) {
    std::vector<StartMap> maps;
    for (const auto asset : retail) maps.push_back({std::string(asset), world_level_name(asset), {}, {}});
    // The mods the game's level list reads (mod_catalog.cpp, level_manifest_paths): switched on,
    // not left out by the last merge, and made for this game build.
    const auto list = mods::scan_mods(game_directory);
    if (!list.issue.empty()) return maps;
    for (const auto& entry : list.entries) {
        const auto& mod = entry.mod;
        if (!entry.enabled || !mod.provides_levels || !mod.outdated.empty() || list.excluded.contains(mod.name)) continue;
        const auto manifest = read_custom_level_manifest(mod.directory / "reskate-levels.json");
        if (!manifest.issue.empty()) continue;
        for (const auto& level : manifest.levels) {
            if (find_start_map(maps, level.asset)) continue;
            StartMap map{level.asset, level.display_name, mod.name, {}};
            // A map with one start point starts there; one with several must be given one.
            if (level.start_points.size() > 1 && level.manifest_start_point.empty())
                map.start_point = level.start_points.front().name;
            maps.push_back(std::move(map));
        }
    }
    return maps;
}

const StartMap* find_start_map(const std::vector<StartMap>& maps, std::string_view asset) {
    for (const auto& map : maps)
        if (same(map.asset, asset)) return &map;
    return nullptr;
}

std::wstring load_arguments(const StartMap& map) {
    if (same(map.asset, san_vansterdam)) return {};
    // Asset and start point names are plain ASCII (custom_level_manifest.cpp); a start point with
    // a space in it is quoted, as the console reads it.
    std::wstring text(map.asset.begin(), map.asset.end());
    if (!map.start_point.empty()) {
        const bool spaced = map.start_point.find(' ') != std::string::npos;
        text += spaced ? L" '" : L" ";
        text.append(map.start_point.begin(), map.start_point.end());
        if (spaced) text += L"'";
    }
    return text;
}

} // namespace dingosdk::launcher_start_maps
