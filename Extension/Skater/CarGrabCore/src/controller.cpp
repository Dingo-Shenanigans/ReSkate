#include "car_grab/controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace car_grab {
namespace {
bool finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool valid(const Config& c) noexcept {
    const float values[]{c.acquisition_distance, c.rear_offset, c.steering_offset,
        c.min_vehicle_speed, c.max_vehicle_speed, c.max_acceleration,
        c.max_catchup_speed, c.max_skater_speed, c.position_gain,
        c.velocity_response, c.max_separation, c.max_sample_age, c.max_frame_seconds, c.grip_standoff,
        c.grip_position_gain, c.grip_velocity_response, c.grip_steering_speed};
    for (float value : values) if (!std::isfinite(value)) return false;
    return c.acquisition_distance > 0 && c.rear_offset >= 0 && c.steering_offset >= 0 &&
        c.min_vehicle_speed >= 0 && c.max_vehicle_speed > c.min_vehicle_speed &&
        c.max_acceleration > 0 && c.max_catchup_speed >= 0 && c.max_skater_speed > 0 &&
        c.position_gain > 0 && c.velocity_response > 0 &&
        c.max_separation >= c.acquisition_distance && c.max_sample_age > 0 && c.max_frame_seconds > 0 &&
        c.grip_standoff > 0 && c.grip_standoff <= 5 && c.grip_position_gain > 0 && c.grip_velocity_response > 0 &&
        c.grip_steering_speed > 0;
}
double dot(Vec3 a, Vec3 b) noexcept {
    return static_cast<double>(a.x)*b.x + static_cast<double>(a.y)*b.y + static_cast<double>(a.z)*b.z;
}
double length(Vec3 v) noexcept { return std::hypot(static_cast<double>(v.x), v.y, v.z); }
Vec3 subtract(Vec3 a, Vec3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
bool bounded(Vec3 v) noexcept {
    return finite(v) && std::abs(static_cast<double>(v.x)) <= 100000 &&
        std::abs(static_cast<double>(v.y)) <= 100000 && std::abs(static_cast<double>(v.z)) <= 100000;
}
bool normalize(Vec3& v) noexcept {
    if (!finite(v)) return false;
    const auto magnitude = length(v);
    if (magnitude < 1e-6) return false;
    v = {static_cast<float>(v.x/magnitude), static_cast<float>(v.y/magnitude), static_cast<float>(v.z/magnitude)};
    return finite(v);
}
bool valid_wrap(VehicleGrip& grip) noexcept {
    if (!grip.wrap_valid) return true;
    if (!bounded(grip.top_point) || !bounded(grip.bottom_point) ||
        !normalize(grip.top_normal) || !normalize(grip.bottom_normal)) return false;
    const auto thickness = dot(subtract(grip.top_point, grip.bottom_point), grip.top_normal);
    return grip.top_normal.y >= .85f && grip.bottom_normal.y <= -.85f &&
        dot(grip.top_normal, grip.bottom_normal) <= -.8 && thickness >= .006-1e-6 && thickness <= .06+1e-6 &&
        grip.top_point.y >= grip.point.y && grip.point.y >= grip.bottom_point.y &&
        length(subtract(grip.top_point, grip.point)) <= .08+1e-6 &&
        length(subtract(grip.bottom_point, grip.point)) <= .08+1e-6 &&
        dot(subtract(grip.point, grip.top_point), grip.top_normal) <= .002+1e-6 &&
        dot(subtract(grip.point, grip.bottom_point), grip.bottom_normal) <= .002+1e-6;
}
bool current_reach(const SkaterGripReach& reach, const Frame& frame, const Config& config) noexcept {
    return reach.valid && bounded(reach.shoulder_offset_world) &&
        std::isfinite(reach.min_reach) && std::isfinite(reach.max_reach) &&
        reach.min_reach >= 0 && reach.max_reach > reach.min_reach && reach.max_reach <= 5 &&
        std::isfinite(reach.sampled_at) && reach.sampled_at >= 0 && reach.sampled_at <= frame.now &&
        frame.now-reach.sampled_at <= config.max_sample_age;
}
// Double intermediates avoid float overflow on otherwise finite boundary inputs.
struct Horizontal { double x{}, z{}; };
Horizontal limit(Horizontal v, double maximum) noexcept {
    const auto size = std::hypot(v.x, v.z);
    if (size > maximum && size > 0) {
        const auto factor = maximum / size;
        v.x *= factor; v.z *= factor;
    }
    return v;
}
struct Anchor {
    double x{}, y{}, z{}, distance{};
    double steering_lateral{}, steering_limit{}, tangent_x{}, tangent_z{};
    double turn_offset_x{}, turn_offset_z{};
    Vec3 velocity{};
    VehicleGrip grip{};
    bool approaching{}, contact_eligible{};
};
DetachReason evaluate(const Vehicle& vehicle, const Frame& frame,
                      const Config& config, Anchor& anchor,
                      bool active = false, bool steering = false,
                      TowMode mode = TowMode::none) noexcept {
    if (!vehicle.id || !vehicle.generation || !finite(vehicle.position) ||
        !finite(vehicle.forward) || !finite(vehicle.velocity) ||
        !std::isfinite(vehicle.sampled_at) || vehicle.sampled_at < 0 || vehicle.sampled_at > frame.now)
        return DetachReason::invalid_input;
    const auto age = frame.now - vehicle.sampled_at;
    if (age > config.max_sample_age) return DetachReason::stale_vehicle;
    const auto speed = std::hypot(static_cast<double>(vehicle.velocity.x), vehicle.velocity.z);
    if ((!active && speed < config.min_vehicle_speed) || speed > config.max_vehicle_speed)
        return DetachReason::ineligible;
    auto steer = steering ? std::clamp(frame.input.steer, -1.f, 1.f) * static_cast<double>(config.steering_offset) : 0.0;
    const auto reject_grip = [&](DetachReason reason) {
        if (config.require_grip || mode == TowMode::measured_surface) return reason;
        // Optional failed geometry cannot invalidate the independently checked
        // traffic pose. A kinematic retry cannot recurse into grip evaluation.
        auto kinematic = vehicle;
        kinematic.grip = {};
        anchor = {};
        return evaluate(kinematic, frame, config, anchor, active, steering, TowMode::kinematic);
    };
    if (vehicle.grip.valid && mode != TowMode::kinematic) {
        auto grip = vehicle.grip;
        if (!grip.surface_id || !bounded(grip.point) || !finite(grip.velocity) ||
            length(grip.velocity) > 45 || // Same bound as the paired contact-motion sampler.
            !normalize(grip.normal) || !normalize(grip.tangent) || std::abs(dot(grip.normal, grip.tangent)) > .05 ||
            !std::isfinite(grip.sampled_at) || grip.sampled_at < 0 || grip.sampled_at > frame.now || !valid_wrap(grip))
            return reject_grip(DetachReason::invalid_input);
        const auto grip_age = frame.now-grip.sampled_at;
        if (grip_age > config.max_sample_age) return reject_grip(DetachReason::stale_vehicle);
        const auto predict = [&](Vec3& point) {
            point = {static_cast<float>(point.x + grip.velocity.x*grip_age),
                static_cast<float>(point.y + grip.velocity.y*grip_age),
                static_cast<float>(point.z + grip.velocity.z*grip_age)};
            return bounded(point);
        };
        if (!predict(grip.point) || (grip.wrap_valid && (!predict(grip.top_point) || !predict(grip.bottom_point))))
            return reject_grip(DetachReason::invalid_input);
        const auto normal_horizontal = std::hypot(static_cast<double>(grip.normal.x), grip.normal.z);
        const auto tangent_horizontal = std::hypot(static_cast<double>(grip.tangent.x), grip.tangent.z);
        if (normal_horizontal < 1e-6 || tangent_horizontal < 1e-6) return reject_grip(DetachReason::invalid_input);
        const auto normal_x = grip.normal.x/normal_horizontal, normal_z = grip.normal.z/normal_horizontal;
        // Project to the native ground plane without demanding vertical motion.
        auto tangent_x = normal_z, tangent_z = -normal_x;
        if (tangent_x*grip.tangent.x + tangent_z*grip.tangent.z < 0) { tangent_x = -tangent_x; tangent_z = -tangent_z; }
        auto stand_off = static_cast<double>(config.grip_standoff);
        Vec3 shoulder_offset{};
        if (current_reach(frame.skater.grip_reach, frame, config)) {
            const auto& reach = frame.skater.grip_reach;
            shoulder_offset = reach.shoulder_offset_world;
            const auto dy = grip.point.y - (static_cast<double>(frame.skater.position.y)+shoulder_offset.y);
            // The anatomical reach is a hard limit, not a towing setpoint.
            // Reserve room for wrist/palm offset and finite response to drag
            // and rotating surfaces; lateral steering shares that same budget.
            const auto stance_reach = std::max(static_cast<double>(reach.min_reach), .70*reach.max_reach);
            const auto remaining = stance_reach*stance_reach - dy*dy;
            if (remaining > 0) {
                const auto maximum = std::sqrt(remaining);
                const auto minimum = std::sqrt(std::max(0.0, static_cast<double>(reach.min_reach)*reach.min_reach-dy*dy));
                stand_off = std::clamp(stand_off, minimum, std::max(minimum, .8*maximum));
                const auto lateral = std::sqrt(std::max(0.0, remaining-stand_off*stand_off));
                steer = std::clamp(steer, -lateral, lateral);
                anchor.steering_limit = lateral;
            } else steer = 0;
            const Vec3 shoulder{frame.skater.position.x+shoulder_offset.x,
                frame.skater.position.y+shoulder_offset.y, frame.skater.position.z+shoulder_offset.z};
            const auto reach_distance = length(subtract(grip.point, shoulder));
            anchor.contact_eligible = reach_distance >= reach.min_reach && reach_distance <= reach.max_reach;
        } else {
            // Without evaluated rig proportions only a bounded approach is
            // authorized. Actual palm contact remains a native-pose decision.
            steer = 0;
        }
        anchor.x = grip.point.x + normal_x*stand_off - shoulder_offset.x + tangent_x*steer;
        anchor.y = grip.point.y - shoulder_offset.y;
        anchor.z = grip.point.z + normal_z*stand_off - shoulder_offset.z + tangent_z*steer;
        anchor.steering_lateral = steer; anchor.tangent_x = tangent_x; anchor.tangent_z = tangent_z;
        // Contact velocity already includes rotation of the measured car point.
        // Only its car-local stance offset needs an additional lever arm.
        anchor.turn_offset_x = normal_x*stand_off + tangent_x*steer;
        anchor.turn_offset_z = normal_z*stand_off + tangent_z*steer;
        anchor.grip = grip; anchor.velocity = grip.velocity;
        anchor.approaching = !anchor.contact_eligible;
    } else {
        if (config.require_grip || mode == TowMode::measured_surface) return DetachReason::ineligible;
        const auto heading_length = std::hypot(static_cast<double>(vehicle.forward.x), vehicle.forward.z);
        if (heading_length < 1e-6) return DetachReason::invalid_input;
        const auto forward_x = vehicle.forward.x/heading_length, forward_z = vehicle.forward.z/heading_length;
        // Rear acquisition is a directional rider policy, not an inferred car
        // hull. A wide configured radius must not authorize a front/side grab.
        const auto rear_projection = (vehicle.position.x + vehicle.velocity.x*age-frame.skater.position.x)*forward_x +
            (vehicle.position.z + vehicle.velocity.z*age-frame.skater.position.z)*forward_z;
        if (!active && rear_projection <= 0) return DetachReason::ineligible;
        anchor.x = vehicle.position.x + vehicle.velocity.x*age - forward_x*config.rear_offset + forward_z*steer;
        anchor.y = vehicle.position.y + vehicle.velocity.y*age;
        anchor.z = vehicle.position.z + vehicle.velocity.z*age - forward_z*config.rear_offset - forward_x*steer;
        anchor.velocity = vehicle.velocity;
        anchor.steering_lateral = steer; anchor.steering_limit = config.steering_offset;
        anchor.tangent_x = forward_z; anchor.tangent_z = -forward_x;
        anchor.turn_offset_x = -forward_x*config.rear_offset + forward_z*steer;
        anchor.turn_offset_z = -forward_z*config.rear_offset - forward_x*steer;
    }
    const auto dx = anchor.x - frame.skater.position.x;
    const auto dy = anchor.y - frame.skater.position.y;
    const auto dz = anchor.z - frame.skater.position.z;
    anchor.distance = std::hypot(dx, dy, dz);
    if (!active && mode == TowMode::none && !config.require_grip && anchor.grip.valid &&
        anchor.distance > config.acquisition_distance) {
        // A valid cosmetic contact may still be too far from the rider. Keep
        // that measured preview unless this same verified car's rear policy
        // actually offers an in-range acquisition; never enlarge the radius.
        Anchor kinematic;
        if (evaluate(vehicle, frame, config, kinematic, false, steering, TowMode::kinematic) == DetachReason::none &&
            kinematic.distance <= config.acquisition_distance) anchor = kinematic;
    }
    return std::isfinite(anchor.distance) ? DetachReason::none : DetachReason::invalid_input;
}
TargetStatus rejected_status(const Vehicle& vehicle, DetachReason reason,
                             const Config& config, bool active) noexcept {
    if (reason == DetachReason::ineligible) {
        const auto speed = std::hypot(static_cast<double>(vehicle.velocity.x), vehicle.velocity.z);
        // Report the actual first acquisition gate, without changing it. An
        // active stopped car skips this gate and can instead lack a surface.
        if (!active && speed < config.min_vehicle_speed) return TargetStatus::too_slow;
        if (speed <= config.max_vehicle_speed && config.require_grip && !vehicle.grip.valid)
            return TargetStatus::missing_surface;
    }
    return TargetStatus::stale_or_invalid;
}
} // namespace

TargetInfo inspect_target(const Frame& frame, Config config, std::uint64_t preferred_id) noexcept {
    TargetInfo info;
    if (!valid(config)) { info.reason = DetachReason::invalid_config; info.status = TargetStatus::stale_or_invalid; return info; }
    if (!std::isfinite(frame.now) || frame.now < 0 || !frame.world || !frame.skater.id ||
        !finite(frame.skater.position) || !finite(frame.skater.velocity) || !std::isfinite(frame.input.steer)) {
        info.reason = DetachReason::invalid_input; info.status = TargetStatus::stale_or_invalid; return info;
    }
    if (!frame.offline || !frame.interactive || !frame.skater.on_board || !frame.skater.grounded ||
        frame.skater.bailing || frame.skater.teleporting ||
        std::hypot(static_cast<double>(frame.skater.velocity.x), frame.skater.velocity.z) > config.max_skater_speed) {
        info.reason = DetachReason::ineligible; return info;
    }
    const Vehicle* selected = nullptr;
    const Vehicle* rejected = nullptr;
    Anchor anchor;
    auto closest = std::numeric_limits<double>::infinity();
    auto closest_rejected = std::numeric_limits<double>::infinity();
    auto diagnostic = frame.vehicles.empty() ? TargetStatus::no_vehicle : TargetStatus::stale_or_invalid;
    for (const auto& car : frame.vehicles) {
        if (preferred_id && car.id != preferred_id) continue;
        Anchor candidate;
        const auto reason = evaluate(car, frame, config, candidate, preferred_id != 0);
        if (preferred_id) {
            if (selected) { info = {}; info.reason = DetachReason::identity_changed; info.status = TargetStatus::stale_or_invalid; return info; }
            selected = &car; anchor = candidate; info.reason = reason;
        } else if (reason == DetachReason::none && (candidate.distance < closest ||
            (candidate.distance == closest && (!selected || car.id < selected->id)))) {
            selected = &car; anchor = candidate; closest = candidate.distance; info.reason = reason;
        } else if (reason != DetachReason::none && bounded(car.position)) {
            // A rejected observed centre can rank diagnostic explanations,
            // but cannot supply a contact anchor, marker or measured distance.
            const auto distance = std::hypot(static_cast<double>(car.position.x)-frame.skater.position.x,
                static_cast<double>(car.position.y)-frame.skater.position.y,
                static_cast<double>(car.position.z)-frame.skater.position.z);
            if (distance < closest_rejected || (distance == closest_rejected && (!rejected || car.id < rejected->id))) {
                rejected = &car; closest_rejected = distance;
                diagnostic = rejected_status(car, reason, config, false);
            }
        }
    }
    if (!selected) { info.status = preferred_id ? TargetStatus::no_vehicle : diagnostic; return info; }
    unsigned duplicates = 0;
    for (const auto& car : frame.vehicles) duplicates += car.id == selected->id ? 1u : 0u;
    if (duplicates != 1) { info = {}; info.reason = DetachReason::identity_changed; info.status = TargetStatus::stale_or_invalid; return info; }
    info.available = true; info.eligible = info.reason == DetachReason::none;
    info.id = selected->id; info.generation = selected->generation;
    info.anchor = {static_cast<float>(anchor.x), static_cast<float>(anchor.y), static_cast<float>(anchor.z)};
    info.distance = static_cast<float>(std::min(anchor.distance, static_cast<double>(std::numeric_limits<float>::max())));
    if (finite(selected->velocity))
        info.speed = static_cast<float>(std::min(std::hypot(static_cast<double>(selected->velocity.x), selected->velocity.z),
            static_cast<double>(std::numeric_limits<float>::max())));
    info.in_reach = info.eligible && anchor.distance <= config.acquisition_distance;
    info.grip = anchor.grip; info.predicted_at = frame.now;
    info.approaching = anchor.approaching; info.contact_eligible = anchor.contact_eligible;
    info.status = info.eligible ? (info.in_reach ? TargetStatus::ready : TargetStatus::out_of_range) :
        rejected_status(*selected, info.reason, config, preferred_id != 0);
    if (info.eligible) info.mode = anchor.grip.valid ? TowMode::measured_surface : TowMode::kinematic;
    return info;
}

Controller::Controller(Config config) noexcept : config_(config) {}
void Controller::reset() noexcept {
    attached_ = require_release_ = have_time_ = false;
    vehicle_id_ = vehicle_generation_ = world_ = skater_id_ = surface_id_ = 0;
    last_now_ = 0;
    steering_lateral_ = steering_speed_ = 0;
    have_local_stance_ = false;
    local_stance_x_ = local_stance_z_ = local_speed_x_ = local_speed_z_ = 0;
    transition_surface_id_ = 0;
    mode_ = TowMode::none;
}
Result Controller::step(const Frame& frame) noexcept {
    double elapsed{};
    const auto detach = [&](DetachReason reason, bool forced = true) {
        attached_ = false;
        vehicle_id_ = vehicle_generation_ = world_ = skater_id_ = surface_id_ = 0;
        steering_lateral_ = steering_speed_ = 0;
        have_local_stance_ = false;
        local_stance_x_ = local_stance_z_ = local_speed_x_ = local_speed_z_ = 0;
        transition_surface_id_ = 0;
        mode_ = TowMode::none;
        if (forced) require_release_ = true;
        Result result;
        result.reason = reason;
        result.elapsed_seconds = elapsed;
        return result;
    };
    if (!valid(config_)) return detach(DetachReason::invalid_config);
    if (!std::isfinite(frame.now) || frame.now < 0 || !std::isfinite(frame.seconds) ||
        frame.seconds <= 0 || !frame.world || !frame.skater.id ||
        !finite(frame.skater.position) || !finite(frame.skater.velocity) || !std::isfinite(frame.input.steer))
        return detach(DetachReason::invalid_input);
    elapsed = have_time_ ? frame.now - last_now_ : static_cast<double>(frame.seconds);
    if (elapsed <= 0) return detach(DetachReason::invalid_input);
    last_now_ = frame.now;
    have_time_ = true;
    if (frame.seconds > config_.max_frame_seconds)
        return detach(DetachReason::frame_gap);
    const bool wall_gap = elapsed > config_.max_frame_seconds;
    // Inhibited controls are not evidence that the user released the button.
    // Only a numerically valid, forward-time, trusted input can rearm a grab.
    if (!frame.offline || !frame.interactive) {
        require_release_ = true;
        return detach(DetachReason::ineligible);
    }
    if (!frame.input.grab) require_release_ = false;
    if (attached_ && (frame.world != world_ || frame.skater.id != skater_id_))
        return detach(DetachReason::identity_changed);
    if (!frame.input.grab)
        return detach(attached_ ? DetachReason::released : DetachReason::none, false);
    if (require_release_) return detach(DetachReason::blocked);
    if (!frame.skater.on_board || !frame.skater.grounded ||
        frame.skater.bailing || frame.skater.teleporting || frame.input.brake || frame.input.jump)
        return detach(DetachReason::ineligible);
    // A pre-existing speed over the cap cannot be corrected immediately while
    // also respecting the acceleration limit. Leave that native motion alone.
    const Horizontal current{frame.skater.velocity.x, frame.skater.velocity.z};
    if (std::hypot(current.x, current.z) > config_.max_skater_speed)
        return detach(DetachReason::ineligible);
    if (wall_gap && !attached_) {
        // There is no previously owned car to retain. Rebase the accepted
        // clock without acquiring or inventing a held-input detach latch.
        Result result;
        result.suspended = true; result.elapsed_seconds = elapsed;
        return result;
    }

    const Vehicle* target = nullptr;
    Anchor anchor;
    const bool continuing = attached_;
    if (attached_) {
        for (const auto& vehicle : frame.vehicles) {
            if (vehicle.id != vehicle_id_) continue;
            if (target || vehicle.generation != vehicle_generation_)
                return detach(DetachReason::identity_changed);
            target = &vehicle;
        }
        if (!target) return detach(DetachReason::vehicle_lost);
        auto reason = mode_ == TowMode::measured_surface &&
            (!target->grip.valid || target->grip.surface_id != surface_id_) ? DetachReason::identity_changed :
            evaluate(*target, frame, config_, anchor, true, true, mode_);
        if (reason != DetachReason::none && mode_ == TowMode::measured_surface && !config_.require_grip) {
            // Optional surface evidence owns contact, not this independently
            // verified traffic incarnation. Retain the reached car-local stance
            // while dropping every hand/surface proof; strict mode still fails.
            mode_ = TowMode::kinematic; surface_id_ = transition_surface_id_ = 0;
            local_speed_x_ = local_speed_z_ = 0;
            anchor = {};
            reason = evaluate(*target, frame, config_, anchor, true, true, mode_);
        }
        if (reason != DetachReason::none) return detach(reason);
    } else {
        auto closest = std::numeric_limits<double>::infinity();
        for (const auto& vehicle : frame.vehicles) {
            Anchor candidate;
            if (evaluate(vehicle, frame, config_, candidate) != DetachReason::none ||
                candidate.distance > config_.acquisition_distance) continue;
            if (candidate.distance < closest || (candidate.distance == closest &&
                (!target || vehicle.id < target->id))) {
                closest = candidate.distance; target = &vehicle; anchor = candidate;
            }
        }
        if (!target) { Result result; result.elapsed_seconds = elapsed; return result; }
        // Ambiguous snapshots cannot establish unique attachment ownership.
        unsigned identities = 0;
        for (const auto& vehicle : frame.vehicles) identities += vehicle.id == target->id ? 1u : 0u;
        if (identities != 1) return detach(DetachReason::identity_changed);
        attached_ = true;
        vehicle_id_ = target->id;
        vehicle_generation_ = target->generation;
        world_ = frame.world;
        skater_id_ = frame.skater.id;
        mode_ = anchor.grip.valid ? TowMode::measured_surface : TowMode::kinematic;
        surface_id_ = anchor.grip.valid ? anchor.grip.surface_id : 0;
        // Steering moves the tether only after the unsteered rear anchor has
        // established reach and ownership. Holding steer cannot hide a grab.
        const auto reason = evaluate(*target, frame, config_, anchor, true, true, mode_);
        if (reason != DetachReason::none) return detach(reason);
    }

    double stance_speed{};
    {
        // Both tow modes keep steering in car-local metres as the car turns.
        // A currently smaller measured reach envelope takes precedence over
        // smoothing old state; a new grab starts from the neutral stance.
        const auto previous = std::clamp(steering_lateral_, -anchor.steering_limit, anchor.steering_limit);
        if (previous != steering_lateral_) steering_speed_ = 0;
        const auto remaining = anchor.steering_lateral-previous;
        const auto wanted_speed = std::clamp(remaining*config_.position_gain,
            -static_cast<double>(config_.grip_steering_speed), static_cast<double>(config_.grip_steering_speed));
        if (!wall_gap) {
            const auto response = anchor.grip.valid ? config_.grip_velocity_response : config_.velocity_response;
            steering_speed_ += (wanted_speed-steering_speed_)*
                -std::expm1(-static_cast<double>(response)*frame.seconds);
        }
        auto step = wall_gap ? 0.0 : steering_speed_*frame.seconds;
        // Decelerating a reversal may still move briefly in the old direction;
        // the configured lateral extent and current hard reach remain binding.
        if (step*remaining >= 0 && std::abs(step) > std::abs(remaining)) { step = remaining; steering_speed_ = 0; }
        const auto lateral = std::clamp(previous+step, -anchor.steering_limit, anchor.steering_limit);
        if (lateral != previous+step) steering_speed_ = 0;
        // The current hard reach clamp remains the stored stance even during
        // suspension; a later wider envelope must not resurrect its old offset.
        steering_lateral_ = lateral;
        if (!wall_gap) stance_speed = (lateral-previous)/frame.seconds;
        const auto adjustment = lateral-anchor.steering_lateral;
        anchor.x += adjustment*anchor.tangent_x; anchor.z += adjustment*anchor.tangent_z;
        anchor.turn_offset_x += adjustment*anchor.tangent_x;
        anchor.turn_offset_z += adjustment*anchor.tangent_z;
        anchor.distance = std::hypot(anchor.x-frame.skater.position.x, anchor.y-frame.skater.position.y,
            anchor.z-frame.skater.position.z);
    }
    Horizontal stance_motion{stance_speed*anchor.tangent_x, stance_speed*anchor.tangent_z};
    bool transitioning_surface = false;
    const auto heading_length = std::hypot(static_cast<double>(target->forward.x), target->forward.z);
    if (heading_length > 1e-6) {
        const auto forward_x = target->forward.x/heading_length, forward_z = target->forward.z/heading_length;
        const auto age = frame.now-target->sampled_at;
        const auto center_x = target->position.x+target->velocity.x*age;
        const auto center_z = target->position.z+target->velocity.z*age;
        const auto to_local = [&](double x, double z) {
            return Horizontal{(x-center_x)*forward_z-(z-center_z)*forward_x,
                (x-center_x)*forward_x+(z-center_z)*forward_z};
        };
        if (mode_ == TowMode::measured_surface) {
            const auto local = to_local(anchor.x, anchor.z);
            local_stance_x_ = local.x; local_stance_z_ = local.z; have_local_stance_ = true;
            local_speed_x_ = local_speed_z_ = 0; transition_surface_id_ = 0;
        } else if (mode_ == TowMode::kinematic) {
            Anchor measured;
            const auto& reach = frame.skater.grip_reach;
            const bool current_surface = continuing && current_reach(reach, frame, config_) &&
                evaluate(*target, frame, config_, measured, true, true, TowMode::measured_surface) == DetachReason::none &&
                measured.distance <= config_.max_separation &&
                std::abs(measured.grip.point.y-frame.skater.position.y-reach.shoulder_offset_world.y) < reach.max_reach;
            if (current_surface || have_local_stance_) {
                if (!have_local_stance_) {
                    // Transition replaces this tick's steering advance, rather
                    // than adding a second 1.2m/s motion to the same stance.
                    const auto local = to_local(anchor.x-stance_motion.x*frame.seconds,
                        anchor.z-stance_motion.z*frame.seconds);
                    local_stance_x_ = local.x; local_stance_z_ = local.z; have_local_stance_ = true;
                }
                Horizontal local_step;
                if (current_surface) {
                    if (transition_surface_id_ != measured.grip.surface_id) {
                        local_speed_x_ = local_speed_z_ = 0;
                        transition_surface_id_ = measured.grip.surface_id;
                    }
                    const auto goal = to_local(measured.x, measured.z);
                    const Horizontal remaining{goal.x-local_stance_x_, goal.z-local_stance_z_};
                    if (!wall_gap) {
                        const auto wanted = limit({remaining.x*config_.position_gain, remaining.z*config_.position_gain},
                            config_.grip_steering_speed);
                        const auto response = -std::expm1(-static_cast<double>(config_.velocity_response)*frame.seconds);
                        local_speed_x_ += (wanted.x-local_speed_x_)*response;
                        local_speed_z_ += (wanted.z-local_speed_z_)*response;
                        local_step = limit({local_speed_x_*frame.seconds, local_speed_z_*frame.seconds},
                            std::min(std::hypot(remaining.x, remaining.z),
                                static_cast<double>(config_.grip_steering_speed)*frame.seconds));
                        local_stance_x_ += local_step.x; local_stance_z_ += local_step.z;
                    }
                    anchor.grip = measured.grip;
                    anchor.contact_eligible = measured.contact_eligible;
                    anchor.approaching = !measured.contact_eligible;
                    transitioning_surface = true;
                    // Promotion preserves exact traffic identity and the same
                    // fresh surface lease. No instantaneous geometry jump is
                    // permitted merely because the hand is now within reach.
                    const auto alignment = std::hypot(goal.x-local_stance_x_, goal.z-local_stance_z_);
                    if (!wall_gap && alignment < 1e-5 &&
                        alignment+std::hypot(local_step.x, local_step.z) <=
                            static_cast<double>(config_.grip_steering_speed)*frame.seconds &&
                        measured.contact_eligible) {
                        mode_ = TowMode::measured_surface; surface_id_ = measured.grip.surface_id;
                        transition_surface_id_ = 0; transitioning_surface = false;
                        steering_lateral_ = measured.steering_lateral; steering_speed_ = 0;
                        anchor = measured;
                        local_stance_x_ = goal.x; local_stance_z_ = goal.z;
                        local_speed_x_ = local_speed_z_ = 0;
                        stance_motion = {};
                    }
                } else {
                    // No cached measured point or contact may survive loss.
                    // Keep only the reached local policy and bounded steering.
                    transition_surface_id_ = 0; local_speed_x_ = local_speed_z_ = 0;
                    local_step.x = wall_gap ? 0 : stance_speed*frame.seconds;
                    local_stance_x_ += local_step.x;
                }
                if (mode_ == TowMode::kinematic) {
                    anchor.turn_offset_x = local_stance_x_*forward_z+local_stance_z_*forward_x;
                    anchor.turn_offset_z = -local_stance_x_*forward_x+local_stance_z_*forward_z;
                    anchor.x = center_x+anchor.turn_offset_x; anchor.z = center_z+anchor.turn_offset_z;
                    stance_motion = wall_gap ? Horizontal{} : Horizontal{
                        (local_step.x*forward_z+local_step.z*forward_x)/frame.seconds,
                        (-local_step.x*forward_x+local_step.z*forward_z)/frame.seconds};
                }
            }
        }
    }
    anchor.distance = std::hypot(anchor.x-frame.skater.position.x, anchor.y-frame.skater.position.y,
        anchor.z-frame.skater.position.z);
    if (continuing && anchor.distance > config_.max_separation) return detach(DetachReason::too_far);

    Result result;
    result.attached = true;
    result.vehicle_id = vehicle_id_;
    result.anchor = {static_cast<float>(anchor.x), static_cast<float>(anchor.y), static_cast<float>(anchor.z)};
    result.grip = anchor.grip; result.predicted_at = frame.now;
    result.approaching = anchor.approaching; result.contact_eligible = anchor.contact_eligible;
    result.mode = mode_; result.elapsed_seconds = elapsed;
    result.transitioning_surface = transitioning_surface;
    Horizontal point_motion = stance_motion;
    if (target->yaw_rate_valid && std::isfinite(target->yaw_rate) &&
        std::isfinite(target->orientation_sampled_at) && target->orientation_sampled_at >= 0 &&
        target->orientation_sampled_at <= frame.now &&
        frame.now-target->orientation_sampled_at <= config_.max_sample_age) {
        // Positive yaw rotates +Z toward +X: omega cross r = (omega*z, -omega*x).
        point_motion.x += static_cast<double>(target->yaw_rate)*anchor.turn_offset_z;
        point_motion.z -= static_cast<double>(target->yaw_rate)*anchor.turn_offset_x;
    }
    point_motion = limit(point_motion, config_.max_catchup_speed);
    result.anchor_velocity = {static_cast<float>(anchor.velocity.x+point_motion.x), anchor.velocity.y,
        static_cast<float>(anchor.velocity.z+point_motion.z)};
    if (wall_gap) {
        // Every control, identity, sample/surface and separation gate above
        // still applies. No elapsed wall time enters tow or steering math.
        result.suspended = true;
        return result;
    }

    const auto position_gain = mode_ == TowMode::measured_surface ? config_.grip_position_gain : config_.position_gain;
    const auto catchup = limit({(anchor.x - frame.skater.position.x) * position_gain,
        (anchor.z - frame.skater.position.z) * position_gain}, config_.max_catchup_speed);
    const auto desired = limit({result.anchor_velocity.x + catchup.x, result.anchor_velocity.z + catchup.z}, config_.max_skater_speed);
    const auto velocity_response = mode_ == TowMode::measured_surface ? config_.grip_velocity_response : config_.velocity_response;
    const auto response = -std::expm1(-static_cast<double>(velocity_response) * frame.seconds);
    const auto delta = limit({(desired.x - current.x) * response, (desired.z - current.z) * response},
                            static_cast<double>(config_.max_acceleration) * frame.seconds);
    result.velocity_delta = {static_cast<float>(delta.x), 0, static_cast<float>(delta.z)};
    if (!finite(result.velocity_delta)) return detach(DetachReason::invalid_input);
    result.write_velocity = delta.x != 0 || delta.z != 0;
    return result;
}
} // namespace car_grab
