#include "gui_internal.h"

#include "Extension/UI/Overlay/playstation_input.h"

#include <Xinput.h>

#include <cstdlib>
#include <cstring>

namespace dingosdk::launcher_gui::detail {
namespace {

using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
// Loaded on first use like the overlay does: a PC without the XInput runtime
// still gets a launcher, just without Xbox pads.
GetState xinput_get_state() {
    static const auto get_state = []() -> GetState {
        for (const auto name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            const auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) continue;
            if (const auto entry = GetProcAddress(module, "XInputGetState")) {
                GetState function{};
                static_assert(sizeof(function) == sizeof(entry));
                std::memcpy(&function, &entry, sizeof(function));
                return function;
            }
            FreeLibrary(module);
        }
        return nullptr;
    }();
    return get_state;
}

// Asking an empty XInput slot is slow (it looks for a device), so only slots
// with a pad are read every frame, and one empty slot is tried a second.
struct Slots {
    unsigned connected{(1u << XUSER_MAX_COUNT) - 1};   // all four tried on the first frame
    DWORD next_probe{};
    ULONGLONG probe_at{};
};

} // namespace

PadState read_pad() {
    static Slots slots;
    PadState pad;
    if (const auto get_state = xinput_get_state()) {
        const auto now = GetTickCount64();
        unsigned probe{};
        if (now >= slots.probe_at) {
            slots.probe_at = now + 1000;
            for (DWORD tried = 0; tried < XUSER_MAX_COUNT && !probe; ++tried) {
                const DWORD slot = (slots.next_probe + tried) % XUSER_MAX_COUNT;
                if (!(slots.connected & (1u << slot))) probe = 1u << slot;
            }
            slots.next_probe = (slots.next_probe + 1) % XUSER_MAX_COUNT;
        }
        // Steam, DS4Windows and ViGEm put virtual pads in any slot, and an idle
        // one may sit ahead of the pad in use, so every pad is merged.
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const unsigned bit = 1u << slot;
            if (!((slots.connected | probe) & bit)) continue;
            XINPUT_STATE sample{};
            if (get_state(slot, &sample) != ERROR_SUCCESS) { slots.connected &= ~bit; continue; }
            slots.connected |= bit;
            const auto& gamepad = sample.Gamepad;
            pad.connected = true;
            pad.buttons |= gamepad.wButtons;
            pad.left_trigger = std::max(pad.left_trigger, gamepad.bLeftTrigger);
            pad.right_trigger = std::max(pad.right_trigger, gamepad.bRightTrigger);
            const auto magnitude = [](SHORT x, SHORT y) { return std::abs(int(x)) + std::abs(int(y)); };
            if (magnitude(gamepad.sThumbLX, gamepad.sThumbLY) > magnitude(pad.left_x, pad.left_y)) {
                pad.left_x = gamepad.sThumbLX; pad.left_y = gamepad.sThumbLY;
            }
            if (magnitude(gamepad.sThumbRX, gamepad.sThumbRY) > magnitude(pad.right_x, pad.right_y)) {
                pad.right_x = gamepad.sThumbRX; pad.right_y = gamepad.sThumbRY;
            }
        }
    }
    // XInput wins whenever a pad is there: DS4Windows and Steam Input leave the
    // PlayStation pad visible beside their XInput twin, and both would double up.
    if (pad.connected) return pad;
    const auto playstation = overlay::detail::read_playstation_pads();
    if (!playstation.available) return {};
    const auto& sony = playstation.pad;
    pad.connected = true;
    pad.buttons = sony.buttons;
    pad.left_trigger = sony.left_trigger; pad.right_trigger = sony.right_trigger;
    pad.left_x = sony.left_x; pad.left_y = sony.left_y;
    pad.right_x = sony.right_x; pad.right_y = sony.right_y;
    return pad;
}

} // namespace dingosdk::launcher_gui::detail
