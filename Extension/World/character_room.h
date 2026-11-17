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
// The room's set and lights without the game's menu. The menu's Skaters page shows them by a global
// event of the game's UI (UI/Foundations/Layers/Backgrounds/EnterCasEnvironment, and
// ExitCasEnvironment when it is left): the level's Prefab_3DMenuSetup has its CAS background and the
// character's light rig made for it (found 2026-11-16 in the game's data: the prefab's switches are
// its interface's, one a background, and the two events are beside the UI's background layers).
// show_set posts the same events itself, from tick: nothing of the menu opens. The room's visual
// environment is the prefab's own doing, while the camera is inside the room. Any thread.
void show_set(bool on) noexcept;
// Whether the event that shows the set was posted on this map (and the one that takes it away was not, since).
[[nodiscard]] bool set_shown() noexcept;
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
