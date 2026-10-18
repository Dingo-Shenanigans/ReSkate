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
// Any thread. Starts at the next game tick; each step holds until the next step's tick, so frame rate does not matter.
// After the last step the pad stays connected and neutral for 2 s.
void play_pad(std::vector<PadStep> steps);
// The game thread calls this once a game tick.
void pad_tick() noexcept;
// True when it wrote the script's pad to `state`.
bool scripted_pad(DWORD user, XINPUT_STATE &state) noexcept;
} // namespace dingosdk::overlay
