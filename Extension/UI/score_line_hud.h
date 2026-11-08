#pragma once
#include "score_line.h"
#include "Extension/Skater/score_hud.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A plugin's own line in skate.'s score HUD at the bottom left (reskate.hud.score_line), display
// only: its points never count toward the game's score, challenges, XP or progression.
//
// The SDK makes a ScoringHUDViewModel of its own (the game's type, the game's authored values copied
// in, its own empty trick list, never the game's live scoring data) and pushes an item of its own
// ("RSHS", priority 3) into HudViewModel's DpadScoringStack whose content is skate.'s own
// ScoringHUD_Widget bound to that model, as the game's own item is (Engine/Game/Build/20260929/
// score_hud.h, hud_corner.h; seen to draw in the scorep2 launch). The stack animates it in and the
// d-pad hides beside it as during a game line. The game's own model is never written, and the SDK's
// readers (score_hud.cpp, hud_corner.cpp, the trick feed) never take ours for the game's. Ours is made
// only while the game's root holds nothing of a line (root_busy), as it is a copy of that root.
//
// The trick names are rows of the game's row type, which the registry lists only once the game has
// made a row of its own in the level; until then, or after a row could not be made, a line shows
// without them (the points, ring and LINE still show).
//
// The two would draw over each other, so ours goes out on the tick skate.'s own line starts and stays
// out until its signals have been idle for game_hold_ms; ours also goes while a plugin hides the HUD
// corner, at a level load, when the plugin stops, and after lease_ms without a call. A take-out is
// tried again every tick until a read of the stack finds none of ours. Client thread, under the model
// lock the game's writers take, inside ui::OwnUiWrites.
namespace dingosdk::score_line {
// Plugins (plugin_game_runtime.cpp). `owner` shows `line`, or (nullopt) clears its own: "" when it is
// kept, or why it does not show (another plugin's line is up, or what the last tick found: skate.'s
// own line is up, the corner is hidden, the HUD is not loaded).
std::string ask(std::string_view owner, std::optional<Line> line);
void release(std::string_view owner);
// Whether tick has anything to do: a line asked for, or something of ours still in the game's UI.
bool wanted() noexcept;
// This tick's reading of skate.'s own score HUD (score_hud.h), before tick: the signals of its line.
void observe(const std::optional<tricks::ScoreHud>& hud) noexcept;
// Every client tick while wanted. `refusal`: why no plugin's line may show now ("" none), as the
// plugin layer refuses an ask; one that starts takes a line down on this tick.
void tick(std::uintptr_t base, std::string_view refusal) noexcept;
// Before a level load: ours out and destroyed while the HUD's models are still there (native_menu.cpp's
// prepare_native_menu_level_load, which ReSkate's own loads call). What cannot be taken out is kept
// for the next tick to take out, as the load may yet be cancelled.
void before_level_load(std::uintptr_t base) noexcept;
// The runtime's level transition callback (runtime_init.cpp): before_level_load for every state that
// ends the world (3, 14, 22, 24 and 25), as tick runs only in 13 and 21.
void before_level_transition(std::uintptr_t base, unsigned next) noexcept;
// The handle of our ScoringHUDViewModel, 0 while there is none; any thread.
std::uint64_t own_model() noexcept;
} // namespace dingosdk::score_line
