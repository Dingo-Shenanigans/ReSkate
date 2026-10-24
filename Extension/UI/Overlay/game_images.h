#pragma once
// Pictures of skate.'s own that plugins draw by name (the draw handler's d:game_image): the scoring
// icons, the THRASHER wordmark and the brush strokes and tiles of its menus, read from the installed
// game's data (Engine/Game/Build/20260929/ui_textures.h) and placed in the overlay's font atlas like
// the chat emotes. Nothing of the game's is shipped.
#include <chrono>
#include <string_view>
#include <imgui.h>

namespace dingosdk::overlay {
// Startup: starts reading the pictures in the background, once.
void prepare_game_images() noexcept;
// Each atlas the overlay builds: their room reserved first, waiting up to `wait` for a read still
// running, and filled after the atlas is built.
void reserve_game_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept;
void fill_game_images(ImFontAtlas& atlas) noexcept;
// Whether `name` is one of the pictures: stopwatch, wipeout, wipeout_broken, spread_eagle, airtime,
// gap_height, flaming_wheel, roll, thrasher, brush_bar, rough_tile, scratches, streak, loading_board.
bool game_image_known(std::string_view name) noexcept;
// How the picture goes onto the box: fitted with its aspect kept and centred; its body stretched onto
// the box with the rest of it around (a brush stroke's splatter beyond its bar); or nine-sliced, its
// corners `edge` pixels square.
enum class GameImageFit { contain, shape, panel };
// Render thread. False while the picture is not loaded (yet, or at all). `frame`: of a flipbook (the
// loading board's 27, at 30 a second), the one drawn, fitted; the others have none.
bool draw_game_image(ImDrawList* draw, std::string_view name, ImVec2 min, ImVec2 max, ImU32 tint, GameImageFit fit, float edge, int frame = 0);
// The THRASHER wordmark's arch: between a fifth in from either side its letters end above this
// fraction of its height, leaving room beneath.
float thrasher_arch() noexcept;
}
