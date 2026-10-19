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
//   input.boost    -> half as much thrust again
//
// The model: the throttle sets motor output, which spools up and down over some tens of
// milliseconds, and thrust grows with output to the power 1.5, so the hover throttle fixes the
// thrust-to-weight ratio (a 22% hover is about 10:1, a 5-inch's). Thrust falls off as the drone
// climbs into its own inflow and comes back near the ground. The flight controller turns the
// drone at the rate the sticks ask, catching up in about 30 ms, or in angle mode leans it to the
// angle they ask. Drag is quadratic and larger across the props than edge on; the top speed
// setting sets how much, so full throttle flat out settles near it. Hits bounce off the surface
// they meet, knock the drone about, and a hard one cuts the motors until it is armed again.
#include "free_flight.h"
#include "drone_radio.h"
#include "drone_beacon.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <iterator>
#include <limits>
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
    float camera_uptilt_deg = 0.0f;
    bool angle_mode = false;        // true = self-levelling (easier), false = acro (full flips)
    float max_angle_deg = 55.0f;
    float angle_response = 8.0f;    // 1/s: how quickly angle mode leans to the asked angle
    bool alt_hold = false;          // a throttle that springs back holds the height in its middle
    float climb_rate = 20.0f;       // m/s at full stick while holding height
    bool crash = true;              // hits bounce and tumble; a hard one cuts the motors
    float crash_speed = 7.0f;       // m/s into a surface that cuts the motors; 0 = never
    float propwash = 0.5f;          // shake when dropping through its own prop wash, 0-1
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
    std::array<float, 3> rate{};    // turn rates, rad/s: about right (+ nose up), up (+ left), forward (+ roll right)
    float motor = 0;                // motor output 0..1, following the throttle with the spool-up lag
    bool crashed = false;           // a hard hit cut the motors: arming again (off, then on) puts it right
    bool armed_last = true;         // armed last tick, to see an arm after a crash
    bool grounded = false;          // touched something underneath last tick
    float glitch = 0;               // video break-up after a hit, 0..1, fading
    std::array<float, 3> wash{};    // prop-wash wobble now, rad/s
    std::uint32_t noise = 0x2545f491u;
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
    std::atomic<float> stick_throttle{-2};  // -1..1 when the throttle stick springs back, else below -1 (shown from the throttle)
    std::atomic<bool> crashed{false};       // a hard hit cut the motors
    std::atomic<float> glitch{0};           // video break-up after a hit, 0..1
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
    kAnalog, kAnalogNoise, kAnalogScan, kAnalogVignette, kAnalogGlitch, kRange, kMotorPitch, kThrottleCentre, kDeadzone, kGoggles, kHideUi,
    kGamepad, kPadRate, kPadExpo, kAltHold, kClimbRate, kFlightMode, kMaxAngle, kCrash, kCrashSpeed, kPropwash, kPadInvert,
    kFollowDistance, kFollowAggression };
