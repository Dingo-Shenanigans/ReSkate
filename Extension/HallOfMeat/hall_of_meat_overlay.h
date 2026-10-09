#pragma once
// The overlay's side of Hall of Meat (hall_of_meat.h): from the local skater's bail until they get
// up, the bones it hurt over the world, the bail's score card in the bottom left corner, and the
// screen's red edge when a bone breaks. Drawn with skate.'s own icons, logo and UI shapes, read from
// the installed game's data (Engine/Game/Build/20260929/ui_textures.h) and placed in the overlay's
// font atlas like the chat emotes. All of it on the background draw list, under ReSkate's own menus
// and chat, taking no input.
#include <chrono>

struct ImFontAtlas;

namespace dingosdk::overlay {
// Startup: starts reading the images in the background, once.
void prepare_hall_of_meat_images() noexcept;
// Each atlas the overlay builds: their room reserved first, waiting up to `wait` for a read still
// running, and filled after the atlas is built.
void reserve_hall_of_meat_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept;
void fill_hall_of_meat_images(ImFontAtlas& atlas) noexcept;
// Every presented frame: whether there is something to draw (so the overlay renders with the menu
// closed), then the drawing.
bool hall_of_meat_pending();
void draw_hall_of_meat();
}
