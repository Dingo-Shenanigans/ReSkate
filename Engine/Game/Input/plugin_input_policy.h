#pragma once
#include <cstdint>

namespace dingosdk::input {
// Options on PlayStation is Start in the game's normalized pad layout.
inline constexpr std::uint16_t menu_button = 0x0010;
constexpr std::uint16_t preserve_menu_button(std::uint16_t physical, std::uint16_t replacement) noexcept {
    return static_cast<std::uint16_t>((replacement & ~menu_button) | (physical & menu_button));
}
}
