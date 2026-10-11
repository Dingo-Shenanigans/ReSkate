#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The style editor plays clips recorded on the trick stage on a stand-in in the menu's character room, with the current style. Solo play only.
namespace dingosdk::style_editor {
// Console adapters. Thread-safe. Shows the clip of a trick. 0 clears the stand-in.
void request_show(std::uint8_t trick);
void request_hide();
// Holds the clip at a timeline time (0 to 3). The stand-in eases to it.
void request_hold(float time) noexcept;
// Plays the clip in a loop from the shown moment, or pauses it there.
void request_play(bool play) noexcept;
// Moves a held clip by frames of 1/60 s. A playing clip is paused first.
void request_step(int frames) noexcept;
// Plays the clip at this fraction of its recorded speed, from the shown moment.
void request_speed(float speed) noexcept;
// True if the clip of this trick is saved.
[[nodiscard]] bool has_clip(std::uint8_t trick);
// Records the local skater for the next 4 s and keeps the jump in it as the trick's clip, if the skater did that trick.
// `flick_ms`: the scripted pad's flick, which the clip notes. False with the reason.
bool request_record(std::uint8_t trick, std::uint32_t flick_ms, std::string &error);
// The tricks to record, one bit for each: those with no clip, and those with an outdated clip.
[[nodiscard]] std::uint64_t to_record();
// On: every clip counts as outdated, so that `style record outdated` records all of them. Saved. For development.
void request_record_always(bool always);
[[nodiscard]] bool record_always() noexcept;
struct Recorded {
    std::uint8_t trick{};
    std::string failure; // empty when the clip was kept
};
// The end of the last recording of the local skater, once.
[[nodiscard]] std::optional<Recorded> take_recorded();
// Deletes the saved clip of a trick. 0 deletes all clips.
void request_forget(std::uint8_t trick);
// Opens the editor screen. Nothing shows until the player selects a trick.
void request_open();
// The editor screen opened or closed.
void screen_open(bool open) noexcept;
// The fov for the editor camera while a stand-in shows. 0 keeps the current fov.
[[nodiscard]] float camera_fov() noexcept;
// Turns the editor camera around the stand-in (radians). Changes its distance and target height (metres).
void request_orbit(float yaw, float pitch, float distance, float height = 0) noexcept;
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
