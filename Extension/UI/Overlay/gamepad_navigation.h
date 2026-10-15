#pragma once
#include <imgui.h>
#include <algorithm>
#include <cstdint>

namespace dingosdk::overlay::detail {
// Feed the existing raw multi-device reader rather than ImGui's slot-zero
// XInput backend. That backend is disabled; it would also miss native HID pads.
inline void feed_gamepad_navigation(ImGuiIO& io, bool available, std::uint16_t buttons,
                                    short lx, short ly, unsigned char lt, unsigned char rt) {
    if (available) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const auto digital = [&](ImGuiKey key, unsigned mask) { io.AddKeyEvent(key,available && (buttons & mask) != 0); };
    digital(ImGuiKey_GamepadStart,0x0010); digital(ImGuiKey_GamepadBack,0x0020);
    digital(ImGuiKey_GamepadFaceDown,0x1000); digital(ImGuiKey_GamepadFaceRight,0x2000);
    digital(ImGuiKey_GamepadFaceLeft,0x4000); digital(ImGuiKey_GamepadFaceUp,0x8000);
    digital(ImGuiKey_GamepadDpadUp,0x0001); digital(ImGuiKey_GamepadDpadDown,0x0002);
    digital(ImGuiKey_GamepadDpadLeft,0x0004); digital(ImGuiKey_GamepadDpadRight,0x0008);
    digital(ImGuiKey_GamepadL1,0x0100); digital(ImGuiKey_GamepadR1,0x0200);
    digital(ImGuiKey_GamepadL3,0x0040); digital(ImGuiKey_GamepadR3,0x0080);
    const auto analog = [&](ImGuiKey key, float value, float low, float high) {
        const float amount = available ? std::clamp((value-low)/(high-low),0.f,1.f) : 0.f;
        io.AddKeyAnalogEvent(key,amount > .1f,amount);
    };
    analog(ImGuiKey_GamepadL2,lt,30,255); analog(ImGuiKey_GamepadR2,rt,30,255);
    analog(ImGuiKey_GamepadLStickLeft,-static_cast<float>(lx),7849,32767);
    analog(ImGuiKey_GamepadLStickRight,lx,7849,32767);
    analog(ImGuiKey_GamepadLStickUp,ly,7849,32767);
    analog(ImGuiKey_GamepadLStickDown,-static_cast<float>(ly),7849,32767);
}
} // namespace dingosdk::overlay::detail
