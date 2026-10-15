#pragma once
#include "Extension/Style/style_pose.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// A style as a file: <profile folder>/styles/<name>.style.json.
namespace dingosdk::style {
inline constexpr std::size_t maximum_style_bytes = 256 * 1024;
// decode_style throws this for a "format" above the one this version reads.
class NewerStyle : public std::runtime_error {
public:
    NewerStyle() : std::runtime_error("this preset is from a newer ReSkate") {}
};
// A preset change (a new field or name, or a new meaning of a saved value) raises "format"; the reader upgrades the older format.
std::string encode_style(const Style &style);
// Skips unknown tricks, states and joints; clamps angles and times. Throws runtime_error for text that is not a style.
Style decode_style(std::string_view text);
[[nodiscard]] inline bool presets_due(bool enabled, std::uint64_t now, std::uint64_t at) noexcept { return enabled && now >= at; }
} // namespace dingosdk::style
