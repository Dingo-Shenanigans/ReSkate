#pragma once
#include "bmx_standalone_shadow_status.h"
#include "bmx_native_motion.h"
#include "Engine/Game/BMX/bmx_mount_pose.h"
#include "bmx_visual.h"
#include "bmx_wheel_surface.h"
#include "Engine/Game/BMX/bmx_approach_surface.h"
#include "Engine/Game/BMX/bmx_contact_compliance.h"
#include "Engine/Game/BMX/bmx_landing_contact_compliance.h"
#include "Engine/Game/BMX/bmx_input.h"
#include "Engine/Game/BMX/bmx_tricks.h"
#include "Engine/Game/BMX/bmx_landing_approach.h"
#include "Engine/Game/BMX/bmx_bail_policy.h"
#include "Engine/Game/BMX/bmx_ground_pump.h"
#include <array>
#include <cstdint>

namespace dingosdk::bmx::chassis_ride {
using Vec = std::array<float,3>;
using Quat = std::array<float,4>;
struct Step {
    bool ready{},grounded{},has_ground{},control_active{},hop_applied{},command_submitted{};
    const char* issue = "not_started";
    visual::Frame frame;
    Vec target_rider_root{},velocity{},observed_com{},angular_velocity{},force{},torque{},hop_impulse{},desired_support_angular_velocity{};
    Quat observed_quaternion{0,0,0,1};
    float speed{},ground_height{};
    float contact_force_scale = 1; // common bounded fraction of the solved contact impulses
    unsigned balance_support_mask{},balance_continuation_mask{}; // front bit1, rear bit2; never hop/scoring eligibility
    float landing_body_alignment{1},landing_support_alignment{1},landing_closing_speed{};
    bool actual_support{}; // measured contact, independent of a newly requested hop
    bail_policy::Result bail;
    bail_policy::Sample impact_sample;
    unsigned body_guard_rays{};
    landing_approach::Evidence landing_approach;
    double native_step_seconds{},native_simulation_seconds{};
    std::uint64_t native_applications{},hop_events{},serial{},body_token{};
    std::array<Vec,2> contact_points{},contact_normals{}; // front, rear; valid only when contact_hit
    std::array<float,2> wheel_separation{},normal_load{},wheel_angular_speed{};
    // Current measured point speed along the actual force normal; positive is
    // opening, negative closing. Valid only behind wheel_contacts_sampled&&contact_hit.
    std::array<float,2> wheel_opening_speed{},wheel_recovery_velocity{};
    // wheel_angular_speed is the portable rotor model, not a native wheel body measurement.
    std::array<unsigned,2> contact_rays{};
    std::array<const char*,2> contact_features{"none","none"}; // queried feature; contact_hit separately marks solver-skin contact
    std::array<float,2> contact_edge_uncertainty{};
    std::array<wheel_surface::EdgeDiagnostic,2> contact_edge_diagnostics{};
    std::array<bool,2> contact_hit{};
    bool query_geometry_fresh{},approach_selected{},pre_contact_prediction_available{};
    std::uint64_t approach_microseconds{};
    bool approach_normal_selected{},approach_normal_attempted{},approach_normal_adopted{};
    bool contact_compliance_selected{},contact_compliance_revoked{};
    bool landing_contact_compliance_selected{},landing_contact_compliance_revoked{};
    landing_contact_compliance::Result landing_compliance;
    unsigned compliant_normal_mask{},compliance_attempted_mask{};
    bool compliance_budget_fallback{};
    std::array<contact_compliance::Result,2> compliance{};
    std::array<double,2> compliant_predicted_gap{};
    std::array<bool,2> compliant_deflection_satisfied{};

    const char* approach_normal_issue="disabled";
    unsigned speculative_normal_mask{};
    std::array<const char*,2> speculative_issues{"disabled","disabled"};
    std::array<float,2> speculative_normal_impulse{},speculative_normal_force{};
    Vec speculative_linear_impulse{},speculative_angular_impulse{};

    std::array<approach_surface::Surface,2> queried_surfaces{}; // actual query geometry, including force-clear planes
    std::array<approach_surface::Result,2> approach{}; // fresh geometry evidence; private normal solver separately gates admission; never actual support
    tire::Body pre_contact_prediction; // exact pure-controller state before tire contacts, no added gravity here
    bool wheel_contacts_sampled{}; // false on an early body-guard bail; default contacts are unknown
    std::array<bool,2> actual_wheel_support{}; // strict measured eligibility, front/rear
    Actions actions;
    TrickState tricks;
    TrickEvents trick_events;
    bool static_surface_velocity_assumed{true}; // moving support velocity is not observed in this first backend
    std::array<Vec,2> contact_balance_normals{}; // current measured face, never the edge radial force normal
    std::array<bool,2> contact_balance_normal_available{};
    Vec balance_reference_up{},desired_balance_up{},balance_torque{},air_control_torque{},motor_torque{},contact_torque{};
    Vec air_damping_torque{}; // passive neutral-flight torque, never player input
    bool air_rotation_reserved{},neutral_air_damping_active{};
    Vec pump_force{}; // pre-contact WORLD-down COM component, not a second native command
    ground_pump::Result pump;
    float requested_front_steer_angle{},turning_lean_angle{};
    bool steering_limited{};
    standalone_shadow::Report shadow; // copied into the bounded ride history, never a command
};
// Explicit process opt-in and validated separate development root; no legacy
// motion fallback is supplied when this backend is selected but unavailable.
// Client-update-thread only: bounded last evaluation, disabled in an ordinary build.
standalone_shadow::Report standalone_shadow_status() noexcept;
bool enabled() noexcept;
bool start(std::uintptr_t base,const NativeFrame& rider,const Vec& bike_root,
           float yaw,float root_height,const mount_pose::Seed* seed=nullptr) noexcept;
Step update(std::uintptr_t base,const NativeFrame& rider,const RawInput& input,double native_h) noexcept;
// Pause parks in place; update resumes the same native pose after guards recover.
// Stop ends this mount; a later start may reuse/reseed the backend's owned pool.
void pause(std::uintptr_t base,const NativeFrame& rider) noexcept;
void stop(std::uintptr_t base,const NativeFrame& rider) noexcept;
// Ends only the portable controller/pose feed. Caller must independently cancel
// pending native commands and own the body's crash/recovery lifecycle.
void detach() noexcept;
}
