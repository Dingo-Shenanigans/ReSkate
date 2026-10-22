#pragma once
#include "bmx_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

// Portable longitudinal/yaw BMX experiment. This is not a collision engine: the caller must
// supply verified support for BOTH wheels at the current bike geometry. ride_height is the
// supported bike root's world Y, not an unprocessed ray-hit height. There is no chassis sweep,
// slope orientation, tire slip, manual balance, dynamic-body response or native game access here.
namespace dingosdk::bmx {
inline constexpr double pi = 3.14159265358979323846;
inline constexpr double tau = 2.0 * pi;
inline constexpr double fixed_seconds = 1.0 / 120.0;
inline constexpr unsigned max_steps_per_advance = 12;

enum class QueryState { unavailable, clear, hit };
struct Support {
    QueryState query = QueryState::unavailable;
    double ride_height{};
};
struct Contacts { Support front, rear; };
struct Command {
    double steer{}; // [-1,1], positive rider-right; +Z is forward and +Y is up
    double pedal{}, brake{}; // [0,1]; pedaling drives forward even when rolling fakie
    bool hop{}; // edge/event; holding true cannot repeat hops
    double hop_strength = 1.0; // [0,1], scales takeoff speed; zero consumes the event without a jump
    bool input_enabled = true; // false cancels queued gestures; passive motion/gravity continue
    double air_spin{}; // [-1,1], positive rider-right; rotates facing, never airborne momentum
};
struct Tuning {
    double acceleration = 4.0;
    double braking = 8.0;
    double rolling_drag = 0.25;
    double air_drag = 0.025;
    double forward_limit = 15.0;
    double reverse_limit = 10.0;
    double gravity = 9.81;
    double hop_speed = 3.2;
    double wheelbase = geometry::wheelbase;
    double wheel_radius = geometry::wheel_radius;
    double slow_steer = 0.6; // radians; less handlebar travel at higher road speed
    double fast_steer = 0.16;
    double steer_speed_scale = 8.0;
    double steer_response = 12.0;
    double crank_rate = 2.0; // revolutions/second at full pedal input while supported
    double air_spin_rate = 8.0; // radians/second at full input; limited to 12
};
struct State {
    double x{}, y{}, z{}, yaw{}; // native heading: forward={sin(yaw),0,cos(yaw)}
    double speed{}, vertical_speed{}, steering_angle{}; // signed native rotation, positive rider-left
    double front_wheel_phase{}, rear_wheel_phase{}, crank_phase{}; // radians [0,2pi)
    bool grounded{}, suspended{};
    // Airborne momentum uses this facing bearing with SIGNED speed. A fakie departure
    // retains negative speed, so its velocity points opposite travel_yaw. Rebased on landing.
    double travel_yaw{};
};
struct AdvanceResult {
    unsigned steps{};
    bool hopped{}, suspended{};
    double dropped_seconds{}; // excess stall time is discarded; no later catch-up burst
};

namespace model_detail {
inline double bounded(double value, double low, double high, double fallback = 0.0) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
inline double phase(double value) {
    const double wrapped = std::fmod(value, tau);
    return wrapped < 0.0 ? wrapped + tau : wrapped;
}
inline double angle(double value) { return std::remainder(value, tau); }
inline Tuning sanitize(Tuning t) {
    const Tuning defaults;
    t.acceleration = bounded(t.acceleration, 0.0, 30.0, defaults.acceleration);
    t.braking = bounded(t.braking, 0.0, 50.0, defaults.braking);
    t.rolling_drag = bounded(t.rolling_drag, 0.0, 20.0, defaults.rolling_drag);
    t.air_drag = bounded(t.air_drag, 0.0, 5.0, defaults.air_drag);
    t.forward_limit = bounded(t.forward_limit, 0.1, 100.0, defaults.forward_limit);
    t.reverse_limit = bounded(t.reverse_limit, 0.1, 100.0, defaults.reverse_limit);
    t.gravity = bounded(t.gravity, 0.1, 50.0, defaults.gravity);
    t.hop_speed = bounded(t.hop_speed, 0.0, 15.0, defaults.hop_speed);
    t.wheelbase = bounded(t.wheelbase, 0.2, 3.0, defaults.wheelbase);
    t.wheel_radius = bounded(t.wheel_radius, 0.1, 1.0, defaults.wheel_radius);
    t.slow_steer = bounded(t.slow_steer, 0.0, 1.2, defaults.slow_steer);
    t.fast_steer = bounded(t.fast_steer, 0.0, t.slow_steer, std::min(defaults.fast_steer, t.slow_steer));
    t.steer_speed_scale = bounded(t.steer_speed_scale, 0.1, 100.0, defaults.steer_speed_scale);
    t.steer_response = bounded(t.steer_response, 0.1, 100.0, defaults.steer_response);
    t.crank_rate = bounded(t.crank_rate, 0.0, 10.0, defaults.crank_rate);
    t.air_spin_rate = bounded(t.air_spin_rate, 0.0, 12.0, defaults.air_spin_rate);
    return t;
}
inline bool finite(const State& s) {
    return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && std::isfinite(s.yaw) &&
        std::isfinite(s.speed) && std::isfinite(s.vertical_speed) && std::isfinite(s.steering_angle) &&
        std::isfinite(s.front_wheel_phase) && std::isfinite(s.rear_wheel_phase) && std::isfinite(s.crank_phase) &&
        std::isfinite(s.travel_yaw);
}
inline bool ready(const Support& support) {
    if (support.query == QueryState::clear) return true;
    return support.query == QueryState::hit && std::isfinite(support.ride_height) &&
        std::abs(support.ride_height) <= 1000000.0;
}
} // namespace model_detail

class Model {
  public:
    explicit Model(Tuning tuning = {}) : tuning_(model_detail::sanitize(tuning)) {}
    const State& state() const noexcept { return state_; }
    const Tuning& tuning() const noexcept { return tuning_; }
    void set_tuning(Tuning tuning) {
        tuning_ = model_detail::sanitize(tuning);
        constrain_state();
    }
    void reset(State initial = {}) {
        initial.travel_yaw = initial.yaw;
        state_ = model_detail::finite(initial) ? initial : State{};
        state_.grounded = false;
        state_.suspended = false;
        accumulator_ = 0.0;
        pending_hop_ = held_hop_ = false;
        pending_hop_strength_ = 1.0;
        constrain_state();
    }
    // Native contact resolution is authoritative. Reconcile after each observation rather
    // than letting a free-running target accumulate behind a wall or during a menu.
    bool reconcile_position(double x, double y, double z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            std::abs(x) > 1000000 || std::abs(y) > 1000000 || std::abs(z) > 1000000) return false;
        state_.x = x; state_.y = y; state_.z = z;
        return true;
    }
    void stop_horizontal() noexcept { state_.speed = 0.0; }

