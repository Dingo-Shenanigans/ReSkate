#pragma once
#include <cstdint>
#include <functional>

// The character room on the game menu's Skaters page, where the editor shows its stand-in (plain backdrop, unlike the HUB).
namespace dingosdk::menu_room {
// Opens the menu's Skaters page if it is closed, then calls `then` on the client thread when the room's skater shows.
void open(std::function<void()> then);
// Closes the game's menu. Any thread.
void leave() noexcept;
// Any thread.
[[nodiscard]] bool idle() noexcept;
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, bool ready) noexcept;
} // namespace dingosdk::menu_room
