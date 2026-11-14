#include <Windows.h>
#include "Extension/Input/input_source_environment.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
ULONGLONG fixture_now = 1000;
ULONGLONG WINAPI fixture_tick_count() noexcept { return fixture_now; }
}
// The fixture calls the production exports and rewrites over a controlled clock.
// Link-time section removal leaves the unrelated hook and UI entry points unused.
#define GetTickCount64 fixture_tick_count
#include "Extension/UI/Overlay/input_capture.cpp"
#undef GetTickCount64

namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace capture_detail = dingosdk::overlay::detail;
namespace capture_fixture {
extern bool hid_mode, menu_capture;
}
namespace {
unsigned checks{};
void check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
void set_turn(bool on) { DingoSDKOverlaySetGrindStickTurn(on, on ? 0.5f : 0.0f, 0.0f, 1.0f); }
void set_lean(bool on) { DingoSDKOverlaySetGrindStickLean(on, on ? 0.5f : 0.0f, 0.0f); }
using Setter = void (*)(bool);
void disabled_helpers(bool turn, bool lean) {
    if (turn) set_turn(false);
    if (lean) set_lean(false);
}
lease::Receipt receipt(lease::Handle handle) {
    lease::Receipt result; lease::Reason why;
    check(source::lease_state(1, handle, result, why) && why == lease::Reason::none,
        "the exact owner can read its receipt");
    return result;
}
void client_tick() { source::tick(1, 2, true, {true, false}); }
void report() {
    source::Poll poll;
    source::begin(true, 0, poll);
    ++fixture_now;
    const XINPUT_GAMEPAD raw{0, 0, 0, -1234, 2345, 0, -2222};
    auto returned = raw;
    (void)capture_detail::turn_grind_stick(returned);
    capture_detail::lean_grind_stick(returned);
    source::finish(poll, 0, true, 7, capture_detail::passive_pad(raw),
        0, true, 7, capture_detail::passive_pad(returned));
}
lease::Handle acquire() {
    client_tick(); report();
    lease::Handle result; lease::Reason why;
    check(source::lease_request(1, 0, result, why) && result.sequence && why == lease::Reason::none,
        "a real constructed original admits a passive lease");
    check(receipt(result).status == lease::Status::warming, "admission waits for a later original");
    return result;
}
void read_and_renew(lease::Handle handle) {
    lease::Snapshot sample; lease::Reason why;
    check(source::lease_read(1, handle, sample, why) && why == lease::Reason::none,
        "the later original supplies a readable sample");
    check(sample.handle == handle && sample.packet == 7 && sample.pad.left_x == -1234 &&
        sample.pad.left_y == 2345 && sample.pad.right_x == 0 && sample.pad.right_y == -2222,
        "disabled helpers preserve the exact physical pad and packet");
    check(source::lease_renew(1, handle, sample, why) && why == lease::Reason::none,
        "the actual issued sample renews the lease");
    check(receipt(handle).status == lease::Status::active, "the renewed lease stays active");
}
void acknowledge(lease::Handle handle) {
    lease::Reason why;
    check(source::lease_acknowledge(1, handle, why), "a revoked lease is acknowledged after callbacks drain");
}
void idle(bool turn, bool lean) {
    const auto handle = acquire();
    // Runtime dispatches plugins before the trainer refreshes its disabled helpers.
    // Several frame updates must leave this warming lease intact before the next poll.
    for (unsigned frame = 0; frame < 3; ++frame) {
        fixture_now += 16; client_tick();
        const auto before = source::publication_revision();
        disabled_helpers(turn, lean);
        check(receipt(handle).status == lease::Status::warming,
            "trainer-style disabled updates preserve a newly warming lease");
        check(source::publication_revision() == before, "repeated disabled updates do not publish interference");
    }
    report(); read_and_renew(handle);
    for (unsigned frame = 0; frame < 7; ++frame) {
        fixture_now += 16; client_tick();
        const auto before = source::publication_revision();
        disabled_helpers(turn, lean);
        check(source::publication_revision() == before, "disabled frame refresh preserves publication authority");
        report(); read_and_renew(handle);
    }
    check(receipt(handle).renewals == 8, "every later original can renew across disabled frame updates");
    lease::Reason why;
    check(source::lease_release(1, handle, why), "the live passive lease releases normally");
    acknowledge(handle);
}
void changes(Setter setter) {
    auto handle = acquire();
    auto before = source::publication_revision();
    setter(true);
    check(source::publication_revision() > before, "enabling a real helper publishes interference");
    auto ended = receipt(handle);
    check(ended.status == lease::Status::revoked && ended.reason == lease::Reason::interference,
        "enabling revokes a warming lease immediately");
    fixture_now += 16;
    before = source::publication_revision(); setter(true);
    check(source::publication_revision() > before, "an active deadline renewal stays publication-visible");
    before = source::publication_revision(); setter(false);
    check(source::publication_revision() > before, "deactivation stays publication-visible");
    before = source::publication_revision(); setter(false);
    check(source::publication_revision() == before, "a repeated deactivation is idle");
    acknowledge(handle);

    handle = acquire();
    source::Poll pending; source::begin(true, 0, pending);
    before = source::publication_revision(); setter(true); setter(false);
    check(source::publication_revision() > before, "enable-disable ABA advances publication");
    ended = receipt(handle);
    check(ended.status == lease::Status::revoked && ended.reason == lease::Reason::interference &&
        ended.polls_in_flight == 1 && !ended.source_released,
        "ABA retains the exact in-flight original after revocation");
    ++fixture_now;
    source::finish(pending, 0, true, 7, {}, 0, true, 7, {});
    check(receipt(handle).source_released, "the actual old original drains before acknowledgement");
    acknowledge(handle);

    handle = acquire();
    before = source::publication_revision(); setter(false);
    check(source::publication_revision() == before, "a fresh owner survives the now-idle helper");
    report(); read_and_renew(handle);
    lease::Reason why;
    check(source::lease_release(1, handle, why), "the later owner releases normally");
    acknowledge(handle);
}
// Check the CRC independently from the production bit-at-a-time implementation.
std::uint32_t report_crc(const std::uint8_t* bytes) {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value & 1u) ? 0xedb88320u ^ (value >> 1) : value >> 1;
        table[i] = value;
    }
    std::uint32_t crc = 0xffffffffu;
    const auto feed = [&](std::uint8_t byte) { crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8); };
    feed(0xa1);
    for (std::size_t i = 0; i < 74; ++i) feed(bytes[i]);
    return ~crc;
}
void hid_menu() {
    capture_fixture::hid_mode = true;
    auto& overlay = capture_detail::state();
    client_tick();
    // The long report 0x01 has USB buttons despite a length usually associated with Bluetooth.
    // Include Options and another button so preserving the already-mutated pad cannot pass.
    const auto edge_report = [] {
        std::array<std::uint8_t, 78> bytes{0x01, 220, 10, 30, 240, 40, 90, 0x17, 0x41, 0x32};
        bytes[12] = 0x33;
        bytes[50] = 0x5a;
        return bytes;
    };
    const auto bluetooth_report = [] {
        std::array<std::uint8_t, 78> bytes{0x31, 0x10, 220, 10, 30, 240, 40, 90, 0x17, 0x41, 0x32};
        bytes[12] = 0x33;
        bytes[50] = 0x5a;
        dingosdk::refresh_playstation_bluetooth_crc(bytes.data(), bytes.size());
        return bytes;
    };
    for (const bool full_bluetooth : {false, true}) {
        for (const bool menu_capture : {false, true}) {
            auto bytes = full_bluetooth ? bluetooth_report() : edge_report();
            const auto before = bytes;
            capture_fixture::menu_capture = menu_capture;
            overlay.freecam_controller_active.store(!menu_capture);
            const capture_detail::InputCaptureHooks::HidRead read{nullptr, bytes.data(),
                static_cast<DWORD>(bytes.size()), dingosdk::PlayStationPad::dualsense};
            capture_detail::neutral_hid(read, static_cast<DWORD>(bytes.size()));
            const auto pad = dingosdk::parse_playstation_report(read.kind, true, bytes.data(), bytes.size());
            check(pad && pad->buttons == (menu_capture ? 0 : dingosdk::input::menu_button),
                "gameplay capture preserves the physical Menu button, while SDK menu capture neutralizes it");
            check(pad->left_trigger == 0 && pad->right_trigger == 0 &&
                std::abs(int{pad->left_x}) <= 258 && std::abs(int{pad->left_y}) <= 258 &&
                std::abs(int{pad->right_x}) <= 258 && std::abs(int{pad->right_y}) <= 258,
                "the actual HID neutralizer clears gameplay controls in the content-selected layout");
            const auto buttons = full_bluetooth ? std::size_t{9} : std::size_t{8};
            check((bytes[buttons] & 0x0f) == 8 && bytes[12] == before[12] && bytes[50] == before[50],
                "a long USB read leaves the D-pad neutral and preserves its identifying tail");
            if (full_bluetooth) {
                const auto stored = std::uint32_t{bytes[74]} | std::uint32_t{bytes[75]} << 8 |
                    std::uint32_t{bytes[76]} << 16 | std::uint32_t{bytes[77]} << 24;
                check(stored == report_crc(bytes.data()), "actual Bluetooth neutralization refreshes the wire CRC");
            }
        }
    }
    capture_fixture::menu_capture = false;
    overlay.freecam_controller_active.store(false);
    capture_detail::challenge_menu_capture.store(true);
    // This tests the Options-only leaf directly. The current dispatcher calls it only while
    // input capture is active, so this does not qualify challenge-menu dispatch.
    auto bytes = edge_report();
    auto expected = bytes;
    expected[9] &= std::uint8_t{0xdf};
    const capture_detail::InputCaptureHooks::HidRead read{nullptr, bytes.data(),
        static_cast<DWORD>(bytes.size()), dingosdk::PlayStationPad::dualsense};
    capture_detail::neutral_hid(read, static_cast<DWORD>(bytes.size()));
    check(bytes == expected, "a challenge menu suppresses only Options in the long USB layout");
    capture_detail::challenge_menu_capture.store(false);

    // The extra native HID path must follow the same menu observation as XInput hiding.
    dingosdk::overlay::hide_game_buttons(0x2000 | 0x0001 | dingosdk::input::menu_button);
    check(!(capture_detail::mode_hidden_buttons.load() & dingosdk::input::menu_button),
        "a plugin cannot request that the Menu button be hidden");
    const auto check_hiding = [&](source::NativeMenu menu, bool stale, bool allowed) {
        client_tick();
        source::tick(1, 2, true, menu);
        if (stale) fixture_now += lease::maximum_age_ms + 1;
        auto report = edge_report();
        const auto physical = dingosdk::parse_playstation_report(read.kind, true, report.data(), report.size());
        const auto before = report;
        const capture_detail::InputCaptureHooks::HidRead hidden_read{nullptr, report.data(),
            static_cast<DWORD>(report.size()), read.kind};
        capture_detail::hide_hid_buttons(hidden_read, static_cast<DWORD>(report.size()));
        if (allowed) {
            const auto returned = dingosdk::parse_playstation_report(read.kind, true, report.data(), report.size());
            check(physical && returned && returned->buttons == (physical->buttons & ~(0x2000 | 0x0001)),
                "a fresh closed menu admits hiding through the actual long-USB HID path");
        } else check(report == before, "open, unknown and stale native menus retain every physical HID byte");
    };
    check_hiding({true, false}, false, true);
    check_hiding({true, true}, false, false);
    check_hiding({}, false, false);
    check_hiding({true, false}, true, false);
}
}
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept { return {fixture_now, 1, true, true, true, true}; }
bool trusted_capture() noexcept { return false; }
bool preview_report_valid(std::uint64_t, std::uint64_t) noexcept { return false; }
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "turn_idle") idle(true, false);
    else if (mode == "lean_idle") idle(false, true);
    else if (mode == "both_idle") idle(true, true);
    else if (mode == "turn_changes") changes(set_turn);
    else if (mode == "lean_changes") changes(set_lean);
    else if (mode == "hid_menu") hid_menu();
    else return 2;
    std::printf("PASS %s %u actual setter and native source checks\n", argv[1], checks);
    return 0;
}
