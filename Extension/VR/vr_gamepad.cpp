// Right sticks the game's XInput hook does not see, for look turns and the bail camera orbit:
// XInput slots polled by ReSkate, and a DirectInput Sony pad (DualShock 4 / DualSense: right X is the Z axis),
// read by ReSkate itself for look turns: those pads are not XInput, so the XInput hook never
// sees them. Non-exclusive and in the background, so the game keeps its own access.
#include "vr.h"
#include "Engine/Core/Log/logging.h"
#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>

namespace dingosdk::vr {
namespace {
// GUIDs from dinput.h's dxguid.lib, defined here so no import library is needed.
constexpr GUID input8_iid{0xbf798031, 0x483a, 0x4da2, {0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00}};
constexpr DWORD optional_object = 0x80000000; // DIDFT_OPTIONAL (not in every dinput.h)
GUID z_axis{0xa36d02e2, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct Reader {
    std::mutex mutex;
    IDirectInput8W* input = nullptr;
    IDirectInputDevice8W* device = nullptr;
    ULONGLONG retry_at = 0;
    bool logged = false;
};
Reader& reader() {
    static auto* value = new Reader;
    return *value;
}

std::string narrow(const wchar_t* text) {
    char out[MAX_PATH]{};
    (void)WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}
// The Sony pad (vendor 054C: DualShock 4, DualSense); other controllers (wheels, sticks,
// generic HID devices) may hold their Z axis anywhere, so they are only listed.
struct Search {
    DIDEVICEINSTANCEW found{};
    std::string seen;
};
BOOL CALLBACK find_sony(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    auto& search = *static_cast<Search*>(context);
    const auto vendor = LOWORD(instance->guidProduct.Data1);
    search.seen += std::format("{}'{}' ({:04X}:{:04X})", search.seen.empty() ? "" : ", ", narrow(instance->tszProductName), vendor,
        HIWORD(instance->guidProduct.Data1));
    if (vendor == 0x054c && search.found.guidInstance == GUID{}) search.found = *instance;
    return DIENUM_CONTINUE;
}

void release_device(Reader& r) {
    if (r.device) {
        r.device->Unacquire();
        r.device->Release();
        r.device = nullptr;
    }
}

bool open_device(Reader& r) {
    if (!r.input) {
        const auto module = LoadLibraryW(L"dinput8.dll");
        using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        const auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "DirectInput8Create")) : nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, input8_iid, reinterpret_cast<void**>(&r.input),
                           nullptr)))
            return false;
    }
    Search search;
    const bool listed = SUCCEEDED(r.input->EnumDevices(DI8DEVCLASS_GAMECTRL, find_sony, &search, DIEDFL_ATTACHEDONLY));
    if (!r.logged) {
        r.logged = true;
        logging::write(logging::Level::info, logging::Channel::graphics,
            std::format("VR: DirectInput controllers: {}; look turns read {}.", search.seen.empty() ? "none" : search.seen,
                search.found.guidInstance == GUID{} ? "none (no Sony pad)" : narrow(search.found.tszProductName)));
    }
    const auto& found = search.found;
    if (!listed || found.guidInstance == GUID{}) return false;
    if (FAILED(r.input->CreateDevice(found.guidInstance, &r.device, nullptr))) {
        r.device = nullptr;
        return false;
    }
    // Only the Z axis, scaled to the XInput range. No cooperative level: the default is
    // non-exclusive background.
    static DIOBJECTDATAFORMAT objects[]{{&z_axis, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE | optional_object, 0}};
    DIDATAFORMAT format{sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 4, 1, objects};
    DIPROPRANGE range{};
    range.diph = {sizeof(range), sizeof(range.diph), 0, DIPH_DEVICE};
    range.lMin = -32768;
    range.lMax = 32767;
    if (FAILED(r.device->SetDataFormat(&format)) || FAILED(r.device->SetProperty(DIPROP_RANGE, &range.diph))) {
        release_device(r);
        return false;
    }
    (void)r.device->Acquire();
    return true;
}
}

PadPeek xinput_peek() noexcept {
    try {
        using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        static const auto get_state = [] {
            const auto module = LoadLibraryW(L"xinput1_4.dll");
            return module ? reinterpret_cast<GetState>(GetProcAddress(module, "XInputGetState")) : nullptr;
        }();
        if (!get_state) return {};
        // An empty slot is slow to poll: retried every 2 s, connected slots every call.
        static std::mutex mutex;
        static std::array<ULONGLONG, 4> retry_at{};
        std::lock_guard lock(mutex);
        const auto now = GetTickCount64();
        PadPeek peek;
        for (DWORD slot = 0; slot < 4; ++slot) {
            if (now < retry_at[slot]) continue;
            XINPUT_STATE state{};
            if (get_state(slot, &state) != ERROR_SUCCESS) {
                retry_at[slot] = now + 2000;
                continue;
            }
            if (std::abs(state.Gamepad.sThumbRX) > std::abs(peek.right_x)) peek.right_x = state.Gamepad.sThumbRX;
            peek.trigger = std::max({peek.trigger, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger});
        }
        return peek;
    } catch (...) {
        return {};
    }
}

std::int16_t directinput_right_x() noexcept {
    try {
        auto& r = reader();
        std::lock_guard lock(r.mutex);
        const auto now = GetTickCount64();
        if (!r.device) {
            if (now < r.retry_at) return 0;
            r.retry_at = now + 3000; // controllers plugged in later are found within 3 s
            if (!open_device(r)) return 0;
        }
        LONG z = 0;
        (void)r.device->Poll();
        HRESULT result = r.device->GetDeviceState(sizeof(z), &z);
        if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
            (void)r.device->Acquire();
            result = r.device->GetDeviceState(sizeof(z), &z);
        }
        if (FAILED(result)) {
            release_device(r);
            return 0;
        }
        return static_cast<std::int16_t>(std::clamp<LONG>(z, -32768, 32767));
    } catch (...) {
        return 0;
    }
}
}
