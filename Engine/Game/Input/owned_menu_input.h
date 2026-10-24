#pragma once
#include <cstdint>
namespace dingosdk::game {
// Keep an opening press hidden until its physical release. Otherwise handing
// input to the newly opened native page replays that same Start and closes it.
constexpr bool owned_menu_release_latch(bool latched,bool capturing,bool down) noexcept {
    return down && (capturing || latched);
}
constexpr std::uint16_t owned_menu_buttons(std::uint16_t buttons,bool suppress) noexcept {
    return suppress?static_cast<std::uint16_t>(buttons & ~0x0010u):buttons;
}
constexpr bool owned_menu_countdown_capture(bool countdown,bool native_panel) noexcept {
    return countdown && !native_panel;
}
}
