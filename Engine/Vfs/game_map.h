#pragma once
// The game's own map of a level: the picture its pause-screen map shows, read from the installed
// game's data for whatever level is asked for, so nothing is kept per map and nothing of the game's
// is shipped.
//
// What the data holds (found with ReSkate Studio's CLI on 2026-11-16, build 20260929):
//  - A level's map screen is fed by a DataModelResourceAsset "ui/features/map/data/coremap_<x>_resource"
//    in the level's own bundle (San Vansterdam's is in its "..coregameassets" bundle, and a copy of
//    it rides in every other level's bundle, beside that level's own). It imports a DingoMapAsset.
//  - The DingoMapAsset ("ui/features/map/downtowndistrict_map" and the like) is a top-down render
//    of the level cut into tiles at five sizes: Lods[] of {ScaleFactor, TileCountX, TileCountY},
//    and SourceTextureSize (7680 x 4320). A tile's texture is named
//    "<asset's leaf>_<lod>_<index>_<w>,<h>_<w>_<h>", its index counted along the rows from the top left.
//  - The camera that render was made from is the one CameraEntityData of "<level's folder>/dev_mapcameras"
//    (bundle "<level's folder>/dev_mapscreenshots"): straight down from high above the level, its
//    right the world's +x and its up the world's -z, with a Fov of 5 degrees. Read as the vertical
//    angle (the other reading makes San Vansterdam 780 m across), that is what the picture covers.
//    NOT yet checked against a known place in the game.
#include "game_textures.h"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::vfs {
struct GameMapLod {
    float scale{};                       // of the source picture's size
    std::uint32_t columns{}, rows{};
    std::vector<std::string> tiles;      // their textures' names, row by row from the top left; empty: missing
};
struct GameMap {
    std::string toc, bundle;             // where the tiles are
    std::string name;                    // the map asset's
    std::vector<GameMapLod> lods;        // the largest first
    float source_width{}, source_height{};
    // What the picture covers, in metres of the world: its middle, and its size. Its right is +x
    // and its top is -z.
    float centre_x{}, centre_z{}, width{}, height{};
};

// The map of `level` (the asset the game names it by, "levels/game/BAM_LevelRoot/BAM_LevelRoot"),
// or nothing when the level has none (a custom map, a level the game made no map for). Slow: it
// opens the level's superbundle. Any thread.
// `patchRoot`: the merged mods' data (Mods/.reskate; game_bundles.h), where a level a mod adds is,
// with the map its author built it with; empty for the game's own data alone.
[[nodiscard]] std::optional<GameMap> read_game_map(const std::filesystem::path& gameRoot, std::string_view level,
                                                   const std::filesystem::path& patchRoot = {});

// Parts of a map, cut out of its tiles. One thread at a time.
class GameMapReader {
public:
    GameMapReader(std::filesystem::path gameRoot, GameMap map, const std::filesystem::path& patchRoot = {});
    [[nodiscard]] const GameMap& map() const noexcept { return map_; }
    // The square of the world `metres` a side about (x, z), `side` pixels a side, its top toward
    // -z. What lies off the map is clear. From the smallest tiles that still have a pixel for each
    // of the square's.
    [[nodiscard]] frostbite::Image window(float x, float z, float metres, std::uint32_t side);

private:
    const frostbite::Image& tile(std::size_t lod, std::uint32_t index);

    GameTextures textures_;
    GameMap map_;
    std::map<std::pair<std::size_t, std::uint32_t>, frostbite::Image> tiles_;
    std::deque<std::pair<std::size_t, std::uint32_t>> order_; // the tiles held, the oldest first
};
}