inline constexpr KnobInfo knob_info[] = {
    {"gravity", "How hard the drone is pulled down. 9.8 is Earth; higher feels heavier and snappier", 0.0f, 50.0f, 9.81f},
    {"drag", "Air resistance. Higher slows the drone down sooner when you let off", 0.0f, 3.0f, 0.15f},
    {"hover", "The throttle that keeps the drone in the air (0.1-0.9). Lower means more power to spare: 0.22 is about ten times its weight in thrust, a 5-inch's", 0.1f, 0.9f, 0.35f},
    {"rollcenter", "Roll speed near the middle of the stick, in degrees per second (Betaflight \"Actual\" rates)", 1.0f, 1000.0f, 80.0f},
    {"rollmax", "Roll speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 700.0f},
    {"rollexpo", "How much softer roll is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.15f},
    {"pitchcenter", "Pitch speed near the middle of the stick, in degrees per second", 1.0f, 1000.0f, 80.0f},
    {"pitchmax", "Pitch speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 700.0f},
    {"pitchexpo", "How much softer pitch is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.15f},
    {"yawcenter", "Yaw (turning) speed near the middle of the stick, in degrees per second", 1.0f, 1000.0f, 130.0f},
    {"yawmax", "Yaw speed with the stick fully over, in degrees per second", 30.0f, 2000.0f, 780.0f},
    {"yawexpo", "How much softer yaw is near the middle of the stick (0 = none, 1 = most)", 0.0f, 1.0f, 0.30f},
    {"uptilt", "How far the camera points up, in degrees. Real FPV cameras sit at about 25-40; faster flying needs more", -30.0f, 70.0f, 0.0f},
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
    {"goggles", "What your skater wears over their eyes while you fly: 0 = nothing, 1-8 = one of the eyewear choices in Addons > Drone", 0.0f, 8.0f, 1.0f},
    {"hideui", "Hide the game's own interface while you fly or the drone follows you (Start flying or Follow me hides it, stopping brings it back)", 0.0f, 1.0f, 1.0f},
    {"gamepad", "Fly with a gamepad (Xbox or PlayStation) instead of an RC radio: left stick throttle and yaw, right stick pitch and roll, with gentler sticks. A arms, Menu / Options puts the drone back", 0.0f, 1.0f, 1.0f},
    {"padrate", "Gamepad: how much of your rates the sticks reach at full tilt (0.2-1). Lower is calmer. In angle mode it slows turning only: full stick still leans all the way", 0.2f, 1.0f, 0.6f},
    {"padexpo", "Gamepad: how much softer the middle of the sticks is, for small corrections (0 = none, 1 = most)", 0.0f, 1.0f, 0.45f},
    {"althold", "With a throttle that springs back to the middle (a gamepad, or the keyboard): hold your height (1), adding throttle as you lean to keep it; up climbs, down descends. Off (0): the middle is hover power and the throttle is yours alone, so a hard lean loses height until you add some", 0.0f, 1.0f, 0.0f},
    {"climbrate", "Height hold: how fast full throttle stick climbs or descends, in metres per second", 1.0f, 40.0f, 20.0f},
    {"mode", "Flight mode: 0 = acro (the stick sets how fast it turns, full flips), 1 = angle (self-levelling: the stick sets how far it leans, and letting go levels it)", 0.0f, 1.0f, 0.0f},
    {"maxangle", "Angle mode: how far the drone leans at full stick, in degrees", 10.0f, 80.0f, 55.0f},
    {"crash", "Crashes: hits bounce and tumble the drone, and a hard one cuts the motors until you arm again (1). Off: it just stops against things (0)", 0.0f, 1.0f, 1.0f},
    {"crashspeed", "Crashes: how hard a hit cuts the motors, in metres per second into the surface. 0 = never", 0.0f, 30.0f, 7.0f},
    {"propwash", "How much the drone shakes when it drops through its own prop wash (0-1)", 0.0f, 1.0f, 0.5f},
    {"padinvert", "Gamepad: push the right stick up to tip the nose up, like aiming a camera (1); or to tip it down and fly forward, like an RC radio (0)", 0.0f, 1.0f, 1.0f},
    {"followdistance", "Follow me: how far behind you the drone flies, in metres (it flies a little higher the further back it is)", 3.0f, 12.0f, 4.5f},
    {"followaggression", "Follow me: how hard the pilot flies (0-1). Low is a calm cinematic pilot who lags a little and leans gently; high reacts fast, leans hard and stays right on you", 0.0f, 1.0f, 0.5f},
};
inline constexpr std::size_t knob_count = std::size(knob_info);
static_assert(knob_count == kFollowAggression + 1, "knob enum and table must match");
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
inline bool gamepad_mode() { return knobs().value[kGamepad].load(std::memory_order_relaxed) >= 0.5f; }
// A gamepad's sticks, Mode 2: left stick throttle (springing back) and yaw, right stick pitch and
// roll. Each stick has a round dead zone (a pad's drifts more than a radio's gimbals); then the
// middle is softened (`expo`) and the whole travel scaled down (`scale`), since a pad's short
// sticks at a radio's rates make every touch a flip. `invert`: up on the right stick tips the nose
// up (as a camera is aimed) rather than down (as on a radio). In angle mode the right stick sets
// how far the drone leans, not how fast it turns, so `scale` leaves it alone there: full stick is
// the full lean.
inline radio::Command gamepad_sticks(const overlay::Gamepad& pad, float dz, float scale, float expo, float hover, bool invert = false,
                                     bool angle = false) {
    radio::Command out;
    if (!pad.valid) return out;
    out.valid = true;
    const float zone = std::clamp(std::isfinite(dz) ? dz : 0.0f, 0.08f, 0.5f);
    const auto stick = [zone](float x, float y) -> std::array<float, 2> {
        x = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
        y = std::isfinite(y) ? std::clamp(y, -1.0f, 1.0f) : 0.0f;
        const float length = std::sqrt(x * x + y * y);
        if (!(length > zone)) return {0.0f, 0.0f};
        const float k = std::min(1.0f, (length - zone) / (1.0f - zone)) / length;
        return {x * k, y * k};
    };
    const float e = std::clamp(std::isfinite(expo) ? expo : 0.0f, 0.0f, 1.0f);
    const float s = std::clamp(std::isfinite(scale) ? scale : 1.0f, 0.05f, 1.0f);
    const auto shape = [e](float v, float most) { const float a = std::abs(v); return std::copysign((e * a * a * a + (1.0f - e) * a) * most, v); };
    const float tilt = angle ? 1.0f : s;
    const auto left = stick(pad.left_x, pad.left_y), right = stick(pad.right_x, pad.right_y);
    out.yaw = shape(left[0], s);
    out.roll = shape(right[0], tilt);
    out.pitch = shape(invert ? -right[1] : right[1], tilt);   // + = forward = nose down
    out.centred = true;
    out.climb = left[1];
    out.throttle = radio::spring_throttle(left[1], hover);
    return out;
}
inline radio::Command read_gamepad(const overlay::Gamepad& pad) {
    const auto get = [](Knob k) { return knobs().value[k].load(std::memory_order_relaxed); };
    return gamepad_sticks(pad, get(kDeadzone), get(kPadRate), get(kPadExpo), get(kHover), get(kPadInvert) >= 0.5f,
                          get(kFlightMode) >= 0.5f);
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
    s.angle_mode = get(kFlightMode) >= 0.5f; s.max_angle_deg = get(kMaxAngle);
    s.alt_hold = get(kAltHold) >= 0.5f; s.climb_rate = get(kClimbRate);
    s.crash = get(kCrash) >= 0.5f; s.crash_speed = get(kCrashSpeed); s.propwash = get(kPropwash);
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
// A camera on a tilt gimbal (as on a DJI Avata): it faces the way the body faces and holds its own
// pitch, `tilt_rad` (+ = look up), however the body leans, keeping only a share of the body's roll.
inline std::array<float, 16> camera_on_gimbal(const std::array<float, 16>& body, const std::array<float, 3>& position,
    float tilt_rad, float roll_share) {
    Vec3 forward = scale(row(body, 2), -1.0f);
    forward[1] = 0.0f;
    if (dot(forward, forward) < 0.0025f) forward = cross(row(body, 0), Vec3{0, 1, 0});
    forward = normalize(forward);
    auto cam = body;
    set_row(cam, 0, Vec3{-forward[2], 0.0f, forward[0]});
    set_row(cam, 1, Vec3{0.0f, 1.0f, 0.0f});
    set_row(cam, 2, scale(forward, -1.0f));
    rotate_basis(cam, row(cam, 0), tilt_rad);
    // The body's roll (+ = right wing down), about the camera's own view axis.
    const float roll = -std::asin(std::clamp(row(body, 0)[1], -1.0f, 1.0f));
    rotate_basis(cam, scale(row(cam, 2), -1.0f), roll * roll_share);
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

// A gamepad's buttons (XInput's bits; a PlayStation pad's are given the same): A (cross) arms or
// disarms, Menu (Options) puts the drone back where it took off. Each on the press, not the hold;
// with `act` false (the ReSkate menu has the pad) they are only watched, so a button still held
// when the menu closes does nothing.
inline constexpr unsigned pad_start = 0x0010, pad_a = 0x1000;
inline void apply_gamepad_buttons(unsigned buttons, bool act = true) {
    static unsigned before = ~0u; // nothing counts as pressed until it has been seen up
    const unsigned pressed = buttons & ~before;
    before = buttons;
    if (!act) return;
    if (pressed & pad_a) set_armed(!armed());
    if (pressed & pad_start) request_reset();
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
    bool hold = false;        // hold the height: `climb` asks for a climb or a descent instead of `throttle`
    float climb = 0;          // -1..1 of the climb rate, while holding height
    bool boost = false;
    bool park = false;        // motors off: hold position
    float gimbal_tilt_rad = std::numeric_limits<float>::quiet_NaN(); // set: the camera is on a tilt gimbal at this pitch (+ = up)
};

// The physical constants of the model (see the top of this file).
inline constexpr float gimbal_roll_share = 0.3f;  // how much of the body's roll a tilt gimbal's picture keeps
inline constexpr float rate_lag = 0.03f;          // s: the flight controller's catch-up to the asked turn rate
inline constexpr float spool_up = 0.04f, spool_down = 0.07f; // s: motor output following the throttle
inline constexpr float thrust_power = 1.5f;       // thrust grows with motor output to this power
inline constexpr float boost_thrust = 1.5f;       // boost: this much more thrust
inline constexpr float hard_hit = 2.0f;           // m/s into a surface: a bump rather than a touch
inline constexpr float bounce = 0.35f;            // how much of a hard hit's speed it bounces back with
inline constexpr float hold_gain = 4.0f;          // 1/s: height hold's pull toward the asked climb

namespace detail {
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline float noise(std::uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 8388608.0f - 1.0f; // -1..1
}
inline float hover_of(const Settings& c) { return std::clamp(std::isfinite(c.hover_throttle) ? c.hover_throttle : 0.35f, 0.05f, 0.95f); }
// Thrust (m/s² along the drone's up) at full output, and at `output`.
inline float max_thrust(const Settings& c) { return std::max(0.0f, c.gravity) / std::pow(hover_of(c), thrust_power); }
inline float thrust_at(float output, const Settings& c) {
    return max_thrust(c) * std::pow(std::clamp(output, 0.0f, 1.0f), thrust_power);
}
// The output that gives `accel` m/s² of thrust.
inline float output_for(float accel, const Settings& c) {
    const float most = max_thrust(c);
    if (!(most > 0.0f) || !std::isfinite(accel)) return hover_of(c);
    return std::pow(std::clamp(accel / most, 0.0f, 1.0f), 1.0f / thrust_power);
}
// The top speed (m/s) the speed setting asks for, and the quadratic drag that gives it: flat out,
// the thrust left over from holding the drone up is spent on drag at that speed. (Scaled for the
// drag across the props and the inflow below, so that leaning anywhere from 45 to 85 degrees
// settles within a few percent of it.)
inline float top_speed(float speed) { return std::clamp(std::isfinite(speed) ? speed : 10.0f, 0.6f, 1500.0f) * 4.0f; }
inline float drag_for(const Settings& c, float speed) {
    const float most = max_thrust(c), g = std::max(0.0f, c.gravity);
    const float spare = std::sqrt(std::max(most * most - g * g, 0.01f));
    const float top = top_speed(speed);
    return 0.36f * spare / (top * top);
}
// The surface at a hit: two more rays beside the first find two more points of it. Without them
// (an edge, or a thin thing) the surface is taken to face straight back along the move.
inline Vec3 surface_normal(const World& world, const Vec3& from, const Vec3& dir, float reach, const Vec3& hit, float spread) {
    const Vec3 side = normalize(cross(dir, std::abs(dir[1]) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 other = cross(dir, side);
    std::array<Vec3, 2> points{};
    int found = 0;
    for (const auto& offset : {side, other, scale(side, -1.0f), scale(other, -1.0f)}) {
        if (found == 2) break;
        const Vec3 start = add(from, scale(offset, spread));
        Vec3 at{};
        if (world.cast(start, add(start, scale(dir, reach)), at)) points[static_cast<std::size_t>(found++)] = at;
    }
    if (found == 2) {
        Vec3 n = normalize(cross(sub(points[0], hit), sub(points[1], hit)));
        if (dot(n, dir) > 0.0f) n = scale(n, -1.0f);
        if (length(n) > 0.5f && dot(n, dir) < -0.05f) return n;
    }
    return scale(dir, -1.0f);
}
// Body turn rates (about right, up, forward) from a world-space spin vector, and back.
inline std::array<float, 3> body_rates(const std::array<float, 16>& body, const Vec3& spin) {
    return {dot(spin, row(body, 0)), dot(spin, row(body, 1)), -dot(spin, row(body, 2))};
}
}

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
        // Start level, facing the way the view faces, at rest and hovering, as a drone sits at takeoff.
        Vec3 heading = scale(row(camera, 2), -1.0f);
        heading[1] = 0.0f;
        if (length(heading) < 0.05f) heading = cross(row(camera, 0), Vec3{0, 1, 0});
        heading = normalize(heading);
        state.body = camera;
        set_row(state.body, 0, Vec3{-heading[2], 0.0f, heading[0]});
        set_row(state.body, 1, Vec3{0.0f, 1.0f, 0.0f});
        set_row(state.body, 2, scale(heading, -1.0f));
        state.velocity = {0, 0, 0};
        state.start_y = position[1];
        state.flight_time = 0;
        state.start_pos = position;
        state.start_body = state.body;
        state.valid = true;
        state.failsafe = false;
        state.rate = {0, 0, 0};
        state.motor = cfg.armed ? hover_of(cfg) : 0.0f;
        state.crashed = false;
        state.armed_last = cfg.armed;
        state.glitch = 0;
        reset_flag().store(false, std::memory_order_relaxed);
    }
    if (reset_flag().exchange(false, std::memory_order_acq_rel)) {
        state.body = state.start_body;
        state.velocity = {0, 0, 0};
        state.flight_time = 0;
        position = state.start_pos;
        state.failsafe = false;
        state.signal_lost = 0;
        state.rate = {0, 0, 0};
        state.motor = cfg.armed ? hover_of(cfg) : 0.0f;
        state.crashed = false;
        state.glitch = 0;
    }
    // After a crash the motors stay off until the drone is armed again: off, then on.
    if (cfg.armed && !state.armed_last) state.crashed = false;
    state.armed_last = cfg.armed;
    Cmd cmd = cmd_in;
    // Range: the link weakens with distance from the takeoff point, and past the failsafe
    // distance the motors cut until the drone is reset (signal_strength above).
    const float home = length(sub(position, state.start_pos));
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
    // Disarmed, crashed or out of range: the pilot's inputs and the motors are off. A crashed
    // drone falls even when a disarmed one would hang.
    const bool motors = cfg.armed && !state.failsafe && !state.crashed;
    if (!motors) {
        const float gimbal = cmd.gimbal_tilt_rad; // the gimbal keeps working without the motors
        cmd = Cmd{};
        cmd.gimbal_tilt_rad = gimbal;
        cmd.throttle = 0.0f;
        cmd.park = cfg.disarm_freeze && !state.crashed;
    }

    const float pitch_in = clamp1(cmd.pitch);
    const float roll_in = clamp1(cmd.roll);
    const float yaw_rate = dt > 0.0f && std::isfinite(cmd.yaw_rad) ? std::clamp(cmd.yaw_rad / dt, -40.0f, 40.0f) : 0.0f;
    const float look_pitch = std::isfinite(cmd.look_pitch_rad) ? cmd.look_pitch_rad : 0.0f;

    // Attitude: the flight controller asks for a turn rate on each axis and the drone catches up
    // with it; with the motors off nothing drives it and only the air slows a spin.
    if (motors) {
        std::array<float, 3> want{};
        if (!cfg.angle_mode) {
            want = {actual_rate(pitch_in, cfg.pitch) * kDegToRad, yaw_rate, actual_rate(roll_in, cfg.roll) * kDegToRad};
        } else {
            // Lean toward the angle the sticks ask, about the heading the drone faces.
            const Vec3 fwd = scale(row(state.body, 2), -1.0f);
            Vec3 level{fwd[0], 0.0f, fwd[2]};
            if (length(level) < 0.05f) level = cross(row(state.body, 0), Vec3{0, 1, 0});
            level = normalize(level);
            const Vec3 level_right{-level[2], 0.0f, level[0]};
            const float most = std::clamp(cfg.max_angle_deg, 1.0f, 89.0f) * kDegToRad;
            Vec3 desired = rotate(Vec3{0, 1, 0}, level_right, pitch_in * most);
            desired = rotate(desired, rotate(level, level_right, pitch_in * most), roll_in * most);
            const Vec3 up = row(state.body, 1);
            const Vec3 axis = cross(up, desired);
            const float angle = std::atan2(length(axis), dot(up, desired));
            const Vec3 spin = scale(normalize(axis), angle * std::max(cfg.angle_response, 0.5f));
            const auto r = body_rates(state.body, spin);
            const float limit_pitch = std::max(cfg.pitch.max, 30.0f) * kDegToRad, limit_roll = std::max(cfg.roll.max, 30.0f) * kDegToRad;
            want = {std::clamp(r[0], -limit_pitch, limit_pitch), yaw_rate, std::clamp(r[2], -limit_roll, limit_roll)};
        }
        const float k = 1.0f - std::exp(-dt / rate_lag);
        for (std::size_t i = 0; i < 3; ++i) state.rate[i] += (want[i] - state.rate[i]) * k;
    } else {
        const float damp = std::exp(-dt * (state.grounded ? 8.0f : 0.6f));
        for (auto& r : state.rate) r *= damp;
    }
    // Prop wash: dropping straight down through its own wash shakes it.
    {
        const Vec3 up = row(state.body, 1);
        const float axial = dot(state.velocity, up);
        const float across = length(sub(state.velocity, scale(up, axial)));
        const float sink = std::clamp((-axial - 1.5f) / 6.0f, 0.0f, 1.0f) * std::clamp(1.0f - across / 8.0f, 0.0f, 1.0f);
        const float amount = motors ? std::clamp(cfg.propwash, 0.0f, 1.0f) * sink * 3.0f : 0.0f;
        const float k = 1.0f - std::exp(-dt * 20.0f);
        for (std::size_t i : {0u, 2u}) state.wash[i] += (noise(state.noise) * amount - state.wash[i]) * k;
        state.wash[1] = 0.0f;
    }
    for (auto& r : state.rate) if (!std::isfinite(r)) r = 0.0f;
    rotate_basis(state.body, row(state.body, 1), state.rate[1] * dt);                                // yaw about body up
    rotate_basis(state.body, row(state.body, 0), (state.rate[0] + state.wash[0]) * dt - look_pitch);  // pitch about body right
    rotate_basis(state.body, scale(row(state.body, 2), -1.0f), (state.rate[2] + state.wash[2]) * dt); // roll about body forward

    // Translation
    Vec3 next_pos = position;
    float hardest = 0.0f;
    if (cmd.park) {
        state.velocity = {0, 0, 0};
        state.motor = 0.0f;
    } else {
        const Vec3 right = row(state.body, 0), up = row(state.body, 1), back = row(state.body, 2);
        // The throttle, or with height hold the output that keeps the asked climb, leaning or not.
        // Height hold never holds the stick back: the stick's own throttle wins whenever it asks
        // for more (up) or less (down), so full stick is always full or no throttle.
        float want = std::clamp(cmd.throttle, 0.0f, 1.0f);
        if (cmd.hold && motors) {
            const float stick = clamp1(cmd.climb);
            const float direct = radio::spring_throttle(stick, hover_of(cfg));
            const float lift = std::max(0.0f, cfg.gravity + hold_gain * (stick * std::max(cfg.climb_rate, 0.0f) - state.velocity[1]));
            const float held = up[1] > 0.25f ? output_for(lift / up[1], cfg) : direct;
            want = stick > 0.0f ? std::max(held, direct) : stick < 0.0f ? std::min(held, direct) : held;
        }
        if (!motors) want = 0.0f;
        state.motor += (want - state.motor) * (1.0f - std::exp(-dt / (want > state.motor ? spool_up : spool_down)));
        float thrust = thrust_at(state.motor, cfg) * (cmd.boost && motors ? boost_thrust : 1.0f);
        // Inflow: climbing into its own air it pushes less, sinking a little more.
        const float axial = dot(state.velocity, up);
        thrust *= std::clamp(1.0f - axial / std::max(top_speed(speed) * 2.5f, 5.0f), 0.3f, 1.25f);
        // Ground effect: within a couple of prop sizes of the ground it pushes more.
        const float r = std::max(cfg.body_radius, 0.01f);
        if (thrust > 0.0f && world && world->cast && cfg.collide) {
            Vec3 below{};
            const float reach = 4.0f * r;
            if (world->cast(position, add(position, Vec3{0, -reach, 0}), below))
                thrust *= 1.0f + 0.25f * std::clamp(1.0f - (position[1] - below[1]) / reach, 0.0f, 1.0f);
        }
        // Drag: quadratic, larger across the props than edge on, plus the plain drag setting.
        const float k = drag_for(cfg, speed) * length(state.velocity);
        const Vec3 drag = add(add(scale(right, -dot(state.velocity, right) * k), scale(up, -dot(state.velocity, up) * 1.5f * k)),
                              scale(back, -dot(state.velocity, back) * k));
        const Vec3 accel = add(add(scale(up, thrust), drag), Vec3{-state.velocity[0] * cfg.lin_drag,
            -cfg.gravity - state.velocity[1] * cfg.lin_drag, -state.velocity[2] * cfg.lin_drag});
        for (std::size_t i = 0; i < 3; ++i) state.velocity[i] += accel[i] * dt;
        // (Only a guard against runaway numbers: drag sets the real top speed.)
        const float cap = std::max(top_speed(speed) * 3.0f, 30.0f);
        if (const float now = length(state.velocity); now > cap) state.velocity = scale(state.velocity, cap / now);
        next_pos = add(position, scale(state.velocity, dt));
        state.grounded = false;
        if (world && world->cast && cfg.collide) {
            // Swept move: up to the first surface on the way, then what is left of the move slides
            // along it. A hit takes away the speed into the surface; a hard one bounces back some
            // of it and knocks the drone about.
            Vec3 p = position;
            Vec3 move = sub(next_pos, position);
            const float spread = std::max(r, 0.05f);
            for (int pass = 0; pass < 3; ++pass) {
                const float len = length(move);
                if (!(len > 1e-6f)) break;
                const Vec3 dir = scale(move, 1.0f / len);
                Vec3 hit{};
                if (!world->cast(p, add(p, scale(dir, len + r)), hit)) { p = add(p, move); break; }
                const float gap = length(sub(hit, p)) - r;
                const float allowed = std::clamp(gap, 0.0f, len);
                p = add(p, scale(dir, allowed));
                const Vec3 n = surface_normal(*world, p, dir, len + r + 2.0f * spread, hit, spread);
                if (gap < 0.0f) p = add(p, scale(n, std::min(-gap, r)));   // started too close: step out
                if (n[1] > 0.6f) state.grounded = true;
                const float into = dot(state.velocity, n);
                if (into < 0.0f) {
                    const float impact = -into;
                    const Vec3 along = sub(state.velocity, scale(n, into));
                    if (cfg.crash && impact > hard_hit) {
                        state.velocity = add(scale(along, 0.75f), scale(n, impact * bounce));
                        // Knocked about: spun away from the surface by the speed along it, plus some
                        // of the hit at random.
                        const float slide = std::min(length(along), 15.0f);
                        Vec3 spin = scale(normalize(cross(n, along)), slide * 0.6f);
                        spin = add(spin, scale(Vec3{noise(state.noise), noise(state.noise), noise(state.noise)}, impact * 0.4f));
                        const auto kick = body_rates(state.body, spin);
                        for (std::size_t i = 0; i < 3; ++i) state.rate[i] = std::clamp(state.rate[i] + kick[i], -30.0f, 30.0f);
                    } else {
                        state.velocity = along;
                    }
                    hardest = std::max(hardest, impact);
                }
                if (n[1] > 0.6f) {   // on the ground: skid to a stop
                    const float grip = std::exp(-6.0f * dt);
                    const float normal = dot(state.velocity, n);
                    state.velocity = add(scale(sub(state.velocity, scale(n, normal)), grip), scale(n, normal));
                }
                Vec3 rest = scale(dir, len - allowed);
                if (const float push = dot(rest, n); push < 0.0f) rest = sub(rest, scale(n, push));
                move = rest;
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
                state.grounded = true;
            }
        }
    }
    // A hit hard enough cuts the motors (the drone falls and tumbles); any real one breaks up the picture.
    if (cfg.crash && hardest > hard_hit) {
        state.glitch = std::max(state.glitch, std::clamp(hardest / std::max(cfg.crash_speed * 1.5f, 4.0f), 0.2f, 1.0f));
        if (motors && cfg.crash_speed > 0.0f && hardest >= cfg.crash_speed) {
            state.crashed = true;
            set_armed(false);
        }
    }
    state.glitch *= std::exp(-dt * 2.5f);

    const auto out = std::isfinite(cmd.gimbal_tilt_rad) ? camera_on_gimbal(state.body, next_pos, cmd.gimbal_tilt_rad, gimbal_roll_share)
                                                        : camera_from_body(state.body, next_pos, uptilt);
    if (!valid_flight_transform(out)) { state.valid = false; return camera; }
    if (cfg.armed) state.flight_time += dt;
    auto& t = telemetry();
    t.throttle.store(cmd.park ? 0.0f : std::clamp(state.motor, 0.0f, 1.0f), std::memory_order_relaxed);
    t.speed.store(length(state.velocity), std::memory_order_relaxed);
    t.altitude.store(next_pos[1] - state.start_y, std::memory_order_relaxed);
    t.seconds.store(state.flight_time, std::memory_order_relaxed);
    t.armed.store(cfg.armed && !state.crashed, std::memory_order_relaxed);
    t.crashed.store(state.crashed, std::memory_order_relaxed);
    t.glitch.store(state.glitch, std::memory_order_relaxed);
    beacon::publish_local(next_pos, beacon::rotation_from_basis(state.body), cfg.beacon);
    t.last_ms.store(now_ms(), std::memory_order_relaxed);
    return out;
}

// Keyboard / mouse / game-controller input (the Freecam's own FlightInput). The keyboard has no
// throttle that stays put: with height hold the middle holds the height and Q / E climb and
// descend; without it the middle is hover power.
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
    const float up_in = clamp1(input.up);
    cmd.throttle = radio::spring_throttle(up_in, cfg.hover_throttle);
    cmd.hold = cfg.alt_hold;
    cmd.climb = up_in;
    cmd.boost = input.boost;
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(input.right), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(input.forward), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(input.look_x / 40.0f), std::memory_order_relaxed);
        t.stick_throttle.store(up_in, std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}

// A controller: an RC radio, or a gamepad read as one (gamepad_sticks). The sticks drive the rates;
// a throttle stick that stays put is absolute thrust, and at the bottom lets the drone fall, as a
// real one would. One that springs back holds the height (height hold) or hovers in its middle.
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
    cmd.hold = sticks.centred && cfg.alt_hold;
    cmd.climb = clamp1(sticks.climb);
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(sticks.roll), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(sticks.pitch), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(sticks.yaw), std::memory_order_relaxed);
        t.stick_throttle.store(sticks.centred ? clamp1(sticks.climb) : -2.0f, std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}
}