    // simulation_seconds is scaled GAME time supplied by the caller, not wall-clock time.
    // Zero/invalid time rebases the accumulator and never queues a hop. Unavailable contacts
    // freeze the complete model and discard accumulated time/events. The provider must refresh supports as the bike moves;
    // this constant-contact API is intended for small steps and deterministic fixtures.
    AdvanceResult advance(double simulation_seconds, Command command, const Contacts& contacts) {
        AdvanceResult result;
        if (!command.input_enabled) {
            pending_hop_ = held_hop_ = false;
            pending_hop_strength_ = 1.0;
            command.steer = command.pedal = command.brake = command.hop_strength = command.air_spin = 0.0;
            command.hop = false;
        }
        if (!std::isfinite(simulation_seconds) || simulation_seconds <= 0.0) {
            accumulator_ = 0.0;
            pending_hop_ = false;
            pending_hop_strength_ = 1.0;
            held_hop_ = command.hop;
            result.suspended = state_.suspended;
            return result;
        }
        if (!model_detail::ready(contacts.front) || !model_detail::ready(contacts.rear)) {
            accumulator_ = 0.0;
            pending_hop_ = false;
            pending_hop_strength_ = 1.0;
            held_hop_ = command.hop;
            state_.suspended = result.suspended = true;
            result.dropped_seconds = simulation_seconds;
            return result;
        }
        state_.suspended = false;
        command.steer = model_detail::bounded(command.steer, -1.0, 1.0);
        command.pedal = model_detail::bounded(command.pedal, 0.0, 1.0);
        command.brake = model_detail::bounded(command.brake, 0.0, 1.0);
        command.hop_strength = model_detail::bounded(command.hop_strength, 0.0, 1.0);
        command.air_spin = model_detail::bounded(command.air_spin, -1.0, 1.0);
        if (command.hop && !held_hop_ && !pending_hop_) pending_hop_strength_ = command.hop_strength;
        pending_hop_ = pending_hop_ || (command.hop && !held_hop_);
        held_hop_ = command.hop;
        const double accepted = std::min(simulation_seconds, fixed_seconds * max_steps_per_advance);
        result.dropped_seconds = simulation_seconds - accepted;
        accumulator_ += accepted;
        while (result.steps < max_steps_per_advance && accumulator_ + 1e-12 >= fixed_seconds) {
            result.hopped = step(command, contacts, pending_hop_, pending_hop_strength_) || result.hopped;
            pending_hop_ = false; // an unsupported hop is consumed, never saved for a future landing
            accumulator_ = std::max(0.0, accumulator_ - fixed_seconds);
            ++result.steps;
        }
        return result;
    }

  private:
    Tuning tuning_;
    State state_;
    double accumulator_{};
    bool pending_hop_{}, held_hop_{};
    double pending_hop_strength_ = 1.0;

