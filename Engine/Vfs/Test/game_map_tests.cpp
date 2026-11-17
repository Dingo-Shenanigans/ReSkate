// Reads the game's own map of each of its levels from an installed game, and cuts parts out of it.
// Only runs when given the game folder; a second argument is a folder the parts are written to as
// PPM pictures, to be looked at.
//   dingosdk_game_map_tests <Skate folder> [folder for pictures]
#include "Engine/Vfs/game_map.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
bool failed = false;
void check(bool ok, const std::string& what) {
    if (!ok) {
        failed = true;
        std::cerr << "FAIL: " << what << '\n';
    }
}
// How much of a picture has something in it, and how many different colours a coarse count finds.
struct Seen {
    double filled{};
    std::size_t colours{};
};
Seen look(const dingosdk::frostbite::Image& image) {
    std::size_t opaque{};
    std::vector<bool> found(4096);
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        if (image.rgba[i + 3]) ++opaque;
        found[(std::size_t{image.rgba[i]} >> 4) << 8 | (std::size_t{image.rgba[i + 1]} >> 4) << 4 | std::size_t{image.rgba[i + 2]} >> 4] = true;
    }
    Seen seen;
    seen.filled = image.rgba.empty() ? 0 : static_cast<double>(opaque) / static_cast<double>(image.rgba.size() / 4);
    for (const bool one : found) seen.colours += one ? 1 : 0;
    return seen;
}
void save(const std::string& folder, const std::string& name, const dingosdk::frostbite::Image& image) {
    if (folder.empty()) return;
    std::ofstream out(folder + "/" + name + ".ppm", std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(image.rgba.data() + i), 3);
}
}

