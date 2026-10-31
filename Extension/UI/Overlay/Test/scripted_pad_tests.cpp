#include "Extension/UI/Overlay/scripted_pad.h"
#include "Extension/Input/input_publication.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace dingosdk::overlay;
static void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main() {
    try {
        int axis{};
        SHORT value{};
        XINPUT_STATE pad{};
        check(!held_stick(axis, value), "No native stick hold before a request");
        for (int requested = 0; requested < 4; ++requested) {
            const auto before = dingosdk::native_input_source::publication_revision();
            hold_stick(requested, -0.5f, 10000);
            check(dingosdk::native_input_source::publication_revision() > before,
                "Held stick changes the real passive publication revision");
            check(!dingosdk::native_input_source::publication_held(),
                "Held stick returns outside the publication gate");
            check(held_stick(axis, value) && axis == requested && value == -16384, "Native reads get the held axis and value");
            check(scripted_pad(0, pad), "XInput sees the same hold");
            const SHORT axes[]{pad.Gamepad.sThumbLX, pad.Gamepad.sThumbLY, pad.Gamepad.sThumbRX, pad.Gamepad.sThumbRY};
            check(axes[requested] == value && pad.Gamepad.wButtons == 0, "XInput and native hold agree");
            check(!scripted_pad(1, pad), "Stand-in remains restricted to player zero");
        }
        const auto before_script = dingosdk::native_input_source::publication_revision();
        play_pad({PadStep{0, {}}});
        check(dingosdk::native_input_source::publication_revision() > before_script,
            "Character room script changes the real passive publication revision");
        check(!dingosdk::native_input_source::publication_held(),
            "Character room script returns outside the publication gate");
        check(!held_stick(axis, value), "Character room script takes priority over native hold");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        check(held_stick(axis, value), "Completed script expires even without XInput polls");
        play_pad({});
        hold_stick(2, 1, 0);
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "Expired hold reaches neither route");
        hold_stick(2, 1, 10000);
        hold_stick(2, 0, 10000);
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "Cancellation releases both routes");
        std::puts("scripted pad tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
