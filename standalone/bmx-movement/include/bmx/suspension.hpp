#pragma once
#include "movement.hpp"
#include "joint_actuation.hpp"
#include "attitude.hpp"

namespace bmx {
// A bounded, implicit six-axis joint between measured chassis and rider bodies.
// Anchors are COM-local; frame quaternions map joint axes into each body. The
// result contains internal reactions only. The host applies gravity separately.
enum class SuspensionDriveMode { force, acceleration };
struct SuspensionDrive {
    double spring{},damper{},maximum_effort{100000}; // N / N m in force mode
    SuspensionDriveMode mode{SuspensionDriveMode::force};
};
struct SuspensionLimit {
    bool enabled{};
    double minimum{-1},maximum{1};
    SuspensionDrive drive{}; // bounded soft stop; never a position correction
};
// An explicit reduction may fold followers into the chassis while retaining
// source acceleration-drive gains defined on its original connected bodies.
// Disabled by default: ordinary hosts use their measured body mass/inertia.
struct SuspensionDriveReference {
    bool enabled{};
    double chassis_mass{1},rider_mass{1};
    Vec3 chassis_inertia{1,1,1},rider_inertia{1,1,1};
    Quat chassis_inertia_orientation{0,0,0,1},rider_inertia_orientation{0,0,0,1};
    Vec3 chassis_com_offset{},rider_com_offset{}; // physical-body-local
};
struct SuspensionSettings {
    Vec3 chassis_anchor{},rider_anchor{};
    Quat chassis_frame{0,0,0,1},rider_frame{0,0,0,1};
    Vec3 target_position{},target_linear_velocity{},target_angular_velocity{};
    Quat target_rotation{0,0,0,1};
    std::array<SuspensionDrive,3> linear{},angular{};
    std::array<SuspensionLimit,3> linear_limits{},angular_limits{};
    SuspensionLimit linear_radial_limit{false,0,1,{}}; // norm of anchor separation, matching Limited XYZ
    SuspensionDriveReference acceleration_reference{};
    double maximum_linear_force{150000},maximum_angular_torque{150000};
    unsigned iterations{48};
    // Separate nonadhesive implicit stop rows. Inside a bound they oppose only
    // motion that would cross it; outside they retain the spring/damper bias.
    bool unilateral_soft_limits{};
};
enum class SuspensionIssue { none,invalid_step,invalid_settings,invalid_body,ambiguous_rotation,energy_increase };
struct SuspensionResult {
    bool valid{},saturated{},energy_limited{};
    SuspensionIssue issue{SuspensionIssue::invalid_body};
    Vec3 chassis_force{},chassis_torque{},rider_force{},rider_torque{};
    Vec3 chassis_anchor{},rider_anchor{}; // measured world points
    Vec3 displacement{},linear_velocity{},rotation{},angular_velocity{}; // joint frame
    std::array<double,7> impulse{}; // XYZ, rotation XYZ, optional radial stop
    double energy_before{},predicted_energy{},actuator_work{},dissipated_energy{},limit_violation{};
};
bool valid_suspension(const SuspensionSettings&) noexcept;
// Acceleration-mode gains are converted using each measured joint row's
// effective mass, including anchor leverage; coupled rows are solved together.
// Fixed zero-velocity targets admit no increase of joint-plus-body energy.
// Nonzero target velocities explicitly report the actuator work allowance.
SuspensionResult solve_suspension(double seconds,const Body& chassis,const Body& rider,const SuspensionSettings&) noexcept;
const char* suspension_issue_name(SuspensionIssue) noexcept;

// Explicit selected-rig reduction: the chassis Body may represent a rigid fold
// of chosen vehicle followers; the independent rider Body is the Spring role.
// This does not silently include avatar/ragdoll/hair masses in either body.
struct JointActuationPolicy {
    bool enabled{};
    Vec3 ground_inertia{6,8,6},air_inertia{150,150,150};
    double inertia_rate{500},ground_limit{.3},air_limit{2.5};
    double down_damper{140},up_damper{140},falling_max_damper{600},up_velocity_threshold{1.5};
    double ground_damper_rate{100},falling_damper_rate{200},drive_lerp_rate{30};
};
struct ManualPitchPolicy {
    bool enabled{};
    double manny_angle_degrees{30},nosey_angle_degrees{30};
    double ground_lerp_rate{4},air_lerp_rate{6};
    bool manny_direct_torque{true},nosey_direct_torque{true};
    double direct_torque{450};
    bool chassis_pid{true};
};
struct CoupledSettings {
    SuspensionSettings suspension{};
    Vec3 chassis_com_from_root{0,.3,0},observed_chassis_com_from_root{0,.3,0};
    double rider_mass{80};Vec3 rider_inertia{6,8,6};
    Quat rider_inertia_orientation{0,0,0,1};
    Vec3 rider_initial_com_from_root{0,.7,0},rider_neutral_com_from_root{0,.7,0};
    Quat rider_orientation_from_chassis{0,0,0,1};
    double rider_hop_multiplier{1};
    unsigned coupling_iterations{32}; // Picard budget; bounded Newton fallback is additional
    double coupling_tolerance{.05}; // maximum true applied-point wrench residual N/N m
    bool chassis_gravity{true},rider_gravity{true};
    double rider_linear_drag{},rider_angular_drag{},chassis_angular_drag{};
    // Optional source-observed airborne damping; actual strict support selects
    // this rate. Torques are already included in the returned role wrenches.
    bool use_air_angular_drag{};double air_angular_drag{2.75};
    double rider_air_yaw_acceleration{},rider_air_pitch_acceleration{},rider_maximum_angular_speed{12};
    // Reference-host Rigidbody-style speed limits. Zero leaves the role free;
    // an engine adapter configures the same property on its actual bodies.
    double chassis_body_angular_speed_limit{},rider_body_angular_speed_limit{};
    // The physics-to-posture mapping is independent of native transform ancestry.
    Vec3 posture_translation_gain{1,1,1};bool posture_follows_rider_rotation{true};
    JointActuationPolicy actuation{}; // disabled preserves a host's own commands
    AttitudePIDSettings rider_stabilization{},chassis_stabilization{};
    ManualPitchPolicy manual_pitch{};
};
struct CoupledProfile {ObservedProfile movement{};CoupledSettings coupled{};};
CoupledProfile streets_observed_v2() noexcept;
// Optional exact per-step observations for isolated measured-drive replay.
// Ordinary hosts already provide actual rider inertia through the Body input.
struct CoupledTargets {
    bool suspension_available{},rider_inertia_available{};
    SuspensionSettings suspension{};
    Vec3 rider_inertia{6,8,6};Quat rider_inertia_orientation{0,0,0,1};
};
struct CoupledOutput {
    bool valid{},converged{};
    // Exact supplied queries from the pre-integration observation, rather than
    // recomputed gaps using the reference host's already integrated Body.
    std::array<ContactSample,2> input_contacts{};
    Output bike{};Body rider{};
    Vec3 rider_force{},rider_torque{},rider_hop_impulse{},rider_angular_impulse{};
    Vec3 observed_chassis_com{},rider_relative_com{};
    RiderFrame posture_frame{};
    SuspensionResult suspension{};SuspensionSettings applied_suspension{};
    bool joint_actuation_available{};std::uint16_t joint_actuation_id{};double joint_actuation_seconds{};
    AttitudePIDResult rider_stabilization{},chassis_stabilization{};
    double manual_pitch_setpoint_degrees{};
    Vec3 manual_chassis_torque{},manual_rider_torque{};
    unsigned coupling_iterations{}; // all map evaluations, including Newton probes/line search
    double coupling_residual{};
};
class CoupledMovement {
public:
    explicit CoupledMovement(CoupledProfile) noexcept;
    CoupledOutput step(const RawInput&,const Body& chassis,const Body& rider,
        const std::array<ContactSample,2>&,std::uint64_t identity=1,const CoupledTargets& = {}) noexcept;
    void reset() noexcept;
    void invalidate_sample() noexcept;
    const CoupledProfile& profile() const noexcept{return profile_;}
    double fixed_seconds() const noexcept{return profile_.movement.simulation.seconds;}
private:
    CoupledProfile profile_;Movement movement_;
    AttitudePID rider_stabilizer_,chassis_stabilizer_;
    const JointActuationTrack* actuation_track_{};
    Vec3 actuation_phase_offset_{},last_actuation_target_{};
    Quat actuation_phase_rotation_{0,0,0,1},last_actuation_rotation_{0,0,0,1};
    bool actuation_command_known_{};
    double actuation_seconds_{},actuation_weight_{1},wanted_y_damper_{140},current_y_damper_{140},current_radial_limit_{.3};
    double air_seconds_{},since_hop_{1e6};
    double manual_pitch_angle_{},manual_ground_seconds_{},manny_input_seconds_{};
    std::uint64_t actuation_owner_{};std::uint32_t actuation_device_{};
    bool actuation_identity_known_{};
    Vec3 approach_velocity_{};bool approach_reserved_{},approach_air_seen_{};
    void reset_actuation() noexcept;
};
class CoupledWorld {
public:
    explicit CoupledWorld(CoupledProfile) noexcept;
    void reset(Vec3 bike_root={0,0,0},double yaw=0) noexcept;
    CoupledOutput step(const RawInput&,const CoupledTargets& = {}) noexcept;
    const CoupledOutput& output() const noexcept{return output_;}
    const Body& body() const noexcept{return chassis_;}
    const Body& rider_body() const noexcept{return rider_;}
    const CoupledProfile& profile() const noexcept{return movement_.profile();}
    double fixed_seconds() const noexcept{return movement_.fixed_seconds();}
private:
    CoupledMovement movement_;Body chassis_,rider_;CoupledOutput output_;
};
}
