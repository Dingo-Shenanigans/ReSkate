#pragma once
// Joysticks and radio controllers for plugins (reskate.input.joystick, reskate.input.joysticks):
// anything Windows lists as a game controller (a radio in its USB Joystick mode, a flight stick, a
// gamepad). Read through DirectInput, which gives all eight axes a radio sends (winmm gave six):
// axes 1..8 are X, Y, Z, Rx, Ry, Rz and the two sliders, each -1..1, 0 for one the controller
// does not have. Windows only.
//
// A thread of its own finds the controllers and reads them: looking at the device list can take
// far longer than a plugin's tick may (empty winmm slots took over 50 ms on some PCs, and a plugin
// was stopped for it), so a call only copies what the thread read last. It reads only while a
// plugin has asked in the last two seconds.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk::overlay { // UI/Overlay/overlay.h: reads that see the controller while the game is kept off it
void begin_input_access() noexcept;
void end_input_access() noexcept;
}
namespace dingosdk::plugins::joystick {
// While it lives, this thread's reads see the controllers even while a plugin keeps the game off them.
struct InputAccess {
    InputAccess() noexcept { overlay::begin_input_access(); }
    ~InputAccess() { overlay::end_input_access(); }
    InputAccess(const InputAccess&) = delete;
    InputAccess& operator=(const InputAccess&) = delete;
};

inline constexpr std::size_t axis_count = 8;
struct Sample {
    bool valid = false;
    std::array<float, axis_count> axes{};
    std::uint32_t buttons = 0; // bit n-1 set = button n pressed
    std::string name;
    int device = -1;           // its number while it stays plugged in (list())
};

// The controller `device` (a number list() gives), or with -1 the first one, preferring one named
// a radio. Not valid when there is none. Any thread; never waits on Windows.
Sample poll(int device = -1);
// Every controller plugged in, lowest number first. Any thread; never waits on Windows.
std::vector<Sample> list();
// The first of `found` a plugin reads when it names none: one named a radio, else the lowest number.
int preferred(const std::vector<Sample>& found);

// True while this game's window has focus, so a controller does not drive it from the background.
bool game_in_foreground();
}
