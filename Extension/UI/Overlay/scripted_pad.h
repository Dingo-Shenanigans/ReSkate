#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <cstdint>
#include <vector>
#include <string_view>
#include "Engine/Game/Input/plugin_input_policy.h"

// A scripted Xbox pad for player 0, read by the game through the XInput hook (PR 297, after PR 230 by
// Norms): the character room's walk presses RB on it (Extension/World/character_room.cpp). Not for
// plugins: they never press the game's buttons.
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
// Read-only current authorship; no native input consumption claim.
bool scripted_pad_active() noexcept;
// True when it wrote the script's pad to `state`; authorship records this exact
// report's SDK navigation or preview axis/value/epoch, never global script state.
bool scripted_pad(DWORD user, XINPUT_STATE &state, input::PadAuthorship* authorship = nullptr,
    bool physical_connected = true) noexcept;
// One physical pad stick held at `value` (-1..1) for `milliseconds` more while its
// owned preview is visible; other controls stay physical. Zero lets go. `stick`: 0 left x,
// 1 left y, 2 right x, 3 right y. A script playing (play_pad) comes first. Any thread.
// Only a client-thread observation of the owning, visible character preview
// grants a short permit. A plugin's repeated hold request cannot refresh it.
void preview_context(std::string_view owner, bool visible);
bool preview_report_valid(std::uint64_t epoch, std::uint64_t now_ms) noexcept;
bool hold_stick(std::string_view owner, int stick, float value, unsigned milliseconds) noexcept;
// Snapshot the same expiring hold for native controller reads. Scripts retain priority.
bool held_stick(int& stick, SHORT& value) noexcept;
} // namespace dingosdk::overlay
