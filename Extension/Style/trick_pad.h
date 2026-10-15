#pragma once
#include <cstdint>
#include <string>

// Plays a stick gesture on a scripted pad, so that the local skater does a trick with the game's own physics.
namespace dingosdk::trick_pad {
// Any thread.
[[nodiscard]] std::uint64_t game_ticks() noexcept;
// Any thread. `error` is a lowercase fragment.
bool play_trick(std::uint8_t trick, std::string &error);
inline constexpr std::uint32_t flick_ms = 24, tap_ms = 150, tap_end_ms = 400;
// The pad stays neutral from hold_ms to end_ms. Times are rounded to game ticks. Any thread.
void press(std::uint16_t buttons, std::uint32_t hold_ms, std::uint32_t end_ms);
// Presses Y, which gets a skater on foot onto the board. The caller checks that the skater is on foot. Any thread.
void mount();
// Starts reading the game's gestures if that has not started. True once the read ended, with or without them. Any thread.
[[nodiscard]] bool gestures_ready();
// Client thread only, after the native client tick: one game tick.
void tick() noexcept;
} // namespace dingosdk::trick_pad
