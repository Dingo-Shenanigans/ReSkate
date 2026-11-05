#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Input/playstation_report.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
using namespace dingosdk;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// CRC-32 over the 0xA1 input prefix and `size` bytes, table-driven, so it is written apart from the
// bit-at-a-time one under test and the two cannot share a mistake.
std::uint32_t bluetooth_crc(const std::uint8_t* bytes, std::size_t size) {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value & 1u) != 0 ? 0xedb88320u ^ (value >> 1) : value >> 1;
        table[i] = value;
    }
    std::uint32_t crc = 0xffffffffu;
    const auto feed = [&](std::uint8_t byte) { crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8); };
    feed(0xa1);
    for (std::size_t i = 0; i < size; ++i) feed(bytes[i]);
    return ~crc;
}
// The CRC a full Bluetooth report carries, little-endian at offset 74.
std::uint32_t stored_crc(const std::uint8_t* report) {
    return std::uint32_t{report[74]} | std::uint32_t{report[75]} << 8 | std::uint32_t{report[76]} << 16 |
        std::uint32_t{report[77]} << 24;
}
int main() {
    try {
        check(playstation_pad(0x054c, 0x09cc) == PlayStationPad::dualshock4, "DualShock 4 v2 is recognised");
        check(playstation_pad(0x054c, 0x0ce6) == PlayStationPad::dualsense, "DualSense is recognised");
        check(playstation_pad(0x045e, 0x0ce6) == PlayStationPad::none, "Other vendors are ignored");
        check(!playstation_bluetooth(64) && playstation_bluetooth(78) && playstation_bluetooth(547), "Transport follows the input report length");

        // Menu holds reach native reports without changing buttons, sensors or padding.
        for (const auto kind : {PlayStationPad::dualshock4, PlayStationPad::dualsense}) {
            for (const std::size_t length : {std::size_t{64}, std::size_t{78}, std::size_t{547}}) {
                if (kind == PlayStationPad::dualsense && length == 547) continue;
                std::array<std::uint8_t, 547> report{};
                report.fill(0x5a);
                report[0] = length == 64 ? 1 : kind == PlayStationPad::dualshock4 ? 0x11 : 0x31;
                const unsigned offset = report[0] == 0x31 ? 2 : report[0] == 0x11 ? 3 : 1;
                for (int axis = 0; axis < 4; ++axis) {
                    for (const std::int16_t value : {std::int16_t{-32768}, std::int16_t{0}, std::int16_t{32767}}) {
                        const auto before = report;
                        check(set_playstation_stick(kind, length > 64, report.data(), length, axis, value), "Native stick hold accepted");
                        const auto read = parse_playstation_report(kind, length > 64, report.data(), length);
                        const std::int16_t axes[]{read->left_x, read->left_y, read->right_x, read->right_y};
                        check(std::abs(int{axes[axis]} - int{value}) <= 258, "All four native axes retain XInput direction and range");
                        for (std::size_t i = 0; i < report.size(); ++i) {
                            if (i == offset + static_cast<unsigned>(axis) || (length > 64 && i >= 74 && i < 78)) continue;
                            check(report[i] == before[i], "Hold preserves unrelated report bytes and padding");
                        }
                        if (length > 64) check(stored_crc(report.data()) == bluetooth_crc(report.data(), 74), "Hold updates Bluetooth CRC before game reads");
                    }
                }
                const auto before = report;
                check(!set_playstation_stick(kind, length > 64, report.data(), length, 4, 100), "Invalid hold axis rejected");
                check(report == before, "Rejected hold leaves report intact");
            }
        }
        check(!set_playstation_stick(PlayStationPad::none, false, nullptr, 0, 0, 0), "Unknown and absent reports are ignored");

        // DS4 USB 0x01: sticks centred, hat NE, Cross, L1 + Options, L2 fully pressed.
        std::array<std::uint8_t, 64> ds4{0x01, 128, 128, 128, 128, 0x21, 0x21, 0x00, 255, 0};
        auto pad = parse_playstation_report(PlayStationPad::dualshock4, false, ds4.data(), ds4.size());
        check(pad && pad->buttons == (0x1 | 0x8 | 0x1000 | 0x100 | 0x10), "DS4 USB buttons map to XInput bits");
        check(pad->left_trigger == 255 && pad->right_trigger == 0, "DS4 USB triggers are analog");
        check(pad->left_x == 0 && pad->left_y == -258, "Centred sticks stay near zero");
        ds4[5] = 0x08; ds4[1] = 255; ds4[2] = 0;
        pad = parse_playstation_report(PlayStationPad::dualshock4, false, ds4.data(), ds4.size());
        check(pad && pad->buttons == 0x100 + 0x10 && pad->left_x > 32700 && pad->left_y > 32700, "Neutral hat and full up-right stick");

        // DS4 Bluetooth full 0x11: layout shifted by two, Triangle + R3.
        std::array<std::uint8_t, 547> bt{0x11, 0xc0, 0x00, 128, 128, 128, 128, 0x88, 0x80, 0, 0, 200};
        pad = parse_playstation_report(PlayStationPad::dualshock4, true, bt.data(), bt.size());
        check(pad && pad->buttons == (0x8000 | 0x80) && pad->right_trigger == 200, "DS4 Bluetooth full report");

        // DualSense USB 0x01: triggers at [5]/[6], buttons at [8]/[9]. Circle + Create + R1.
        std::array<std::uint8_t, 64> ds5{0x01, 128, 128, 128, 128, 0, 90, 0x17, 0x48, 0x12};
        pad = parse_playstation_report(PlayStationPad::dualsense, false, ds5.data(), ds5.size());
        check(pad && pad->buttons == (0x2000 | 0x20 | 0x200) && pad->right_trigger == 90, "DualSense USB report");

        // DualSense Bluetooth: the full 0x31 report, and the simple 0x01 report padded to 78 bytes.
        std::array<std::uint8_t, 78> ds5bt{0x31, 0x10, 128, 128, 128, 128, 40, 0, 0, 0x14, 0x20};
        pad = parse_playstation_report(PlayStationPad::dualsense, true, ds5bt.data(), ds5bt.size());
        check(pad && pad->buttons == (0x2 | 0x4000 | 0x10) && pad->left_trigger == 40, "DualSense Bluetooth full report");
        std::array<std::uint8_t, 78> simple{0x01, 128, 128, 128, 128, 0x28, 0x02, 0, 0, 0};
        pad = parse_playstation_report(PlayStationPad::dualsense, true, simple.data(), simple.size());
        check(pad && pad->buttons == (0x1000 | 0x200), "DualSense Bluetooth simple report uses the DS4 layout");
        check(!parse_playstation_report(PlayStationPad::dualsense, false, simple.data(), 5), "Short reports are rejected");

        // A changed full Bluetooth report needs a new CRC, or the game's reader discards it. A
        // DualShock 4 read over Bluetooth is padded to 547 bytes: its CRC was left stale.
        std::array<std::uint8_t, 547> ds4bt{};
        for (std::size_t i = 0; i < ds4bt.size(); ++i) ds4bt[i] = static_cast<std::uint8_t>(i * 7 + 3);
        ds4bt[0] = 0x11;
        auto padded = ds4bt;
        check(refresh_playstation_bluetooth_crc(padded.data(), padded.size()), "A padded DualShock 4 Bluetooth read has a CRC");
        check(stored_crc(padded.data()) == bluetooth_crc(padded.data(), 74), "Its CRC is at offset 74, over the first 74 bytes");
        check(std::equal(padded.begin(), padded.begin() + 74, ds4bt.begin()) &&
            std::equal(padded.begin() + 78, padded.end(), ds4bt.begin() + 78), "Only the four CRC bytes change; the padding is left alone");
        auto wire = ds4bt;
        check(refresh_playstation_bluetooth_crc(wire.data(), playstation_bluetooth_report_length) &&
            std::equal(wire.begin(), wire.begin() + 78, padded.begin()), "The 78-byte report gets the same CRC as its padded read");
        // Known answers, from zlib's crc32 over 0xA1 and the 74 bytes.
        std::array<std::uint8_t, 547> zero{0x11};
        check(refresh_playstation_bluetooth_crc(zero.data(), zero.size()) && stored_crc(zero.data()) == 0x990118acu,
            "CRC of a zeroed DualShock 4 report");
        std::array<std::uint8_t, 78> sense{0x31};
        check(refresh_playstation_bluetooth_crc(sense.data(), sense.size()) && stored_crc(sense.data()) == 0x1c9c519du,
            "CRC of a zeroed DualSense report");
        // A report with no CRC is left alone, as is a length no pad reads.
        const auto unchanged = [](unsigned id, std::size_t size) {
            std::array<std::uint8_t, 548> report{};
            report.fill(std::uint8_t{0x5a});
            report[0] = static_cast<std::uint8_t>(id);
            const auto before = report;
            return !refresh_playstation_bluetooth_crc(report.data(), size) && report == before;
        };
        check(unchanged(0x01, 64) && unchanged(0x01, 78) && unchanged(0x01, 547), "USB and simple reports have no CRC");
        check(unchanged(0x31, 547), "Only a DualShock 4 read is padded to 547 bytes");
        check(unchanged(0x11, 64) && unchanged(0x11, 77) && unchanged(0x11, 79) && unchanged(0x11, 546) && unchanged(0x11, 548),
            "A full report of another length is not written");
        check(!refresh_playstation_bluetooth_crc(nullptr, 547), "No report, nothing written");

        check(controller_combo_label(0x300) == "LB + RB", "Xbox labels");
        check(controller_combo_label(0x300 | 0x1000, ControllerStyle::dualshock4) == "L1 + R1 + Cross", "PlayStation labels");
        check(controller_combo_label(0x20, ControllerStyle::dualsense) == "Create" &&
            controller_combo_label(0x20, ControllerStyle::dualshock4) == "Share", "Back button names per pad");
        std::puts("PlayStation report checks passed."); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
