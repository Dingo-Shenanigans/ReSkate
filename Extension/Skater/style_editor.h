#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include <array>
#include <cstdint>
#include <string>

// The style editor plays clips learned from Skatepedia's demonstration on a stand-in, with the current style. Solo play only.
namespace dingosdk::style_editor {
// Console adapters. Thread-safe. Shows the clip of a trick. 0 clears the stand-in.
void request_show(std::uint8_t trick);
void request_hide();
// Holds the clip at a timeline time (0 to 3), or plays it in a loop.
void request_hold(float time);
void request_play();
// Moves a held clip by whole frames. A playing clip is held first.
void request_step(int frames);
// True if the clip of this trick is saved.
[[nodiscard]] bool has_clip(std::uint8_t trick);
// Skatepedia's skater now performs this trick for a learn. 0 cancels.
void expect(std::uint8_t trick);
// Deletes the saved clip of a trick. 0 deletes all clips.
void request_forget(std::uint8_t trick);
// Opens the editor screen. Nothing shows until the player selects a trick.
void request_open();
// The editor screen opened or closed.
void screen_open(bool open) noexcept;
// True while Skatepedia's stage exists. The caller then gives the game's camera to note_view each frame.
[[nodiscard]] bool wants_view() noexcept;
void note_view(const std::array<float, 16> &matrix, float fov);
// The fov for the editor camera. 0 keeps the current fov.
[[nodiscard]] float camera_fov() noexcept;
// Turns the editor camera around the stand-in (radians). Changes its distance and target height (metres).
void request_orbit(float yaw, float pitch, float distance, float height = 0);
// Writes the editor camera pose (rows: right, up, backward, position). False when no stand-in shows. Client thread.
bool camera_pose(std::array<float, 16> &matrix) noexcept;
// For the menu timeline: the clip on the stand-in, or a recognised replay. Thread-safe.
[[nodiscard]] style::Playhead playhead() noexcept;
// Adds the editor state to the style model for the menus. Thread-safe.
void fill(style::StyleModel &model);
// One line: the saved clips and the state of the stand-in.
[[nodiscard]] std::string status();
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::style_editor
