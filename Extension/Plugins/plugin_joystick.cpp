#include "plugin_joystick.h"
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <Windows.h>
#include <dinput.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace dingosdk::plugins::joystick {
namespace {
using Microsoft::WRL::ComPtr;
constexpr LONG range = 1000;                               // each axis reads -range..range
constexpr auto read_every = std::chrono::milliseconds(4);  // 250 a second while asked
constexpr ULONGLONG look_every_ms = 2000;                  // the device list, while asked
constexpr ULONGLONG asked_for_ms = 2000;                   // reads go on this long after a call

std::mutex mutex;
std::vector<Sample> latest;        // under mutex: what the thread read last
std::atomic<ULONGLONG> asked_at{};
std::once_flag started;

std::string narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? static_cast<char>(*text) : '?';
    return out;
}
std::string lowered(std::string text) {
    for (auto& ch : text) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 'a' - 'A');
    return text;
}

// One controller the thread has open. Its number stays while it is plugged in.
struct Device {
    GUID instance{};
    int number{};
    std::string name;
    ComPtr<IDirectInputDevice8W> input;
    bool seen{};
};

BOOL CALLBACK found_one(const DIDEVICEINSTANCEW* found, void* list) {
    static_cast<std::vector<DIDEVICEINSTANCEW>*>(list)->push_back(*found);
    return DIENUM_CONTINUE;
}

// Opens a controller for reading: the standard joystick layout, every axis -range..range, no dead
// zone. Left at the default cooperative level (background, not exclusive), so the game and other
// programs read it as before.
ComPtr<IDirectInputDevice8W> open(IDirectInput8W& input, const GUID& instance) {
    ComPtr<IDirectInputDevice8W> device;
    if (FAILED(input.CreateDevice(instance, device.GetAddressOf(), nullptr))) return nullptr;
    if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) return nullptr;
    DIPROPRANGE axes{};
    axes.diph = {sizeof(axes), sizeof(axes.diph), 0, DIPH_DEVICE};
    axes.lMin = -range;
    axes.lMax = range;
    device->SetProperty(DIPROP_RANGE, &axes.diph);
    DIPROPDWORD dead{};
    dead.diph = {sizeof(dead), sizeof(dead.diph), 0, DIPH_DEVICE};
    device->SetProperty(DIPROP_DEADZONE, &dead.diph);
    device->Acquire();
    return device;
}

bool read(Device& device, Sample& out) {
    DIJOYSTATE2 state{};
    device.input->Poll();
    auto result = device.input->GetDeviceState(sizeof(state), &state);
    if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
        device.input->Acquire();
        device.input->Poll();
        result = device.input->GetDeviceState(sizeof(state), &state);
    }
    if (FAILED(result)) return false;
    const auto axis = [](LONG value) { return std::clamp(static_cast<float>(value) / static_cast<float>(range), -1.0f, 1.0f); };
    out.axes = {axis(state.lX), axis(state.lY), axis(state.lZ), axis(state.lRx), axis(state.lRy), axis(state.lRz),
                axis(state.rglSlider[0]), axis(state.rglSlider[1])};
    out.buttons = 0;
    for (int i = 0; i < 32; ++i)
        if (state.rgbButtons[i] & 0x80) out.buttons |= 1u << i;
    out.name = device.name;
    out.device = device.number;
    out.valid = true;
    return true;
}

// Looks at the device list: new controllers open with the lowest free number, gone ones close.
void look(IDirectInput8W& input, std::vector<Device>& devices) {
    std::vector<DIDEVICEINSTANCEW> found;
    if (FAILED(input.EnumDevices(DI8DEVCLASS_GAMECTRL, found_one, &found, DIEDFL_ATTACHEDONLY))) return;
    for (auto& device : devices) device.seen = false;
    for (const auto& instance : found) {
        const auto known = std::find_if(devices.begin(), devices.end(),
                                        [&](const Device& device) { return IsEqualGUID(device.instance, instance.guidInstance); });
        if (known != devices.end()) {
            known->seen = true;
            continue;
        }
        if (devices.size() >= 16) continue;
        auto opened = open(input, instance.guidInstance);
        if (!opened) continue;
        int number = 0;
        while (std::any_of(devices.begin(), devices.end(), [&](const Device& device) { return device.number == number; })) ++number;
        devices.push_back({instance.guidInstance, number, narrow(instance.tszProductName), std::move(opened), true});
    }
    std::erase_if(devices, [](const Device& device) { return !device.seen; });
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.number < b.number; });
}

void run() {
    // The overlay keeps captured controllers from the game's own reads; these are ReSkate's, for
    // the thread's whole life.
    const InputAccess access;
    ComPtr<IDirectInput8W> input;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(input.GetAddressOf()), nullptr)))
        return;
    std::vector<Device> devices;
    ULONGLONG looked_at{};
    for (;;) {
        const auto now = GetTickCount64();
        if (now - asked_at.load() > asked_for_ms) {
            // Nobody is asking: nothing is read, and what was read is forgotten.
            {
                std::lock_guard lock(mutex);
                latest.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!looked_at || now - looked_at >= look_every_ms) {
            look(*input.Get(), devices);
            looked_at = now;
        }
        std::vector<Sample> read_now;
        for (auto& device : devices) {
            Sample sample;
            if (read(device, sample)) read_now.push_back(std::move(sample));
        }
        {
            std::lock_guard lock(mutex);
            latest = std::move(read_now);
        }
        std::this_thread::sleep_for(read_every);
    }
}

std::vector<Sample> snapshot() {
    asked_at.store(GetTickCount64());
    std::call_once(started, [] {
        try {
            std::thread(run).detach();
        } catch (...) {}
    });
    std::lock_guard lock(mutex);
    return latest;
}
} // namespace

int preferred(const std::vector<Sample>& found) {
    for (const auto& sample : found) {
        const auto name = lowered(sample.name);
        if (name.find("radio") != std::string::npos || name.find("edgetx") != std::string::npos ||
            name.find("opentx") != std::string::npos)
            return sample.device;
    }
    return found.empty() ? -1 : found.front().device;
}

Sample poll(int device) {
    const auto found = snapshot();
    const int wanted = device < 0 ? preferred(found) : device;
    for (const auto& sample : found)
        if (sample.device == wanted) return sample;
    return {};
}

std::vector<Sample> list() { return snapshot(); }

bool game_in_foreground() {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
}
