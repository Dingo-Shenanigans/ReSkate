#pragma once
#include "Extension/Console/commands.h"
#include "Extension/Style/style_model.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

// The style console commands' argument helpers.
namespace dingosdk::console {
inline Argument trick_argument(bool optional = false) {
    auto trick = argument("trick", Type::text, optional);
    trick.choices.assign(style::flip_trick_names.begin() + 1, style::flip_trick_names.end());
    return trick;
}
inline Argument number_argument(std::string name, double low, double high, bool optional = false) {
    auto number = argument(std::move(name), Type::number, optional);
    number.minimum = low;
    number.maximum = high;
    return number;
}
inline float number_at(const Values &args, std::size_t i) { return static_cast<float>(std::get<double>(args[i])); }
// 0 when no trick is given at `i`.
inline std::uint8_t trick_at(const Values &args, std::size_t i) {
    return i < args.size() ? style::parse_flip_trick(std::get<std::string>(args[i])).value_or(0) : std::uint8_t{};
}
} // namespace dingosdk::console