int main(int argc, char** argv) {
    using namespace dingosdk;
    if (argc < 2 || !*argv[1]) {
        std::cout << "No game folder given; skipped.\n";
        return 0;
    }
    const std::string pictures = argc > 2 ? argv[2] : "";
    // With the merged mods' folder and levels named: what each of those has, said in full (to see how
    //   dingosdk_game_map_tests <Skate folder> <folder for pictures> <Mods/.reskate> <level> ...
    // a mod's map is laid out), and nothing else.
    if (argc > 4) {
        for (int i = 4; i < argc; ++i) {
            try {
                std::string level = argv[i];
                for (auto& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const vfs::GameData data(argv[1], argv[3]);
                const auto toc = data.read_toc("Win32/" + level + ".toc");
                std::cout << level << ": " << toc.bundles.size() << " bundles\n";
                for (const auto& entry : toc.bundles) {
                    const auto bundle = data.read_bundle(toc, entry.name);
                    if (!bundle) continue;
                    std::size_t tiles{};
                    for (const auto& asset : bundle->manifest.ebx)
                        if (asset.name.find("reskate/coremap") != std::string::npos) std::cout << "   " << entry.name << ": " << asset.name << '\n';
                        else if (asset.name.find("features/map") != std::string::npos || asset.name.find("mapcamera") != std::string::npos) {
                            if (asset.name.find("flow_map_internal") == std::string::npos && asset.name.find("/widgets/") == std::string::npos &&
                                (asset.name.find("/data/") != std::string::npos || asset.name.ends_with("_map") || asset.name.find("mapcamera") != std::string::npos))
                                std::cout << "   " << entry.name << ": " << asset.name << '\n';
                        }
                    for (const auto& asset : bundle->manifest.resources)
                        if (asset.name.find("_map_") != std::string::npos && asset.name.find(',') != std::string::npos) ++tiles;
                    if (tiles) std::cout << "   " << entry.name << ": " << tiles << " tile resources\n";
                }
                const auto map = vfs::read_game_map(argv[1], level, argv[3]);
                if (!map) {
                    std::cout << "   no map read\n";
                    continue;
                }
                std::cout << "   map " << map->name << " in " << map->bundle << ", about (" << map->centre_x << ", " << map->centre_z << "), " << map->width << " x "
                          << map->height << " m\n";
                vfs::GameMapReader reader(argv[1], *map, argv[3]);
                const auto whole = reader.window(map->centre_x, map->centre_z, map->width, 512);
                const auto seen = look(whole);
                std::cout << "   the whole of it: " << seen.filled * 100 << "% filled, " << seen.colours << " colours\n";
                save(pictures, level.substr(level.rfind('/') + 1), whole);
            } catch (const std::exception& error) {
                std::cout << "   " << error.what() << '\n';
            }
        }
        return 0;
    }
    try {
        // A level that is not the game's has no map, and says so quietly.
        check(!vfs::read_game_map(argv[1], "levels/custom/nothing/nothing"), "a level that is not there has no map");
        check(!vfs::read_game_map(argv[1], ""), "no level has no map");

        const auto started = std::chrono::steady_clock::now();
        const auto map = vfs::read_game_map(argv[1], "levels/game/BAM_LevelRoot/BAM_LevelRoot");
        const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        check(map.has_value(), "San Vansterdam has a map");
        if (map) {
            std::cout << map->name << " in " << map->bundle << ": " << map->lods.size() << " sizes, " << map->source_width << " x " << map->source_height
                      << ", about (" << map->centre_x << ", " << map->centre_z << "), " << map->width << " x " << map->height << " m, read in " << read_ms << " ms\n";
            check(map->name == "ui/features/map/downtowndistrict_map", "it is the downtown district's");
            check(map->lods.size() == 5 && map->lods[0].columns == 15 && map->lods[0].rows == 8 && map->lods[4].columns == 4 && map->lods[4].rows == 2,
                  "five sizes, from 15 x 8 tiles to 4 x 2");
            for (const auto& lod : map->lods)
                check(std::none_of(lod.tiles.begin(), lod.tiles.end(), [](const auto& name) { return name.empty(); }), "every tile has its texture");
            check(std::abs(map->centre_x - 300.6f) < 0.5f && std::abs(map->centre_z - 1444.1f) < 0.5f, "its middle is where the map's camera is");
            check(std::abs(map->height - 1241) < 5 && std::abs(map->width - 2206) < 8, "it covers about 2206 x 1241 m");

            vfs::GameMapReader reader(argv[1], *map);
            // The whole of it, small: mostly something, of many colours.
            auto timed = std::chrono::steady_clock::now();
            const auto whole = reader.window(map->centre_x, map->centre_z, map->width, 512);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timed).count();
            auto seen = look(whole);
            std::cout << "the whole map: " << seen.filled * 100 << "% filled, " << seen.colours << " colours, " << ms << " ms\n";
            check(whole.width == 512 && whole.height == 512 && seen.colours > 200, "the whole map is a picture");
            check(seen.filled > 0.5 && seen.filled < 0.6, "below and above it (it is 16:9 in a square) there is nothing");
            save(pictures, "bam_whole", whole);
            // A radar's worth about the middle of the city, from the largest tiles.
            timed = std::chrono::steady_clock::now();
            const auto near = reader.window(map->centre_x, map->centre_z, 220, 768);
            ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timed).count();
            seen = look(near);
            std::cout << "220 m about its middle: " << seen.filled * 100 << "% filled, " << seen.colours << " colours, " << ms << " ms\n";
            check(seen.filled > 0.999 && seen.colours > 100, "a part of the city is a picture, all of it filled");
            save(pictures, "bam_near", near);
            // And again: the tiles are held.
            timed = std::chrono::steady_clock::now();
            (void)reader.window(map->centre_x + 15, map->centre_z - 10, 220, 768);
            std::cout << "the next part: " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timed).count() << " ms\n";
            // Far off the map: nothing, and no error.
            check(look(reader.window(map->centre_x + 50000, map->centre_z, 220, 64)).filled == 0, "off the map there is nothing");
        }
        // The game's other levels: each has its own.
        for (const auto& [level, name] : {std::pair{"Levels/Game/DingoLevel_Isle_of_Grom/DingoLevel_Isle_of_Grom", "ui/features/map/isleofgrom_map"},
                                           std::pair{"levels/game/dingolevel_mpr/dingolevel_mpr", "ui/features/map/megapark_map"},
                                           std::pair{"levels/game/dingolevel_sdm/dingolevel_sdm_int_001/dingolevel_sdm_int_001", "ui/features/map/stadium_default_map"},
                                           std::pair{"levels/game/dingolevel_sdm/dingolevel_sdm_int_002/dingolevel_sdm_int_002", "ui/features/map/stadium_nanaimo_map"}}) {
            const auto other = vfs::read_game_map(argv[1], level);
            check(other && other->name == name, std::string(level) + " has the map " + name + (other ? ", got " + other->name : ", got none"));
            if (!other) continue;
            std::cout << other->name << ": about (" << other->centre_x << ", " << other->centre_z << "), " << other->width << " x " << other->height << " m\n";
            vfs::GameMapReader reader(argv[1], *other);
            const auto whole = reader.window(other->centre_x, other->centre_z, other->width, 256);
            check(look(whole).colours > 50, std::string(name) + " is a picture");
            save(pictures, std::string(name).substr(std::string(name).rfind('/') + 1), whole);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failed) return 1;
    std::cout << "game map ok\n";
    return 0;
}
