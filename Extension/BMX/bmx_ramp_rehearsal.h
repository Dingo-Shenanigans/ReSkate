#pragma once
#include "Engine/Game/BMX/bmx_input.h"
#include <array>
#include <cstdint>

// Bounded synthetic-input experiment. The driver observes native measurements;
// it never predicts/integrates a bike pose, teleports, or supplies hop/spin input.
namespace dingosdk::bmx::ramp_rehearsal {
using Vec=std::array<float,3>;
inline constexpr std::uint32_t synthetic_device=0x424d5804u;
inline constexpr double maximum_seconds=9.05; // one rounded 60 Hz terminal observation is allowed
// Authored smaller-bike rehearsal input, not a physical motor/default change.
// Native196 overran the unchanged5m corridor; .8 is a trial, not a predicted pass.
inline constexpr double supported_runup_pedal=.8;
// The cleared landing corridor ends at progress6. A pitched front capsule can
// extend .96m beyond the measured bike root, so keep the root inside5m.
inline constexpr double maximum_route_progress=5;
inline constexpr double ballistic_force_tolerance=.01; // N; previous/current owned submissions must be clear
inline constexpr double ballistic_torque_tolerance=.01; // N*m
// Separate natural-flight contract: gravity-only COM translation may coexist
// with the producer's bounded, dissipative neutral angular torque. This is
// not zero-owned-torque ballistic rotation. Matches its current 160 N*m cap.
inline constexpr double passive_air_torque_limit=160;
inline constexpr double ballistic_velocity_tolerance=.001; // m/s per adjacent native interval
inline constexpr Vec verified_gravity{0,-9.8f,0};
// Mount inside the measured corridor, leaving the existing rollback guard
// available for physical motion. The native teleport/dismount is not exact.
inline bool entry_position_valid(Vec position, Vec start, Vec forward) noexcept {
    double along=0, lateral=0, norm=0;
    for(unsigned i=0;i<3;++i) {
        if(!std::isfinite(position[i])||!std::isfinite(start[i])||!std::isfinite(forward[i]))return false;
        norm+=double(forward[i])*forward[i];
        along+=(double(position[i])-start[i])*forward[i];
    }
    lateral=(double(position[0])-start[0])*forward[2]-(double(position[2])-start[2])*forward[0];
    return std::abs(norm-1)<=.001&&std::abs(forward[1])<=.001&&along>=0&&along<=1&&std::abs(lateral)<=.09;
}
struct Route {
    Vec origin{},forward{0,0,1},start{}; // horizontal forward points UP the measured ramp
    double floor_y{},lip_height{},lip_progress{}; // height above floor; progress from origin along forward
};
struct Observation {
    bool valid{},grounded{}; // grounded must use actual eligible support, not the ray skin
    double elapsed{},step_seconds{};
    Vec root{},com{},velocity{};
    double body_up_alignment{1},support_up_alignment{1};
    std::uint64_t serial{},body_token{};
    std::array<bool,2> actual_wheel_support{};
    Vec force{},torque{}; // recorded owned submissions for this measured sample's next interval
    Vec air_damping_torque{},angular_velocity{};
    std::array<bool,2> queried_clear{}; // current admitted wheel queries, not actual support alone
    bool neutral_air_damping_active{},air_rotation_reserved{},hop_applied{};
    Vec air_control_torque{},motor_torque{},contact_torque{},balance_torque{};
};
struct Tally {
    bool completed{},passed{},ascent{},flight{},landed{};
    const char* phase="neutral";
    const char* issue="not_started";
    double elapsed{},progress{},lateral_error{},com_rise{},max_rise{},flight_seconds{},landing_support_seconds{},brake_seconds{},final_speed{};
    double first_ascent_seconds{},takeoff_seconds{},landing_seconds{},start_com_y{};
    std::uint64_t samples{},last_serial{},body_token{};
    double ballistic_seconds{},maximum_ballistic_seconds{},ballistic_velocity_error{},maximum_ballistic_velocity_error{};
    double natural_flight_seconds{},maximum_natural_flight_seconds{};
    std::uint64_t passive_angular_intervals{};
};
class Driver {
    Route route_;
    Tally tally_;
    bool configured_{},seen_{},last_grounded_{},touchdown_{},brake_sent_{},launch_{};
    double previous_elapsed_{},control_elapsed_{},start_progress_{};
    Vec previous_com_{};
    Observation previous_{};
    static bool finite(Vec v) noexcept {
        for(float x:v)if(!std::isfinite(x)||std::abs(x)>1e6)return false;
        return true;
    }
    static double dot(Vec a,Vec b) noexcept {double result=0;for(unsigned i=0;i<3;++i)result+=double(a[i])*b[i];return result;}
    static Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
    static double length(Vec v) noexcept {return std::sqrt(dot(v,v));}
    static bool qualified_owned_torque(const Observation& s) noexcept {
        if(s.air_rotation_reserved||s.hop_applied||length(s.air_control_torque)>ballistic_torque_tolerance||
           length(s.motor_torque)>ballistic_torque_tolerance||length(s.contact_torque)>ballistic_torque_tolerance||
           length(s.balance_torque)>ballistic_torque_tolerance)return false;
        if(!s.neutral_air_damping_active)
            return length(s.air_damping_torque)==0&&length(s.torque)<=ballistic_torque_tolerance;
        const double magnitude=length(s.air_damping_torque);
        // Qualification uses observed attribution, never an inferred amount
        // subtracted from arbitrary total torque. Residual player/contact/
        // balance/motor torque still fails the original torque tolerance.
        return !s.grounded&&s.queried_clear[0]&&s.queried_clear[1]&&
            magnitude>0&&magnitude<=passive_air_torque_limit&&
            dot(s.angular_velocity,s.air_damping_torque)<0&&
            length(sub(s.torque,s.air_damping_torque))<=ballistic_torque_tolerance;
    }
    static bool natural_interval(const Observation& prior,const Observation& sample) noexcept {
        if(prior.grounded||sample.grounded||length(prior.force)>ballistic_force_tolerance||
           length(sample.force)>ballistic_force_tolerance||!qualified_owned_torque(prior)||!qualified_owned_torque(sample))return false;
        // A passive interval requires BOTH adjacent samples to have known-
        // clear queries. A preceding speculative lip hit cannot seed it.
        return !(prior.neutral_air_damping_active||sample.neutral_air_damping_active)||
            (prior.queried_clear[0]&&prior.queried_clear[1]&&sample.queried_clear[0]&&sample.queried_clear[1]);
    }
    void fail(const char* issue) noexcept {
        tally_.completed=true;tally_.passed=false;tally_.phase="failed";tally_.issue=issue;
    }
public:
    Driver() noexcept=default;
    explicit Driver(Route route) noexcept {reset(route);}
    bool reset(Route route) noexcept {
        route_=route;tally_={};configured_=seen_=last_grounded_=touchdown_=brake_sent_=launch_=false;
        previous_elapsed_=control_elapsed_=start_progress_=0;previous_com_={};previous_={};
        if(!finite(route.origin)||!finite(route.forward)||!finite(route.start)||
           std::abs(dot(route.forward,route.forward)-1)>.001||std::abs(route.forward[1])>.001||
           !std::isfinite(route.floor_y)||std::abs(route.floor_y)>1e6||
           !std::isfinite(route.lip_height)||route.lip_height<.3||route.lip_height>3||
           !std::isfinite(route.lip_progress)||std::abs(route.lip_progress)>10) {
            fail("invalid_measured_route");return false;
        }
        start_progress_=dot(sub(route.start,route.origin),route.forward);
        if(start_progress_>=route.lip_progress-2||start_progress_<route.lip_progress-25) {
            fail("invalid_ramp_runup");return false;
        }
        configured_=true;tally_.issue="waiting_native_measurement";return true;
    }
    const Tally& tally() const noexcept {return tally_;}
    RawInput controls(double elapsed) noexcept {
        RawInput input;input.available=true;input.focused=true;input.device=synthetic_device;
        if(!configured_)return input;
        if(!std::isfinite(elapsed)||elapsed<0||elapsed+1e-7<control_elapsed_) {
            fail("invalid_native_control_time");return input;
        }
        control_elapsed_=elapsed;
        if(!tally_.completed&&elapsed>=maximum_seconds)fail(!tally_.ascent?"no_verified_ramp_ascent":
            !tally_.flight?"no_qualified_natural_flight":!tally_.landed?"no_settled_ramp_landing":"did_not_stop_in_time");
        if(tally_.completed) {input.brake=1;return input;}
        if(elapsed<.5) {tally_.phase="neutral";return input;}
        if(tally_.landed) {input.brake=1;brake_sent_=true;tally_.phase="braking";return input;}
        if(launch_) {tally_.phase=touchdown_?"landing_settle":"natural_flight";return input;}
        // A stray pre-lip airborne sample receives no motor couple either.
        if(seen_&&last_grounded_)input.pedal=supported_runup_pedal;
        tally_.phase=tally_.ascent?"ramp_ascent":"runup";return input;
    }
    const Tally& observe(const Observation& sample) noexcept {
        if((tally_.completed&&!tally_.passed)||!configured_)return tally_;
        if(!sample.valid||!std::isfinite(sample.elapsed)||sample.elapsed<0||
           !std::isfinite(sample.step_seconds)||sample.step_seconds<=0||sample.step_seconds>.10||
           !finite(sample.root)||!finite(sample.com)||!finite(sample.velocity)||!finite(sample.force)||!finite(sample.torque)||
           !finite(sample.air_damping_torque)||!finite(sample.angular_velocity)||
           !finite(sample.air_control_torque)||!finite(sample.motor_torque)||!finite(sample.contact_torque)||!finite(sample.balance_torque)||
           sample.grounded!=(sample.actual_wheel_support[0]||sample.actual_wheel_support[1])||!sample.serial||!sample.body_token||
           !std::isfinite(sample.body_up_alignment)||std::abs(sample.body_up_alignment)>1.000001||
           !std::isfinite(sample.support_up_alignment)||std::abs(sample.support_up_alignment)>1.000001) {
            fail("invalid_native_observation");return tally_;
        }
        if(seen_) {
            if(sample.body_token!=tally_.body_token) {fail("native_body_owner_changed");return tally_;}
            if(sample.serial==tally_.last_serial)return tally_;
            if(sample.serial!=tally_.last_serial+1||sample.elapsed<=previous_elapsed_||
               std::abs(sample.elapsed-previous_elapsed_-sample.step_seconds)>1e-5) {
                fail("native_observation_discontinuity");return tally_;
            }
            if(length(sub(sample.com,previous_com_))>20*sample.step_seconds+.10) {
                fail("native_position_discontinuity");return tally_;
            }
        } else {
            if(sample.elapsed>.5) {fail("missing_neutral_baseline");return tally_;}
            tally_.body_token=sample.body_token;tally_.start_com_y=sample.com[1];
        }
        const auto prior=previous_;
        const bool was_both=seen_&&prior.actual_wheel_support[0]&&prior.actual_wheel_support[1];
        const bool both=sample.actual_wheel_support[0]&&sample.actual_wheel_support[1];
        seen_=true;previous_elapsed_=sample.elapsed;previous_com_=sample.com;previous_=sample;last_grounded_=sample.grounded;
        tally_.elapsed=sample.elapsed;tally_.last_serial=sample.serial;++tally_.samples;
        tally_.progress=dot(sub(sample.root,route_.origin),route_.forward);
        const Vec right{route_.forward[2],0,-route_.forward[0]};
        tally_.lateral_error=dot(sub(sample.root,route_.start),right);
        tally_.final_speed=length(sample.velocity);
        // Only .04m of rollback keeps the .96m rear capsule extent inside the
        // extra1m of floor verified behind the initial root.
        if(std::abs(tally_.lateral_error)>.4||tally_.progress<start_progress_-.04||
           tally_.progress>maximum_route_progress||length(sub(sample.root,route_.start))>30||
           sample.root[1]<route_.floor_y-.5||sample.root[1]>route_.floor_y+route_.lip_height+4||
           tally_.final_speed>20) {fail("bounded_route_or_motion_violation");return tally_;}
        // The runtime retains braking until the fixed rehearsal deadline. A
        // provisional early pass must survive those remaining measurements.
        if(tally_.passed) {
            if(!both||sample.body_up_alignment<.5||sample.support_up_alignment<.15||tally_.final_speed>=.75)
                fail("lost_stable_stop_after_pass");
            return tally_;
        }
        if(sample.elapsed<.5) {tally_.issue="neutral_baseline";return tally_;}
        tally_.com_rise=sample.com[1]-tally_.start_com_y;
        tally_.max_rise=std::max(tally_.max_rise,tally_.com_rise);
        if(!tally_.ascent&&sample.grounded&&sample.support_up_alignment<.98&&
           sample.support_up_alignment>=.15&&tally_.com_rise>.2&&
           tally_.progress>start_progress_+.5) {
            tally_.ascent=true;tally_.first_ascent_seconds=sample.elapsed;tally_.issue="measured_ramp_ascent";
        }
        if(!launch_&&tally_.ascent&&!sample.grounded&&tally_.progress>route_.lip_progress) {
            launch_=true;tally_.takeoff_seconds=sample.elapsed;tally_.issue="waiting_ballistic_evidence";
            // The first clear sample seeds an interval; its preceding support
            // or pre-lip time is never counted as measured ballistic flight.
        } else if(launch_&&!tally_.landed) {
            if(!sample.grounded) {
                tally_.landing_support_seconds=0;
                if(!touchdown_) {
                    tally_.flight_seconds+=sample.step_seconds; // query/support-clear duration, not a ballistic claim
                    Vec error=sub(sample.velocity,prior.velocity);
                    for(unsigned i=0;i<3;++i)error[i]-=verified_gravity[i]*static_cast<float>(sample.step_seconds);
                    tally_.ballistic_velocity_error=length(error);
                    tally_.maximum_ballistic_velocity_error=std::max(tally_.maximum_ballistic_velocity_error,tally_.ballistic_velocity_error);
                    const bool clear_interval=!prior.grounded&&length(prior.force)<=ballistic_force_tolerance&&
                        length(sample.force)<=ballistic_force_tolerance&&length(prior.torque)<=ballistic_torque_tolerance&&
                        length(sample.torque)<=ballistic_torque_tolerance;
                    if(clear_interval&&tally_.ballistic_velocity_error<=ballistic_velocity_tolerance)
                        tally_.ballistic_seconds+=sample.step_seconds;
                    else tally_.ballistic_seconds=0;
                    tally_.maximum_ballistic_seconds=std::max(tally_.maximum_ballistic_seconds,tally_.ballistic_seconds);
                    if(natural_interval(prior,sample)&&tally_.ballistic_velocity_error<=ballistic_velocity_tolerance) {
                        tally_.natural_flight_seconds+=sample.step_seconds;
                        if(prior.neutral_air_damping_active||sample.neutral_air_damping_active)++tally_.passive_angular_intervals;
                    } else tally_.natural_flight_seconds=0;
                    tally_.maximum_natural_flight_seconds=std::max(tally_.maximum_natural_flight_seconds,tally_.natural_flight_seconds);
                    if(tally_.natural_flight_seconds+1e-9>=.12&&tally_.max_rise>.3) {
                        tally_.flight=true;tally_.issue=tally_.passive_angular_intervals?
                            "measured_gravity_translation_with_passive_angular_damping":"measured_ballistic_flight";
                    }
                }
            } else {
                if(!tally_.flight) {
                    // A short clear interval can be a lip/edge transition, not
                    // a qualified jump. Actual support resumes the ascent;
                    // later real flight must start a NEW contiguous interval.
                    launch_=touchdown_=false;tally_.ballistic_seconds=tally_.natural_flight_seconds=0;
                    tally_.flight_seconds=tally_.landing_support_seconds=0;
                    tally_.takeoff_seconds=0;tally_.issue="resumed_supported_ascent";
                } else {
                const bool first_touch=!touchdown_;
                touchdown_=true;tally_.ballistic_seconds=tally_.natural_flight_seconds=0;
                if(sample.body_up_alignment<.5||sample.support_up_alignment<.15) {
                    fail("unstable_native_landing");return tally_;
                }
                // A front-only landing can settle naturally before braking.
                // Credit starts from zero on the first observation of BOTH
                // actual wheel supports; opening one wheel resets that window.
                if(first_touch||!both||!was_both)tally_.landing_support_seconds=0;
                else tally_.landing_support_seconds+=sample.step_seconds;
                if(tally_.landing_support_seconds+1e-9>=.06) {
                    if(!tally_.flight||tally_.maximum_natural_flight_seconds+1e-9<.12||tally_.max_rise<=.3) {
                        fail("insufficient_natural_flight");return tally_;
                    }
                    tally_.landed=true;tally_.landing_seconds=sample.elapsed;tally_.issue="measured_settled_landing";
                }
                }
            }
        } else if(tally_.landed&&brake_sent_) {
            if(!both||sample.body_up_alignment<.5||sample.support_up_alignment<.15) {fail("lost_support_during_stop");return tally_;}
            tally_.brake_seconds+=sample.step_seconds;
            if(tally_.brake_seconds+1e-9>=.10&&tally_.final_speed<.75) {
                tally_.completed=tally_.passed=true;tally_.phase="passed";tally_.issue="natural_ramp_launch_landing_and_stop";return tally_;
            }
        }
        if(sample.elapsed>=9)fail(!tally_.ascent?"no_verified_ramp_ascent":
            !tally_.flight?"no_qualified_natural_flight":!tally_.landed?"no_settled_ramp_landing":"did_not_stop_in_time");
        return tally_;
    }
};
}
