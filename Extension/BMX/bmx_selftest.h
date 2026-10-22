#pragma once
#include "Engine/Game/BMX/bmx_input.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx::selftest {
enum class Mode { focused, unattended };
inline constexpr std::uint64_t wall_limit_ms = 120000; // finite dev lease; background native simulation may run slowly
inline constexpr double simulation_duration = 9.0;
inline constexpr std::uint32_t synthetic_device = 0x424d5801;
inline constexpr double minimum_render_poses_per_second = 15.0;
inline constexpr std::uint64_t render_fresh_ms = 250;

// Native +Z forward uses positive yaw toward camera-left. The positive/right
// stick phase must therefore produce a negative signed native heading change.
// Observe through the following half-second of braking to include native latency.
struct RightTurn {
    bool started{}, measured{};
    double start_yaw{}, delta{};
    void observe(double seconds, double native_yaw) noexcept {
        if (!std::isfinite(seconds) || !std::isfinite(native_yaw) || seconds < 4 || seconds >= 5.5) return;
        if (!started) { started = true; start_yaw = native_yaw; return; }
        delta = std::remainder(native_yaw - start_yaw, 6.2831853071795864769);
        measured = true;
    }
    bool passed() const noexcept { return measured && std::isfinite(delta) && delta < -.01; }
};

// Physical rehearsals must observe the actual admitted chassis quaternion,
// independently of the preserved native rider basis. Heading is unavailable
// for nonunit, inverted or near-vertical bikes; no fallback yaw is invented.
inline bool observed_bike_heading(const std::array<float,4>& q,double& yaw) noexcept {
    double norm=0;for(float x:q){if(!std::isfinite(x))return false;norm+=double(x)*x;}
    if(std::abs(norm-1)>.001)return false;
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    const double forward_x=2*(x*z+y*w),forward_z=1-2*(x*x+y*y);
    const double up_y=1-2*(x*x+z*z);
    if(up_y<.5||std::hypot(forward_x,forward_z)<.5)return false;
    yaw=std::atan2(forward_x,forward_z);return std::isfinite(yaw);
}
struct ObservedBikeTurn {
    RightTurn right_turn;
    bool initialized{},window_qualified{true};
    double initial_yaw{},maximum_turn{},last_seconds{};
    std::uint64_t last_serial{},accepted{},rejected{};
    void observe(double seconds,std::uint64_t serial,const std::array<float,4>& actual_q) noexcept {
        const bool in_window=std::isfinite(seconds)&&seconds>=4&&seconds<5.5;
        double yaw{};
        const bool successor=!last_serial||(serial==last_serial+1&&seconds>last_seconds);
        if(!std::isfinite(seconds)||seconds<0||!serial||!successor||!observed_bike_heading(actual_q,yaw)) {
            ++rejected;if(in_window||!std::isfinite(seconds))window_qualified=false;
            // Track observation identity even when orientation is unavailable;
            // the next valid sample cannot bridge a missing turn observation.
            if(serial>last_serial&&std::isfinite(seconds)&&seconds>=0){last_serial=serial;last_seconds=seconds;}
            return;
        }
        last_serial=serial;last_seconds=seconds;++accepted;
        if(!initialized){initialized=true;initial_yaw=yaw;}
        maximum_turn=std::max(maximum_turn,std::abs(std::remainder(yaw-initial_yaw,6.2831853071795864769)));
        right_turn.observe(seconds,yaw);
    }
    bool passed() const noexcept {return initialized&&window_qualified&&maximum_turn>.01&&right_turn.passed();}
};

