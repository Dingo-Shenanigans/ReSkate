#pragma once
namespace dingosdk::overlay {
struct SkateMenu;
struct Model;
struct CallbacksV3;
// The MODS page: the installed mods.
void draw_modding_menu(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks);
}
