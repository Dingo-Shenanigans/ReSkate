#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Board Break: a hard slam can break the local skater's board. It snaps at the nose, in the middle
// or at the tail; half the time the grip tape holds the pieces together and the board folds or an
// end hangs, otherwise a piece comes off, flies and lies where it lands. The board is whole again
// once the skater has picked it up, rides again or respawns. How hard the slam was (the force of
// the bail cause that led to the wipeout) decides whether the board breaks: a heavy one always, a
// lighter one now and then.
//
// The pieces are not drawn by ReSkate: they are in the deck model of a content mod, one mod for
// each deck shape, any folder named Board_Break_<Name> (Engine/Vfs/mod_catalog.h). The game has one
// deck model for every board; such a mod replaces it with the same deck plus four stretches of it
// (nose, front, back, tail) cut along the break rows, each on a spare bone of the board's rig
// (contrib/board-break.md says how such a model is made). This only moves bones,
// in the boards' render pose hook (Extension/Multiplayer/Remote/native_skater.h):
//   - a broken board has its real deck moved far away, the trucks moved back to where they were,
//     and each stretch moved to where its piece lies;
//   - a whole board has its stretches moved far away, every frame, on every board in the game: the
//     game draws a deck's wood and grip as layers that do not hide what lies just under them, so
//     stretches left inside the deck show as cracks.
// Nothing is scaled: the game's replay records where a bone is and how it is turned, not its size,
// so a break made of moves plays back in the replay editor as it happened.
//
// Without such a mod installed and loading first nothing ever breaks. With one, mods.json is not
// changed: the deck the player chose goes first for the run, and the other decks and any mod that
// reshapes the deck are set aside (mod_catalog.cpp). Choosing another deck applies the mods afresh
// and loads the level again (Extension/Assets/live_mods.h); the game reads the model then.
namespace dingosdk::board_break {
// Requires the validated build, No Bail started and the local profile loaded: the switch starts
// from the saved choice (on until a player changes it).
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick, with the local client.
void on_client_tick(std::uintptr_t client) noexcept;
// From No Bail's hooks, on the physics thread: a bail cause recorded for the watched skater, and
// the moment its selector chose Wipeout (`causes` is its cause collector, read before it resets).
void note_cause(std::int32_t reason, float magnitude) noexcept;
void note_wipeout(std::uintptr_t causes) noexcept;
// From the boards' render pose hook, after a board's pose has been handed to the renderer and
// after another player's pose has been written into it (`animation_interface` is the board's
// animation holder + 0xc0): what is written to the pose here is what this frame draws.
void on_render_pose(std::uintptr_t animation_interface) noexcept;

// Any thread.
bool available() noexcept;
bool enabled() noexcept; // off: boards hold; one that is broken stays so until it is picked up
void set_enabled(bool enabled) noexcept;
// The name of the deck in the game ("standard", "wide" and so on), empty when none of Board Break's
// decks is: boards cannot break then. And a deck's name as the menus show it ("Powell"; "None").
std::string deck();
std::string deck_label(std::string_view deck);
// Asks for another deck: its name, its number in the list `boardbreak deck` prints, or "next" for
// the one after the deck in the game. Says what happens, for the console; the mods are applied and
// the level loaded again from the next tick.
std::string choose_deck(std::string wanted);
// Done at the next tick: breaks the board at `place` (0 nose, 1 middle, 2 tail; anything else is
// left to chance), to be looked at: that break stays until the board is mended here or the skater
// respawns. Or makes a broken board whole.
void snap(int place) noexcept;
void mend() noexcept;
}
