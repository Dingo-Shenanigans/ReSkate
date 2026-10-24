#pragma once
#include <cstdint>
#include <string_view>

// skate.'s HUD in the bottom left corner (the d-pad menu, and the score HUD there while a line goes
// on), hidden while a plugin shows something of its own there (reskate.hud.corner) (Engine/Game/Build/20260929/hud_corner.h):
// - the d-pad hides itself while it is not alone in its stack: an empty item of ours lies in the
//   stack beside it (the game's items are never touched);
// - the score HUD shows by its view model alone: a hook on the UI model's write of one value
//   (Engine/Game/Build/20260929/ui_model.h) writes ours over its HudWidgetActive and
//   ExtraInfoStyle and keeps what the game meant, so the game keeps writing them but the widget only
//   ever sees the HUD down, until it is given back.
// The game's models are read and written on the client thread only, under the model lock its own
// writers take too.
namespace dingosdk::hud_corner {
// Startup, with the game's image base: prepares the model write hook, which patches nothing; without
// it the score HUD stays as it is.
void start_hud(std::uintptr_t base) noexcept;
// Client thread: the model write hook on while a plugin may hide the corner, off otherwise, so the
// game's UI writes never pass through it for nothing. False when it could not be changed (logged).
bool hook_hud(bool on) noexcept;
// Client thread, every tick: hides the corner while `hidden`, and gives the game's own back after.
// True once nothing of ours is left in it.
bool hide_hud(bool hidden) noexcept;

// Plugins: `hidden` hides the corner while any plugin asks; false keeps the plugin ready to (the
// hook on, so hiding it later is immediate); release drops the plugin. Client thread.
void ask(std::string_view owner, bool hidden);
void release(std::string_view owner);
// Client thread, every tick.
void tick() noexcept;
}
