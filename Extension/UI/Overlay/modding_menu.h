#pragma once
namespace dingosdk::overlay {
struct SkateMenu;
struct Model;
struct CallbacksV3;
// The MODS page: tab 0 lists installed mods, tab 1 custom Lua scripts.
void draw_modding_menu(SkateMenu& menu, int tab, const Model& model, const CallbacksV3& callbacks);
}
