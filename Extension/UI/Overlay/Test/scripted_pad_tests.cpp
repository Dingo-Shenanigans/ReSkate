#include "Extension/UI/Overlay/scripted_pad.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/native_input_capture.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace dingosdk::overlay;
namespace source = dingosdk::native_input_source;
namespace {
unsigned checks{}, safety_writes{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
void fresh_preview(std::string_view owner = "preview") {
    source::tick(1, 2, true, {true, true});
    preview_context(owner, true);
}
dingosdk::input_lease::Pad raw_pad(const XINPUT_GAMEPAD& pad) {
    return {pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger,
        pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY};
}
bool publish(void* output, std::uint32_t packet, const dingosdk::input_lease::Pad& pad) noexcept {
    if (!source::publication_held()) std::abort();
    ++safety_writes;
    *static_cast<XINPUT_STATE*>(output) = {packet, {pad.buttons, pad.left_trigger, pad.right_trigger,
        pad.left_x, pad.left_y, pad.right_x, pad.right_y}};
    return true;
}
XINPUT_STATE physical() { return {123, {0x1011, 190, 171, 22000, -17000, 11000, -23000}}; }
bool same(const XINPUT_STATE& a, const XINPUT_STATE& b) {
    return a.dwPacketNumber == b.dwPacketNumber && raw_pad(a.Gamepad) == raw_pad(b.Gamepad);
}
void finish(source::Poll& poll, const XINPUT_STATE& raw, XINPUT_STATE& candidate,
    dingosdk::input::PadAuthorship authorship) {
    const auto result = source::finish_capture(poll, 0, true, raw.dwPacketNumber, raw_pad(raw.Gamepad),
        0, true, candidate.dwPacketNumber, raw_pad(candidate.Gamepad), source::publication_revision(),
        &candidate, &publish, authorship);
    check(result == 0, "finalizer keeps the actual transport result");
}
void sdk_navigation() {
    for (const auto button : {XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_B}) {
        source::tick(1, 2, true, {true, true});
        source::Poll poll; source::begin(true, 0, poll);
        const auto raw = physical(); auto candidate = raw;
        XINPUT_GAMEPAD pressed{}; pressed.wButtons = static_cast<WORD>(button);
        play_pad({{0, pressed}, {6, {}}, {12, {}}});
        dingosdk::input::PadAuthorship authorship;
        check(scripted_pad(0, candidate, &authorship) &&
            authorship.source == dingosdk::input::PadSource::sdk_navigation && !authorship.preview_epoch,
            "the actual fixed SDK walk marks the report it authored");
        const auto before = safety_writes;
        finish(poll, raw, candidate, authorship);
        check(candidate.Gamepad.wButtons == button && safety_writes == before,
            "connected user zero retains actual SDK RB/B through the native-menu finalizer");
        source::Poll unrelated; source::begin(true, 0, unrelated);
        XINPUT_STATE hostile{};
        finish(unrelated, raw, hostile, {});
        check(same(hostile, raw) && safety_writes == before + 1,
            "a globally active SDK script does not authorize an unrelated filtered report");
        play_pad({});
    }
}
}
int main() {
    try {
        int axis{}; SHORT value{}; XINPUT_STATE pad{};
        check(!held_stick(axis, value) && !hold_stick("preview", 1, 1, 10000),
            "a plugin cannot create a hold before host preview observation");
        for (int requested = 0; requested < 4; ++requested) {
            fresh_preview();
            const auto before = source::publication_revision();
            check(hold_stick("preview", requested, -0.5f, 10000), "the observed owner can request its preview axis");
            check(source::publication_revision() > before && !source::publication_held(),
                "preview changes advance the publication revision outside their gate");
            check(held_stick(axis, value) && axis == requested && value == -16384,
                "the shared native consumer receives the permitted axis");
            source::Poll preview_poll; source::begin(true, 0, preview_poll);
            const auto raw = physical(); pad = raw;
            dingosdk::input::PadAuthorship authorship;
            check(scripted_pad(0, pad, &authorship) &&
                authorship.source == dingosdk::input::PadSource::preview_hold && authorship.preview_epoch,
                "a plugin preview hold is distinct from fixed SDK navigation");
            const SHORT axes[]{pad.Gamepad.sThumbLX, pad.Gamepad.sThumbLY, pad.Gamepad.sThumbRX, pad.Gamepad.sThumbRY};
            const SHORT raw_axes[]{raw.Gamepad.sThumbLX, raw.Gamepad.sThumbLY, raw.Gamepad.sThumbRX, raw.Gamepad.sThumbRY};
            check(axes[requested] == value && pad.Gamepad.wButtons == raw.Gamepad.wButtons &&
                pad.Gamepad.bLeftTrigger == raw.Gamepad.bLeftTrigger && pad.Gamepad.bRightTrigger == raw.Gamepad.bRightTrigger,
                "preview holds preserve physical menu buttons and unrelated controls");
            for (int other = 0; other != 4; ++other)
                if (other != requested) check(axes[other] == raw_axes[other], "a preview hold preserves every unrelated physical axis");
            const auto before_finish = pad;
            finish(preview_poll, raw, pad, authorship);
            check(same(pad, before_finish), "a still-current preview report retains its one permitted axis through finalization");
            check(!scripted_pad(1, pad) && !scripted_pad(0, pad, nullptr, false),
                "a preview hold neither adopts another user nor invents a connected controller");
            check(!hold_stick("other", requested, 1, 10000), "another plugin cannot turn the owned preview");
        }
        preview_context({}, false);
        for (unsigned i = 0; i != 10; ++i)
            check(!hold_stick("preview", 1, 1, 10000), "settings cannot be reauthorized by repeated preview requests");
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "non-preview pages revoke both native and XInput consumption");
        source::tick(1, 2, true);
        preview_context("preview", true);
        check(!hold_stick("preview", 1, 1, 10000), "unknown native-menu input context denies even a claimed visible preview");
        fresh_preview(); check(hold_stick("preview", 1, 1, 10000), "fresh preview prepares expiry case");
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        source::tick(1, 2, true, {true, true});
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "missing animation observations expire despite fresh input ticks");
        fresh_preview(); check(hold_stick("preview", 1, 1, 10000), "fresh preview prepares input-stall case");
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        preview_context("preview", true);
        check(!hold_stick("preview", 1, 1, 10000) && !held_stick(axis, value),
            "animation observations alone cannot replace missing verified input ticks");
        fresh_preview(); check(hold_stick("preview", 1, 1, 10000), "fresh preview prepares staged leave case");
        source::Poll staged; source::begin(true, 0, staged);
        const auto raw = physical(); auto candidate = raw; dingosdk::input::PadAuthorship preview_authorship;
        check(scripted_pad(0, candidate, &preview_authorship), "staged preview report is actually authored");
        preview_context({}, false);
        finish(staged, raw, candidate, preview_authorship);
        check(same(candidate, raw), "leaving preview revokes a staged axis mutation before publication");
        sdk_navigation();
        fresh_preview(); check(hold_stick("preview", 2, 1, 10000), "hold exists before an SDK script");
        play_pad({PadStep{0, {}}});
        check(!held_stick(axis, value), "fixed SDK navigation has priority over preview holds");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        check(!held_stick(axis, value), "script completion cannot revive an expired preview permit");
        play_pad({}); fresh_preview();
        check(hold_stick("preview", 2, 1, 10000) && hold_stick("preview", 2, 0, 10000), "the owner can cancel its fresh hold");
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "cancellation releases both routes");
        std::printf("PASS %u scripted-pad/provider checks; constructed menu observations only\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
