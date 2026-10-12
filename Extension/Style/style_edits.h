#pragma once
#include "Extension/Style/style_pose.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// No game access. An edit that fails sets `error` and leaves `style` as it was.
namespace dingosdk::style::edits {
// Degrees. All zero removes the rotation. False with `error`.
bool set_joint(Style &style, Target target, std::string_view joint, float x, float y, float z, std::string &error);
// No target removes every rotation. A trick target also removes its keyframes and blend-outs.
void clear(Style &style, std::optional<Target> target);
// The new keyframe starts as the pose the trick shows at `time` and `pace`, so nothing moves on screen. Returns its number, or -1.
int add_key(Style &style, std::uint8_t trick, float time, const Pace &pace, std::string &error);
bool move_key(Style &style, std::uint8_t trick, std::uint8_t key, float time, std::string &error);
// Later keyframes, their rotations and their blend-outs move down one number.
bool delete_key(Style &style, std::uint8_t trick, std::uint8_t key, std::string &error);
// The time from the keyframe back to the game's pose, clamped to 50-2000. 0 or less removes it.
bool set_blend_out(Style &style, std::uint8_t trick, std::uint8_t key, float ms, std::string &error);
} // namespace dingosdk::style::edits
