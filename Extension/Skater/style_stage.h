#pragma once
#include <cstdint>
#include <functional>
#include <string>

// The style editor's stage: the game's Skatepedia, opened through the pause menu. Solo play. The game window needs the keyboard.
namespace dingosdk::style_stage {
// Opens Skatepedia if it is closed, then calls `then` on the client thread when its stage exists.
void open(std::function<void()> then);
// Closes the game's menu and Skatepedia, and returns to the world. Any thread.
void leave() noexcept;
// Makes Skatepedia's skater perform this trick, so that the editor can learn the clip. Skatepedia must be open.
void fetch(std::uint8_t trick);
// Sets the board-only entry (true), or restores the highlighted entry. Returns false on failure. Client thread only.
bool park(std::uintptr_t base, bool park) noexcept;
// The flip trick that Skatepedia shows, 0 for none. `title` gets the highlighted entry's name, empty when Skatepedia is closed.
[[nodiscard]] std::uint8_t shown_trick(std::uintptr_t base, std::string *title = nullptr) noexcept;
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, bool ready) noexcept;
} // namespace dingosdk::style_stage
