#pragma once
#include <cstdint>
#include <string>

// A plain floor far from the map to do the tricks on while they are recorded: one scaled-up Build Kit
// pad that moves under the skater. Never saved. Solo play only. From PR #230 (Norms).
namespace dingosdk::anim_studio::stage {
// `ui_hidden`: whether the game's UI is hidden now; it is put back on close. Any thread.
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
} // namespace dingosdk::anim_studio::stage
