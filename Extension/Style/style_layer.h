#pragma once
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <Windows.h> // no_bail.h's masks are LONG
#include "Engine/Game/Build/20260929/no_bail.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Adds joint rotations to the local skater's pose. It does not edit animation data.
namespace dingosdk::style_layer {
// Console adapters. Thread-safe. Changes apply on the next client tick.
void request_enabled(bool enabled);
void request_share(bool share);
// Degrees. All zero removes it.
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &error);
// The new keyframe's number, or -1.
int request_key_add(std::uint8_t trick, float time, std::string &error);
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &error);
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error);
// 0 blends to the next keyframe.
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &error);
void request_clear(std::optional<style::Target> target = std::nullopt);
// Discards unsaved state and reads the saved style again.
void request_reload();
bool request_undo();
bool request_redo();
// The edits between an open and a close are one undo step, such as one drag.
void request_group(bool open);
void request_history_clear();
void request_save();
// On: each edit saves after autosave_delay_ms.
void request_auto_save(bool on);
[[nodiscard]] bool auto_saving() noexcept;
// Thread-safe.
[[nodiscard]] style::StyleModel model();
[[nodiscard]] constexpr bool grounded(std::uint32_t state) noexcept {
    return state == addr::style::riding_ground_state || state == addr::style::riding_landed_state;
}
[[nodiscard]] constexpr bool on_foot(std::uint32_t state) noexcept { return state == addr::no_bail::offboard_physics_state; }
[[nodiscard]] bool enabled() noexcept;
// False when this game build or its data does not allow the layer. Thread-safe.
[[nodiscard]] bool available() noexcept;
inline constexpr std::string_view unavailable_text = "Style is unavailable for this game build. 'style status' says why.";
[[nodiscard]] bool sharing() noexcept;
enum class PresetAction : std::uint8_t { load, create, copy, remove, open_folder };
// In the enum's order.
inline constexpr std::array<std::string_view, 5> preset_action_names{"load", "new", "copy", "delete", "folder"};
static_assert(preset_action_names.size() == static_cast<std::size_t>(PresetAction::open_folder) + 1);
[[nodiscard]] inline std::optional<PresetAction> parse_preset_action(std::string_view word) noexcept {
    const auto action = style::index_of(preset_action_names, word);
    return action ? std::optional<PresetAction>(static_cast<PresetAction>(*action)) : std::nullopt;
}
bool request_preset(PresetAction action, std::string_view name, std::string &error);
[[nodiscard]] std::string status();
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::style_layer
