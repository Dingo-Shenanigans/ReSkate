#include "car_grab/vehicle_samples.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace car_grab {
namespace {
bool valid(const VehicleSamplingConfig& config) noexcept {
    return std::isfinite(config.min_interval) && config.min_interval > 0 &&
        std::isfinite(config.max_interval) && config.max_interval >= config.min_interval &&
        std::isfinite(config.max_gap) && config.max_gap >= config.max_interval &&
        std::isfinite(config.motion_hold) && config.motion_hold >= 0 &&
        config.motion_hold <= config.max_interval &&
        std::isfinite(config.velocity_window) && config.velocity_window > 0 &&
        config.velocity_window <= config.max_interval &&
        std::isfinite(config.min_heading_speed) && config.min_heading_speed >= 0 &&
        config.min_heading_speed <= config.max_speed &&
        std::isfinite(config.max_speed) && config.max_speed > 0 &&
        config.max_speed <= std::numeric_limits<float>::max() &&
        std::isfinite(config.max_position) && config.max_position > 0 &&
        config.max_position <= std::numeric_limits<float>::max() &&
        config.max_observations > 0 && config.max_observations <= 2000;
}
bool valid(const VehicleObservation& observation, double max_position) noexcept {
    const auto position = observation.position;
    return observation.id && observation.context && observation.record &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::abs(static_cast<double>(position.x)) <= max_position &&
        std::abs(static_cast<double>(position.y)) <= max_position &&
        std::abs(static_cast<double>(position.z)) <= max_position;
}
bool verified_forward(Vec3 input, Vec3& output) noexcept {
    if (!std::isfinite(input.x) || !std::isfinite(input.y) || !std::isfinite(input.z) ||
        std::abs(input.y) > .0001f) return false;
    const auto horizontal = std::hypot(static_cast<double>(input.x), input.z);
    if (horizontal < 1e-6) return false;
    output = {static_cast<float>(input.x / horizontal), 0, static_cast<float>(input.z / horizontal)};
    return true;
}
} // namespace

VehicleSampler::VehicleSampler(VehicleSamplingConfig config, std::uint64_t first_generation)
    : config_(config), next_generation_(first_generation), exhausted_(!first_generation) {}

void VehicleSampler::reset() noexcept {
    histories_.clear();
    world_ = 0;
    last_now_ = 0;
    have_time_ = false;
}

void VehicleSampler::forget(std::uint64_t id) noexcept {
    histories_.erase(id);
}

