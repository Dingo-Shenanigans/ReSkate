#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Records the clips of a list of tricks in one trip to the trick stage: a fade, each trick under the editor's camera,
// then back to the editor, or to the game when the editor was not open at the start. Solo play only.
namespace dingosdk::style_capture {
// `tricks` is not empty and each one can be captured. False with `why` on a multiplayer server, or while a capture or a
// recording runs. Any thread.
bool start(std::vector<std::uint8_t> tricks, std::string &why);
// Any thread.
[[nodiscard]] bool active() noexcept;
// The free camera's matrix while a trick shows (rows right, up, backward, position); false when not framing. Any thread.
bool camera_pose(std::array<float, 16> &matrix) noexcept;
// Client thread only, after trick_recorder::tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::style_capture
