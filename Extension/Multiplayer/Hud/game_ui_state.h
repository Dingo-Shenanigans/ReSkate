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
    // A menu page has the player (pause, map, Social, a S.K.A.T.E. score card...). `in_menu` is
    // also up, without this, for the second or so the game rebuilds the skater (a respawn after
    // a bail, each S.K.A.T.E. turn) and the whole time the party Spectate camera follows someone.
    bool menu_focus{};
};

// Client thread only (it takes the UI model lock): reads the state at most every 100 ms and
// returns the latest. Before the model is found it reports no menu and nothing hidden.
GameUiState sample_game_ui_state(std::uintptr_t base) noexcept;
} // namespace dingosdk::multiplayer
