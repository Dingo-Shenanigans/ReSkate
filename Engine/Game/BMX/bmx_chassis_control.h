#pragma once
#include "Engine/Game/BMX/bmx_tire_dynamics.h"
#include "bmx_coupled_brake.h"
#include "bmx_ground_pump.h"
#include "bmx_contact_compliance.h"
#include "bmx_landing_contact_compliance.h"
#include "bmx_geometry.h"
#include <cstdint>

// Portable force preparation for an explicitly calibrated native 60 Hz adapter.
// No native calls, pose integration, position correction or heading servo lives here.
// Contact normals/points/platform velocity must come from the caller's real queries.
namespace dingosdk::bmx::chassis_control {
using Vec = tire::Vec;
inline constexpr double verified_native_seconds = .01666666567325592;
inline constexpr double proposed_mass = 106;
inline constexpr double actual_support_tolerance = .005;
inline constexpr Vec proposed_inertia{26,32,10}; // local X pitch, Y yaw, Z roll; rotor axial spin excluded
inline tire::Body proposed_body() noexcept {
    tire::Body body;body.mass=proposed_mass;body.inertia_body=proposed_inertia;return body;
}
inline std::array<tire::Wheel,2> proposed_wheels() noexcept {
    return {tire::Wheel{geometry::wheel_radius,.23,0},tire::Wheel{geometry::wheel_radius,.23,0}};
}
struct ContactSample {
    tire::Contact contact;
    double separation{}; // signed wheel-surface gap, positive clear; supplied geometry, not an averaged height
    // Optional current measured face normal for balance only. An edge uses
    // its radial contact normal for force, but its fitted face for riding-up.
    Vec balance_normal{};
    bool balance_normal_available{};
    bool verified_plane{}; // actual current plane feature; never inferred from a radial edge
};
// Private adapter proof only: exact current geometry and pre-contact prediction.
// This row can never supply tangential friction, support, balance or a hop.
struct SpeculativeNormal {
    bool selected{},qualified{};Vec point{},normal{};double separation{};tire::Body prediction;
};
enum class SpeculativeIssue {disabled,unqualified,unverified_step,disabled_controls,hop_reserved,
    not_clear_plane,geometry_changed,prediction_changed,nonstatic_surface,gap_outside,not_crossing,admitted};
inline const char* speculative_name(SpeculativeIssue issue) noexcept {
    switch(issue){
    case SpeculativeIssue::disabled:return "disabled";case SpeculativeIssue::unqualified:return "unqualified";
    case SpeculativeIssue::unverified_step:return "unverified_step";case SpeculativeIssue::disabled_controls:return "disabled_controls";
    case SpeculativeIssue::hop_reserved:return "hop_reserved";case SpeculativeIssue::not_clear_plane:return "not_clear_plane";
    case SpeculativeIssue::geometry_changed:return "geometry_changed";case SpeculativeIssue::prediction_changed:return "prediction_changed";
    case SpeculativeIssue::nonstatic_surface:return "nonstatic_surface";case SpeculativeIssue::gap_outside:return "gap_outside";
    case SpeculativeIssue::not_crossing:return "not_crossing";case SpeculativeIssue::admitted:return "admitted";
    }return "unqualified";
}
struct Controls {
    bool enabled{true};
    double pedal{},brake{},steer{},manual{},nose_manual{},air_yaw{},hop_strength{};
    std::uint64_t hop_event_id{}; // monotonically increasing per controller lifetime; 0 means no event
    // Bridge proof only: same owned body token/world and exactly consecutive
    // native serial/time, after current wheel queries. Default is unavailable.
    bool recovery_step_verified{};
    bool contact_compliance_available{}; // explicit private physics opt-in; default old rigid contact
    bool landing_contact_compliance_selected{}; // separate mode selector; a rigid retry disables the law without replaying its event
    // Adapter reservation: current raw gesture or active/pending catalog trick.
    // Once airborne this intent survives release until strict wheel recontact.
    bool rotation_reserved{};
    // Explicit fresh raw-operator opt-in; default callers retain old physics.
    // ground_pump is mapped LEFT Y, never the rear hop/manual axis.
    bool pump_operator_available{},pump_inhibited{};
    double ground_pump{};
};
// These are proposed first-test values, not recovered effective Streets settings.
// Measured Body.mass/inertia and supplied Wheel values are authoritative.
struct Tuning {
    Vec gravity{0,-9.8,0};
    double wheelbase{geometry::wheelbase},steer_limit{.4},steer_speed_scale{8};
    double pedal_torque{75},front_brake_torque{60},rear_brake_torque{45};
    tire::Parameters friction{1,1};
    // Authored supported-balance damping reduces lean-release oscillation in
    // force/contact benches. Stiffness, caps and airborne controls retain their values.
    double balance_stiffness{1600},balance_damping{360},balance_torque_limit{1200};
    // Authored supported banking target for the smaller bike; this changes no
    // fall/contact threshold, force cap, air input or stiffness/damping gain.
    double lean_limit{.47},manual_pitch{.4},air_yaw_torque{200},air_pitch_torque{160};
    // Authored ride-feel decay rate (1/s), not a recovered Streets consumer/gain.
    double neutral_air_damping_rate{2.75};
    double hop_delta_speed{4.5};
    double contact_gap{.02},maximum_penetration{.10};
    double recovery_fraction{.15},recovery_speed_limit{.4};
    double force_limit{40000},torque_limit{4000},wheel_speed_limit{500};
    unsigned contact_iterations{12};
    double pedal_fade_start{6},pedal_cutoff{8}; // proposed m/s; cadence/forward propulsion only
    double curvature_rate_limit{6},curvature_step_limit{.2},curvature_min_speed{.5};
    unsigned brake_iterations{96}; // numerical convergence only; zero-brake retains contact_iterations
};
enum class Issue { none,unsupported_step,invalid_sample,unavailable_contact,penetration_limit,output_limit };
struct Result {
    bool valid{},grounded{},hop_applied{}; // grounded = eligible queried support, not a native manifold flag
    Issue issue{Issue::invalid_sample};
    Vec force{},torque{}; // force*h==linear_impulse; torque*h==angular_impulse; native gravity excluded
    Vec linear_impulse{},angular_impulse{},hop_impulse{}; // hop is separate and NOT included in force
    std::uint64_t hop_event_id{};
    std::array<tire::Wheel,2> wheels{}; // front, rear; only adopt when valid
    std::array<double,2> normal_load{},normal_impulse{},longitudinal_impulse{},lateral_impulse{},recovery_velocity{};
    double front_steer_angle{};
    double requested_front_steer_angle{},turning_lean_angle{};
    bool steering_limited{};
    // One shared fraction for brake+contact body/rotor responses. Motor,
    // balance and separate hop retain their original contributions. Scaling
    // can leave unresolved brake/contact slip, never a full brake against a
    // partially submitted traction response.
    double contact_scale{1};
    Vec desired_support_angular_velocity{}; // bounded measured-normal transport, not a world orientation target
    unsigned balance_support_mask{},balance_continuation_mask{}; // front/rear bits; never input/hop/landing support
    std::array<double,2> brake_impulse{}; // signed actual scaled axle N*m*s; per-step total, also valid in air
    Vec balance_reference_up{},desired_balance_up{};
    // Torque attribution; contact includes the bounded brake/contact block.
    // Air controls and motor reaction are never labeled automatic balance.
    Vec balance_torque{},air_control_torque{},motor_torque{},contact_torque{};
    tire::Body pre_contact_prediction; // exact existing base-actuated state; diagnostic copy only
    bool pre_contact_prediction_available{};
    landing_contact_compliance::Result landing_compliance; // selection only; applied rows remain separately masked
    unsigned compliant_normal_mask{},compliance_attempted_mask{};
    bool compliance_budget_fallback{}; // model failed its linear deflection bound after caps; original solve retained
    std::array<contact_compliance::Result,2> compliance{}; // raw normal model; submitted normal_load still uses shared fraction
    std::array<double,2> compliant_predicted_gap{}; // linear current-point prediction AFTER actual shared scaling
    std::array<bool,2> compliant_deflection_satisfied{};
    unsigned speculative_normal_mask{};
    std::array<SpeculativeIssue,2> speculative_issues{};
    std::array<double,2> speculative_normal_impulse{},speculative_normal_force{}; // separate from actual normal_load/support
    Vec speculative_linear_impulse{},speculative_angular_impulse{}; // included in existing contact budget, never base actuation
    Vec pump_force{}; // WORLD-down COM component, included before current contact solve
    ground_pump::Result pump;
    Vec air_damping_torque{}; // passive only, separate from player air controls
    bool air_rotation_reserved{},neutral_air_damping_active{};
};
namespace detail {
inline bool recovery_loaded_pair(const Result& result) noexcept {
    return result.valid&&std::isfinite(result.normal_load[0])&&std::isfinite(result.normal_load[1])&&
        result.normal_load[0]>0&&result.normal_load[1]>0;
}
inline Vec add(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec a,double b) noexcept {for(auto& v:a)v*=b;return a;}
inline double dot(Vec a,Vec b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Vec cross(Vec a,Vec b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline double length(Vec a) noexcept {return std::sqrt(dot(a,a));}
inline bool finite(Vec a,double limit=1e6) noexcept {for(double v:a)if(!std::isfinite(v)||std::abs(v)>limit)return false;return true;}
inline bool unit(Vec a,Vec& b) noexcept {const auto n=length(a);if(!std::isfinite(n)||n<1e-9)return false;b=mul(a,1/n);return true;}
inline tire::Quat normalized(tire::Quat q) noexcept {double n=0;for(double v:q)n+=v*v;for(auto& v:q)v/=std::sqrt(n);return q;}
inline Vec turn(tire::Quat q,Vec v) noexcept {
    const Vec axis{q[0],q[1],q[2]};const auto twice=mul(cross(axis,v),2);
    return add(v,add(mul(twice,q[3]),cross(axis,twice)));
}
inline Vec tensor(const tire::Body& body,Vec v,bool inverse) noexcept {
    const auto q=normalized(body.orientation);auto local=turn({-q[0],-q[1],-q[2],q[3]},v);
    for(unsigned i=0;i<3;++i)local[i]*=inverse?1/body.inertia_body[i]:body.inertia_body[i];
    return turn(q,local);
}
inline Vec around(Vec v,Vec axis,double angle) noexcept {
    const auto c=std::cos(angle),s=std::sin(angle);
    return add(add(mul(v,c),mul(cross(axis,v),s)),mul(axis,dot(axis,v)*(1-c)));
}
inline Vec limited(Vec v,double cap) noexcept {const auto n=length(v);return n>cap?mul(v,cap/n):v;}
// Largest feasible point along base+s*contact, with s=0 required feasible.
// Every contact/rotor uses the same fraction; independent axis clipping would
// destroy friction cones and the contact's angular-momentum accounting.
inline double contact_fraction(Vec base,Vec contact,double cap) noexcept {
    if(!std::isfinite(length(base))||!std::isfinite(length(contact))||length(base)>cap)return -1;
    if(length(add(base,contact))<=cap)return 1;
    double low=0,high=1;
    for(unsigned i=0;i<56;++i) {
        const double middle=(low+high)*.5;
        if(length(add(base,mul(contact,middle)))<=cap)low=middle;
        else high=middle;
    }
    return low;
}
// Implicit viscous damping with measured principal inertia. A common norm cap
// preserves T=-a*I_world*omega, so omega.T<=0 and the fixed-tensor update is
// omega_next=(1-a*h)*omega with0<=a*h<1. No orientation target is involved.
inline Vec neutral_air_torque(const tire::Body& body,double h,double rate,double cap) noexcept {
    if(!tire::valid(body)||!std::isfinite(h)||h<=0||!std::isfinite(rate)||rate<=0||
       !std::isfinite(cap)||cap<=0)return {};
    const auto momentum=tensor(body,body.angular_velocity,false);
    return limited(mul(momentum,-rate/(1+rate*h)),cap);
}
// Only strict queried support may establish or clear a flight reservation.
// A missing successor/disabled controller loses qualification, rather than
// forgetting a deliberate rotation and damping its released momentum in air.
struct AirReservation {
    bool ground_seen{},airborne{},reserved{},launch_reserved{};
    bool observe(bool successor,bool enabled,bool strict_support,bool intent,bool launch_intent) noexcept {
        if(!successor||!enabled) {*this={};return false;}
        if(strict_support) {
            ground_seen=true;airborne=reserved=false;launch_reserved=launch_intent;return false;
        }
        if(!ground_seen) {reserved=true;return false;}
        if(!airborne) {airborne=true;reserved=launch_reserved||intent;}
        else reserved=reserved||intent;
        return !reserved;
    }
};
// Rate-only supported steering adaptation informed by the current Streets
// balance-input curve. This is not its separate physical steering-joint filter.
// The authored domain is 0..30m/s; clamping outside it is our explicit policy.
inline double supported_steering_rate(double speed) noexcept {
    const double u=std::clamp(std::abs(speed)/30.0,0.0,1.0);
    return 5.0-3.0*u*u*(3.0-2.0*u);
}
inline bool tuning_valid(const Tuning& t) noexcept {
    if(!finite(t.gravity,100)||length(t.gravity)<.1||!t.contact_iterations||t.contact_iterations>32||
       !t.brake_iterations||t.brake_iterations>128)return false;
    for(double v:{t.wheelbase,t.steer_limit,t.steer_speed_scale,t.force_limit,t.torque_limit,t.wheel_speed_limit})
        if(!std::isfinite(v)||v<=0||v>1e6)return false;
    for(double v:{t.pedal_torque,t.front_brake_torque,t.rear_brake_torque,t.balance_stiffness,t.balance_damping,
        t.balance_torque_limit,t.lean_limit,t.manual_pitch,t.air_yaw_torque,t.air_pitch_torque,t.hop_delta_speed,
        t.contact_gap,t.maximum_penetration,t.recovery_fraction,t.recovery_speed_limit,t.pedal_fade_start,t.pedal_cutoff,
        t.curvature_rate_limit,t.curvature_step_limit,t.curvature_min_speed,t.neutral_air_damping_rate})
        if(!std::isfinite(v)||v<0||v>1e6)return false;
    return t.steer_limit<=1&&t.lean_limit<=.8&&t.manual_pitch<=.6&&t.hop_delta_speed<=4.5&&
        t.contact_gap<=.05&&t.maximum_penetration<=.15&&t.recovery_fraction<=1&&t.recovery_speed_limit<=1&&
        t.pedal_fade_start<t.pedal_cutoff&&t.pedal_cutoff<=50&&
        t.curvature_rate_limit<=10&&t.curvature_step_limit<=.35&&t.curvature_min_speed<=5&&t.neutral_air_damping_rate<=10&&
        t.friction.longitudinal_friction>=0&&t.friction.longitudinal_friction<=10&&
        t.friction.lateral_friction>=0&&t.friction.lateral_friction<=10;
}
inline bool controls_valid(const Controls& c) noexcept {
    for(double v:{c.pedal,c.brake,c.steer,c.manual,c.nose_manual,c.air_yaw,c.hop_strength,c.ground_pump})if(!std::isfinite(v))return false;
    return true;
}
} // namespace detail

// Shared query-support contract for input, tricks and force preparation. The
// wider ray/contact skin permits speculative collision response, not an early
// grounded hop or landing. No normal is invented when a query is unavailable.
inline bool eligible_support(const tire::Body& body,const ContactSample& sample,
                             double tolerance=actual_support_tolerance,double maximum_penetration=.10) noexcept {
    const auto& contact=sample.contact;tire::Basis basis;
    if(!tire::valid(body)||!detail::finite(body.linear_velocity,50)||!detail::finite(body.angular_velocity,40)||
       contact.status!=tire::ContactStatus::hit||!std::isfinite(tolerance)||tolerance<0||tolerance>.05||
       !std::isfinite(maximum_penetration)||maximum_penetration<0||maximum_penetration>.15||
       !std::isfinite(sample.separation)||sample.separation < -maximum_penetration||sample.separation>tolerance||
       !tire::contact_basis(contact.normal,contact.forward,basis)||!detail::finite(contact.point)||
       !detail::finite(contact.surface_velocity,50))return false;
    const auto relative=detail::sub(tire::point_velocity(body,contact.point),contact.surface_velocity);
    return detail::finite(relative)&&detail::dot(relative,basis.normal)<=.05;
}

class Controller {
    Tuning tuning_;
    std::uint64_t last_hop_event_{};
    struct NormalHistory {
        bool valid{};Vec normal{};double normal_load{};Vec balance_normal{};
        Vec point{},surface_velocity{};double separation{},recovery_velocity{},recovery_seconds{};
        bool verified_plane{},recovery_seeded{};
    };
    std::array<NormalHistory,2> normals_{};
    int travel_direction_{};
    detail::AirReservation air_reservation_;
    struct SteeringHistory {bool valid{};double angle{};};
    SteeringHistory steering_;
    ground_pump::Controller pump_;
    landing_contact_compliance::Controller landing_compliance_;
public:
    explicit Controller(Tuning tuning={}) noexcept:tuning_(tuning){}
    void reset() noexcept {last_hop_event_=0;normals_={};travel_direction_=0;air_reservation_={};steering_={};pump_.reset();landing_compliance_.reset();}
    void reset_pump() noexcept {pump_.reset();landing_compliance_.reset();}
    std::uint64_t consumed_hop_event_id() const noexcept {return last_hop_event_;}
    Result step(double h,const tire::Body& measured,const std::array<tire::Wheel,2>& wheels,
                const std::array<ContactSample,2>& samples,Controls controls={},
                const std::array<SpeculativeNormal,2>& speculative={}) noexcept {
        // A current modeled load is known only AFTER contact preparation. First
        // evaluate the unchanged solve without the proposed recovery admission.
        // Neither provisional result is submitted to the native body here.
        const auto original_controller=*this;
        const auto finish=[&](Result selected) {
            bool violated=false;
            for(unsigned i=0;i<2;++i)if((selected.compliant_normal_mask&(1u<<i))&&!selected.compliant_deflection_satisfied[i])violated=true;
            if(selected.valid&&violated) {
                // Under caps/nonconvergence the soft model may not protect its
                // declared linear deflection. Retry the original constitutive
                // law from the original state; never submit a smaller unsafe
                // soft command or enlarge a native cap/backup to hide it.
                auto rigid_controller=original_controller;auto rigid_controls=controls;
                rigid_controls.contact_compliance_available=false;
                auto rigid=rigid_controller.step(h,measured,wheels,samples,rigid_controls,speculative);
                rigid.compliance_budget_fallback=true;rigid.compliance_attempted_mask=selected.compliant_normal_mask;
                rigid.compliance=selected.compliance;rigid.compliant_predicted_gap=selected.compliant_predicted_gap;
                rigid.compliant_deflection_satisfied=selected.compliant_deflection_satisfied;
                *this=rigid_controller;return rigid;
            }
            return selected;
        };
        auto baseline_controller=*this;
        const auto baseline=baseline_controller.step_impl(h,measured,wheels,samples,controls,0,speculative);
        const auto mask=baseline.valid?recovery_balance_mask(h,measured,samples,controls,baseline):0;
        if(mask) {
            auto assisted_controller=*this;
            const auto assisted=assisted_controller.step_impl(h,measured,wheels,samples,controls,mask,speculative);
            // The admission proof was a loaded PAIR. Even a one-wheel mask
            // cannot adopt an assist that unloads the other strict wheel.
            if(detail::recovery_loaded_pair(assisted)) {*this=assisted_controller;return finish(assisted);}
        }
        *this=baseline_controller;return finish(baseline);
    }
private:
    unsigned recovery_balance_mask(double h,const tire::Body& measured,
        const std::array<ContactSample,2>& samples,const Controls& controls,const Result& baseline) const noexcept {
        using namespace detail;
        if(!controls.recovery_step_verified||!controls.enabled||baseline.balance_support_mask==3||
           baseline.hop_applied||controls.hop_strength>0||controls.manual!=0||controls.nose_manual!=0||
           controls.steer!=0||controls.air_yaw!=0)return 0;
        const auto up=turn(normalized(measured.orientation),{0,1,0});
        const auto forward=turn(normalized(measured.orientation),{0,0,1});
        Vec gravity_up;if(!unit(mul(tuning_.gravity,-1),gravity_up))return 0;
        Vec platform{};unsigned mask=0;
        for(unsigned i=0;i<2;++i) {
            const auto& previous=normals_[i];const auto& sample=samples[i];tire::Basis basis;
            // Two established, still loaded plane contacts only. A new wheel,
            // clear/edge query, fresh air or speculative positive gap cannot
            // bootstrap this history. Strict support remains untouched.
            if(!previous.valid||!previous.recovery_seeded||!previous.verified_plane||previous.normal_load<=0||
               !sample.verified_plane||sample.contact.status!=tire::ContactStatus::hit||
               !sample.balance_normal_available||baseline.normal_load[i]<=0||
               sample.separation>=0||previous.separation>=0||sample.separation<=previous.separation||
               previous.recovery_velocity<=0||previous.recovery_seconds+h>.2||
               !tire::contact_basis(sample.contact.normal,sample.contact.forward,basis))return 0;
            Vec face;if(!unit(sample.balance_normal,face)||dot(face,basis.normal)<std::cos(.002)||
               dot(previous.normal,basis.normal)<std::cos(.002)||
               dot(previous.balance_normal,face)<std::cos(.002)||dot(up,face)<.95||dot(face,gravity_up)<.5)return 0;
            const auto plane_delta=sub(sub(sample.contact.point,previous.point),mul(previous.surface_velocity,h));
            if(length(plane_delta)>geometry::wheel_radius||std::abs(dot(plane_delta,previous.normal))>.0025||
               std::abs(dot(plane_delta,basis.normal))>.0025||
               length(sub(sample.contact.surface_velocity,previous.surface_velocity))>1e-6)return 0;
            const auto relative=sub(tire::point_velocity(measured,sample.contact.point),sample.contact.surface_velocity);
            const auto opening=dot(relative,basis.normal),gravity_normal=dot(tuning_.gravity,basis.normal);
            // Existing recovery fraction times one gravity step bounds the
            // unexplained opening; an absolute25mm/s ceiling stays narrow.
            // Run161's first surplus is21.124mm/s; following errors<1.362mm/s.
            const auto allowance=std::min(.025,-gravity_normal*h*tuning_.recovery_fraction);
            if(!finite(relative)||opening<=0||gravity_normal>=0||
               std::abs(opening-previous.recovery_velocity)>allowance)return 0;
            platform=add(platform,sample.contact.surface_velocity);
            if(!eligible_support(measured,sample,actual_support_tolerance,tuning_.maximum_penetration))mask|=1u<<i;
        }
        const auto speed=dot(sub(measured.linear_velocity,mul(platform,.5)),forward);
        if(travel_direction_&&speed*travel_direction_<0)return 0;
        return mask;
    }
    Result step_impl(double h,const tire::Body& measured,const std::array<tire::Wheel,2>& wheels,
                const std::array<ContactSample,2>& samples,Controls controls,unsigned recovery_mask,
                const std::array<SpeculativeNormal,2>& speculative) noexcept {
        Result out;
        auto prior_normals=normals_;
        const auto prior_direction=travel_direction_;
        auto air_reservation=air_reservation_;
        const auto prior_steering=steering_;
        auto pump=pump_;
        auto landing_compliance=landing_compliance_;
        // Commit history only after a valid step. All unavailable/invalid exits
        // invalidate it, so a later sample cannot transport an old ramp normal.
        normals_={};travel_direction_=0;air_reservation_={};steering_={};pump_.reset();landing_compliance_.reset();
        // Run56 only calibrates this native step. This is deliberately NOT a
        // general promise that an arbitrary native rate accepts impulse/h.
        if(!std::isfinite(h)||std::abs(h-verified_native_seconds)>1e-8) {
            out.issue=Issue::unsupported_step;return out;
        }
        if(!detail::tuning_valid(tuning_)||!detail::controls_valid(controls)||!tire::valid(measured)||
           !detail::finite(measured.linear_velocity,50)||!detail::finite(measured.angular_velocity,40)||
           !tire::valid(wheels[0])||!tire::valid(wheels[1]))return out;
        const auto q=detail::normalized(measured.orientation);
        const auto body_up=detail::turn(q,{0,1,0}),body_forward=detail::turn(q,{0,0,1}),body_left=detail::turn(q,{1,0,0});
        const double steer=controls.enabled?std::clamp(controls.steer,-1.0,1.0):0;
        std::array<tire::Contact,2> contacts;
        std::array<NormalHistory,2> next_normals{};
        Vec support_sum{},surface_velocity_sum{};
        unsigned surface_velocity_count=0;
        for(unsigned i=0;i<2;++i) {
            contacts[i]=samples[i].contact;
            if(contacts[i].status==tire::ContactStatus::unavailable) {out.issue=Issue::unavailable_contact;return out;}
            if(contacts[i].status!=tire::ContactStatus::clear&&contacts[i].status!=tire::ContactStatus::hit)return out;
            tire::Basis basis;
            if(!std::isfinite(samples[i].separation)||!tire::contact_basis(contacts[i].normal,contacts[i].forward,basis)||
                !detail::finite(contacts[i].point)||!detail::finite(contacts[i].surface_velocity,50))return out;
            contacts[i].normal=basis.normal;contacts[i].forward=basis.forward;
            if(contacts[i].status==tire::ContactStatus::hit) {
                if(samples[i].separation < -tuning_.maximum_penetration) {out.issue=Issue::penetration_limit;return out;}
                if(samples[i].separation > tuning_.contact_gap)contacts[i].status=tire::ContactStatus::clear;
                else {
                    Vec balance_normal=basis.normal;
                    if(samples[i].balance_normal_available&&
                       (!detail::unit(samples[i].balance_normal,balance_normal)||
                        detail::dot(balance_normal,basis.normal)<=0))return out;
                    surface_velocity_sum=detail::add(surface_velocity_sum,contacts[i].surface_velocity);
                    ++surface_velocity_count;
                    const bool actual=eligible_support(measured,samples[i],actual_support_tolerance,tuning_.maximum_penetration);
                    if(actual)out.grounded=true;
                    bool continuation=!actual&&(recovery_mask&(1u<<i));
                    if(!actual&&!continuation&&controls.enabled&&prior_normals[i].valid&&prior_normals[i].normal_load>0&&
                       samples[i].separation<=actual_support_tolerance) {
                        const auto relative=detail::sub(tire::point_velocity(measured,contacts[i].point),contacts[i].surface_velocity);
                        const double opening=detail::dot(relative,basis.normal),gravity_normal=detail::dot(tuning_.gravity,basis.normal);
                        const double angle=std::atan2(detail::length(detail::cross(prior_normals[i].normal,basis.normal)),
                            std::clamp(detail::dot(prior_normals[i].normal,basis.normal),-1.0,1.0));
                        // A previously loaded near contact can open by a small
                        // solver residual, yet close under gravity within this
                        // single native step. Continue BALANCE only. Fresh air,
                        // speculative skin hits and genuinely departing wheels
                        // cannot acquire or prolong this history.
                        continuation=detail::finite(relative)&&opening>0&&gravity_normal<0&&
                            opening+gravity_normal*h<=0&&angle<=tuning_.curvature_step_limit;
                    }
                    if(actual||continuation) {
                        next_normals[i]={true,basis.normal,0,balance_normal};
                        if(continuation)out.balance_continuation_mask|=1u<<i;
                    }
                    const double bias=std::min(tuning_.recovery_speed_limit,
                        tuning_.recovery_fraction*std::max(0.0,-samples[i].separation)/h);
                    out.recovery_velocity[i]=bias;
                    if(next_normals[i].valid) {
                        auto& history=next_normals[i];history.point=contacts[i].point;
                        history.surface_velocity=samples[i].contact.surface_velocity;
                        history.separation=samples[i].separation;history.recovery_velocity=bias;
                        history.verified_plane=samples[i].verified_plane;
                        history.recovery_seconds=actual?0:prior_normals[i].recovery_seconds+h;
                        history.recovery_seeded=controls.recovery_step_verified&&samples[i].verified_plane&&
                            samples[i].separation<0&&(actual||prior_normals[i].recovery_seeded);
                    }
                    // Explicit bounded constraint bias. This is NOT measured
                    // platform motion: it can inject recovery energy, and never
                    // changes COM/height or invents a terrain normal.
                    // A positive gap permits closing at -gap/h, rather than
                    // holding the chassis above the surface at the ray skin.
                    const double target=bias-std::max(0.0,samples[i].separation)/h;
                    contacts[i].surface_velocity=detail::add(contacts[i].surface_velocity,detail::mul(basis.normal,target));
                }
            }
        }
        unsigned previous_support_mask=0,support_mask=0;
        for(unsigned i=0;i<2;++i) {
            if(prior_normals[i].valid)previous_support_mask|=1u<<i;
            if(next_normals[i].valid)support_mask|=1u<<i;
        }
        const auto surface_velocity=surface_velocity_count?detail::mul(surface_velocity_sum,1.0/surface_velocity_count):Vec{};
        const double speed=detail::dot(detail::sub(measured.linear_velocity,surface_velocity),body_forward);
        const int direction=std::abs(speed)>=.25?(speed>0?1:-1):0;
        if(direction&&prior_direction&&direction!=prior_direction) {
            prior_normals={};
            // A reversal must establish genuine support again before it may
            // carry balance-only contact history in the other direction.
            for(unsigned i=0;i<2;++i)if(out.balance_continuation_mask&(1u<<i))next_normals[i]={};
            out.balance_continuation_mask=0;support_mask=0;
            for(unsigned i=0;i<2;++i)if(next_normals[i].valid)support_mask|=1u<<i;
        }
        // Wheel entry/exit can coincide with a seam or a new support plane.
        // Seed that topology first; it is not a measured angular trajectory.
        if(previous_support_mask!=support_mask)prior_normals={};
        out.balance_support_mask=support_mask;
        for(const auto& support:next_normals)if(support.valid)support_sum=detail::add(support_sum,support.balance_normal);
        if(support_mask&&std::abs(speed)>=tuning_.curvature_min_speed) {
            Vec transport{};unsigned count=0;
            for(unsigned i=0;i<2;++i)if(next_normals[i].valid&&prior_normals[i].valid) {
                const auto axis=detail::cross(prior_normals[i].balance_normal,next_normals[i].balance_normal);
                const auto sine=detail::length(axis);
                const auto angle=std::atan2(sine,std::clamp(detail::dot(prior_normals[i].balance_normal,next_normals[i].balance_normal),-1.0,1.0));
                // A seam/normal discontinuity is not measured ramp curvature.
                if(angle>tuning_.curvature_step_limit)continue;
                const auto rate=sine>1e-10?detail::mul(axis,std::min(angle/h,tuning_.curvature_rate_limit)/sine):Vec{};
                transport=detail::add(transport,rate);++count;
            }
            if(count)out.desired_support_angular_velocity=detail::mul(transport,1.0/count);
        }
        out.requested_front_steer_angle=out.front_steer_angle=
            -steer*tuning_.steer_limit/(1+std::abs(speed)/tuning_.steer_speed_scale);
        // Same fitted tilted steerer as the source fork/wheel presentation. The
        // existing tire basis projects this heading onto the measured plane;
        // physical camber/steering joints remain outside the zero-camber model.
        Vec steering_axis;
        if(!detail::unit(tire::detail::turn(tire::detail::rotation(measured),
            Vec{geometry::steering_axis[0],geometry::steering_axis[1],geometry::steering_axis[2]}),steering_axis))return out;
        Vec support_up{},support_forward{};
        const bool balance_frame=support_mask&&detail::unit(support_sum,support_up)&&
            detail::unit(detail::sub(body_forward,detail::mul(support_up,detail::dot(body_forward,support_up))),support_forward);
        // Advance one shared mechanical actuator only on qualified support.
        // Cold state has no published angle to invent; it retains first-step
        // behavior. Valid air steps keep their existing raw bar deflection,
        // which becomes the next supported step's actual starting angle.
        if(controls.enabled&&balance_frame&&detail::dot(body_up,support_up)>.2&&prior_steering.valid) {
            const double gain=tuning_.steer_limit/(1+std::abs(speed)/tuning_.steer_speed_scale);
            const double rate_delta=detail::supported_steering_rate(speed)*gain*h;
            // Current Streets' authored physical joint response is6/s. Here
            // it softens final approach to the same shared tire/bank command;
            // combining that response with the rate cap is our adaptation.
            const double joint_delta=std::clamp(6.0*h,0.0,1.0)*
                std::abs(out.requested_front_steer_angle-prior_steering.angle);
            const double delta=std::min(rate_delta,joint_delta);
            out.front_steer_angle=std::clamp(out.requested_front_steer_angle,
                prior_steering.angle-delta,prior_steering.angle+delta);
        }
        const double filtered_steer_candidate=out.front_steer_angle;
        // A fork angle and an achievable lean have to describe the same turn.
        // Previously8m/s full input asked for~46deg of lean while the balance
        // target stopped at28.6deg, so tire steering fought the riding assist.
        // Measure the projected heading produced by the tilted steerer rather
        // than treating its mechanical angle as a vertical-axis yaw angle.
        const auto heading_angle=[&](double deflection,double& angle) {
            tire::Basis front;
            if(!balance_frame||!tire::contact_basis(support_up,
                detail::around(contacts[0].forward,steering_axis,deflection),front))return false;
            angle=std::atan2(detail::dot(support_up,detail::cross(support_forward,front.forward)),
                std::clamp(detail::dot(support_forward,front.forward),-1.0,1.0));
            return std::isfinite(angle)&&std::abs(angle)<1.4;
        };
        if(balance_frame&&detail::dot(body_up,support_up)>.2) {
            double heading{};
            if(!heading_angle(out.front_steer_angle,heading))return out;
            const double maximum_heading=std::atan2(detail::length(tuning_.gravity)*tuning_.wheelbase*
                std::tan(tuning_.lean_limit),speed*speed);
            if(std::abs(heading)>maximum_heading) {
                // Find the largest admissible fraction of this actual input.
                // The queried support and all collision impulses are unchanged.
                double low=0,high=1;
                for(unsigned iteration=0;iteration<40;++iteration) {
                    const double middle=(low+high)*.5;double candidate{};
                    if(!heading_angle(filtered_steer_candidate*middle,candidate))return out;
                    if(std::abs(candidate)<=maximum_heading)low=middle;else high=middle;
                }
                out.front_steer_angle=filtered_steer_candidate*low;
                out.steering_limited=true;
                if(!heading_angle(out.front_steer_angle,heading))return out;
            }
            out.turning_lean_angle=std::atan(-speed*speed*std::tan(heading)/
                (detail::length(tuning_.gravity)*tuning_.wheelbase));
        }
        contacts[0].forward=detail::around(contacts[0].forward,steering_axis,out.front_steer_angle);
        const auto clamp01=[](double v){return std::clamp(v,0.0,1.0);};
        // Taper motor torque rather than clamping measured speed. Include rear
        // cadence so a lifted/free rear wheel cannot spin up forever either.
        // Surface-relative COM speed keeps this invariant under moving ground.
        const double motor_speed=std::max({0.0,speed,wheels[1].angular_speed*wheels[1].radius});
        const double pedal_scale=clamp01((tuning_.pedal_cutoff-motor_speed)/(tuning_.pedal_cutoff-tuning_.pedal_fade_start));
        const double pedal=controls.enabled?clamp01(controls.pedal)*pedal_scale:0,brake=controls.enabled?clamp01(controls.brake):0;
        const bool braking=brake>0&&(tuning_.front_brake_torque>0||tuning_.rear_brake_torque>0);
        const double pitch=controls.enabled?clamp01(controls.nose_manual)-clamp01(controls.manual):0;
        Vec assist_torque{},world_up;
        if(!detail::unit(detail::mul(tuning_.gravity,-1),world_up))return out;
        if(support_mask) {
            if(!balance_frame)return out;
            out.balance_reference_up=support_up;
            if(detail::dot(body_up,support_up)>.2) {
                const double lean=out.turning_lean_angle;
                const auto lateral=detail::cross(support_up,support_forward);
                auto desired=detail::around(support_up,lateral,pitch*tuning_.manual_pitch);
                desired=detail::around(desired,support_forward,lean);
                out.desired_balance_up=desired;
                const auto tilt_omega=detail::sub(measured.angular_velocity,
                    detail::mul(support_up,detail::dot(measured.angular_velocity,support_up)));
                const auto desired_tilt=detail::sub(out.desired_support_angular_velocity,
                    detail::mul(support_up,detail::dot(out.desired_support_angular_velocity,support_up)));
                assist_torque=detail::limited(detail::sub(detail::mul(detail::cross(body_up,desired),tuning_.balance_stiffness),
                    detail::mul(detail::sub(tilt_omega,desired_tilt),tuning_.balance_damping)),tuning_.balance_torque_limit);
                out.balance_torque=assist_torque;
            }
        } else if(controls.enabled) {
            assist_torque=detail::add(detail::mul(world_up,-std::clamp(controls.air_yaw,-1.0,1.0)*tuning_.air_yaw_torque),
                detail::mul(body_left,pitch*tuning_.air_pitch_torque));
            out.air_control_torque=assist_torque;
        }
        const bool rotation_intent=controls.rotation_reserved||controls.manual!=0||controls.nose_manual!=0||
            controls.steer!=0||controls.air_yaw!=0||controls.brake>0;
        // Air braking transfers rotor momentum into chassis pitch. Preserve its
        // deliberate reaction after release, including a last-ground request.
        const bool launch_intent=controls.rotation_reserved||controls.manual!=0||controls.nose_manual!=0||controls.brake>0;
        const bool neutral_flight=air_reservation.observe(controls.recovery_step_verified,controls.enabled,
            false,rotation_intent,launch_intent);
        out.air_rotation_reserved=air_reservation.reserved;
        // Any queried force contact (including speculative hits and edges)
        // retains its original response. Damping begins only in known-clear
        // flight; balance continuation never establishes/clears its reservation.
        if(neutral_flight&&contacts[0].status==tire::ContactStatus::clear&&
           contacts[1].status==tire::ContactStatus::clear) {
            out.air_damping_torque=detail::neutral_air_torque(measured,h,tuning_.neutral_air_damping_rate,
                std::min(tuning_.air_pitch_torque,tuning_.air_yaw_torque));
            out.neutral_air_damping_active=detail::length(out.air_damping_torque)>0;
            assist_torque=detail::add(assist_torque,out.air_damping_torque);
        }
        auto prediction=measured;
        const auto gravity_delta=detail::mul(tuning_.gravity,h);
        prediction.linear_velocity=detail::add(prediction.linear_velocity,gravity_delta); // ONCE, native still owns actual gravity
        // Qualified support is the original strict measured contract, NEVER the
        // balance continuation/recovery mask. Hop/preload cancels the window.
        out.pump=pump.step(h,{controls.enabled,controls.pump_operator_available,out.grounded,
            controls.pump_inhibited||controls.hop_strength>0,controls.ground_pump,
            controls.manual,controls.nose_manual,measured.linear_velocity[1],controls.pedal});
        if(!out.pump.valid)return Result{};
        out.pump_force={0,-measured.mass*out.pump.down_acceleration,0};
        // At COM this has no direct torque. Tire impulses below see it NOW;
        // base_linear automatically retains it after removing only gravity/hop.
        prediction.linear_velocity=detail::add(prediction.linear_velocity,
            detail::mul(out.pump_force,h/measured.mass));
        prediction.angular_velocity=detail::add(prediction.angular_velocity,detail::tensor(measured,detail::mul(assist_torque,h),true));
        const bool new_event=controls.hop_event_id>last_hop_event_;
        if(new_event&&controls.enabled&&out.grounded&&clamp01(controls.hop_strength)>0) {
            out.hop_impulse=detail::mul(world_up,measured.mass*tuning_.hop_delta_speed*clamp01(controls.hop_strength));
            prediction.linear_velocity=detail::add(prediction.linear_velocity,detail::mul(out.hop_impulse,1/measured.mass));
            out.hop_applied=true;out.hop_event_id=controls.hop_event_id;
        }
        out.wheels=wheels;
        // Apply motor once. Braking is a bounded constraint coupled to contact,
        // not a free-wheel stop that traction immediately spins up again.
        for(unsigned i=0;i<2;++i) {
            auto free=contacts[i];free.status=tire::ContactStatus::clear;
            const tire::Controls actuation{i==1?pedal*tuning_.pedal_torque:0,0};
            if(!tire::solve(h,prediction,out.wheels[i],free,actuation,tuning_.friction).valid)return Result{};
        }
        const auto actuated=prediction;
        out.pre_contact_prediction=actuated;out.pre_contact_prediction_available=true;
        const auto actuated_wheels=out.wheels;
        // Admission consumes this exact accepted pre-contact state. No observer
        // presence, positive gap or copied old plane alone creates a row.
        const auto same_prediction=[](const tire::Body& a,const tire::Body& b) noexcept {
            return a.mass==b.mass&&a.inertia_body==b.inertia_body&&a.orientation==b.orientation&&
                a.center_of_mass==b.center_of_mass&&a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity;
        };
        for(unsigned i=0;i<2;++i) {
            const auto& proof=speculative[i];auto& issue=out.speculative_issues[i];
            if(!proof.selected)continue;
            if(!proof.qualified){issue=SpeculativeIssue::unqualified;continue;}
            if(!controls.recovery_step_verified){issue=SpeculativeIssue::unverified_step;continue;}
            if(!controls.enabled){issue=SpeculativeIssue::disabled_controls;continue;}
            if(out.hop_applied||controls.hop_strength>0){issue=SpeculativeIssue::hop_reserved;continue;}
            if(samples[i].contact.status!=tire::ContactStatus::clear||contacts[i].status!=tire::ContactStatus::clear||
               !samples[i].verified_plane){issue=SpeculativeIssue::not_clear_plane;continue;}
            if(!std::isfinite(proof.separation)||proof.point!=samples[i].contact.point||
               proof.normal!=samples[i].contact.normal||proof.separation!=samples[i].separation){issue=SpeculativeIssue::geometry_changed;continue;}
            if(!same_prediction(proof.prediction,actuated)){issue=SpeculativeIssue::prediction_changed;continue;}
            if(samples[i].contact.surface_velocity!=Vec{}){issue=SpeculativeIssue::nonstatic_surface;continue;}
            if(proof.separation<=std::max(.03,tuning_.contact_gap)||proof.separation>.25){issue=SpeculativeIssue::gap_outside;continue;}
            const auto vn=detail::dot(tire::point_velocity(actuated,contacts[i].point),contacts[i].normal);
            if(!std::isfinite(vn)||vn>=-proof.separation/h){issue=SpeculativeIssue::not_crossing;continue;}
            out.speculative_normal_mask|=1u<<i;issue=SpeculativeIssue::admitted;
        }
        struct SpeculativeEffect {Vec linear{},angular{};double impulse{};};
        std::array<SpeculativeEffect,2> speculative_effects{};

        // Only current near/inside-plane static tire contacts can use this
        // private constitutive law. Edges, skin-only air and stale steps retain
        // the old path; this never changes support/history eligibility.
        unsigned qualified_compliance_mask=0;
        if((controls.contact_compliance_available||controls.landing_contact_compliance_selected)&&controls.recovery_step_verified)
            for(unsigned i=0;i<2;++i)if(contacts[i].status==tire::ContactStatus::hit&&samples[i].verified_plane&&
                samples[i].contact.surface_velocity==Vec{}&&
                contact_compliance::domain(h,samples[i].separation,tuning_.recovery_fraction))
                qualified_compliance_mask|=1u<<i;
        // Existing pump qualification supplies the event. Strict support,
        // pump/hop/landing/scoring rules and all constitutive constants stay
        // unchanged. A support blink cannot switch a loaded spring to rigid.
        const bool genuinely_unloaded=contacts[0].status==tire::ContactStatus::clear&&
            contacts[1].status==tire::ContactStatus::clear&&
            samples[0].separation>contact_compliance::maximum_positive_gap&&
            samples[1].separation>contact_compliance::maximum_positive_gap;
        out.landing_compliance=landing_compliance.step({controls.landing_contact_compliance_selected,
            controls.recovery_step_verified,controls.enabled&&controls.pump_operator_available,
            out.pump.automatic_armed,qualified_compliance_mask!=0,genuinely_unloaded});
        if(controls.contact_compliance_available&&
           (!controls.landing_contact_compliance_selected||out.landing_compliance.active))
            out.compliant_normal_mask=qualified_compliance_mask;
        out.compliance_attempted_mask=out.compliant_normal_mask;
        struct ContactEffect {
            Vec linear{},angular{};double spin{},brake{};tire::Result solved;
            Vec normal_linear{},normal_angular{};
        };
        std::array<ContactEffect,2> effects;
        const unsigned iterations=braking?tuning_.brake_iterations:tuning_.contact_iterations;
        for(unsigned iteration=0;iteration<iterations;++iteration)for(unsigned order=0;order<2;++order) {
            const unsigned i=(iteration+order)%2;
            if(out.speculative_normal_mask&(1u<<i)) {
                // Normal-only row participates in the SAME alternating coupled
                // iterations. Retain this wheel's preceding brake response so
                // the normal row can correct brake-induced pitch velocity.
                auto& effect=speculative_effects[i];
                prediction.linear_velocity=detail::sub(prediction.linear_velocity,effect.linear);
                prediction.angular_velocity=detail::sub(prediction.angular_velocity,effect.angular);
                const auto before_speculative=prediction;const auto& contact=contacts[i];
                const auto arm=detail::sub(contact.point,prediction.center_of_mass);
                const auto inverse_mass=detail::dot(contact.normal,tire::detail::response(prediction,arm,contact.normal));
                const auto vn=detail::dot(tire::point_velocity(prediction,contact.point),contact.normal);
                if(!std::isfinite(inverse_mass)||inverse_mass<=0)return Result{};
                const auto impulse=std::max(0.0,(-samples[i].separation/h-vn)/inverse_mass);
                if(!std::isfinite(impulse)||!tire::apply_impulse(prediction,contact.point,detail::mul(contact.normal,impulse)))return Result{};
                effect={detail::sub(prediction.linear_velocity,before_speculative.linear_velocity),
                    detail::sub(prediction.angular_velocity,before_speculative.angular_velocity),impulse};
            }
            // During braking, first replace only the accumulated normal row
            // while retaining this wheel's prior brake/traction. Then replace
            // its joint tangent/brake block against that updated normal load.
            // Otherwise normal-first/full-wheel replacement cannot correct the
            // wheel's own brake-induced normal velocity, even at convergence.
            // With no brake request, retain the original whole-contact path.
            Vec normal_linear{},normal_angular{};double normal_impulse=0;
            const bool compliant=(out.compliant_normal_mask&(1u<<i))!=0;
            const bool separated_normal=braking||compliant;
            if(separated_normal) {
                prediction.linear_velocity=detail::sub(prediction.linear_velocity,effects[i].normal_linear);
                prediction.angular_velocity=detail::sub(prediction.angular_velocity,effects[i].normal_angular);
                const auto before_normal=prediction;
                if(contacts[i].status==tire::ContactStatus::hit) {
                    tire::Basis basis;
                    if(!tire::contact_basis(contacts[i].normal,contacts[i].forward,basis))return Result{};
                    const auto arm=detail::sub(contacts[i].point,prediction.center_of_mass);
                    const double inverse_mass=detail::dot(basis.normal,tire::detail::response(prediction,arm,basis.normal));
                    const auto relative=detail::sub(tire::point_velocity(prediction,contacts[i].point),contacts[i].surface_velocity);
                    if(!std::isfinite(inverse_mass)||inverse_mass<=0)return Result{};
                    normal_impulse=std::max(0.0,-detail::dot(relative,basis.normal)/inverse_mass);
                    if(compliant) {
                        const auto actual_relative=detail::sub(tire::point_velocity(prediction,contacts[i].point),
                            samples[i].contact.surface_velocity);
                        const double legacy_target=out.recovery_velocity[i]-std::max(0.,samples[i].separation)/h;
                        const auto model=contact_compliance::solve(h,samples[i].separation,tuning_.recovery_fraction,
                            inverse_mass,detail::dot(actual_relative,basis.normal),legacy_target);
                        if(!model.available)return Result{}; // current admitted domain must remain internally valid
                        normal_impulse=model.impulse;out.compliance[i]=model;
                    }

                    if(!std::isfinite(normal_impulse)||!tire::apply_impulse(prediction,contacts[i].point,
                        detail::mul(basis.normal,normal_impulse)))return Result{};
                }
                normal_linear=detail::sub(prediction.linear_velocity,before_normal.linear_velocity);
                normal_angular=detail::sub(prediction.angular_velocity,before_normal.angular_velocity);
            }
            prediction.linear_velocity=detail::sub(prediction.linear_velocity,effects[i].linear);
            prediction.angular_velocity=detail::sub(prediction.angular_velocity,effects[i].angular);
            out.wheels[i].angular_speed-=effects[i].spin;
            const auto before=prediction;const auto before_spin=out.wheels[i].angular_speed;
            tire::Result solved;double brake_impulse=0;
            const double brake_torque=brake*(i==0?tuning_.front_brake_torque:tuning_.rear_brake_torque);
            if(separated_normal) {
                const auto block=coupled_brake::solve_tangent(h,prediction,out.wheels[i],contacts[i],normal_impulse,brake_torque,tuning_.friction);
                solved=block.contact;brake_impulse=block.brake_impulse;
            } else solved=tire::solve(h,prediction,out.wheels[i],contacts[i],{},tuning_.friction);
            if(!solved.valid)return Result{};
            effects[i]={detail::sub(prediction.linear_velocity,before.linear_velocity),
                detail::sub(prediction.angular_velocity,before.angular_velocity),out.wheels[i].angular_speed-before_spin,
                brake_impulse,solved,normal_linear,normal_angular};
        }
        const auto base_linear=detail::sub(detail::mul(detail::sub(detail::sub(actuated.linear_velocity,measured.linear_velocity),
            gravity_delta),measured.mass),out.hop_impulse);
        const auto base_angular=detail::tensor(measured,detail::sub(actuated.angular_velocity,measured.angular_velocity),false);
        const auto contact_linear=detail::mul(detail::sub(prediction.linear_velocity,actuated.linear_velocity),measured.mass);
        const auto contact_angular=detail::tensor(measured,detail::sub(prediction.angular_velocity,actuated.angular_velocity),false);
        // The native capsule manifold absorbs any remaining hard impact. This
        // is an under-relaxed velocity-contact response, never a pose repair.
        // A small interior margin survives the adapter's double-to-float cast.
        const double force_budget=tuning_.force_limit*h*(1-1e-6);
        const double torque_budget=tuning_.torque_limit*h*(1-1e-6);
        double fraction=std::min({detail::contact_fraction(base_linear,contact_linear,force_budget),
            detail::contact_fraction(detail::add(base_linear,out.hop_impulse),contact_linear,force_budget),
            detail::contact_fraction(base_angular,contact_angular,torque_budget)});
        for(unsigned i=0;i<2;++i)fraction=std::min(fraction,detail::contact_fraction(
            {actuated_wheels[i].angular_speed,0,0},
            {braking?out.wheels[i].angular_speed-actuated_wheels[i].angular_speed:effects[i].spin,0,0},tuning_.wheel_speed_limit));
        if(fraction<0) {Result rejected;rejected.issue=Issue::output_limit;return rejected;}
        out.contact_scale=fraction;
        for(unsigned i=0;i<2;++i) {
            const auto& effect=speculative_effects[i];
            out.speculative_normal_impulse[i]=fraction*effect.impulse;
            out.speculative_normal_force[i]=out.speculative_normal_impulse[i]/h;
            out.speculative_linear_impulse=detail::add(out.speculative_linear_impulse,detail::mul(effect.linear,measured.mass*fraction));
            out.speculative_angular_impulse=detail::add(out.speculative_angular_impulse,detail::mul(detail::tensor(measured,effect.angular,false),fraction));
        }

        out.linear_impulse=detail::add(base_linear,detail::mul(contact_linear,fraction));
        out.angular_impulse=detail::add(base_angular,detail::mul(contact_angular,fraction));
        out.force=detail::mul(out.linear_impulse,1/h);out.torque=detail::mul(out.angular_impulse,1/h);
        out.motor_torque=detail::sub(detail::mul(base_angular,1/h),assist_torque);
        out.contact_torque=detail::mul(contact_angular,fraction/h);
        for(unsigned i=0;i<2;++i) {
            out.wheels[i].angular_speed=actuated_wheels[i].angular_speed+
                fraction*(braking?out.wheels[i].angular_speed-actuated_wheels[i].angular_speed:effects[i].spin);
            out.brake_impulse[i]=fraction*effects[i].brake;
            out.normal_impulse[i]=fraction*effects[i].solved.normal_impulse;out.normal_load[i]=out.normal_impulse[i]/h;
            out.longitudinal_impulse[i]=fraction*effects[i].solved.longitudinal_impulse;
            out.lateral_impulse[i]=fraction*effects[i].solved.lateral_impulse;
        }
        if(out.compliant_normal_mask) {
            auto submitted_prediction=actuated;
            submitted_prediction.linear_velocity=detail::add(actuated.linear_velocity,detail::mul(contact_linear,fraction/measured.mass));
            submitted_prediction.angular_velocity=detail::add(actuated.angular_velocity,
                detail::tensor(measured,detail::mul(contact_angular,fraction),true));
            for(unsigned i=0;i<2;++i)if(out.compliant_normal_mask&(1u<<i)) {
                const auto actual_relative=detail::sub(tire::point_velocity(submitted_prediction,contacts[i].point),samples[i].contact.surface_velocity);
                out.compliant_predicted_gap[i]=samples[i].separation+h*detail::dot(actual_relative,contacts[i].normal);
                out.compliant_deflection_satisfied[i]=out.compliant_predicted_gap[i]>=-contact_compliance::maximum_deflection-1e-7;
            }
        }
        if(!detail::finite(out.force)||!detail::finite(out.torque)||
           detail::length(out.force)>tuning_.force_limit||
           detail::length(detail::mul(detail::add(out.linear_impulse,out.hop_impulse),1/h))>tuning_.force_limit||
           detail::length(out.torque)>tuning_.torque_limit||
           std::abs(out.wheels[0].angular_speed)>tuning_.wheel_speed_limit||std::abs(out.wheels[1].angular_speed)>tuning_.wheel_speed_limit) {
            Result rejected;rejected.issue=Issue::output_limit;return rejected;
        }
        // Commit only event bookkeeping. An airborne/disabled new ID is consumed
        // without a jump, so holding it through a later landing cannot replay it.
        if(new_event)last_hop_event_=controls.hop_event_id;
        if(support_mask&&!out.hop_applied&&controls.enabled) {
            for(unsigned i=0;i<2;++i)next_normals[i].normal_load=out.normal_load[i];
            normals_=next_normals;travel_direction_=direction?direction:prior_direction;
        }
        // A strict point that is already separating can have zero actual
        // normal impulse. Only a strict, currently loaded support establishes
        // a new flight; a speculative/continuation hit cannot clear intent.
        bool loaded_recontact=false;
        for(unsigned i=0;i<2;++i)loaded_recontact=loaded_recontact||
            (out.normal_load[i]>0&&eligible_support(measured,samples[i],actual_support_tolerance,tuning_.maximum_penetration));
        if(loaded_recontact&&!out.hop_applied)air_reservation.observe(controls.recovery_step_verified,
            controls.enabled,true,rotation_intent,launch_intent);
        air_reservation_=air_reservation;out.air_rotation_reserved=air_reservation.reserved;
        // Store achieved post-envelope angle, avoiding hidden command windup.
        // The speed/lean safety envelope takes priority over the slew bound.
        if(controls.enabled)steering_={true,out.front_steer_angle};
        pump_=pump;landing_compliance_=landing_compliance;
        out.valid=true;out.issue=Issue::none;return out;
    }
};
} // namespace dingosdk::bmx::chassis_control
