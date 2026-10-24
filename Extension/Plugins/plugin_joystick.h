#pragma once
// Joysticks and radio controllers for plugins (reskate.input.joystick): anything that shows up in
// Windows' "Set up USB game controllers" (a radio in its USB Joystick mode, a flight stick). Axis
// numbers 1..6 are winmm's X, Y, Z, R, U, V, each -1..1. Windows only (winmm).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace dingosdk::overlay { // UI/Overlay/overlay.h: reads that see the controller while the game is kept off it
void begin_input_access() noexcept;
void end_input_access() noexcept;
}
namespace dingosdk::plugins::joystick {
struct Sample {
    bool valid = false;
    std::array<float, 6> axes{};
    std::uint32_t buttons = 0; // bit n-1 set = button n pressed
    std::string name;
    int device = -1;
};

// While it lives, reads see the controllers even while a plugin keeps the game off them.
struct InputAccess {
    InputAccess() noexcept { overlay::begin_input_access(); }
    ~InputAccess() { overlay::end_input_access(); }
    InputAccess(const InputAccess&) = delete;
    InputAccess& operator=(const InputAccess&) = delete;
};

namespace detail {
inline float normalize_axis(std::uint32_t value, std::uint32_t lo, std::uint32_t hi) {
    if (hi <= lo) return 0.0f;
    const float t = (static_cast<float>(value) - static_cast<float>(lo)) / (static_cast<float>(hi) - static_cast<float>(lo));
    return std::clamp(t * 2.0f - 1.0f, -1.0f, 1.0f);
}
inline std::string narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? static_cast<char>(*text) : '?';
    return out;
}
inline std::string lowered(std::string s) {
    for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 'a' - 'A');
    return s;
}
inline bool read_device(UINT id, Sample& out) {
    JOYCAPSW caps{};
    if (joyGetDevCapsW(id, &caps, sizeof(caps)) != JOYERR_NOERROR) return false;
    JOYINFOEX info{};
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNX | JOY_RETURNY | JOY_RETURNZ | JOY_RETURNR | JOY_RETURNU | JOY_RETURNV | JOY_RETURNBUTTONS;
    if (joyGetPosEx(id, &info) != JOYERR_NOERROR) return false;
    const UINT n = caps.wNumAxes;
    out.axes = {};
    if (n > 0) out.axes[0] = normalize_axis(info.dwXpos, caps.wXmin, caps.wXmax);
    if (n > 1) out.axes[1] = normalize_axis(info.dwYpos, caps.wYmin, caps.wYmax);
    if (n > 2) out.axes[2] = normalize_axis(info.dwZpos, caps.wZmin, caps.wZmax);
    if (n > 3) out.axes[3] = normalize_axis(info.dwRpos, caps.wRmin, caps.wRmax);
    if (n > 4) out.axes[4] = normalize_axis(info.dwUpos, caps.wUmin, caps.wUmax);
    if (n > 5) out.axes[5] = normalize_axis(info.dwVpos, caps.wVmin, caps.wVmax);
    out.buttons = info.dwButtons;
    out.name = narrow(caps.szPname);
    out.device = static_cast<int>(id);
    out.valid = true;
    return true;
}
}

// Reads the first controller, preferring one named a radio. Any thread; rescans at most once a second.
inline Sample poll() {
    InputAccess access;
    static std::atomic<int> cached{-1};
    static std::atomic<ULONGLONG> next_scan{0};
    Sample sample;
    int id = cached.load();
    if (id >= 0 && detail::read_device(static_cast<UINT>(id), sample)) return sample;
    cached.store(-1);
    const auto now = GetTickCount64();
    if (now < next_scan.load()) return sample;
    next_scan.store(now + 1000);
    const UINT count = std::min<UINT>(joyGetNumDevs(), 16);
    int first = -1;
    for (UINT i = 0; i < count; ++i) {
        Sample candidate;
        if (!detail::read_device(i, candidate)) continue;
        if (first < 0) first = static_cast<int>(i);
        if (detail::lowered(candidate.name).find("radio") != std::string::npos) { cached.store(static_cast<int>(i)); return candidate; }
    }
    if (first >= 0) { detail::read_device(static_cast<UINT>(first), sample); cached.store(first); }
    return sample;
}

// True while this game's window has focus, so a controller does not drive it from the background.
inline bool game_in_foreground() {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
}
#endif
