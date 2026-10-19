#pragma once
// USB radio-controller input for the FPV drone (RadioMaster, Jumper, FrSky... anything that
// shows up in Windows' "Set up USB game controllers" when its USB mode is Joystick).
//
// The pure maths (normalise / dead-zone / axis mapping) has no Windows dependency so it can be
// unit-tested anywhere. Only poll() and game_in_foreground() need Windows (winmm).
//
// Axis numbers 1..6 are winmm's X, Y, Z, R, U, V. `droneradiocheck` prints all six so you can see
// which one each stick moves; `droneaxes roll pitch throttle yaw` maps them (negative flips).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace dingosdk::drone::radio {

// Mode 2 on an EdgeTX radio sends AETR: channel 1 aileron (roll), 2 elevator (pitch),
// 3 throttle, 4 rudder (yaw). Those are the defaults below: roll=1, pitch=2, throttle=3, yaw=4.
struct Config {
    std::atomic<bool> enabled{true}; // with nothing plugged in, the drone flies with the keyboard
    std::atomic<int> map[4];   // roll, pitch, throttle, yaw -> signed axis number (1..6)
    std::atomic<int> device{-1}; // -1 = pick automatically
    Config() { for (int i = 0; i < 4; ++i) map[i] = i + 1; }
};
inline Config& config() { static Config value; return value; }
inline std::array<int, 4> mapping_snapshot() {
    auto& c = config();
    return {c.map[0].load(), c.map[1].load(), c.map[2].load(), c.map[3].load()};
}

// Sticks as the flight model wants them.
struct Command {
    bool valid = false;
    float roll = 0;      // + = right
    float pitch = 0;     // + = stick forward (nose down)
    float yaw = 0;       // + = right
    float throttle = 0;  // 0..1 (absolute; the throttle stick does not self-centre)
    bool centred = false; // the throttle stick springs back to the middle (a gamepad's)
    float climb = 0;     // that stick, -1..1 past its dead zone: what height hold reads
};

inline float normalize_axis(std::uint32_t value, std::uint32_t lo, std::uint32_t hi) {
    if (hi <= lo) return 0.0f;
    const float t = (static_cast<float>(value) - static_cast<float>(lo)) / (static_cast<float>(hi) - static_cast<float>(lo));
    return std::clamp(t * 2.0f - 1.0f, -1.0f, 1.0f);
}
inline float deadzone(float v, float dz) {
    const float a = std::abs(v);
    if (!(a > dz)) return 0.0f;
    return std::copysign(std::min(1.0f, (a - dz) / (1.0f - dz)), v);
}
inline float pick_axis(const std::array<float, 6>& axes, int signed_index) {
    const int index = std::abs(signed_index) - 1;
    if (index < 0 || index > 5) return 0.0f;
    return signed_index < 0 ? -axes[index] : axes[index];
}
// A throttle stick that springs back to the middle (a gamepad's): the middle holds the drone up,
// up climbs to full, down drops to nothing. `hover` is the throttle that holds altitude.
inline float spring_throttle(float stick, float hover) {
    const float h = std::clamp(std::isfinite(hover) ? hover : 0.35f, 0.05f, 0.95f);
    const float v = std::clamp(std::isfinite(stick) ? stick : 0.0f, -1.0f, 1.0f);
    return v >= 0.0f ? h + v * (1.0f - h) : h * (1.0f + v);
}
inline Command map_axes(const std::array<float, 6>& axes, const std::array<int, 4>& map, float dz = 0.03f,
                        bool centred_throttle = false, float hover = 0.35f) {
    Command out;
    out.valid = true;
    out.roll = deadzone(pick_axis(axes, map[0]), dz);
    out.pitch = deadzone(pick_axis(axes, map[1]), dz);
    out.centred = centred_throttle;
    if (centred_throttle) out.climb = deadzone(pick_axis(axes, map[2]), dz);
    out.throttle = centred_throttle ? spring_throttle(out.climb, hover)
                                    : std::clamp((pick_axis(axes, map[2]) + 1.0f) * 0.5f, 0.0f, 1.0f);
    out.yaw = deadzone(pick_axis(axes, map[3]), dz);
    return out;
}

struct Sample {
    bool valid = false;
    std::array<float, 6> axes{};
    std::uint32_t buttons = 0;   // bit n-1 set = button n pressed
    std::string name;
    int device = -1;
};

