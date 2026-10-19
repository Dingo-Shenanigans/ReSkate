#pragma once
// The Animator Studio's loading screen draws with skate.'s own pictures, read from the installed game's
// data (Engine/Game/Build/20260929/ui_textures.h) and placed in the overlay's font atlas like Hall of
// Meat's: the grainy background, the rough tile, the brush bar, and the kickflipping board of skate.'s
// own loading screen.
#include <chrono>

struct ImDrawList;
struct ImFontAtlas;
struct ImVec2;
typedef unsigned int ImU32;

namespace dingosdk::overlay {
// Startup: starts reading the pictures in the background, once.
void prepare_animation_studio_images() noexcept;
// Each atlas the overlay builds: their room reserved first, waiting up to `wait` for a read still
// running, and filled after the atlas is built.
void reserve_animation_studio_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept;
void fill_animation_studio_images(ImFontAtlas& atlas) noexcept;

enum class StudioPicture { grain, tile, brush, kickflip, backdrop };
// Stretched onto the box. False when the picture is not there.
bool draw_studio_picture(ImDrawList* draw, StudioPicture picture, ImVec2 min, ImVec2 max, ImU32 tint);
// Filled over the box with its own shape kept: what does not fit is cropped off, evenly on both sides (a 21:9
// backdrop on a 16:9 screen loses its sides rather than being squeezed). False when the picture is not there.
bool draw_studio_picture_cover(ImDrawList* draw, StudioPicture picture, ImVec2 min, ImVec2 max, ImU32 tint);
// The brush bar's body on the box, its splatter around it.
bool draw_studio_brush(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 tint);
// The rough tile nine-sliced onto the box, `edge` pixels of rough edge.
bool draw_studio_tile(ImDrawList* draw, ImVec2 min, ImVec2 max, float edge, ImU32 tint);
// The kickflipping board, its frame at `seconds`, fitted into the box.
bool draw_studio_kickflip(ImDrawList* draw, ImVec2 min, ImVec2 max, double seconds, ImU32 tint);
}