VehicleSampleResult VehicleSampler::sample(const VehicleSampleFrame& frame) {
    const auto interrupt = [&](VehicleSampleStatus status) {
        histories_.clear();
        return VehicleSampleResult{status, {}};
    };
    if (!valid(config_) || exhausted_) return interrupt(VehicleSampleStatus::invalid);
    if (!frame.complete) return interrupt(VehicleSampleStatus::incomplete);
    if (!frame.world || !std::isfinite(frame.now) || frame.now < 0 ||
        (have_time_ && frame.now <= last_now_) || frame.observations.size() > config_.max_observations)
        return interrupt(VehicleSampleStatus::invalid);

    // A single invalid entry rejects the claimed complete snapshot. Validating
    // before mutation also prevents ambiguous handles from producing output.
    std::unordered_set<std::uint64_t> present;
    present.reserve(frame.observations.size());
    for (const auto& observation : frame.observations) {
        if (!valid(observation, config_.max_position) || !present.insert(observation.id).second)
            return interrupt(VehicleSampleStatus::invalid);
    }
    const bool cadence_gap = have_time_ && frame.now - last_now_ > config_.max_gap;
    if (frame.world != world_) histories_.clear();
    world_ = frame.world;
    last_now_ = frame.now;
    have_time_ = true;
    for (auto iterator = histories_.begin(); iterator != histories_.end();) {
        if (!present.contains(iterator->first)) iterator = histories_.erase(iterator);
        else ++iterator;
    }

    const auto baseline = [&](const VehicleObservation& observation, std::uint64_t same_generation = 0) {
        if (!same_generation && !next_generation_) {
            exhausted_ = true;
            return false;
        }
        const auto generation = same_generation ? same_generation : next_generation_;
        if (!same_generation)
            next_generation_ = generation == std::numeric_limits<std::uint64_t>::max() ? 0 : generation + 1;
        History history;
        history.context = observation.context; history.record = observation.record; history.generation = generation;
        history.position = observation.position;
        if (verified_forward(observation.forward, history.forward)) {
            history.headings[0] = {history.forward, frame.now}; history.heading_count = 1;
            history.orientation_sampled_at = frame.now;
        }
        history.now = history.last_seen = history.sampled_at = frame.now;
        history.poses[0] = {observation.position, frame.now}; history.pose_count = 1;
        histories_[observation.id] = history;
        return true;
    };
    VehicleSampleResult result{VehicleSampleStatus::warming, {}};
    result.vehicles.reserve(frame.observations.size());
    for (const auto& observation : frame.observations) {
        const auto found = histories_.find(observation.id);
        if (found == histories_.end() || found->second.context != observation.context ||
            found->second.record != observation.record) {
            if (!baseline(observation)) return interrupt(VehicleSampleStatus::invalid);
            continue;
        }
        if (cadence_gap || frame.now - found->second.last_seen > config_.max_interval) {
            // The caller freshly verified this exact full native identity in
            // a complete collection. A timing pause revokes all motion/heading
            // history, but is not evidence that this car was removed/replaced.
            // No velocity is derived across the gap, including apparent rest.
            const auto generation = found->second.generation;
            if (!baseline(observation, generation)) return interrupt(VehicleSampleStatus::invalid);
            continue;
        }
        auto& history = found->second;
        history.last_seen = frame.now;
        Vec3 orientation;
        const auto have_orientation = verified_forward(observation.forward, orientation);
        history.yaw_rate = 0; history.yaw_rate_valid = false;
        if (have_orientation) {
            history.forward = orientation;
            history.orientation_sampled_at = frame.now;
            std::size_t retained{};
            for (std::size_t i = 0; i < history.heading_count; ++i)
                if (frame.now - history.headings[i].now <= config_.max_interval + 1e-9)
                    history.headings[retained++] = history.headings[i];
            history.heading_count = retained;
            const auto minimum_span = std::max(config_.velocity_window, config_.min_interval);
            for (std::size_t i = history.heading_count; i > 0; --i) {
                const auto& previous = history.headings[i-1];
                const auto span = frame.now - previous.now;
                if (span + 1e-9 < minimum_span || span > config_.max_interval + 1e-9) continue;
                // atan2 uses the shortest signed turn, including a +/-pi wrap.
                const auto sine = static_cast<double>(orientation.x)*previous.forward.z -
                    static_cast<double>(orientation.z)*previous.forward.x;
                const auto cosine = static_cast<double>(orientation.x)*previous.forward.x +
                    static_cast<double>(orientation.z)*previous.forward.z;
                const auto rate = std::atan2(sine, cosine)/span;
                if (std::isfinite(rate) && std::abs(rate) <= std::numeric_limits<float>::max()) {
                    history.yaw_rate = static_cast<float>(rate); history.yaw_rate_valid = true;
                }
                break;
            }
            if (!history.heading_count ||
                frame.now-history.headings[history.heading_count-1].now + 1e-9 >= config_.min_interval) {
                if (history.heading_count == history.headings.size()) {
                    std::move(history.headings.begin()+1, history.headings.end(), history.headings.begin());
                    --history.heading_count;
                }
                history.headings[history.heading_count++] = {orientation, frame.now};
            }
        } else {
            // Optional orientation loss does not poison proven origin motion,
            // but no angular estimate may bridge an unreadable pose direction.
            history.heading_count = 0; history.orientation_sampled_at = 0;
        }
        const auto emit = [&] {
            if (std::hypot(history.forward.x, history.forward.z) < 1e-6 || !history.motion_ready) return;
            result.vehicles.push_back({observation.id, history.generation, history.position,
                history.forward, history.velocity, history.sampled_at});
            auto& vehicle = result.vehicles.back();
            vehicle.yaw_rate = history.yaw_rate; vehicle.yaw_rate_valid = history.yaw_rate_valid;
            vehicle.orientation_sampled_at = history.orientation_sampled_at;
        };
        const auto restart_window = [&] {
            history.poses[0] = {history.position, frame.now}; history.pose_count = 1;
        };
        const auto seconds = frame.now - history.now;
        const auto dx = static_cast<double>(observation.position.x) - history.position.x;
        const auto dy = static_cast<double>(observation.position.y) - history.position.y;
        const auto dz = static_cast<double>(observation.position.z) - history.position.z;
        const auto distance = std::hypot(dx, dy, dz);
        // Fresh repeated presence is distinct from motion freshness. A stale
        // velocity may bridge the next transform publication only briefly.
        if (distance < 1e-6) {
            if (seconds + 1e-9 >= config_.motion_hold) {
                history.velocity = {};
                history.sampled_at = frame.now;
                history.motion_ready = true;
                // Resumed motion must not average the preceding moving trace
                // or its stationary wait into a new velocity measurement.
                restart_window();
            } else if (!history.motion_ready && !history.motion_seen && have_orientation) {
                // Two fresh identical poses can establish oriented parked
                // presence. Observed cold motion still needs its finite window.
                history.motion_ready = true;
                history.sampled_at = frame.now;
            }
            emit();
            continue;
        }
        const auto speed = distance / std::min(seconds, config_.max_interval);
        if (!std::isfinite(speed) || speed > config_.max_speed) {
            if (!baseline(observation)) return interrupt(VehicleSampleStatus::invalid);
            continue;
        }
        history.motion_seen = true;
        // Complete short frames still establish presence/identity and check
        // discontinuities; retain their prior baseline until enough time passes.
        if (seconds < config_.min_interval) { emit(); continue; }

        // A car observed stationary for a long time keeps its known native
        // incarnation. Its first resumed pose starts a new motion baseline;
        // the next bounded pair measures velocity without joining across rest.
        if (seconds > config_.max_interval) {
            history.position = observation.position;
            history.velocity = {};
            history.now = history.sampled_at = frame.now;
            history.motion_ready = true;
            restart_window();
            emit();
            continue;
        }

        // Callback timestamps mark when a pose was observed, not when the game
        // published it. Use the nearest observation at least one finite window
        // behind this one; adjacent long/short callbacks cannot create a burst.
        std::size_t retained{};
        for (std::size_t i = 0; i < history.pose_count; ++i)
            if (frame.now - history.poses[i].now <= config_.max_interval + 1e-9)
                history.poses[retained++] = history.poses[i];
        history.pose_count = retained;
        const auto minimum_span = std::max(config_.velocity_window, config_.min_interval);
        const MotionPose* anchor = nullptr;
        for (std::size_t i = history.pose_count; i > 0; --i) {
            const auto span = frame.now - history.poses[i-1].now;
            if (span + 1e-9 >= minimum_span && span <= config_.max_interval) {
                anchor = &history.poses[i-1]; break;
            }
        }
        history.velocity = {};
        if (anchor) {
            const auto span = frame.now - anchor->now;
            const auto vx = (static_cast<double>(observation.position.x) - anchor->position.x) / span;
            const auto vy = (static_cast<double>(observation.position.y) - anchor->position.y) / span;
            const auto vz = (static_cast<double>(observation.position.z) - anchor->position.z) / span;
            const auto derived_speed = std::hypot(vx, vy, vz);
            if (!std::isfinite(derived_speed) || derived_speed > config_.max_speed) {
                if (!baseline(observation)) return interrupt(VehicleSampleStatus::invalid);
                continue;
            }
            history.velocity = {static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz)};
            history.motion_ready = true;
            const auto horizontal = std::hypot(history.velocity.x, history.velocity.z);
            if (!have_orientation && horizontal > 1e-6 && horizontal >= config_.min_heading_speed)
                history.forward = {static_cast<float>(history.velocity.x / horizontal), 0,
                    static_cast<float>(history.velocity.z / horizontal)};
        }
        history.position = observation.position;
        history.now = history.sampled_at = frame.now;
        if (history.pose_count == history.poses.size()) {
            std::move(history.poses.begin() + 1, history.poses.end(), history.poses.begin());
            --history.pose_count;
        }
        history.poses[history.pose_count++] = {observation.position, frame.now};
        emit();
    }
    result.incarnations.reserve(frame.observations.size());
    for (const auto& observation : frame.observations) {
        const auto& history = histories_.at(observation.id);
        result.incarnations.push_back({observation.id, history.generation, history.context, history.record});
    }
    if (frame.observations.empty() || !result.vehicles.empty()) result.status = VehicleSampleStatus::ready;
    return result;
}
} // namespace car_grab
