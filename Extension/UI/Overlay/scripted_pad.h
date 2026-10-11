#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <cstdint>
#include <vector>

// A scripted Xbox pad for player 0, read by the game through the XInput hook.
namespace dingosdk::overlay {
struct PadStep {
    std::uint32_t tick{}; // game ticks from the start of the script
    XINPUT_GAMEPAD pad{};
};
// Plays the steps from the next game tick. Each step holds until the next one's tick, so the game reads each step in
// whole ticks at any frame rate. After the last step the pad stays connected and neutral for 2 s. Any thread.
void play_pad(std::vector<PadStep> steps);
// The game thread calls this once a game tick.
void pad_tick() noexcept;
// The XInput hook calls this. True if `state` now holds the script's pad for `user`.
bool scripted_pad(DWORD user, XINPUT_STATE &state) noexcept;
} // namespace dingosdk::overlay