    void constrain_state() {
        state_.x = model_detail::bounded(state_.x, -1000000.0, 1000000.0);
        state_.y = model_detail::bounded(state_.y, -1000000.0, 1000000.0);
        state_.z = model_detail::bounded(state_.z, -1000000.0, 1000000.0);
        state_.speed = model_detail::bounded(state_.speed, -tuning_.reverse_limit, tuning_.forward_limit);
        state_.vertical_speed = model_detail::bounded(state_.vertical_speed, -200.0, 200.0);
        state_.yaw = model_detail::angle(state_.yaw);
        state_.travel_yaw = model_detail::angle(state_.travel_yaw);
        state_.steering_angle = model_detail::bounded(state_.steering_angle, -tuning_.slow_steer, tuning_.slow_steer);
        state_.front_wheel_phase = model_detail::phase(state_.front_wheel_phase);
        state_.rear_wheel_phase = model_detail::phase(state_.rear_wheel_phase);
        state_.crank_phase = model_detail::phase(state_.crank_phase);
    }
    bool step(const Command& command, const Contacts& contacts, bool hop, double hop_strength) {
        const bool supported = contacts.front.query == QueryState::hit && contacts.rear.query == QueryState::hit;
        const double height = (contacts.front.ride_height + contacts.rear.ride_height) * 0.5;
        // A support cannot pull a rising bike back down after takeoff.
        const bool was_grounded = state_.grounded;
        state_.grounded = supported && state_.vertical_speed <= 0.0 && state_.y <= height + 0.001;
        if (state_.grounded) {
            if (!was_grounded) project_landing_speed();
            state_.y = height;
            state_.vertical_speed = 0.0;
        } else if (was_grounded) {
            // Walking off an edge has the same pinned momentum as a hop.
            state_.travel_yaw = state_.yaw;
        }
        const double steer_limit = tuning_.fast_steer + (tuning_.slow_steer - tuning_.fast_steer) /
            (1.0 + std::abs(state_.speed) / tuning_.steer_speed_scale);
        // Native +yaw rotates +Z toward +X. The camera behind a +Z-facing rider has
        // right=-X, so a rider-right handlebar command requires negative native yaw.
        const double target = -command.steer * steer_limit;
        state_.steering_angle += (target - state_.steering_angle) *
            (1.0 - std::exp(-tuning_.steer_response * fixed_seconds));
        if (state_.grounded) state_.speed += command.pedal * tuning_.acceleration * fixed_seconds;
        const double drag = (state_.grounded ? tuning_.rolling_drag + command.brake * tuning_.braking : 0.0) +
            tuning_.air_drag * state_.speed * state_.speed;
        state_.speed = std::copysign(std::max(0.0, std::abs(state_.speed) - drag * fixed_seconds), state_.speed);
        state_.speed = std::clamp(state_.speed, -tuning_.reverse_limit, tuning_.forward_limit);
        if (state_.grounded) {
            state_.yaw += state_.speed / tuning_.wheelbase * std::tan(state_.steering_angle) * fixed_seconds;
            state_.travel_yaw = state_.yaw;
            state_.crank_phase += command.pedal * tuning_.crank_rate * tau * fixed_seconds;
        }
        const bool hopped = hop && state_.grounded && tuning_.hop_speed > 0.0 && hop_strength > 0.0;
        if (hopped) {
            state_.vertical_speed = tuning_.hop_speed * hop_strength;
            state_.grounded = false;
        }
        if (!state_.grounded) state_.yaw -= command.air_spin * tuning_.air_spin_rate * fixed_seconds;
        const double movement_yaw = state_.grounded ? state_.yaw : state_.travel_yaw;
        state_.x += std::sin(movement_yaw) * state_.speed * fixed_seconds;
        state_.z += std::cos(movement_yaw) * state_.speed * fixed_seconds;
        if (!state_.grounded) {
            state_.vertical_speed -= tuning_.gravity * fixed_seconds;
            state_.y += state_.vertical_speed * fixed_seconds;
            if (supported && state_.vertical_speed <= 0.0 && state_.y <= height) {
                state_.y = height;
                state_.vertical_speed = 0.0;
                state_.grounded = true;
                project_landing_speed();
            }
        }
        // Visual rolling phases only: independent tire slip/freewheel dynamics are deferred.
        const double wheel_turn = state_.speed * fixed_seconds / tuning_.wheel_radius;
        state_.front_wheel_phase += wheel_turn;
        state_.rear_wheel_phase += wheel_turn;
        constrain_state();
        return hopped;
    }
    void project_landing_speed() {
        // Discard sideways momentum in this simple no-slip model. Facing 180 degrees
        // from takeoff retains the travel direction as negative (fakie) rolling speed.
        state_.speed *= std::cos(model_detail::angle(state_.yaw - state_.travel_yaw));
        state_.travel_yaw = state_.yaw;
    }
};
} // namespace dingosdk::bmx
