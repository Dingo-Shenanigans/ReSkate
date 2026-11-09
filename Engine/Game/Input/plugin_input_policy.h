#pragma once
#include <cstdint>

namespace dingosdk::input {
// Start (Options on PlayStation) must still open the game's settings when a
// plugin captures or replaces gameplay controls. A plugin cannot synthesize it.
inline constexpr std::uint16_t menu_button = 0x0010;
constexpr std::uint16_t preserve_menu_button(std::uint16_t physical, std::uint16_t replacement) noexcept {
    return static_cast<std::uint16_t>((replacement & ~menu_button) | (physical & menu_button));
}
constexpr bool menu_escape(bool key_down, bool key_up, std::uint64_t key) noexcept {
    return (key_down || key_up) && key == 0x1b;
}
enum class PadSource { physical, sdk_navigation, preview_hold };
struct PadAuthorship {
    PadSource source{PadSource::physical};
    std::uint64_t preview_epoch{};
};
}
