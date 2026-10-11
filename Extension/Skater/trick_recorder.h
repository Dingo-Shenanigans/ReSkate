#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Records flip tricks one after another on the trick stage, each as its trick's clip. It opens the stage, gets the skater
// on the board, plays each trick on the scripted pad, and tries a trick again when the take is not kept. It closes the
// stage at the end if it opened it. Offline only.
namespace dingosdk::trick_recorder {
// The recording starts this long before the stick moves, so that the clip opens with the skater standing still.
inline constexpr std::uint32_t still_ms = 600;
// `flick`: the flick's duration in ms, empty for the default. `ui_hidden`: whether the game's UI is hidden now. `keep_stage`: the
// stage stays open at the end, for the caller to close. Any thread.
bool start(std::vector<std::uint8_t> tricks, std::string flick, bool ui_hidden, std::string &error, bool keep_stage = false);
void stop();
// True while a recording session runs. Any thread.
[[nodiscard]] bool active();
[[nodiscard]] std::string status();
// Client thread only, after trick_stage::tick.
void tick(std::uintptr_t base, bool ready) noexcept;
} // namespace dingosdk::trick_recorder
