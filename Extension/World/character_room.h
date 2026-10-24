#pragma once
#include <cstdint>
#include <string>

// The character room of the game menu's Skaters page: a plain lit studio under every map, which the
// game draws and lights only while that page is open. open() walks the game's menu there (its own
// navigation events, and RB on a scripted pad from page to page: PR 230's walk by 22sh, as PR 297's
// Animator Studio took it) and leave() walks back to the world. skater_animation.h shows the local
// skater in it (show_in_room). reskate.room is the plugins' way in (plugin_game_runtime.cpp).
namespace dingosdk::character_room {
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
// The game menu's open page, as the navigations of its buttons in one line ("CASPresetPurchase ..."),
// empty when none shows; and whether that is the Skaters page. Client thread.
[[nodiscard]] std::string menu_buttons(std::uintptr_t base) noexcept;
[[nodiscard]] bool is_skaters_page(const std::string& buttons) noexcept;
} // namespace dingosdk::character_room
