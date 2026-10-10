#pragma once

#include "car_grab/controller.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace car_grab {
// The caller validates native lifetime and the complete collection before and
// after copying these values. The sampler never dereferences native identities.
struct VehicleObservation {
    std::uint64_t id{}, context{}, record{};
    Vec3 position{};
    // Optional caller-verified horizontal path axis from this fresh native pose.
    // Zero means unavailable; malformed directions fall back to motion heading.
    Vec3 forward{};
};
struct VehicleSampleFrame {
    std::uint64_t world{};
    double now{};
    std::span<const VehicleObservation> observations{};
    bool complete{};
};
enum class VehicleSampleStatus { incomplete, invalid, warming, ready };
// Copied current identity evidence only. A warming incarnation never
// authorizes towing velocity, prediction, contact or a native write.
struct VehicleIncarnation {
    std::uint64_t id{}, generation{}, context{}, record{};
};
struct VehicleSampleResult {
    VehicleSampleStatus status = VehicleSampleStatus::incomplete;
    std::vector<Vehicle> vehicles{};
    std::vector<VehicleIncarnation> incarnations{};
};
struct VehicleSamplingConfig {
    double min_interval = 1.0 / 240.0;
    double max_interval = .1;
    double max_gap = .15;
    // Repeated verified transforms can be published slower than native reads.
    // Retain a measured estimate for this finite window, with its original age.
    double motion_hold = .05;
    double min_heading_speed = .5; // Parking noise does not define a rear axis.
    double max_speed = 40.0;
    double max_position = 100000.0;
    std::size_t max_observations = 2000;
    // Estimate motion over a finite span of distinct observed poses, rather
    // than a short callback interval straddling step-published native updates.
    double velocity_window = .05;
};
// Results own their samples. A ready empty result means a validated empty
// collection, whereas incomplete/invalid results invalidate every history.
// Two fresh same-incarnation poses can establish a caller-verified path axis
// while stopped. Motion otherwise warms for velocity_window; no axis is invented.
// A pure observation cadence gap resets motion and heading while retaining a
// freshly verified same full identity/context/record. Missing/removed/replaced
// identities, world changes and invalid collections still revoke continuity.
// incarnations contains caller-owned identity evidence for complete valid
// collections, including warming cars; vehicles alone authorizes motion.
// Generations remain monotonic across reset. first_generation lets the caller
// continue a previous instance's serial; zero or exhaustion fail closed.
class VehicleSampler {
public:
    explicit VehicleSampler(VehicleSamplingConfig config = {},
                            std::uint64_t first_generation = 1);
    VehicleSampleResult sample(const VehicleSampleFrame& frame);
    void reset() noexcept;
    // A verified native removal can invalidate a handle between snapshots even
    // when its native serial wraps back to the same value before the next read.
    // Discard this motion history only; keep the synthetic generation sequence.
    void forget(std::uint64_t id) noexcept;
private:
    struct MotionPose { Vec3 position{}; double now{}; };
    struct HeadingPose { Vec3 forward{}; double now{}; };
    struct History {
        std::uint64_t context{}, record{}, generation{};
        Vec3 position{}, forward{}, velocity{};
        double now{}, last_seen{}, sampled_at{};
        // A time bound alone cannot bound allocation with arbitrary timestamps.
        std::array<MotionPose, 32> poses{};
        std::size_t pose_count{};
        bool motion_ready{}, motion_seen{};
        std::array<HeadingPose, 32> headings{};
        std::size_t heading_count{};
        float yaw_rate{};
        bool yaw_rate_valid{};
        double orientation_sampled_at{};
    };
    VehicleSamplingConfig config_{};
    std::unordered_map<std::uint64_t, History> histories_{};
    std::uint64_t world_{}, next_generation_{};
    double last_now_{};
    bool have_time_{}, exhausted_{};
};
} // namespace car_grab
