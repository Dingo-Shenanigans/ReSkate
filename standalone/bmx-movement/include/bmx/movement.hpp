#pragma once

#include "detail/source/bmx_chassis_control.h"
#include "detail/source/bmx_input.h"
#include "detail/source/bmx_tricks.h"
#include <array>
#include <cstdint>

// Independent BMX movement adaptation. SI units; +Y up, +Z forward,
// +X rider-left (the inherited geometry convention). Input +X means RIGHT,
// so positive steering naturally turns toward negative world X at yaw zero.
// Quaternions are Hamilton XYZW, rotating body-space vectors into world space.
// The original game implementation, tuning and animation parity are not claimed.
namespace bmx {
using Vec3 = std::array<double,3>;
using Quat = std::array<double,4>;
using Body = detail::source::tire::Body;
using Wheel = detail::source::tire::Wheel;
using RawInput = detail::source::RawInput;
using Actions = detail::source::Actions;
using HopKind = detail::source::HopKind;
using ContactSample = detail::source::chassis_control::ContactSample;
using ContactStatus = detail::source::tire::ContactStatus;
using Tuning = detail::source::chassis_control::Tuning;
using TrickSettings = detail::source::TrickSettings;
using InputSettings = detail::source::InputSettings;
using TrickState = detail::source::TrickState;
using TrickEvents = detail::source::TrickEvents;
using TrickId = detail::source::catalog::TrickId;
using TrickFailure = detail::source::TrickFailure;
namespace catalog = detail::source::catalog;
namespace geometry = detail::source::geometry;
inline constexpr double fixed_step = 1.0/60.0;
inline constexpr double minimum_fixed_step = 1.0/240.0;
inline constexpr double maximum_fixed_step = 1.0/30.0;

// Per-instance simulation clock and reference-host initialization. Host body
// observations remain authoritative in Movement::step. Clock/body/wheel/hop
// settings reject invalid values; input/trick settings retain bounded policies.
struct SimulationSettings {
    double seconds{fixed_step};
    double mass{detail::source::chassis_control::proposed_mass};
    Vec3 inertia_body{detail::source::chassis_control::proposed_inertia};
    Quat inertia_orientation{0,0,0,1};
    std::array<Wheel,2> wheels{detail::source::chassis_control::proposed_wheels()};
    // A hop request waits this long, then launches only if support is still
    // eligible. The accepted impulse is emitted in this many fixed-step parts.
    double hop_delay_seconds{0};
    unsigned hop_impulse_steps{1};
    bool predict_hop_parts{}; // opt-in multi-body mode; legacy/v1 prediction unchanged
    // Optional observed negative-hop eligibility in a two-wheel/no-peg host:
    // rear-only rejects; both-wheel support scales the selected impulse by.95.
    bool observed_nollie_support_policy{};
    // Normal-only anticipation from an exact current static plane query and
    // this step's accepted pre-contact prediction; never establishes support.
    bool swept_plane_normals{};
    double hop_impulse_multiplier{1}; // source recipient multiplicity or explicit reduction
    InputSettings input_settings{};
    TrickSettings trick_settings{};
};

// Procedural skeleton dimensions and neutral bike-root-local template. Array
// order is left/right. Axes and bend poles use the inherited +X rider-left basis.
// Bone length is independent of the neutral pose and of authored motion scale.
struct RiderSettings {
    double upper_arm{.34},forearm{.33},thigh{.47},shin{.45};
    Vec3 hips{0,1.00,-.13},chest{0,1.31,.08},head_from_chest{0,.25,.035};
    double shoulder_half_width{.19},hip_joint_half_width{.105};
    Vec3 shoulder_axis{1,0,0},hip_joint_axis{1,0,0};
    Vec3 shoulder_center_from_chest{},hip_joint_center_from_hips{};
    std::array<Vec3,2> elbow_poles{Vec3{.5,-.1,-.3},Vec3{-.5,-.1,-.3}};
    std::array<Vec3,2> knee_poles{Vec3{.12,0,.9},Vec3{-.12,0,.9}};
    double motion_scale{.77},preload_crouch{.13},chest_preload_fraction{.8};
};

// Explicit opt-in profile from the current observed build. The hop's timing
// follows observed scheduling; its launch speed is an empirical reduced-body
// fit, not the game's recovered native velocity. Combined mass and inertia,
// force/catch tuning and procedural trick trajectories remain independent.
struct ObservedProfile { Tuning tuning;SimulationSettings simulation;RiderSettings rider; };
ObservedProfile streets_observed_v1() noexcept;

struct RiderPose {
    // World-space joint targets. A host can retarget these to its own skeleton.
    // These are procedural targets, not extracted humanoid clips.
    Vec3 hips{}, chest{}, head{};
    std::array<Vec3,2> shoulders{}, elbows{}, hands{}, hips_joint{}, knees{}, feet{};
    std::array<double,2> hand_release{},foot_release{};
    // If a procedural motion requests an unreachable anchor, the target is
    // projected onto the limb's reachable annulus and this flag is set.
    std::array<bool,2> hand_target_clamped{},foot_target_clamped{};
};
// Optional measured suspension/rider frame for proximal skeleton targets. The
// neutral frame is bike-root-local; the current frame is world-space. Contact
// hands/feet retain the bike anchors and all limbs retain RiderSettings lengths.
struct RiderFrame {
    Vec3 center_of_mass{},neutral_center_from_bike_root{};
    Quat orientation{0,0,0,1},neutral_orientation_from_bike{0,0,0,1};
};
struct Output {
    bool valid{},grounded{};
    bool hop_request_accepted{}; // this valid step reserved a new hop; raw gesture events can be denied while pending
    // Verified joint actuation already rotates the physical chassis. Retain
    // trick state for gameplay while suppressing independent presentation tilt.
    bool physical_trick_rotation{};
    detail::source::chassis_control::Issue issue{detail::source::chassis_control::Issue::invalid_sample};
    Body body{};
    Vec3 bike_root{},force{},torque{},hop_impulse{},hop_velocity_change{}; // unmultiplied pulse velocity
    unsigned swept_plane_normal_mask{};
    std::array<double,2> swept_plane_normal_impulse{};
    Vec3 bike_com_from_root{geometry::com_from_root[0],geometry::com_from_root[1],geometry::com_from_root[2]};
    double steering_angle{},front_wheel_phase{},rear_wheel_phase{},crank_phase{};
    Actions actions{};
    TrickState tricks{};
    TrickEvents events{};
    RiderPose rider{};
};
RiderPose rider_pose(const Output&,const RiderSettings& = {},const RiderFrame* = nullptr) noexcept;
struct ExternalWrench {Vec3 force{},torque{};}; // included once in returned force/torque; excludes gravity
// Canonical rider rotation for a host with a separately simulated rider. This
// affects rotation judging only; chassis contacts/quality and forces stay real.
struct TrickRotationObservation {
    bool available{};Quat orientation{0,0,0,1};
    bool travel_available{};Vec3 travel_velocity{}; // measured canonical approach/rig-COM velocity
};

// Converts measured host-body state and the TWO real wheel queries into force,
// torque and the current step's hop impulse. Apply force/torque for
// fixed_seconds() exactly once; apply hop_impulse once per returned frame;
// the host applies gravity separately.
// Inputs are sampled once per step. Available controls must first be neutral
// to arm, and focus/menu/device transitions require another neutral sample.
class Movement {
public:
    explicit Movement(Tuning tuning = {},SimulationSettings simulation = {},RiderSettings rider = {}) noexcept;
    // The host must change this nonzero identity when replacing any simulated
    // rig role, including an observed rider, so scheduled impulses and control
    // history cannot carry into another bike/rider.
    Output step(const RawInput&,const Body&,const std::array<ContactSample,2>&,std::uint64_t body_identity = 1,ExternalWrench = {},TrickRotationObservation = {}) noexcept;
    void reset() noexcept;
    // Reject an auxiliary host observation: clear queued controls/contact
    // history and require neutral while preserving completed trick tally.
    void invalidate_sample() noexcept;
    const Tuning& tuning() const noexcept { return tuning_; }
    double fixed_seconds() const noexcept { return simulation_.seconds; }
    const SimulationSettings& simulation_settings() const noexcept { return simulation_; }
    const RiderSettings& rider_settings() const noexcept { return rider_; }
private:
    Tuning tuning_;
    SimulationSettings simulation_;
    RiderSettings rider_;
    detail::source::chassis_control::Controller controller_;
    detail::source::InputMapper mapper_;
    detail::source::TrickController tricks_;
    std::array<detail::source::tire::Wheel,2> wheels_;
    std::uint64_t serial_{},hop_event_{};
    std::uint64_t body_identity_{};
    std::uint32_t input_device_{};
    bool device_known_{},hop_pending_{};
    double hop_delay_left_{},hop_strength_{};
    unsigned hop_steps_left_{};
    Vec3 hop_remaining_{};
    double front_phase_{},rear_phase_{},crank_phase_{};
    void cancel_hop() noexcept;
    void reject_sample() noexcept;
};

// Small runnable reference host, with an infinite static Y=0 plane. It supplies
// wheel contacts and rigid-body integration so a demo needs no game engine.
// It has no chassis/character/world-obstacle collision or ragdoll solver.
class FlatWorld {
public:
    explicit FlatWorld(Tuning tuning = {},SimulationSettings simulation = {},RiderSettings rider = {}) noexcept;
    void reset(Vec3 bike_root = {0,0,0},double yaw = 0) noexcept;
    Output step(const RawInput&) noexcept;
    const Body& body() const noexcept { return body_; }
    const Output& output() const noexcept { return output_; }
    double fixed_seconds() const noexcept { return movement_.fixed_seconds(); }
private:
    Movement movement_;
    Body body_;
    Output output_;
};

Vec3 rotate(Quat,Vec3) noexcept;
// Presentation only. tail_frame rotates the frame/rear assembly about the
// steering pivot; false keeps the front assembly with the hands. Physics
// wheel_hub() deliberately does not consume these authored trick rotations.
Vec3 bike_point(const Output&,Vec3 bike_local,bool tail_frame = true) noexcept;
Vec3 wheel_hub(const Output&,bool front) noexcept;
Vec3 grip_point(const Output&,bool left) noexcept;
Vec3 pedal_point(const Output&,bool left) noexcept;
const char* issue_name(detail::source::chassis_control::Issue) noexcept;
const char* trick_failure_name(TrickFailure) noexcept;
}