// Guided controller setup: works out which axis each stick is on, and which way round, by asking
// the pilot to move one stick at a time. Then, optionally, which switch arms and which resets.
//
// It starts from the sticks at rest (centred, throttle down). For each stick it picks the unused
// axis that moved furthest from rest, once it has been held well over for a moment, and takes the
// direction it moved as "plus" (throttle up, yaw right, pitch forward, roll right). The stick must
// come back to rest before the next one. Switches are any other axis that swings across, or a
// button that was not already down at rest. Plain maths: driven by the DRONE page's frames.
class Calibration {
public:
    enum class Step { idle, rest, throttle, yaw, pitch, roll, arm, reset, done };
    static constexpr float move_threshold = 0.6f;   // how far from rest counts as "pushed"
    static constexpr float switch_threshold = 0.8f; // a switch swings further than a stick drifts
    static constexpr float return_threshold = 0.25f;
    static constexpr float hold_seconds = 0.3f;

    Step step() const { return step_; }
    bool active() const { return step_ != Step::idle && step_ != Step::done; }
    // True after a stick or switch was found, until it goes back to rest.
    bool waiting_for_return() const { return returning_; }
    // The axis (0-5) the current step would pick if held, or -1.
    int candidate() const { return candidate_; }

    // Results, in the same form `droneaxes` and `dronetune` take.
    std::array<int, 4> mapping() const { return map_; } // roll, pitch, throttle, yaw: signed 1..6
    int arm_axis() const { return arm_axis_; }
    int arm_button() const { return arm_button_; }
    int reset_axis() const { return reset_axis_; }
    int reset_button() const { return reset_button_; }
    // The throttle stick rested in the middle, not at the bottom: it springs back (a gamepad).
    bool throttle_centred() const { return throttle_centred_; }

    void start() { *this = Calibration{}; step_ = Step::rest; }
    void cancel() { *this = Calibration{}; }
    // "Ready" on the rest step: the sticks as they are now are where they rest.
    void capture_rest(const std::array<float, 6>& axes, std::uint32_t buttons) {
        if (step_ != Step::rest) return;
        rest_ = axes;
        rest_buttons_ = buttons;
        step_ = Step::throttle;
    }
    // "Skip" on a switch step: no switch for it.
    void skip() {
        if (step_ == Step::arm) { step_ = Step::reset; clear_hold(); }
        else if (step_ == Step::reset) { step_ = Step::done; clear_hold(); }
    }
    // Once per frame while the setup is shown.
    void update(const std::array<float, 6>& axes, std::uint32_t buttons, float seconds) {
        if (!active() || step_ == Step::rest) return;
        if (!std::isfinite(seconds) || seconds < 0.0f) seconds = 0.0f;
        if (returning_) {
            if (back_at_rest(axes, buttons)) { returning_ = false; advance(); }
            return;
        }
        if (step_ == Step::arm || step_ == Step::reset) update_switch(axes, buttons, seconds);
        else update_stick(axes, seconds);
    }

private:
    Step step_ = Step::idle;
    std::array<float, 6> rest_{};
    std::uint32_t rest_buttons_ = 0;
    std::array<int, 4> map_{1, 2, 3, 4};
    std::array<bool, 6> used_{};
    std::uint32_t used_buttons_ = 0;
    int arm_axis_ = 0, arm_button_ = 0, reset_axis_ = 0, reset_button_ = 0;
    int candidate_ = -1, candidate_button_ = 0;
    float held_ = 0.0f;
    bool returning_ = false;
    bool throttle_centred_ = false;
    int found_axis_ = -1, found_button_ = 0;

