#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <optional>

namespace dingosdk {
// DualShock 4 / DualSense HID input reports translated into XInput's
// XINPUT_GAMEPAD layout. Read natively so PlayStation pads work without
// DS4Windows, Steam Input or another XInput wrapper in front of them.
inline constexpr std::uint16_t sony_vendor_id = 0x054c;
enum class PlayStationPad : std::uint8_t { none, dualshock4, dualsense };
inline constexpr PlayStationPad playstation_pad(std::uint16_t vendor, std::uint16_t product) noexcept {
    if (vendor != sony_vendor_id) return PlayStationPad::none;
    switch (product) {
    case 0x05c4: case 0x09cc: case 0x0ba0: return PlayStationPad::dualshock4; // v1, v2, wireless adapter
    case 0x0ce6: case 0x0df2: return PlayStationPad::dualsense;               // DualSense, DualSense Edge
    default: return PlayStationPad::none;
    }
}
struct PlayStationGamepad {
    std::uint16_t buttons{}; // XINPUT_GAMEPAD_* bits
    std::uint8_t left_trigger{}, right_trigger{};
    std::int16_t left_x{}, left_y{}, right_x{}, right_y{};
};
namespace detail {
inline std::int16_t playstation_axis(std::uint8_t value, bool invert) noexcept {
    // 0..255 with 128 centred and Y growing downward; XInput Y grows upward.
    const int centred = invert ? 127 - static_cast<int>(value) : static_cast<int>(value) - 128;
    return static_cast<std::int16_t>(std::clamp(centred * 258, -32768, 32767));
}
// sticks[0..3], then the hat/face byte, shoulder byte and system byte.
inline PlayStationGamepad playstation_common(const std::uint8_t* sticks, const std::uint8_t* buttons,
    std::uint8_t left_trigger, std::uint8_t right_trigger) noexcept {
    PlayStationGamepad pad;
    pad.left_x = playstation_axis(sticks[0], false); pad.left_y = playstation_axis(sticks[1], true);
    pad.right_x = playstation_axis(sticks[2], false); pad.right_y = playstation_axis(sticks[3], true);
    constexpr std::uint16_t hat[8]{0x1, 0x1 | 0x8, 0x8, 0x2 | 0x8, 0x2, 0x2 | 0x4, 0x4, 0x1 | 0x4};
    const auto face = buttons[0], shoulder = buttons[1];
    if ((face & 0x0f) < 8) pad.buttons |= hat[face & 0x0f];
    if (face & 0x10) pad.buttons |= 0x4000; // Square   -> X
    if (face & 0x20) pad.buttons |= 0x1000; // Cross    -> A
    if (face & 0x40) pad.buttons |= 0x2000; // Circle   -> B
    if (face & 0x80) pad.buttons |= 0x8000; // Triangle -> Y
    if (shoulder & 0x01) pad.buttons |= 0x0100; // L1
    if (shoulder & 0x02) pad.buttons |= 0x0200; // R1
    if (shoulder & 0x10) pad.buttons |= 0x0020; // Share / Create -> Back
    if (shoulder & 0x20) pad.buttons |= 0x0010; // Options -> Start
    if (shoulder & 0x40) pad.buttons |= 0x0040; // L3
    if (shoulder & 0x80) pad.buttons |= 0x0080; // R3
    // L2/R2 also have digital bits (0x04/0x08); the analog value feeds the
    // same trigger threshold XInput pads use.
    pad.left_trigger = left_trigger;
    pad.right_trigger = right_trigger;
    return pad;
}
}
// Windows pads every HID read to the device's longest input report, so the
// USB 64-byte report cannot be told from the Bluetooth simple report by size.
// USB pads report a 64-byte input length; Bluetooth pads report a longer one.
inline constexpr bool playstation_bluetooth(std::size_t input_report_length) noexcept {
    return input_report_length > 64;
}
// `report` includes the report id byte, as ReadFile returns it.
inline std::optional<PlayStationGamepad> parse_playstation_report(PlayStationPad kind, bool bluetooth,
    const std::uint8_t* report, std::size_t size) noexcept {
    if (!report || !size || kind == PlayStationPad::none) return {};
    const auto id = report[0];
    // DualSense USB (0x01) and Bluetooth full (0x31): triggers precede the buttons.
    if (kind == PlayStationPad::dualsense && ((id == 0x01 && !bluetooth && size >= 11) || (id == 0x31 && size >= 12))) {
        const auto* data = report + (id == 0x31 ? 2 : 1);
        return detail::playstation_common(data, data + 7, data[4], data[5]);
    }
    // DualShock 4 USB (0x01) and the Bluetooth simple report of both pads
    // (0x01, sent until the game enables full reports) share one layout; DS4
    // Bluetooth full (0x11) shifts it by two.
    if (id == 0x01 && size >= 10) return detail::playstation_common(report + 1, report + 5, report[8], report[9]);
    if (kind == PlayStationPad::dualshock4 && id == 0x11 && size >= 12)
        return detail::playstation_common(report + 3, report + 7, report[10], report[11]);
    return {};
}

// Replace only controls in a validated report. System, sequence, sensors and touch data stay
// untouched; the caller refreshes the transport checksum when this returns true.
inline bool replace_playstation_report(PlayStationPad kind, bool bluetooth, std::uint8_t* bytes,
    std::size_t size, const PlayStationGamepad& pad) noexcept {
    const auto parsed = parse_playstation_report(kind, bluetooth, bytes, size);
    if (!parsed) return false;
    const auto& before = *parsed;
    if (pad.left_trigger == before.left_trigger && pad.right_trigger == before.right_trigger &&
        pad.left_x == before.left_x && pad.left_y == before.left_y && pad.right_x == before.right_x &&
        pad.right_y == before.right_y && pad.buttons == before.buttons) return false;
    // Where parse_playstation_report found them: sticks (left X first), the shoulder byte (L2 0x04,
    // R2 0x08 digital bits) and the two analog triggers.
    std::uint8_t *sticks{}, *shoulder{}, *left{}, *right{};
    const auto id = bytes[0];
    if (kind == PlayStationPad::dualsense && ((id == 0x01 && !bluetooth) || id == 0x31)) {
        auto* data = bytes + (id == 0x31 ? 2 : 1);
        sticks = data; left = data + 4; right = data + 5; shoulder = data + 8;
    } else if (id == 0x01) {
        sticks = bytes + 1; shoulder = bytes + 6; left = bytes + 8; right = bytes + 9;
    } else if (kind == PlayStationPad::dualshock4 && id == 0x11) {
        sticks = bytes + 3; shoulder = bytes + 8; left = bytes + 10; right = bytes + 11;
    } else {
        return false;
    }
    if (pad.left_trigger != before.left_trigger) {
        *left = pad.left_trigger;
        if (pad.left_trigger <= 8) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x04u);
    }
    if (pad.right_trigger != before.right_trigger) {
        *right = pad.right_trigger;
        if (pad.right_trigger <= 8) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x08u);
    }
    const auto axis = [](std::int16_t v, bool invert) {
        const int step = v == -32768 ? -128 : static_cast<int>(std::lround(static_cast<float>(v) / 258.0f));
        return static_cast<std::uint8_t>(std::clamp(invert ? 127 - step : 128 + step, 0, 255));
    };
    if (pad.left_x != before.left_x) sticks[0] = axis(pad.left_x, false);
    if (pad.left_y != before.left_y) sticks[1] = axis(pad.left_y, true);
    if (pad.right_x != before.right_x) sticks[2] = axis(pad.right_x, false);
    if (pad.right_y != before.right_y) sticks[3] = axis(pad.right_y, true);
    if (pad.left_trigger != before.left_trigger && pad.left_trigger > 8) *shoulder |= 0x04;
    if (pad.right_trigger != before.right_trigger && pad.right_trigger > 8) *shoulder |= 0x08;
    if (pad.buttons != before.buttons) {
        const auto buttons = pad.buttons;
        std::uint8_t hat = 8;
        if (buttons & 1) hat = (buttons & 8) ? 1 : (buttons & 4) ? 7 : 0;
        else if (buttons & 2) hat = (buttons & 8) ? 3 : (buttons & 4) ? 5 : 4;
        else if (buttons & 8) hat = 2;
        else if (buttons & 4) hat = 6;
        auto& face = shoulder[-1];
        face = static_cast<std::uint8_t>(hat | ((buttons & 0x4000) ? 0x10 : 0) | ((buttons & 0x1000) ? 0x20 : 0) |
            ((buttons & 0x2000) ? 0x40 : 0) | ((buttons & 0x8000) ? 0x80 : 0));
        *shoulder = static_cast<std::uint8_t>((*shoulder & 0x0c) | ((buttons & 0x100) ? 1 : 0) | ((buttons & 0x200) ? 2 : 0) |
            ((buttons & 0x20) ? 0x10 : 0) | ((buttons & 0x10) ? 0x20 : 0) |
            ((buttons & 0x40) ? 0x40 : 0) | ((buttons & 0x80) ? 0x80 : 0));
    }
    return true;
}
}
