#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include <string>
#include <string_view>

// A style as a shareable file: <Mods>/<folder>/styles/<name>.style.json.
namespace dingosdk::style {
inline constexpr std::size_t maximum_style_bytes = 256 * 1024;
std::string encode_style(const Style &style);
// Throws on text that is not a style. Skips unknown tricks, states and joints. Clamps angles and times.
Style decode_style(std::string_view text);
} // namespace dingosdk::style
