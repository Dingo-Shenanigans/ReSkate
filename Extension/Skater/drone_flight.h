#pragma once
// FPV drone flight model for the Freecam. Replaces step_free_flight() while drone mode is on.
//
// Matrix convention (same as free_flight.h): row-major 4x4, rows are right / up / backward,
// translation in elements 12..14. The camera looks along -backward. World up is +Y.
//
// Controls (all come from the existing FlightInput, so keyboard, mouse and controller work):
//   input.forward  -> pitch   (forward = nose down)
//   input.right    -> roll    (right = roll right)
//   input.up       -> throttle (0 = hover, + = climb, - = descend)
//   input.look_x   -> yaw     (mouse look delta, right = turn right)
//   input.look_y   -> extra pitch (mouse look delta)
//   input.boost    -> doubles the speed cap
#include "free_flight.h"
#include "drone_radio.h"
#include "drone_beacon.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <iterator>
#include <string_view>

namespace dingosdk::drone {

// Betaflight "Actual" rates for one axis: deg/s near the stick centre, deg/s at full stick, expo 0-1.
struct Rate { float center, max, expo; };

struct Settings {
    Rate roll{80.0f, 700.0f, 0.15f};
    Rate pitch{80.0f, 700.0f, 0.15f};
    Rate yaw{130.0f, 780.0f, 0.30f};
    float yaw_look_scale = 0.0025f; // rad per unit of mouse delta (matches freecam look)
    float hover_throttle = 0.35f;   // throttle at which thrust equals gravity
    float gravity = 9.81f;
    float lin_drag = 0.15f;         // 1/s
    float quad_drag = 0.02f;        // 1/unit
    float camera_uptilt_deg = 25.0f;
    bool angle_mode = false;        // true = self-levelling (easier), false = acro (full flips)
    float max_angle_deg = 55.0f;
    float angle_response = 8.0f;
    bool collide = true;            // collide with the world (needs the game's collision query)
    float body_radius = 0.3f;       // how close it gets to a surface before stopping
    float floor_depth = 1.5f;       // invisible floor this far below the start height (only without collisions); 0 = none
    float range = 0.0f;             // radio and video reach from the takeoff point, metres; 0 = unlimited
    bool armed = true;              // false = motors off: no control, no thrust (the drone falls)
    bool disarm_freeze = false;     // true = a disarmed drone hangs in place instead of falling
    bool beacon = false;            // tell other players in the lobby where the drone is
};

struct State {
    bool valid = false;
    std::array<float, 16> body{};   // body orientation (translation unused); camera = body tilted up
    std::array<float, 3> velocity{};
    float start_y = 0;              // height the flight began at; the invisible floor hangs below it
    float flight_time = 0;          // seconds the motors have been armed since the flight began (the OSD timer)
    std::array<float, 3> start_pos{};   // where the flight began (`dronereset` returns here)
    std::array<float, 16> start_body{};
    bool failsafe = false;          // flew out of range: motors cut until the drone is reset
    float signal_lost = 0;          // seconds in a row without a picture (range)
};

// What the OSD and the motor sound read (written by the flight tick, read from other threads).
struct Telemetry {
    std::atomic<float> throttle{0}, speed{0}, altitude{0}, seconds{0};
    std::atomic<bool> armed{true};
    std::atomic<float> signal{1.0f};       // radio and video link, 1 = perfect, 0 = lost (range)
    std::atomic<bool> failsafe{false};     // out of range for too long: motors cut until reset
    std::atomic<float> home_distance{0.0f}; // metres from the takeoff point
    std::atomic<float> signal_lost{0.0f};   // seconds in a row without a picture
    std::atomic<float> stick_roll{0}, stick_pitch{0}, stick_yaw{0};   // -1..1, + = right / forward / right
    std::atomic<long long> last_ms{0};   // steady-clock milliseconds of the last flight tick
};
inline Telemetry& telemetry() { static Telemetry value; return value; }
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// True while the drone is being flown (a flight tick ran in the last quarter second).
inline bool flying() { return now_ms() - telemetry().last_ms.load(std::memory_order_relaxed) < 250; }

inline std::atomic<bool>& enabled() {
    static std::atomic<bool> value{false};
    return value;
}
inline bool is_enabled() { return enabled().load(std::memory_order_acquire); }
// While the drone flies with a controller (RC radio or gamepad), the game must not read that
// controller too, or the skater moves with the sticks: ReSkate keeps it off the controller, as
// its freecam controller mode does (Runtime/client_tick.cpp), and the drone reads it regardless.
inline bool controller_capture() { return is_enabled() && radio::config().enabled.load(std::memory_order_acquire); }
inline std::atomic<bool>& armed_flag() {
    static std::atomic<bool> value{true};
    return value;
}
inline bool armed() { return armed_flag().load(std::memory_order_acquire); }
inline void set_armed(bool on) { armed_flag().store(on, std::memory_order_release); }
// `dronemenu` asks for the ReSkate menu to open on its DRONE page (UI/Overlay/drone_page.cpp);
// the menu takes the request when it next draws.
inline std::atomic<bool>& menu_flag() {
    static std::atomic<bool> value{false};
    return value;
}
// Asks the flight tick to put the drone back at its starting point and restart the timer and battery.
inline std::atomic<bool>& reset_flag() {
    static std::atomic<bool> value{false};
    return value;
}
inline void request_reset() { reset_flag().store(true, std::memory_order_release); }
inline void set_enabled(bool on) { enabled().store(on, std::memory_order_release); }

// Settings you can change while flying (`dronetune`). The index order matters: live_settings() uses it.
struct KnobInfo { const char* name; const char* help; float lo, hi, def; };
enum Knob : std::size_t { kGravity, kDrag, kHover, kRollCenter, kRollMax, kRollExpo, kPitchCenter, kPitchMax, kPitchExpo,
    kYawCenter, kYawMax, kYawExpo, kUptilt, kFloor, kCollide, kRadius, kOsd, kVolume,
    kOsdScale, kOsdColor, kOsdFont, kOsdMargin, kCrosshair, kShowArmed, kShowBattery, kShowTimer, kShowThrottle, kShowAlt,
    kShowSpeed, kSticks, kStickSize, kArmAxis, kResetAxis, kDisarmFreeze, kBeacon, kShowBeacons, kArmButton, kResetButton,
    kAnalog, kAnalogNoise, kAnalogScan, kAnalogVignette, kAnalogGlitch, kRange, kMotorPitch, kThrottleCentre, kDeadzone, kGoggles, kHideUi };
inline constexpr KnobInfo knob_info[] = {
    {"gravity", "How hard the drone is pulled down. 9.8 is Earth; higher feels heavier and snappier", 0.0f, 50.0f, 20.0f},
    {"drag", "Air resistance. Higher slows the drone down sooner when you let off", 0.0f, 3.0f, 0.15f},
    {"hover", "The throttle that keeps the drone level in the air (0.1-0.9). Lower means more power to spare", 0.1f, 0.9f, 0.35f},
    {"rollcenter", "Roll speed near the middle of the stick, in degrees per second (Betaflight \"Actual\" rates)", 1.0f, 1000.0f, 80.0f},
    {"rollmax", "Roll speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 700.0f},
    {"rollexpo", "How much softer roll is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.15f},
    {"pitchcenter", "Pitch speed near the middle of the stick, in degrees per second", 1.0f, 1000.0f, 80.0f},
    {"pitchmax", "Pitch speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 700.0f},
    {"pitchexpo", "How much softer pitch is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.15f},
    {"yawcenter", "Yaw (turning) speed near the middle of the stick, in degrees per second", 1.0f, 1000.0f, 130.0f},
    {"yawmax", "Yaw speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 780.0f},
    {"yawexpo", "How much softer yaw is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.30f},
    {"uptilt", "How far the camera points up, in degrees. Real FPV cameras sit at about 25-40; faster flying needs more", -30.0f, 70.0f, 25.0f},
    {"floor", "With collisions off: an invisible floor this many metres below where you took off (0 = none)", 0.0f, 1000.0f, 1.5f},
    {"collide", "Hit the ground, walls and ramps (1), or fly through everything (0)", 0.0f, 1.0f, 1.0f},
    {"radius", "How big the drone is for collisions, in metres: how close it gets to things", 0.05f, 3.0f, 0.3f},
    {"osd", "Show the goggle display: battery, timer, throttle, altitude, speed and signal. The range warnings show either way", 0.0f, 1.0f, 1.0f},
    {"volume", "Motor sound volume (0 = silent)", 0.0f, 1.0f, 0.5f},
    {"osdscale", "Size of the goggle display's text (1 = normal, 2 = double)", 0.4f, 5.0f, 1.5f},
    {"osdcolor", "Goggle display colour: 0 white, 1 green, 2 yellow, 3 cyan, 4 orange, 5 pink", 0.0f, 5.0f, 0.0f},
    {"osdfont", "Goggle display font: 0 = pixel, like real goggles; 1 = smooth", 0.0f, 1.0f, 0.0f},
    {"osdmargin", "How far the goggle display sits in from the screen edges (0-0.3 of the screen)", 0.0f, 0.3f, 0.06f},
    {"crosshair", "Show a crosshair in the middle of the screen", 0.0f, 1.0f, 0.0f},
    {"osdarmed", "Show ARMED / DISARMED", 0.0f, 1.0f, 1.0f},
    {"osdbattery", "Show the battery voltage", 0.0f, 1.0f, 1.0f},
    {"osdtimer", "Show the flight timer", 0.0f, 1.0f, 1.0f},
    {"osdthrottle", "Show the throttle", 0.0f, 1.0f, 1.0f},
    {"osdalt", "Show the altitude", 0.0f, 1.0f, 1.0f},
    {"osdspeed", "Show the speed", 0.0f, 1.0f, 1.0f},
    {"sticks", "Show your sticks on screen (left: throttle and yaw, right: pitch and roll)", 0.0f, 1.0f, 1.0f},
    {"sticksize", "Size of the on-screen sticks (1 = normal)", 0.4f, 3.0f, 1.0f},
    {"armaxis", "Controller axis (1-6) for an arm switch: switch on = armed. Minus reverses it; 0 = none. Use one no stick is on", -6.0f, 6.0f, 0.0f},
    {"resetaxis", "Controller axis (1-6) for a reset switch: flick it on to put the drone back. 0 = none. Use one no stick is on", -6.0f, 6.0f, 0.0f},
    {"disarmfreeze", "When disarmed, the drone hangs in the air (1) instead of falling (0)", 0.0f, 1.0f, 0.0f},
    {"beacon", "Let other players see your drone (they need this version of ReSkate too)", 0.0f, 1.0f, 1.0f},
    {"showbeacons", "Show other players' drones (0 hides them)", 0.0f, 1.0f, 1.0f},
    {"armbutton", "Controller button (1-32) to arm with: held = armed. 0 = none", 0.0f, 32.0f, 0.0f},
    {"resetbutton", "Controller button (1-32) that puts the drone back where it took off. 0 = none", 0.0f, 32.0f, 0.0f},
    {"analog", "Make the picture look like analogue FPV video: scanlines, static, dark corners and glitches", 0.0f, 1.0f, 0.0f},
    {"analognoise", "Analogue look: how much static (0-1)", 0.0f, 1.0f, 0.35f},
    {"analogscan", "Analogue look: how strong the scanlines are (0-1)", 0.0f, 1.0f, 0.5f},
    {"analogvignette", "Analogue look: how dark the corners are (0-1)", 0.0f, 1.0f, 0.5f},
    {"analogglitch", "Analogue look: how often the picture rolls and tears (0-1)", 0.0f, 1.0f, 0.3f},
    {"range", "How far your signal reaches from where you took off, in metres. 0 = unlimited", 0.0f, 5000.0f, 0.0f},
    {"motorpitch", "Motor sound pitch. Higher sounds like a smaller drone", 0.4f, 2.5f, 1.0f},
    {"throttlecentre", "Your throttle stick springs back to the middle (gamepads): the middle hovers, up climbs, down descends", 0.0f, 1.0f, 0.0f},
    {"deadzone", "How far a stick must move before the drone reacts (0-0.3). Raise it if the drone drifts on its own", 0.0f, 0.3f, 0.03f},
    {"goggles", "What your skater wears over their eyes while you fly: 0 = nothing, 1-8 = one of the eyewear choices on the DRONE page", 0.0f, 8.0f, 1.0f},
    {"hideui", "Hide the game's own interface while you fly (Start flying hides it, Stop flying brings it back)", 0.0f, 1.0f, 1.0f},
};
inline constexpr std::size_t knob_count = std::size(knob_info);
static_assert(knob_count == kHideUi + 1, "knob enum and table must match");
struct KnobValues {
    std::atomic<float> value[knob_count];
    KnobValues() { for (std::size_t i = 0; i < knob_count; ++i) value[i] = knob_info[i].def; }
};
inline KnobValues& knobs() { static KnobValues values; return values; }
inline int find_knob(std::string_view name) {
    for (std::size_t i = 0; i < knob_count; ++i) if (name == knob_info[i].name) return static_cast<int>(i);
    return -1;
}
// The controller's sticks as the drone reads them, with the saved axes, dead zone and throttle kind.
inline radio::Command read_sticks(const std::array<float, 6>& axes) {
    const auto get = [](Knob k) { return knobs().value[k].load(std::memory_order_relaxed); };
    return radio::map_axes(axes, radio::mapping_snapshot(), get(kDeadzone), get(kThrottleCentre) >= 0.5f, get(kHover));
}
inline Settings live_settings() {
    Settings s;
    auto get = [](Knob k) { return knobs().value[k].load(std::memory_order_relaxed); };
    s.gravity = get(kGravity); s.lin_drag = get(kDrag); s.hover_throttle = get(kHover);
    s.roll = {get(kRollCenter), get(kRollMax), get(kRollExpo)};
    s.pitch = {get(kPitchCenter), get(kPitchMax), get(kPitchExpo)};
    s.yaw = {get(kYawCenter), get(kYawMax), get(kYawExpo)};
    s.camera_uptilt_deg = get(kUptilt); s.floor_depth = get(kFloor);
    s.collide = get(kCollide) >= 0.5f; s.body_radius = get(kRadius); s.range = get(kRange);
    s.armed = armed(); s.disarm_freeze = get(kDisarmFreeze) >= 0.5f; s.beacon = get(kBeacon) >= 0.5f;
    return s;
}

// Range. A real video link stays clean most of the way out, breaks up as it nears its limit and
// is lost past it. The radio link holds on a little longer: only after some seconds without a
// picture, or far past the limit, do the motors cut (failsafe), so there is time to turn back.
inline constexpr float range_clean = 0.5f;          // up to this fraction of the range the picture is clean
inline constexpr float range_failsafe = 1.5f;       // past this fraction the motors cut at once
inline constexpr float failsafe_after_seconds = 5.0f; // this long without a picture and the motors cut
inline float signal_strength(float distance, float range) {
    if (!(range > 0.0f) || !std::isfinite(distance)) return 1.0f;
    const float q = distance / range;
    if (q <= range_clean) return 1.0f;
    if (q >= 1.0f) return 0.0f;
    const float t = (q - range_clean) / (1.0f - range_clean); // 0 at the clean edge, 1 at the limit
    return std::clamp(1.0f - t * t * (3.0f - 2.0f * t), 0.0f, 1.0f); // smooth fade
}
inline bool out_of_failsafe_range(float distance, float range) {
    return range > 0.0f && std::isfinite(distance) && distance > range * range_failsafe;
}
// Whether the motors cut now: too long without a picture, or far past the limit.
inline bool failsafe_due(float distance, float range, float seconds_lost) {
    return out_of_failsafe_range(distance, range) || (range > 0.0f && seconds_lost >= failsafe_after_seconds);
}

// Presets: whole drones in one choice. Each sets the handling, rates, size and motor sound, and
// the top speed (the `flyspeed` setting). The DRONE page applies them through the same commands
// a player could type, so they are saved like any other change, and shows "Custom" once one is changed.
struct PresetValue { Knob knob; float value; };
struct Preset {
    const char* name;
    const char* description;
    float top_speed; // flyspeed
    std::array<PresetValue, 14> values;
};
inline constexpr Preset presets[] = {
    {"Tinywhoop", "A 65 mm ducted whoop: small, slow and forgiving, made for indoors and tight lines.", 3.5f,
     {{{kHover, 0.45f}, {kDrag, 0.60f}, {kRadius, 0.06f}, {kUptilt, 15.0f},
       {kRollCenter, 70.0f}, {kRollMax, 600.0f}, {kRollExpo, 0.30f}, {kPitchCenter, 70.0f}, {kPitchMax, 600.0f}, {kPitchExpo, 0.30f},
       {kYawCenter, 100.0f}, {kYawMax, 500.0f}, {kYawExpo, 0.30f}, {kMotorPitch, 2.0f}}}},
    {"Toothpick", "A light 3-inch: quick and nimble, faster than a whoop without the punch of a 5-inch.", 6.0f,
     {{{kHover, 0.30f}, {kDrag, 0.30f}, {kRadius, 0.10f}, {kUptilt, 25.0f},
       {kRollCenter, 80.0f}, {kRollMax, 700.0f}, {kRollExpo, 0.20f}, {kPitchCenter, 80.0f}, {kPitchMax, 700.0f}, {kPitchExpo, 0.20f},
       {kYawCenter, 130.0f}, {kYawMax, 650.0f}, {kYawExpo, 0.30f}, {kMotorPitch, 1.5f}}}},
    {"5 inch", "A 5-inch freestyle quad: lots of power and speed, for big lines across the map.", 10.0f,
     {{{kHover, 0.22f}, {kDrag, 0.12f}, {kRadius, 0.25f}, {kUptilt, 30.0f},
       {kRollCenter, 80.0f}, {kRollMax, 720.0f}, {kRollExpo, 0.15f}, {kPitchCenter, 80.0f}, {kPitchMax, 720.0f}, {kPitchExpo, 0.15f},
       {kYawCenter, 130.0f}, {kYawMax, 780.0f}, {kYawExpo, 0.30f}, {kMotorPitch, 0.85f}}}},
};
inline constexpr std::size_t preset_count = std::size(presets);
// The preset the current settings match, or -1 (custom).
inline int matching_preset(float top_speed) {
    for (std::size_t p = 0; p < preset_count; ++p) {
        bool same = std::abs(top_speed - presets[p].top_speed) < 0.05f;
        for (const auto& v : presets[p].values)
            same = same && std::abs(knobs().value[v.knob].load(std::memory_order_relaxed) - v.value) < 0.005f * std::max(1.0f, std::abs(v.value));
        if (same) return static_cast<int>(p);
    }
    return -1;
}

namespace detail {
using Vec3 = std::array<float, 3>;
constexpr float kDegToRad = 0.017453292519943295f;

inline Vec3 row(const std::array<float, 16>& m, int r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
inline void set_row(std::array<float, 16>& m, int r, const Vec3& v) {
    m[r * 4] = v[0]; m[r * 4 + 1] = v[1]; m[r * 4 + 2] = v[2];
}
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3 normalize(const Vec3& a) {
    const float len = std::sqrt(dot(a, a));
    return len > 1e-8f ? scale(a, 1.0f / len) : Vec3{0, 0, 0};
}
// Rodrigues rotation of v about unit axis by angle (right-handed).
inline Vec3 rotate(const Vec3& v, const Vec3& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return add(add(scale(v, c), scale(cross(axis, v), s)), scale(axis, dot(axis, v) * (1 - c)));
}
// Rotate the whole basis about a world-space axis, then re-orthonormalise.
inline void rotate_basis(std::array<float, 16>& m, const Vec3& axis, float angle) {
    for (int r = 0; r < 3; ++r) set_row(m, r, rotate(row(m, r), axis, angle));
    Vec3 right = normalize(row(m, 0));
    Vec3 back = normalize(cross(right, row(m, 1)));   // right x up = backward
    Vec3 up = cross(back, right);
    set_row(m, 0, right); set_row(m, 1, up); set_row(m, 2, back);
}
// Betaflight's actual-rates curve: deg/s for a stick position -1..1.
inline float actual_rate(float stick, const Rate& r) {
    const float a = std::min(std::abs(stick), 1.0f);
    const float expo = std::clamp(r.expo, 0.0f, 1.0f);
    const float curve = a * (a * a * a * a * a * expo + a * (1.0f - expo));
    const float movement = std::max(0.0f, r.max - r.center);
    return std::copysign(r.center * a + movement * curve, stick);
}
inline float clamp1(float v) { return std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f); }
inline std::array<float, 16> camera_from_body(const std::array<float, 16>& body, const std::array<float, 3>& position,
    float uptilt_rad) {
    auto cam = body;
    rotate_basis(cam, row(body, 0), uptilt_rad);  // + about the right axis = look up
    cam[12] = position[0]; cam[13] = position[1]; cam[14] = position[2]; cam[15] = body[15];
    return cam;
}
}

// Radio switches: an arm switch and a reset switch, each an axis (-6..6) or a button (1..32), if mapped.
// Call once per tick with the raw axes (-1..1) and the pressed buttons. An axis has to go well past
// the middle to count (above +0.5 = on, below -0.5 = off for arming; +0.5 then back under +0.2 for
// reset), so a stick that drifts a little never triggers them. Do not use an axis a stick is on.
inline void apply_switches(const std::array<float, 6>& axes, std::uint32_t buttons = 0) {
    static bool reset_axis_high = false, reset_button_down = false;
    const auto number = [](Knob k) {
        return static_cast<int>(std::lround(knobs().value[k].load(std::memory_order_relaxed)));
    };
    const auto pressed = [buttons](int n) { return n >= 1 && n <= 32 && (buttons & (1u << (n - 1))) != 0; };
    if (const int a = number(kArmAxis); a != 0 && std::abs(a) <= 6) {
        const float v = radio::pick_axis(axes, a);
        if (v > 0.5f) set_armed(true);
        else if (v < -0.5f) set_armed(false);
    }
    if (const int n = number(kArmButton); n != 0) set_armed(pressed(n));
    if (const int a = number(kResetAxis); a != 0 && std::abs(a) <= 6) {
        const float v = radio::pick_axis(axes, a);
        if (v > 0.5f && !reset_axis_high) { request_reset(); reset_axis_high = true; }
        else if (v < 0.2f) reset_axis_high = false;
    }
    if (const int n = number(kResetButton); n != 0) {
        const bool down = pressed(n);
        if (down && !reset_button_down) request_reset();
        reset_button_down = down;
    }
}

// The world the drone can bump into. `cast(from, to, hit)` returns true and the hit point when
// something solid lies on the line. Without it (or with collide off) the drone flies through everything.
struct World {
    std::function<bool(const std::array<float, 3>& from, const std::array<float, 3>& to, std::array<float, 3>& hit)> cast;
};

// What the pilot is asking for, whatever the input device.
struct Cmd {
    float pitch = 0;          // + = nose up
    float roll = 0;           // + = roll right
    float yaw_rad = 0;        // radians this step; negative = turn right
    float look_pitch_rad = 0; // extra pitch from the mouse (radians, + = look down)
    float throttle = 0.35f;   // 0..1 absolute
    bool boost = false;
    bool park = false;        // motors off: hold position
};

// One step of the flight model. `camera` is the pose published last tick (its translation is
// the drone position).
inline std::array<float, 16> step_drone_cmd(const std::array<float, 16>& camera, State& state, const Cmd& cmd_in,
    float seconds, float speed, const Settings& cfg = {}, const World* world = nullptr) {
    using namespace detail;
    if (!valid_flight_transform(camera) || !std::isfinite(seconds) || !std::isfinite(speed)) return camera;
    const float dt = std::clamp(seconds, 0.0f, 0.05f);
    const float uptilt = cfg.camera_uptilt_deg * kDegToRad;
    Vec3 position{camera[12], camera[13], camera[14]};

    if (!state.valid) {
        // Start from the current view with the FPV tilt removed, at rest.
        state.body = camera;
        rotate_basis(state.body, row(camera, 0), -uptilt);
        state.velocity = {0, 0, 0};
        state.start_y = position[1];
        state.flight_time = 0;
        state.start_pos = position;
        state.start_body = state.body;
        state.valid = true;
        state.failsafe = false;
        reset_flag().store(false, std::memory_order_relaxed);
    }
    if (reset_flag().exchange(false, std::memory_order_acq_rel)) {
        state.body = state.start_body;
        state.velocity = {0, 0, 0};
        state.flight_time = 0;
        position = state.start_pos;
        state.failsafe = false;
        state.signal_lost = 0;
    }
    // Disarmed: the pilot's inputs and the motors are off.
    Cmd cmd = cmd_in;
    // Range: the link weakens with distance from the takeoff point, and past the failsafe
    // distance the motors cut until the drone is reset (signal_strength above).
    const float home = std::sqrt(dot(add(position, scale(state.start_pos, -1.0f)), add(position, scale(state.start_pos, -1.0f))));
    const float quality = signal_strength(home, cfg.range);
    state.signal_lost = quality <= 0.0f ? state.signal_lost + dt : 0.0f;
    if (failsafe_due(home, cfg.range, state.signal_lost)) state.failsafe = true;
    {
        auto& link = telemetry();
        link.signal.store(quality, std::memory_order_relaxed);
        link.signal_lost.store(state.signal_lost, std::memory_order_relaxed);
        link.failsafe.store(state.failsafe, std::memory_order_relaxed);
        link.home_distance.store(home, std::memory_order_relaxed);
    }
    if (!cfg.armed || state.failsafe) {
        cmd = Cmd{};
        cmd.throttle = 0.0f;
        cmd.park = cfg.disarm_freeze;
    }

    const float pitch_in = clamp1(cmd.pitch);
    const float roll_in = clamp1(cmd.roll);
    const float yaw_delta = std::isfinite(cmd.yaw_rad) ? cmd.yaw_rad : 0.0f;
    const float look_pitch = std::isfinite(cmd.look_pitch_rad) ? cmd.look_pitch_rad : 0.0f;

    // Attitude
    if (!cfg.angle_mode) {
        const float dp = (actual_rate(pitch_in, cfg.pitch) * kDegToRad) * dt - look_pitch;
        const float dr = (actual_rate(roll_in, cfg.roll) * kDegToRad) * dt;
        rotate_basis(state.body, row(state.body, 1), yaw_delta);        // yaw about body up
        rotate_basis(state.body, row(state.body, 0), dp);               // pitch about body right
        rotate_basis(state.body, scale(row(state.body, 2), -1.0f), dr); // roll about body forward
    } else {
        // Heading from yaw input; tilt eases toward the commanded angle.
        const Vec3 fwd = scale(row(state.body, 2), -1.0f);
        const float heading = std::atan2(fwd[0], -fwd[2]);
        const float new_heading = heading - yaw_delta;
        const float max_a = cfg.max_angle_deg * kDegToRad;
        const float k = 1 - std::exp(-cfg.angle_response * dt);
        const float cur_pitch = std::asin(std::clamp(fwd[1], -1.0f, 1.0f));
        const float cur_roll = std::asin(std::clamp(row(state.body, 0)[1], -1.0f, 1.0f)) * -1.0f;
        const float want_pitch = pitch_in * max_a, want_roll = roll_in * max_a;
        const float p = cur_pitch + (want_pitch - cur_pitch) * k - look_pitch;
        const float r = cur_roll + (want_roll - cur_roll) * k;
        Vec3 f{std::sin(new_heading), 0, -std::cos(new_heading)};
        Vec3 right{-f[2], 0, f[0]};
        Vec3 up{0, 1, 0};
        std::array<float, 16> m = state.body;
        set_row(m, 0, right); set_row(m, 1, up); set_row(m, 2, scale(f, -1.0f));
        rotate_basis(m, right, p);
        rotate_basis(m, scale(row(m, 2), -1.0f), r);
        state.body = m;
    }

    // Translation
    Vec3 next_pos = position;
    if (cmd.park) {
        state.velocity = {0, 0, 0};
    } else {
        const Vec3 up = row(state.body, 1);
        const float thrust = std::clamp(cmd.throttle, 0.0f, 1.0f) * (cfg.gravity / cfg.hover_throttle);
        const float spd = std::sqrt(dot(state.velocity, state.velocity));
        const float drag = cfg.lin_drag + cfg.quad_drag * spd;
        Vec3 accel{up[0] * thrust - state.velocity[0] * drag,
                   up[1] * thrust - cfg.gravity - state.velocity[1] * drag,
                   up[2] * thrust - state.velocity[2] * drag};
        for (int i = 0; i < 3; ++i) state.velocity[i] += accel[i] * dt;
        const float cap = std::clamp(speed, 0.6f, 1500.0f) * 4.0f * (cmd.boost ? 2.0f : 1.0f);
        const float now = std::sqrt(dot(state.velocity, state.velocity));
        if (now > cap) for (auto& v : state.velocity) v *= cap / now;
        next_pos = {position[0] + state.velocity[0] * dt, position[1] + state.velocity[1] * dt,
                    position[2] + state.velocity[2] * dt};
        if (world && world->cast && cfg.collide) {
            // Swept move, one axis at a time (up/down first so landing works), so it slides along
            // walls and rests on the ground. The game's query gives hit points, not surface normals.
            Vec3 p = position;
            const Vec3 d{next_pos[0] - position[0], next_pos[1] - position[1], next_pos[2] - position[2]};
            const float r = std::max(cfg.body_radius, 0.01f);
            for (const int axis : {1, 0, 2}) {
                const float move = d[static_cast<std::size_t>(axis)];
                if (move == 0.0f) continue;
                const auto a = static_cast<std::size_t>(axis);
                const float sign = move > 0 ? 1.0f : -1.0f;
                Vec3 to = p;
                to[a] += move + sign * r;
                Vec3 hit{};
                if (world->cast(p, to, hit)) {
                    const float allowed = (hit[a] - p[a]) - sign * r;
                    p[a] += sign > 0 ? std::clamp(allowed, 0.0f, move) : std::clamp(allowed, move, 0.0f);
                    state.velocity[a] = 0;
                    if (axis == 1 && sign < 0) {   // landed: skid to a stop
                        const float grip = std::exp(-6.0f * dt);
                        state.velocity[0] *= grip;
                        state.velocity[2] *= grip;
                    }
                } else {
                    p[a] += move;
                }
            }
            next_pos = p;
            // Safety net if the collision world is not streamed in yet: do not fall forever.
            const float deep = state.start_y - 300.0f;
            if (next_pos[1] < deep) { next_pos[1] = deep; if (state.velocity[1] < 0) state.velocity[1] = 0; }
        } else if (cfg.floor_depth > 0.0f) {
            // No collisions: an invisible floor so it lands instead of falling through the map.
            const float floor_y = state.start_y - cfg.floor_depth;
            if (next_pos[1] < floor_y) {
                next_pos[1] = floor_y;
                if (state.velocity[1] < 0) state.velocity[1] = 0;
                const float grip = std::exp(-6.0f * dt);
                state.velocity[0] *= grip;
                state.velocity[2] *= grip;
            }
        }
    }

    const auto out = camera_from_body(state.body, next_pos, uptilt);
    if (!valid_flight_transform(out)) { state.valid = false; return camera; }
    if (cfg.armed) state.flight_time += dt;
    auto& t = telemetry();
    t.throttle.store(cmd.park ? 0.0f : std::clamp(cmd.throttle, 0.0f, 1.0f), std::memory_order_relaxed);
    t.speed.store(std::sqrt(dot(state.velocity, state.velocity)), std::memory_order_relaxed);
    t.altitude.store(next_pos[1] - state.start_y, std::memory_order_relaxed);
    t.seconds.store(state.flight_time, std::memory_order_relaxed);
    t.armed.store(cfg.armed, std::memory_order_relaxed);
    beacon::publish_local(next_pos, beacon::rotation_from_basis(state.body), cfg.beacon);
    t.last_ms.store(now_ms(), std::memory_order_relaxed);
    return out;
}

// Keyboard / mouse / game-controller input (the Freecam's own FlightInput).
inline std::array<float, 16> step_drone_flight(const std::array<float, 16>& camera, State& state,
    const overlay::FlightInput& input, float seconds, float speed, const Settings& cfg = {},
    const World* world = nullptr) {
    using namespace detail;
    if (!input.active || !std::isfinite(input.look_x) || !std::isfinite(input.look_y)) return camera;
    Cmd cmd;
    cmd.pitch = clamp1(-input.forward);   // forward stick = nose down
    cmd.roll = clamp1(input.right);
    cmd.yaw_rad = -std::clamp(input.look_x, -250.0f, 250.0f) * cfg.yaw_look_scale;  // right = negative about up
    cmd.look_pitch_rad = std::clamp(input.look_y, -250.0f, 250.0f) * cfg.yaw_look_scale;
    // Keyboard has no absolute throttle: neutral hovers, Q/E climb or descend.
    const float up_in = clamp1(input.up);
    cmd.throttle = up_in >= 0 ? cfg.hover_throttle + up_in * (1.0f - cfg.hover_throttle)
                              : cfg.hover_throttle + up_in * cfg.hover_throttle;
    cmd.boost = input.boost;
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(input.right), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(input.forward), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(input.look_x / 40.0f), std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}

// Radio controller: sticks drive rates directly and the throttle stick is absolute thrust. A throttle
// at the bottom lets the drone fall, as a real one would.
inline std::array<float, 16> step_drone_radio(const std::array<float, 16>& camera, State& state,
    const radio::Command& sticks, float seconds, float speed, const Settings& cfg = {}, const World* world = nullptr) {
    using namespace detail;
    if (!sticks.valid) return camera;
    const float dt = std::clamp(std::isfinite(seconds) ? seconds : 0.0f, 0.0f, 0.05f);
    Cmd cmd;
    cmd.pitch = clamp1(-sticks.pitch);    // stick forward = nose down
    cmd.roll = clamp1(sticks.roll);
    cmd.yaw_rad = -actual_rate(clamp1(sticks.yaw), cfg.yaw) * kDegToRad * dt;
    cmd.throttle = std::clamp(sticks.throttle, 0.0f, 1.0f);
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(sticks.roll), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(sticks.pitch), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(sticks.yaw), std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}
}
