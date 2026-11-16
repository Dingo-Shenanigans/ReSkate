#pragma once
// The game's own map of the level now loaded, for plugins to draw a part of (the draw handler's
// d:game_map, reskate.world.map): a minimap under a plugin's own markers. Read from the installed
// game's data when a level comes up (Engine/Vfs/game_map.h says what is read and how the world lands
// on it), so every level the game has a map for has one here and nothing is kept per map. A level
// with none (a custom map) has none here.
//
// One worker thread reads the level's map and cuts out the part about where it is being drawn; that
// part goes to the GPU as a plugin's picture does (plugin_images.cpp) and is drawn turned, clipped
// to the plugin's box. It is cut out again as what is drawn nears its edge.
#include <optional>
#include <string_view>
#include <imgui.h>

namespace dingosdk::overlay {
// What the loaded level's map covers, in metres of the world: its middle and its size. Its right is
// +x and its top is -z.
struct GameMapArea {
    float centre_x{}, centre_z{}, width{}, height{};
};
// The level now loaded, as the game names it (empty: none). Its map is read in the background.
// Any thread; the same level again changes nothing.
void set_game_map_level(std::string_view level) noexcept;
// Nothing while the level has no map, or it is still being read. Any thread.
[[nodiscard]] std::optional<GameMapArea> game_map_area() noexcept;
// Render thread. The part of the map about (x, z) in the box: `range` metres from the box's middle
// to its side, the way `heading` faces (radians: sin and cos of it are the x and z faced) upward.
// False while there is nothing to draw (no map, or its first part is still on its way).
bool draw_game_map(ImDrawList* draw, ImVec2 min, ImVec2 max, float x, float z, float range, float heading, ImU32 tint);
}
