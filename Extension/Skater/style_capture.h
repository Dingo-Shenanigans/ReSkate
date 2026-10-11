#pragma once
#include <array>
#include <cstdint>
#include <string>

// Captures a trick that has no clip yet: the view fades to dark as the editor closes, fades up when the player's skater stands
// on the trick stage, shows the trick with the editor's camera angle under a banner, fades to dark after the landing, and the
// editor opens again on that trick. Offline only.
namespace dingosdk::style_capture {
// Starts the capture of `trick`. False with `why` in a multiplayer session, while a capture or a recording runs, and after this
// trick's capture failed in this game. Any thread.
bool start(std::uint8_t trick, std::string &why);
// Lets `trick` be captured again after its capture failed. Any thread.
void forget_failure(std::uint8_t trick) noexcept;
// True while a capture runs. Any thread.
[[nodiscard]] bool active() noexcept;
// The camera while the trick shows: the free camera's matrix (rows right, up, backward, position). False when the capture does
// not hold the camera. Any thread.
bool camera_pose(std::array<float, 16> &matrix) noexcept;
// Client thread only, after trick_recorder::tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::style_capture
