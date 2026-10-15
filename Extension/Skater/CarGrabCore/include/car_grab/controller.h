#pragma once

#include <cstdint>
#include <span>

namespace car_grab {
struct Vec3 { float x{}, y{}, z{}; };
struct VehicleGrip {
    bool valid{};
    Vec3 point{}, normal{}, tangent{}, velocity{};
    double sampled_at{};
    std::uint64_t surface_id{};
    bool wrap_valid{};
    Vec3 top_point{}, bottom_point{}, top_normal{}, bottom_normal{};
};
struct SkaterGripReach {
    bool valid{};
    Vec3 shoulder_offset_world{};
    float min_reach{}, max_reach{};
    double sampled_at{};
};
struct Vehicle {
    std::uint64_t id{}, generation{};
    Vec3 position{}, forward{}, velocity{};
    double sampled_at{};
    VehicleGrip grip{};
    // Optional finite-window angular motion of the caller-verified horizontal
    // pose axis. Positive yaw turns +Z toward +X; no native angular field read.
    float yaw_rate{};
    bool yaw_rate_valid{};
    double orientation_sampled_at{};
};
struct Skater {
    std::uint64_t id{};
    Vec3 position{}, velocity{};
    bool on_board{}, grounded{}, bailing{}, teleporting{};
    SkaterGripReach grip_reach{};
};
struct Input { bool grab{}, brake{}, jump{}; float steer{}; };
struct Frame {
    double now{};
    float seconds{};
    std::uint64_t world{};
    bool offline{}, interactive{};
    Skater skater{};
    Input input{};
    std::span<const Vehicle> vehicles{};
};
enum class DetachReason {
    none, released, blocked, invalid_config, invalid_input, ineligible,
    identity_changed, stale_vehicle, vehicle_lost, too_far, frame_gap
};
// Kinematic mode uses a rider/tether policy, not measured bumper geometry.
enum class TowMode { none, kinematic, measured_surface };
struct Result {
    bool attached{}, write_velocity{};
    std::uint64_t vehicle_id{};
    Vec3 velocity_delta{};
    DetachReason reason = DetachReason::none;
    Vec3 anchor{};
    VehicleGrip grip{};
    double predicted_at{};
    bool approaching{}, contact_eligible{};
    TowMode mode = TowMode::none;
    // A wall-clock-only hitch keeps an independently revalidated attachment,
    // but authorizes no correction until the next normal native timestep.
    bool suspended{};
    double elapsed_seconds{}; // Advisory controller-clock interval, never an integration dt.
    Vec3 anchor_velocity{}; // Estimated policy-point motion, not a native write.
    // A held verified car is converging toward this frame's exact surface;
    // native callers must validate result.grip as strictly as measured mode.
    bool transitioning_surface{};
};
struct Config {
    float acquisition_distance = 2.5f;
    float rear_offset = 3.5f; // Kinematic rider/tether policy, not a car dimension.
    float steering_offset = 1.25f;
    float min_vehicle_speed = 1.f;
    float max_vehicle_speed = 30.f;
    float max_acceleration = 35.f;
    float max_catchup_speed = 6.f;
    float max_skater_speed = 30.f;
    float position_gain = 4.f;
    float velocity_response = 8.f;
    float max_separation = 8.f;
    float max_sample_age = .15f;
    float max_frame_seconds = .1f;
    bool require_grip{};
    float grip_standoff = .55f; // Rider stance policy, never a vehicle dimension.
    float grip_position_gain = 16.f;
    float grip_velocity_response = 16.f;
    float grip_steering_speed = 1.2f; // Maximum car-local stance motion in m/s.
};
// Diagnostic state is advisory only; it never authorizes a tow or substitutes
// a car centre for a verified contact point.
enum class TargetStatus {
    none, no_vehicle, missing_surface, too_slow, stale_or_invalid, out_of_range, ready
};
struct TargetInfo {
    bool available{}, eligible{}, in_reach{};
    std::uint64_t id{}, generation{};
    Vec3 anchor{};
    float distance{}, speed{};
    DetachReason reason = DetachReason::vehicle_lost;
    VehicleGrip grip{};
    double predicted_at{};
    bool approaching{}, contact_eligible{};
    TargetStatus status = TargetStatus::none;
    TowMode mode = TowMode::none;
};
// Read-only preview of the same rear target geometry used for acquisition.
// A preferred ID inspects that active target, including a verified stopped car.
// Steering applies after attachment and never moves this acquisition preview.
TargetInfo inspect_target(const Frame& frame, Config config = {},
                          std::uint64_t preferred_id = 0) noexcept;
// Pure state and math; no native pointers, input polling, or body writes.
class Controller {
public:
    explicit Controller(Config config = {}) noexcept;
    void reset() noexcept;
    Result step(const Frame& frame) noexcept;
private:
    Config config_{};
    bool attached_{}, require_release_{}, have_time_{};
    std::uint64_t vehicle_id_{}, vehicle_generation_{}, world_{}, skater_id_{}, surface_id_{};
    double last_now_{};
    double steering_lateral_{}, steering_speed_{};
    bool have_local_stance_{};
    double local_stance_x_{}, local_stance_z_{}, local_speed_x_{}, local_speed_z_{};
    std::uint64_t transition_surface_id_{};
    TowMode mode_ = TowMode::none;
};
} // namespace car_grab
