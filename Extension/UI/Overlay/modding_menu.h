#pragma once
#include <cstddef>

namespace dingosdk::overlay {
struct SkateMenu;
struct Model;
struct CallbacksV3;
// The MODS page: tab 0 lists installed mods, tab 1 custom Lua scripts.
void draw_modding_menu(SkateMenu& menu, int tab, const Model& model, const CallbacksV3& callbacks);
// What of the installed mods wants a look, for HOME: how many Thunderstore has a newer version of, and
// how many were left out or could not load. Render thread.
struct ModAttention {
    std::size_t updates{}, problems{};
};
ModAttention mod_attention();
}
