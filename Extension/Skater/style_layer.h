#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The style layer adds joint rotations to the local skater's pose. It does not edit animation data.
namespace dingosdk::style_layer {
// Console adapters. Thread-safe. Changes apply on the next client tick.
void request_enabled(bool enabled);
void request_share(bool share);
// Sets the rotation of one joint in degrees. All zero removes it. On failure, sets `error`.
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &error);
// Returns the number of the new keyframe, or -1 with `error` set.
int request_key_add(std::uint8_t trick, float time, std::string &error);
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &error);
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error);
// Sets a keyframe's blend out in ms. 0 blends to the next keyframe.
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &error);
// Removes every rotation, or those of one target.
void request_clear(std::optional<style::Target> target = std::nullopt);
// Discards unsaved state and reads the saved style again.
void request_reload();
// Puts the style back to before the last edit, or does that edit again. False if there is none.
bool request_undo();
bool request_redo();
// The edits between an open and a close are one undo step, such as one drag.
void request_group(bool open);
// Forgets the edits to undo and redo. The editor does this when it leaves a trick.
void request_history_clear();
// Writes the preset now.
void request_save();
// On: each edit saves after 750 ms. Off: edits wait for request_save.
void request_auto_save(bool on);
[[nodiscard]] bool auto_saving() noexcept;
// Thread-safe copy for the menus.
[[nodiscard]] style::StyleModel model();
// Shows the pose of one flip trick at a timeline time, or in a loop. 0 stops the preview.
void request_preview(std::uint8_t trick, float time = 0, bool play = false);
// The style editor runs in solo play only. True lets it run in a multiplayer session, for a test.
void request_session_test(bool allowed);
[[nodiscard]] bool session_test() noexcept;
// Shows a replayed flip trick with the current style. Default on. Solo play only.
void request_restyle(bool restyle);
[[nodiscard]] bool restyling() noexcept;
// The replayed trick on screen and its timeline time. Thread-safe.
[[nodiscard]] style::Playhead replay_playhead() noexcept;
// Records all other rigs for 8 s. The editor uses this to capture Skatepedia's demonstration.
void request_learn();
[[nodiscard]] bool enabled() noexcept;
[[nodiscard]] bool sharing() noexcept;
// Presets are named style files. `action`: load, new (an empty preset), copy (the style in use, under a new name),
// delete, folder (opens the folder that holds them). False with the reason in `error`.
bool request_preset(std::string_view action, std::string_view name, std::string &error);
// One line: the layer state, the reason if it is not applied, and the counters.
[[nodiscard]] std::string status();
// Client thread only, after the native client tick. A preview stops when `menu_open` is false.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool menu_open) noexcept;
} // namespace dingosdk::style_layer
