#pragma once
#include "plugin_game.h"
#include <cstdint>
#include <filesystem>

// The wider set's game in the real game (plugin_game.h): each part through the function ReSkate's
// own console or menu uses, under the same rules. Client thread. The binds players change on the
// Binds page are kept in `storage`/binds.json.
namespace dingosdk::plugins {
Game& real_game(std::uintptr_t image_base, const std::filesystem::path& storage);
// The client tick's camera phase, after Spectate's: shows the plugin camera, or gives it back.
void apply_plugin_camera(std::uintptr_t image_base, std::uintptr_t client, bool ready, bool phase) noexcept;
// Whether a plugin holds the skater still: noclip's input is then none.
bool plugin_freeze_active() noexcept;
// Whether a plugin's flight holds the controller (reskate.input.capture): the game gets none of it.
bool plugin_input_capture() noexcept;
// Each client tick: the local skater for plugins' clips (reskate.animation).
void tick_plugin_animation(std::uintptr_t image_base, std::uintptr_t client) noexcept;
// Each client tick: game settings plugins hold (reskate.settings.set) that change how the game plays
// go back as a multiplayer session starts.
void tick_plugin_settings() noexcept;
// Each client tick: plugins' sounds (reskate.audio); `ready` while a level plays.
void tick_plugin_sounds(std::uintptr_t image_base, std::uintptr_t client, bool ready) noexcept;
}
