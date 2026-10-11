#pragma once
#include <cstdint>
#include <string>

// The trick stage: a plain floor far from the map with nothing else in view, for showing tricks. It is one Build Kit
// pad, scaled up, and it moves under the skater so the floor does not end. Its objects are never saved. Offline only.
namespace dingosdk::trick_stage {
// Asks for the stage. `ui_hidden`: whether the game's UI is hidden now; it is put back on close. Any thread.
void open(bool ui_hidden);
// Takes the skater back to where they were and removes the stage. Any thread.
void close();
[[nodiscard]] bool active() noexcept;
// True once the skater stands on the stage.
[[nodiscard]] bool ready() noexcept;
// What the stage is doing, or why it could not open.
[[nodiscard]] std::string status();
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::trick_stage
