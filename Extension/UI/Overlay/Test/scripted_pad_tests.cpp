#include "Extension/UI/Overlay/scripted_pad.h"
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
            hold_stick(requested, -0.5f, 10000);
            check(held_stick(axis, value) && axis == requested && value == -16384, "Native reads get the held axis and value");
            check(scripted_pad(0, pad), "XInput sees the same hold");
            const SHORT axes[]{pad.Gamepad.sThumbLX, pad.Gamepad.sThumbLY, pad.Gamepad.sThumbRX, pad.Gamepad.sThumbRY};
            check(axes[requested] == value && pad.Gamepad.wButtons == 0, "XInput and native hold agree");
            check(!scripted_pad(1, pad), "Stand-in remains restricted to player zero");
        }
        play_pad({PadStep{0, {}}});
        check(!held_stick(axis, value), "Character room script takes priority over native hold");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        check(held_stick(axis, value), "Completed script expires even without XInput polls");
        play_pad({});
        hold_stick(2, 1, 0);
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "Expired hold reaches neither route");
        hold_stick(2, 1, 10000);
        hold_stick(2, 0, 10000);
        check(!held_stick(axis, value) && !scripted_pad(0, pad), "Cancellation releases both routes");
        // The room walk's press reaches a PlayStation pad the game reads itself, is counted when the game reads it, and keeps
        // the stand-in connected while it plays (XInputGetCapabilities answers for it then).
        WORD buttons{};
        check(!scripted_buttons(buttons) && !stand_in_connected(), "No script: no native buttons and no stand-in");
        const auto before = script_reads();
        XINPUT_GAMEPAD rb{};
        rb.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
        play_pad({{0, rb}, {9, XINPUT_GAMEPAD{}}, {24, XINPUT_GAMEPAD{}}});
        check(stand_in_connected(), "A playing script is a connected stand-in");
        check(scripted_buttons(buttons) && buttons == XINPUT_GAMEPAD_RIGHT_SHOULDER, "A native read gets the script's RB");
        check(scripted_pad(0, pad) && pad.Gamepad.wButtons == XINPUT_GAMEPAD_RIGHT_SHOULDER, "XInput gets the same press");
        check(script_reads() == before + 2, "Both reads of the script are counted");
        check(!scripted_pad(2, pad) && script_reads() == before + 2, "Another player's read is not the stand-in's");
        std::this_thread::sleep_for(std::chrono::milliseconds(2700));
        check(!scripted_buttons(buttons) && !stand_in_connected(), "The stand-in goes once the script has lingered");
        std::puts("scripted pad tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
