#pragma once
// Goggle-style on-screen display and stick overlay for the drone. This file only decides WHAT to
// show and where (so it can be tested anywhere); the overlay draws it (UI/Overlay/drone_hud_overlay.cpp).
#include "drone_flight.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::drone::osd {

// A pretend 4S pack: 16.8 V full, sagging a little with throttle and draining over the flight.
inline float battery_volts(float seconds, float throttle) {
    return std::max(13.2f, 16.8f - 0.0045f * seconds - 0.7f * throttle);
}

// RGB for the `osdcolor` presets: white, green, yellow, cyan, orange, pink.
inline std::array<std::uint8_t, 3> preset_colour(int index) {
    static constexpr std::array<std::array<std::uint8_t, 3>, 6> table{{
        {255, 255, 255}, {90, 255, 90}, {255, 232, 70}, {90, 232, 255}, {255, 150, 50}, {255, 100, 220}}};
    return table[static_cast<std::size_t>(std::clamp(index, 0, 5))];
}

enum class Align { left, centre, right };
struct Item {
    float x = 0, y = 0;        // fractions of the screen; y is the top of the text, or its bottom when `bottom`
    Align align = Align::left;
    bool bottom = false;
    bool warn = false;         // drawn red
    std::string text;
};
struct Sticks {
    bool show = false;
    float size = 1;
    float yaw = 0, throttle = 0, roll = 0, pitch = 0;   // each -1..1 (throttle: -1 bottom, 1 top; pitch: + = forward = up)
};
struct Layout {
    bool active = false;
    float scale = 1.5f;
    int font = 0;
    int colour = 0;
    float margin = 0.06f;
    bool crosshair = false;
    std::vector<Item> items;
    Sticks sticks;
};

inline Layout build() {
    Layout out;
    if (!flying()) return out;
    out.active = true;
    auto knob = [](Knob k) { return knobs().value[k].load(std::memory_order_relaxed); };
    const auto& t = telemetry();
    const float throttle = t.throttle.load(std::memory_order_relaxed);
    const float speed = t.speed.load(std::memory_order_relaxed);
    const float altitude = t.altitude.load(std::memory_order_relaxed);
    const float seconds = t.seconds.load(std::memory_order_relaxed);
    out.scale = knob(kOsdScale);
    out.font = knob(kOsdFont) >= 0.5f ? 1 : 0;
    out.colour = static_cast<int>(knob(kOsdColor) + 0.5f);
    out.margin = std::clamp(knob(kOsdMargin), 0.0f, 0.3f);
    out.crosshair = knob(kCrosshair) >= 0.5f;
    const float m = out.margin;
    auto add = [&](bool on, float x, float y, Align a, bool bottom, std::string text, bool warn = false) {
        if (!on) return;
        Item item;
        item.x = x; item.y = y; item.align = a; item.bottom = bottom; item.warn = warn; item.text = std::move(text);
        out.items.push_back(std::move(item));
    };
    const float volts = battery_volts(seconds, throttle);
    const bool low = volts < 14.0f;
    const bool blink = (static_cast<long long>(seconds * 2.0f) & 1) != 0;
    const int total = static_cast<int>(seconds);
    // The goggle display as a whole (`osd`): off hides every readout. The range warnings below
    // still show, since flying on without them loses the drone.
    const bool osd = knob(kOsd) >= 0.5f;
    add(osd && knob(kShowBattery) >= 0.5f, m, m, Align::left, false, std::format("{:.1f}V", volts), low && blink);
    const bool armed_now = t.armed.load(std::memory_order_relaxed);
    add(osd && knob(kShowArmed) >= 0.5f, 0.5f, m, Align::centre, false, armed_now ? "ARMED" : "DISARMED", !armed_now);
    add(osd && knob(kShowTimer) >= 0.5f, 1.0f - m, m, Align::right, false, std::format("{:02}:{:02}", total / 60, total % 60));
    add(osd && knob(kShowThrottle) >= 0.5f, m, 1.0f - m, Align::left, true, std::format("THR {:.0f}%", throttle * 100.0f));
    add(osd && knob(kShowAlt) >= 0.5f, 0.5f, 1.0f - m, Align::centre, true, std::format("ALT {:.1f}M", altitude));
    add(osd && knob(kShowSpeed) >= 0.5f, 1.0f - m, 1.0f - m, Align::right, true, std::format("{:.0f}KM/H", speed * 3.6f));
    // A crash cut the motors: say how to fly on. (Shown with or without the goggle display.)
    const bool crashed = t.crashed.load(std::memory_order_relaxed);
    if (crashed) add(true, 0.5f, 0.42f, Align::centre, false, "CRASHED - ARM TO FLY AGAIN", true);
    // Range: link quality and the way home, and what happens at the edge.
    if (knob(kRange) > 0.0f) {
        const float signal = t.signal.load(std::memory_order_relaxed);
        add(osd, 1.0f - m, m + 0.05f, Align::right, false,
            std::format("RSSI {:.0f}%  {:.0f}M", signal * 100.0f, t.home_distance.load(std::memory_order_relaxed)), signal < 0.35f && blink);
        if (t.failsafe.load(std::memory_order_relaxed)) add(true, 0.5f, crashed ? 0.47f : 0.42f, Align::centre, false, "FAILSAFE - RESET TO FLY AGAIN", true);
        else if (signal <= 0.0f) {
            const float left = std::max(0.0f, failsafe_after_seconds - t.signal_lost.load(std::memory_order_relaxed));
            add(true, 0.5f, 0.42f, Align::centre, false, std::format("NO SIGNAL - TURN BACK ({:.0f})", std::ceil(left)), true);
        }
    }
    out.sticks.show = knob(kSticks) >= 0.5f;
    out.sticks.size = knob(kStickSize);
    out.sticks.yaw = t.stick_yaw.load(std::memory_order_relaxed);
    out.sticks.roll = t.stick_roll.load(std::memory_order_relaxed);
    out.sticks.pitch = t.stick_pitch.load(std::memory_order_relaxed);
    // A throttle stick that springs back is shown where it is; one that stays put, by the throttle.
    const float stick = t.stick_throttle.load(std::memory_order_relaxed);
    out.sticks.throttle = stick >= -1.0f ? std::clamp(stick, -1.0f, 1.0f) : std::clamp(throttle, 0.0f, 1.0f) * 2.0f - 1.0f;
    return out;
}
} // namespace dingosdk::drone::osd
