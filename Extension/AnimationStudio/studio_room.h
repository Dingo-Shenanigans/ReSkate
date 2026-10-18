#pragma once
#include <cstdint>
#include <string>

// The studio's room: the character room of the game menu's Skaters page (a plain lit studio under every
// map). The game draws and lights it only while that page is open, so the studio walks the game's menu
// there when it opens, and back to the world when it closes. The walk is PR #230's (menu_room.cpp, by
// 22sh): the menu's own navigation events, and RB on a scripted pad from page to page.
namespace dingosdk::anim_studio::room {
enum class Phase : std::uint8_t { idle, opening, arrived, leaving, failed };
// Any thread. Starts the walk to the Skaters page.
void open();
// Any thread. Closes the game's menu (Back, then ToggleMenu).
void leave() noexcept;
// Any thread.
[[nodiscard]] Phase phase() noexcept;
// Why the last walk failed, as a sentence.
[[nodiscard]] std::string failure();
// Client thread, after the native client tick. `skater` changes with a respawn or a new map, which stops a walk.
// `prepare`: the room is going to be wanted, so the game's navigation event is looked for now (once a map,
// in the background), and the walk does not wait for it.
void tick(std::uintptr_t base, std::uintptr_t skater, bool ready, bool prepare) noexcept;
} // namespace dingosdk::anim_studio::room
