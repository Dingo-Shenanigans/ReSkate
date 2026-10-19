#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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
// Shared by native menu neutralization and selective gameplay edits.
inline void update_playstation_report_crc(std::span<std::uint8_t> report) noexcept {
    if(report.size()!=78 || (report[0]!=0x31 && report[0]!=0x11)) return;
    std::uint32_t crc=0xffffffffu;
    const auto feed=[&](std::uint8_t byte) {
        crc^=byte;
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    };
    feed(0xa1);
    for(std::size_t i=0;i<report.size()-4;++i) feed(report[i]);
    crc=~crc;
    for(unsigned i=0;i<4;++i) report[report.size()-4+i]=static_cast<std::uint8_t>(crc>>(i*8));
}

}
