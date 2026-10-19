#pragma once
#include <cstdint>

// The game's own menu and HUD visibility (UI/Foundations/State/DelMar_UI_ScreenData), which
// ReSkate's nametags and chat follow: both stay off screen in the main and pause menus and
// while the game hides its UI.
namespace dingosdk::multiplayer {
struct GameUiState {
    bool in_menu{};           // the main menu, the pause menu or another game menu is up
    bool ui_hidden{};         // the game's UI is hidden (captures)
    bool nametags_hidden{};   // the game's nametag setting is off
    bool indicators_hidden{}; // the game's player indicator setting is off
    bool available{}; // the native screen model was resolved
};

// Client thread only (it takes the UI model lock). By default, cached for 100 ms.
// fresh=true reuses the same reader for gameplay input ownership on each native tick.
// Before the model is found, available=false; existing HUD flags retain their defaults.
GameUiState sample_game_ui_state(std::uintptr_t base, bool fresh = false) noexcept;
} // namespace dingosdk::multiplayer