    void clear_hold() { candidate_ = -1; candidate_button_ = 0; held_ = 0.0f; }
    static int slot(Step s) { return s == Step::roll ? 0 : s == Step::pitch ? 1 : s == Step::throttle ? 2 : 3; }
    int furthest(const std::array<float, 6>& axes, float threshold) const {
        int best = -1;
        float distance = threshold;
        for (int i = 0; i < 6; ++i) {
            if (used_[static_cast<std::size_t>(i)]) continue;
            const float d = std::abs(axes[static_cast<std::size_t>(i)] - rest_[static_cast<std::size_t>(i)]);
            if (std::isfinite(d) && d > distance) { distance = d; best = i; }
        }
        return best;
    }
    bool hold(int axis, int button, float seconds) {
        if (axis != candidate_ || button != candidate_button_) {
            candidate_ = axis;
            candidate_button_ = button;
            held_ = 0.0f;
            return false;
        }
        held_ += seconds;
        return (axis >= 0 || button > 0) && held_ >= hold_seconds;
    }
    void update_stick(const std::array<float, 6>& axes, float seconds) {
        const int axis = furthest(axes, move_threshold);
        if (!hold(axis, 0, seconds)) return;
        const auto i = static_cast<std::size_t>(axis);
        const int sign = axes[i] >= rest_[i] ? 1 : -1;
        map_[static_cast<std::size_t>(slot(step_))] = sign * (axis + 1);
        if (step_ == Step::throttle) throttle_centred_ = std::abs(rest_[i]) < 0.5f;
        used_[i] = true;
        found_axis_ = axis;
        found_button_ = 0;
        returning_ = true;
        clear_hold();
    }
    void update_switch(const std::array<float, 6>& axes, std::uint32_t buttons, float seconds) {
        const int axis = furthest(axes, switch_threshold);
        int button = 0;
        const std::uint32_t fresh = buttons & ~rest_buttons_ & ~used_buttons_;
        for (int n = 1; n <= 32 && !button; ++n)
            if (fresh & (1u << (n - 1))) button = n;
        if (axis >= 0) button = 0; // an axis is the clearer signal when both move
        if (!hold(axis, button, seconds)) return;
        if (axis >= 0) {
            const auto i = static_cast<std::size_t>(axis);
            const int value = (axes[i] >= rest_[i] ? 1 : -1) * (axis + 1); // its new side counts as "on"
            (step_ == Step::arm ? arm_axis_ : reset_axis_) = value;
            used_[i] = true;
        } else {
            (step_ == Step::arm ? arm_button_ : reset_button_) = button;
            used_buttons_ |= 1u << (button - 1);
        }
        found_axis_ = axis;
        found_button_ = button;
        // The arm switch can stay on; the reset switch has to be let go before finishing.
        returning_ = step_ == Step::reset;
        if (!returning_) advance();
        clear_hold();
    }
    bool back_at_rest(const std::array<float, 6>& axes, std::uint32_t buttons) const {
        if (found_axis_ >= 0) {
            const auto i = static_cast<std::size_t>(found_axis_);
            return std::abs(axes[i] - rest_[i]) < return_threshold;
        }
        return found_button_ <= 0 || (buttons & (1u << (found_button_ - 1))) == 0;
    }
    void advance() {
        switch (step_) {
        case Step::throttle: step_ = Step::yaw; break;
        case Step::yaw: step_ = Step::pitch; break;
        case Step::pitch: step_ = Step::roll; break;
        case Step::roll: step_ = Step::arm; break;
        case Step::arm: step_ = Step::reset; break;
        case Step::reset: step_ = Step::done; break;
        default: break;
        }
    }
};
// The one setup the DRONE page runs. Overlay thread only; the game thread reads calibrating().
inline Calibration& calibration() { static Calibration value; return value; }
// While the setup is on screen the game holds the drone still instead of flying it with the sticks.
// The DRONE page marks each frame it shows the setup; leaving the tab or closing the menu lets
// the drone fly again within half a second, even with the setup unfinished.
inline std::atomic<std::int64_t>& calibration_seen() { static std::atomic<std::int64_t> value{0}; return value; }
inline std::int64_t calibration_clock() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void mark_calibrating() { calibration_seen().store(calibration_clock(), std::memory_order_relaxed); }
inline bool calibrating() {
    const auto seen = calibration_seen().load(std::memory_order_relaxed);
    return seen != 0 && calibration_clock() - seen < 500;
}

#ifdef _WIN32
}
namespace dingosdk::overlay { // UI/Overlay/overlay.h: reads that see the controller while the game is kept off it
void begin_input_access() noexcept;
void end_input_access() noexcept;
}
namespace dingosdk::drone::radio {
namespace detail {
struct InputAccess {
    InputAccess() noexcept { overlay::begin_input_access(); }
    ~InputAccess() { overlay::end_input_access(); }
    InputAccess(const InputAccess&) = delete;
    InputAccess& operator=(const InputAccess&) = delete;
};
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

// Reads the radio. Safe to call from any thread; rescans for a controller at most once a second.
inline Sample poll() {
    // While the drone flies with a controller, ReSkate keeps the game from reading it; this read
    // has to see it all the same.
    detail::InputAccess access;
    static std::atomic<int> cached{-1};
    static std::atomic<ULONGLONG> next_scan{0};
    Sample sample;
    const int forced = config().device.load();
    if (forced >= 0) { detail::read_device(static_cast<UINT>(forced), sample); return sample; }
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

// True while this game's window has focus, so the radio does not fly the drone from the background.
inline bool game_in_foreground() {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
#endif
}