struct RenderEvidence {
    double simulation_seconds{};
    std::uint64_t baseline_count{}, total_count{}, snapshot_count{};
    std::uintptr_t expected_entity{}, snapshot_entity{};
    std::uint64_t now_ms{}, observed_ms{};
    float hand_error{}, foot_error{};
    bool ready{};
};
struct RenderAssessment {
    std::uint64_t minimum_poses{};
    bool density{}, fresh{}, current_owner{}, coherent_count{}, contacts{};
    bool passed() const noexcept { return density && fresh && current_owner && coherent_count && contacts; }
};
// Counts are successful native pose writes, which can occur twice per frame.
// This deliberately low density floor rejects a handful of early successes;
// it is not an FPS measurement or evidence that pixels were displayed correctly.
inline RenderAssessment assess_render(const RenderEvidence& e) noexcept {
    RenderAssessment result;
    if (std::isfinite(e.simulation_seconds) && e.simulation_seconds >= simulation_duration &&
        e.simulation_seconds <= static_cast<double>(wall_limit_ms) / 1000.0) {
        result.minimum_poses = static_cast<std::uint64_t>(std::ceil(e.simulation_seconds * minimum_render_poses_per_second));
    }
    result.coherent_count = e.ready && e.snapshot_count > e.baseline_count && e.snapshot_count <= e.total_count;
    result.density = result.minimum_poses != 0 && result.coherent_count &&
        e.snapshot_count - e.baseline_count >= result.minimum_poses;
    result.fresh = e.ready && e.observed_ms != 0 && e.now_ms >= e.observed_ms && e.now_ms - e.observed_ms <= render_fresh_ms;
    result.current_owner = e.ready && e.expected_entity != 0 && e.snapshot_entity == e.expected_entity;
    result.contacts = e.ready && std::isfinite(e.hand_error) && std::isfinite(e.foot_error) &&
        e.hand_error >= 0 && e.foot_error >= 0 && e.hand_error < .18f && e.foot_error < .18f;
    return result;
}

struct Readiness {
    bool native_ui_available{}, native_menu{}, overlay_ready{}, focused{};
    bool overlay_menu{}, other_capture{}, menu_requested{}, input_available{};
};

// Only an explicitly requested, isolated rehearsal may replace physical focus/device
// availability. Native UI, overlay menus and other capture owners remain authoritative.
inline bool input_allowed(Mode mode, bool isolated, Readiness gates) noexcept {
    if (!gates.native_ui_available || gates.native_menu || !gates.overlay_ready ||
        gates.overlay_menu || gates.other_capture || gates.menu_requested) return false;
    if (mode == Mode::unattended) return isolated;
    return gates.focused && gates.input_available;
}

// A stopped native clock cannot extend the lifetime of the synthetic input lease.
// Clock reversal is also an expiry; no subtraction underflow can renew the request.
inline bool expired(std::uint64_t started_ms, std::uint64_t now_ms) noexcept {
    return now_ms < started_ms || now_ms - started_ms >= wall_limit_ms;
}

// This sequence supplies game-side BMX commands, never OS input or simulation time.
// Its first second is neutral so the ordinary mapper can establish and rearm a source.
inline RawInput controls(double simulation_seconds) noexcept {
    RawInput raw;
    raw.available = raw.focused = true;
    raw.device = synthetic_device;
    if (!std::isfinite(simulation_seconds) || simulation_seconds < 0 ||
        simulation_seconds >= simulation_duration) return raw;
    if (simulation_seconds >= 1 && simulation_seconds < 4) raw.pedal = .6;
    if (simulation_seconds >= 4 && simulation_seconds < 5) { raw.pedal = .5; raw.left_x = .25; }
    if (simulation_seconds >= 5 && simulation_seconds < 6) raw.brake = 1;
    if (simulation_seconds >= 6 && simulation_seconds < 6.35) raw.right_y = -.95;
    if (simulation_seconds >= 6.35 && simulation_seconds < 6.45) raw.right_y = .95;
    if (simulation_seconds >= 7.8) raw.brake = 1;
    return raw;
}
// Explicit physical-chassis trick rehearsal. Neutral in the air precedes each
// gesture; the short pitch pulse is distinct from the grounded hop release.
inline RawInput chassis_trick_controls(double seconds) noexcept {
    auto raw = controls(seconds);
    if (seconds >= 6.50 && seconds < 6.54) {raw.left_shoulder=true;raw.right_y=-.95;}
    if (seconds >= 6.57 && seconds < 6.61) {raw.left_trigger=raw.right_trigger=1;raw.right_x=.95;}
    if (seconds >= 6.65 && seconds < 6.69) raw.left_y = .55;
    return raw;
}
// A separate opted-in generic catalog rehearsal. Ordinary hop timing and the
// legacy barspin/whip script are unchanged. This gesture is the authored slot7:
// LB + RT + rear up-left. Neutral follows before landing, so recovery/catch
// must complete through the ordinary mapper/controller rather than a shortcut.
inline RawInput catalog_controls(double seconds) noexcept {
    auto raw = controls(seconds);
    if (seconds >= 6.50 && seconds < 6.75) {
        raw.left_shoulder = true;
        raw.right_trigger = 1;
        raw.right_x = -.95;
        raw.right_y = .95;
    }
    return raw;
}
} // namespace dingosdk::bmx::selftest
