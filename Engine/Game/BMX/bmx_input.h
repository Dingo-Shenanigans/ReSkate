#pragma once
#include "bmx_trick_catalog.h"
// Proposed BMX controls, independent of native game input and camera bindings.
// Sticks use +X right / +Y forward. Left = steer/air rotation; right = hop/manual/trick selection.
// Air tricks are requests, not evidence that their animation or landing completed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace dingosdk::bmx {
struct RawInput {
    double left_x{}, left_y{}, right_x{}, right_y{}, pedal{}, brake{};
    bool available{}, focused{true}, menu_open{};
    // Change on pad/source replacement, even when the replacement uses the same slot.
    std::uint32_t device{};
    bool quick_hop{}; // R3; a grounded press requests one full-strength hop
    double left_trigger{}, right_trigger{};
    bool left_shoulder{}, right_shoulder{};
};
struct InputContext { bool grounded{true}; };
enum class TrickDirection { none, left, right };
struct Actions {
    bool active{}, hop_pressed{};
    double steer{}, front_weight{}, rear_weight{}, pedal{}, brake{};
    double preload{}, hop_strength{}, manual{}, nose_manual{};
    TrickDirection handlebar{TrickDirection::none}, tailwhip{TrickDirection::none};
    double air_pitch{}; // Streets' left-stick Flip axis: +nose down / -front up; neutral after takeoff
    catalog::TrickId movement{catalog::TrickId::none}; // one authored slot event per neutral-rearmed gesture
};
struct InputSettings {
    // Thresholds below apply after radial deadzone/remapping to a unit disc.
    // The local .12 deadzone and stricter neutral rearm are adaptations; .7 is
    // Streets' recovered selector radius, not a proven hardware-stick deadzone.
    double deadzone{0.12}, neutral_threshold{0.18}, trick_threshold{0.7};
    double hop_back_threshold{0.72}, hop_forward_threshold{0.55}, hop_lateral_limit{0.35};
    double hop_flick_seconds{0.30}, hop_charge_seconds{0.25}, manual_threshold{0.22};
    double trigger_neutral{0.05}, max_sample_seconds{0.10};
    double modifier_threshold{0.2}; // recovered raw-Gamepad fallback; configured Streets actions may differ
};

class InputMapper {
    struct Stick {
        double x{}, y{}, magnitude{};
    };
    InputSettings settings_;
    bool gated_{true}, device_known_{}, context_known_{}, grounded_{true}, rear_armed_{}, air_pitch_armed_{}, trick_armed_{};
    std::uint32_t device_{};
    bool hop_pending_{};
    bool quick_hop_held_{};
    double charge_seconds_{}, release_seconds_{};

