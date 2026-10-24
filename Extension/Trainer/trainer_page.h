#pragma once
// The overlay's side of the trainer: its menu page and the HUD drawn while skating.
#include "Extension/UI/Overlay/skate_menu.h"

namespace dingosdk::overlay::menu {
void trainer_page(SkateMenu &, const Model &, const CallbacksV3 &);
// `trainer open`: true once per request, after selecting its tab.
bool trainer_take_open();
// True once after a request: the menu should switch to the trainer page.
bool trainer_page_wanted();
bool trainer_cheats_wanted(); // `trainer open movement`: Skater > Cheats
}
namespace dingosdk::overlay {
// True while the HUD has something to draw, so the overlay renders with the menu closed.
bool trainer_hud_pending();
// True once per `trainer open`: the overlay should show the menu.
bool trainer_open_requested();
void draw_trainer_hud();
} // namespace dingosdk::overlay
