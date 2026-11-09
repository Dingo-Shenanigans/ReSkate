#include <Windows.h>
#include "Extension/Input/input_source_environment.h"
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
    else return 2;
    std::printf("PASS %s %u actual setter and native source checks\n", argv[1], checks);
    return 0;
}
