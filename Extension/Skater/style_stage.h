#pragma once
#include <cstdint>
#include <functional>
#include <string>

// The style editor's stage: the character room of the game's menu, opened on its Skaters page. Its backdrop is plain, not the
// HUB page's season art. Solo play.
namespace dingosdk::style_stage {
// Opens the menu's Skaters page if it is closed, then calls `then` on the client thread when the room's skater shows.
void open(std::function<void()> then);
// Closes the game's menu and returns to the world. Any thread.
void leave() noexcept;
// True when no open and no leave is in progress. Any thread.
[[nodiscard]] bool idle() noexcept;
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, bool ready) noexcept;
} // namespace dingosdk::style_stage