    static double finite_or(double value, double fallback) noexcept {
        return std::isfinite(value) ? value : fallback;
    }
    static InputSettings sanitize(InputSettings s) noexcept {
        const InputSettings d;
        const auto bounded = [](double v, double fallback, double lo, double hi) {
            return std::clamp(finite_or(v, fallback), lo, hi);
        };
        s.deadzone = bounded(s.deadzone, d.deadzone, 0, 0.45);
        s.neutral_threshold = bounded(s.neutral_threshold, d.neutral_threshold, 0.05, 0.40);
        s.trick_threshold = bounded(s.trick_threshold, d.trick_threshold,
            std::max(0.55, s.neutral_threshold + 0.10), 0.95);
        s.hop_back_threshold = bounded(s.hop_back_threshold, d.hop_back_threshold, 0.55, 0.95);
        s.hop_forward_threshold = bounded(s.hop_forward_threshold, d.hop_forward_threshold, 0.40, 0.95);
        s.hop_lateral_limit = bounded(s.hop_lateral_limit, d.hop_lateral_limit, 0.10, 0.50);
        s.hop_flick_seconds = bounded(s.hop_flick_seconds, d.hop_flick_seconds, 0.04, 0.75);
        s.hop_charge_seconds = bounded(s.hop_charge_seconds, d.hop_charge_seconds, 0.02, 1.00);
        s.manual_threshold = bounded(s.manual_threshold, d.manual_threshold, 0.05, s.hop_back_threshold - 0.10);
        s.trigger_neutral = bounded(s.trigger_neutral, d.trigger_neutral, 0.01, 0.20);
        s.modifier_threshold = bounded(s.modifier_threshold, d.modifier_threshold,
            std::max(0.1, s.trigger_neutral + 0.05), 0.95);
        s.max_sample_seconds = bounded(s.max_sample_seconds, d.max_sample_seconds, 0.01, 0.25);
        return s;
    }
    Stick normalize(double x, double y) const noexcept {
        x = std::clamp(x, -1.0, 1.0); y = std::clamp(y, -1.0, 1.0);
        const double length = std::hypot(x, y);
        if (length <= settings_.deadzone) return {};
        const double magnitude = (std::min(length, 1.0) - settings_.deadzone) / (1 - settings_.deadzone);
        return {x / length * magnitude, y / length * magnitude, magnitude};
    }
    void clear_gestures() noexcept {
        rear_armed_ = air_pitch_armed_ = trick_armed_ = false;
        hop_pending_ = false; charge_seconds_ = release_seconds_ = 0;
    }
    void update_ground(Stick rear, double seconds, Actions& out) noexcept {
        if (rear.magnitude <= settings_.neutral_threshold && !hop_pending_) rear_armed_ = true;
        const bool aligned = std::abs(rear.x) <= settings_.hop_lateral_limit;
        const bool back = aligned && rear.y <= -settings_.hop_back_threshold;
        if (!hop_pending_ && rear_armed_ && back) {
            hop_pending_ = true; rear_armed_ = false;
            charge_seconds_ = release_seconds_ = 0;
        }
        if (hop_pending_) {
            if (!aligned) { hop_pending_ = false; return; }
            if (back) {
                charge_seconds_ = std::min(charge_seconds_ + seconds, settings_.hop_charge_seconds);
                release_seconds_ = 0;
            } else release_seconds_ += seconds;
            if (release_seconds_ > settings_.hop_flick_seconds) {
                hop_pending_ = false;
                return;
            }
            out.preload = charge_seconds_ / settings_.hop_charge_seconds;
            if (rear.y >= settings_.hop_forward_threshold) {
                out.hop_pressed = true;
                out.hop_strength = std::clamp(out.preload, 0.25, 1.0);
                out.preload = 0; hop_pending_ = false;
            }
            return;
        }
        // Partial rear-stick travel balances; a full back deflection belongs to the
        // hop gesture. Merely holding a partial stick through takeoff/landing cannot
        // enter a fresh balance state until it has returned to neutral.
        const double amount = std::abs(rear.y);
        if (rear_armed_ && aligned && amount >= settings_.manual_threshold && amount < settings_.hop_back_threshold) {
            const double balance = (amount - settings_.manual_threshold) /
                (settings_.hop_back_threshold - settings_.manual_threshold);
            if (rear.y < 0) out.manual = balance;
            else out.nose_manual = balance;
        }
    }


    void update_air_pitch(Stick front, Actions& out) noexcept {
        // Streets binds Flip to left Y; right Y remains preload/manual/trick
        // input. Require an airborne neutral before deliberate flip torque.
        if (front.magnitude <= settings_.neutral_threshold) {
            air_pitch_armed_ = true;
            return;
        }
        // The front stick no longer owns a trick gesture: diagonal yaw + Flip is
        // deliberate after an observed airborne neutral and must remain possible.
        if (!air_pitch_armed_) return;
        const double amount = std::abs(front.y);
        if (amount > settings_.manual_threshold)
            out.air_pitch = std::copysign(std::clamp((amount-settings_.manual_threshold) /
                (1-settings_.manual_threshold), 0.0, 1.0), front.y);
    }

