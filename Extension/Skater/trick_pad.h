#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Plays a stick gesture on a scripted pad, so that the local skater does a trick with the game's own physics.
namespace dingosdk::trick_pad {
// Recordings run on game ticks: one for each client update, which the game runs at this rate while it records. A recording
// is then the same at any frame rate and on any machine.
inline constexpr std::uint32_t tick_hz = 60;
// The game ticks so far. Any thread.
[[nodiscard]] std::uint64_t game_ticks() noexcept;
// Milliseconds and game ticks at tick_hz, rounded.
[[nodiscard]] inline constexpr std::uint32_t to_ticks(std::uint32_t ms) noexcept { return (ms * tick_hz + 500) / 1000; }
[[nodiscard]] inline constexpr std::uint64_t to_ms(std::uint64_t ticks) noexcept { return (ticks * 1000 + tick_hz / 2) / tick_hz; }
// Plays a gesture. A flip trick plays the game's own gesture for it (read from the game's data on first use), and `steps`
// may give the flick's duration in milliseconds. For "steps" it takes
// `ms:rx,ry[,buttons hex,lx,ly,lt,rt]` items: sticks from -1 to 1, triggers from 0 to 255. Times are rounded to game ticks.
// Any thread.
bool play(std::string_view gesture, std::string_view steps, std::string &error);
// A trick's flick when `play` is given none.
inline constexpr std::uint32_t default_flick_ms = 24;
// Starts reading the game's gestures if that has not started. True once they are read. Any thread.
[[nodiscard]] bool gestures_ready();
// Client thread only, after the native client tick: one game tick.
void tick() noexcept;
} // namespace dingosdk::trick_pad