    bool modifiers_neutral(const RawInput& raw) const noexcept {
        return !raw.left_shoulder && !raw.right_shoulder &&
            raw.left_trigger <= settings_.trigger_neutral && raw.right_trigger <= settings_.trigger_neutral;
    }
    void update_air_trick(Stick rear, const RawInput& raw, Actions& out) noexcept {
        if (rear.magnitude <= settings_.neutral_threshold) {
            trick_armed_ = true;
            return;
        }
        if (!trick_armed_ || rear.magnitude < settings_.trick_threshold) return;
        // One selection per rear-stick release; held modifiers may select repeated
        // tricks. Equip/reset/source gates separately require ALL controls neutral.
        // Unsupported slots consume the attempt too: changing modifiers while the
        // stick stays deflected cannot become a late trick.
        trick_armed_ = false;
        const bool lt = raw.left_trigger > settings_.modifier_threshold;
        const bool rt = raw.right_trigger > settings_.modifier_threshold;
        const bool lb = raw.left_shoulder, rb = raw.right_shoulder;
        // Preserve native priority, including unsupported higher-priority sets.
        const int set = lb && rt ? 0 : rb && lt ? 1 : lb && rb ? 2 : lb ? 3 : rb ? 4 :
            lt && rt ? 5 : lt ? 6 : rt ? 7 : -1;
        double degrees = std::atan2(rear.x, rear.y) * 180 / std::numbers::pi;
        if (degrees < 0) degrees += 360;
        const int slot = degrees < 30 || degrees > 330 ? 0 :
            degrees > 150 && degrees < 210 ? 4 : static_cast<int>(std::floor((degrees + 22.5) / 45)) % 8;
        // All nonempty authored slots retain their own identity. The four existing
        // rotation outputs remain compatible with the established native adapter.
        out.movement=catalog::select(set,slot);
        if (set == 3 && slot == 4) out.handlebar = TrickDirection::left;
        else if (set == 4 && slot == 4) out.handlebar = TrickDirection::right;
        else if (set == 5 && slot == 2) out.tailwhip = TrickDirection::right;
        else if (set == 5 && slot == 6) out.tailwhip = TrickDirection::left;
    }

public:
    explicit InputMapper(InputSettings settings = {}) noexcept : settings_(sanitize(settings)) {}
    const InputSettings& settings() const noexcept { return settings_; }
    // Equip, respawn, mode changes and external ownership changes call reset. No held
    // pedal, brake or stick is accepted again until every control is released.
    void reset() noexcept {
        gated_ = true; device_known_ = context_known_ = false;
        quick_hop_held_ = false;
        clear_gestures();
    }
    void configure(InputSettings settings) noexcept { settings_ = sanitize(settings); reset(); }

    Actions update(const RawInput& raw, InputContext context, double simulation_seconds) noexcept {
        if (!raw.available || !raw.focused || raw.menu_open ||
            !std::isfinite(raw.left_x) || !std::isfinite(raw.left_y) ||
            !std::isfinite(raw.right_x) || !std::isfinite(raw.right_y) ||
            !std::isfinite(raw.pedal) || !std::isfinite(raw.brake) ||
            !std::isfinite(raw.left_trigger) || !std::isfinite(raw.right_trigger) ||
            !std::isfinite(simulation_seconds) || simulation_seconds <= 0 ||
            simulation_seconds > settings_.max_sample_seconds) {
            reset(); return {};
        }
        if (!device_known_ || device_ != raw.device) {
            reset(); device_known_ = true; device_ = raw.device;
        }
        const auto front = normalize(raw.left_x, raw.left_y);
        const auto rear = normalize(raw.right_x, raw.right_y);
        const double raw_pedal = std::clamp(raw.pedal, 0.0, 1.0), raw_brake = std::clamp(raw.brake, 0.0, 1.0);
        if (gated_) {
            if (front.magnitude > settings_.neutral_threshold || rear.magnitude > settings_.neutral_threshold ||
                raw_pedal > settings_.trigger_neutral || raw_brake > settings_.trigger_neutral ||
                raw.quick_hop || !modifiers_neutral(raw)) return {};
            gated_ = false;
        }
        if (!context_known_ || grounded_ != context.grounded) {
            clear_gestures(); context_known_ = true; grounded_ = context.grounded;
        }
        Actions out;
        // Preserve the held bit through takeoff/landing: a held R3 cannot become a
        // fresh press when the context changes or provide another airborne impulse.
        const bool quick_hop_pressed = raw.quick_hop && !quick_hop_held_;
        quick_hop_held_ = raw.quick_hop;
        out.active = true;
        out.steer = front.x; out.front_weight = front.y; out.rear_weight = rear.y;
        const auto trigger = [&](double value) {
            return value <= settings_.trigger_neutral ? 0.0 :
                (value - settings_.trigger_neutral) / (1 - settings_.trigger_neutral);
        };
        out.pedal = context.grounded ? trigger(raw_pedal) : 0;
        out.brake = trigger(raw_brake);
        if (context.grounded) {
            update_ground(rear, simulation_seconds, out);
            if (quick_hop_pressed) {
                out.hop_pressed = true; out.hop_strength = 1; out.preload = 0;
                hop_pending_ = false; rear_armed_ = false;
                charge_seconds_ = release_seconds_ = 0;
            }
        }
        else {
            update_air_trick(rear, raw, out);
            update_air_pitch(front, out);
        }
        return out;
    }
};
} // namespace dingosdk::bmx
