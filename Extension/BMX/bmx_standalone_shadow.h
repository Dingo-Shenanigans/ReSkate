#pragma once
#include "bmx_standalone_shadow_status.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include "Engine/Game/BMX/bmx_input.h"
#include <bmx/movement.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

// Portable developer observer. Receives already-qualified native observations;
// owns no native handles, queries, setters, pose integration or force submission.
namespace dingosdk::bmx::standalone_shadow {
struct Lease {
    std::uintptr_t base{},client{},entity{},core{},context{},rig{},query_context{},world{},native_state{};
    std::uint32_t body_index{0xffffffffU},body_generation{};
    std::uint64_t token{};
    bool operator==(const Lease&) const noexcept = default;
};
struct Observation {
    Lease lease;
    std::uint64_t serial{};
    double simulation_time{},seconds{};
    tire::Body body;
    std::array<chassis_control::ContactSample,2> contacts;
};
inline ::bmx::RawInput convert_input(const RawInput& input) noexcept {
    ::bmx::RawInput out;
    out.left_x=input.left_x;out.left_y=input.left_y;out.right_x=input.right_x;out.right_y=input.right_y;
    out.pedal=input.pedal;out.brake=input.brake;out.available=input.available;out.focused=input.focused;
    out.menu_open=input.menu_open;out.device=input.device;out.quick_hop=input.quick_hop;
    out.left_trigger=input.left_trigger;out.right_trigger=input.right_trigger;
    out.left_shoulder=input.left_shoulder;out.right_shoulder=input.right_shoulder;
    return out;
}
inline ::bmx::Body convert_body(const tire::Body& body) noexcept {
    ::bmx::Body out;out.mass=body.mass;out.inertia_body=body.inertia_body;
    // The native public seam exposes principal inertia ALONG body axes.
    out.inertia_orientation={0,0,0,1};out.orientation=body.orientation;
    out.center_of_mass=body.center_of_mass;out.linear_velocity=body.linear_velocity;
    out.angular_velocity=body.angular_velocity;return out;
}
inline ::bmx::ContactSample convert_contact(const chassis_control::ContactSample& sample) noexcept {
    ::bmx::ContactSample out;
    switch(sample.contact.status){
    case tire::ContactStatus::hit:out.contact.status=::bmx::ContactStatus::hit;break;
    case tire::ContactStatus::clear:out.contact.status=::bmx::ContactStatus::clear;break;
    default:out.contact.status=::bmx::ContactStatus::unavailable;break;
    }
    out.contact.point=sample.contact.point;out.contact.normal=sample.contact.normal;
    out.contact.forward=sample.contact.forward;out.contact.surface_velocity=sample.contact.surface_velocity;
    out.separation=sample.separation;out.balance_normal=sample.balance_normal;
    out.balance_normal_available=sample.balance_normal_available;out.verified_plane=sample.verified_plane;
    return out;
}
inline bool geometry_matches() noexcept {
    return geometry::wheel_radius==::bmx::geometry::wheel_radius&&geometry::wheelbase==::bmx::geometry::wheelbase&&
        geometry::com_from_root==::bmx::geometry::com_from_root&&geometry::front_axle==::bmx::geometry::front_axle&&
        geometry::rear_axle==::bmx::geometry::rear_axle&&geometry::steering_pivot==::bmx::geometry::steering_pivot&&
        geometry::steering_axis==::bmx::geometry::steering_axis;
}
inline bool finite_vector(const ::bmx::Vec3& v) noexcept {
    for(double value:v)if(!std::isfinite(value))return false;
    return true;
}
inline double norm(const ::bmx::Vec3& v) noexcept {return std::hypot(v[0],v[1],v[2]);}
struct WrenchAssessment {bool finite{},within_caps{};double combined_force{},torque{},hop{};};
inline WrenchAssessment assess_wrench(const ::bmx::Vec3& force,const ::bmx::Vec3& torque,
                                    const ::bmx::Vec3& hop,double seconds) noexcept {
    WrenchAssessment out;
    if(!finite_vector(force)||!finite_vector(torque)||!finite_vector(hop)||
       !std::isfinite(seconds)||seconds<=0)return out;
    ::bmx::Vec3 combined{};for(unsigned i=0;i<3;++i)combined[i]=force[i]+hop[i]/seconds;
    out.combined_force=norm(combined);out.torque=norm(torque);out.hop=norm(hop);
    const double force_norm=norm(force);
    out.finite=std::isfinite(force_norm)&&std::isfinite(out.combined_force)&&std::isfinite(out.torque)&&std::isfinite(out.hop);
    out.within_caps=out.finite&&force_norm<=40000&&out.combined_force<=40000&&out.torque<=4000&&out.hop<=600;
    return out;
}
class Observer {
public:
    const Report& status() const noexcept {return report_;}
    bool enabled() const noexcept {return enabled_;}
    void set_enabled(bool enabled) noexcept {
        if(enabled_==enabled)return;
        enabled_=enabled;reset();report_.enabled=enabled;report_.issue=enabled?Issue::reset:Issue::disabled;
    }
    // Lifecycle cancellation clears controls but remembers the last consumed
    // epoch: pause/recovery must not evaluate that same body snapshot again.
    void reset() noexcept {
        movement_.reset();count(report_.reset_total);report_.valid=report_.evaluated=report_.within_caps=false;
        report_.comparison_available=false;report_.force=report_.torque=report_.hop_impulse={};
        report_.active=report_.grounded=false;
        report_.combined_force_norm=report_.torque_norm=report_.hop_norm=report_.steering_angle=0;
        report_.issue=enabled_?Issue::reset:Issue::disabled;
    }
    const Report& observe(const RawInput& raw,const Observation& observed) noexcept {
        begin_report();report_.serial=observed.serial;report_.token=observed.lease.token;
        if(!enabled_)return report_;
        if(!valid_lease(observed.lease))return reject(Issue::invalid_lease);
        if(!same_lease_||lease_!=observed.lease){
            reset();lease_=observed.lease;same_lease_=true;have_epoch_=false;configured_=false;
        }
        if(!std::isfinite(observed.seconds)||std::abs(observed.seconds-chassis_control::verified_native_seconds)>1e-8||
           !std::isfinite(observed.simulation_time)||observed.simulation_time<0||!observed.serial)
            return reject(Issue::invalid_clock);
        if(have_epoch_){
            if(observed.serial==last_serial_&&observed.simulation_time==last_time_){
                count(report_.duplicate_total);report_.issue=Issue::duplicate;return report_;
            }
            if(last_serial_==std::numeric_limits<std::uint64_t>::max()||observed.serial!=last_serial_+1||
               std::abs(observed.simulation_time-last_time_-observed.seconds)>1e-7){
                // Burn the refused epoch. A discontinuity cannot become a
                // second attempt with changed controls at the same epoch.
                if(observed.serial>last_serial_&&observed.simulation_time>=last_time_)remember(observed);
                return reject(Issue::discontinuity);
            }
        }
        remember(observed);
        if(!geometry_matches())return reject(Issue::geometry_mismatch);
        if(!configured_||configured_mass_!=observed.body.mass||configured_inertia_!=observed.body.inertia_body){
            movement_=make_movement(observed.body);configured_mass_=observed.body.mass;
            configured_inertia_=observed.body.inertia_body;configured_=true;count(report_.reset_total);
        }
        auto trial=movement_;
        const std::array<::bmx::ContactSample,2> contacts{convert_contact(observed.contacts[0]),convert_contact(observed.contacts[1])};
        const auto output=trial.step(convert_input(raw),convert_body(observed.body),contacts,observed.lease.token);
        report_.evaluated=true;count(report_.evaluated_total);
        const auto budget=assess_wrench(output.force,output.torque,output.hop_impulse,observed.seconds);
        if(!output.valid||!budget.finite||!std::isfinite(output.steering_angle)||
           !std::isfinite(output.front_wheel_phase)||!std::isfinite(output.rear_wheel_phase)||!std::isfinite(output.crank_phase))
            return reject(Issue::invalid_output);
        report_.force=bounded(output.force);report_.torque=bounded(output.torque);report_.hop_impulse=bounded(output.hop_impulse);
        report_.combined_force_norm=bounded(budget.combined_force);report_.torque_norm=bounded(budget.torque);report_.hop_norm=bounded(budget.hop);
        report_.steering_angle=bounded(output.steering_angle);report_.within_caps=budget.within_caps;
        report_.active=output.actions.active;report_.grounded=output.grounded;
        report_.shadow_selection=static_cast<unsigned>(output.actions.movement);
        if(!budget.within_caps)return reject(Issue::output_limit,false);
        movement_=trial;report_.valid=true;report_.issue=Issue::ready;count(report_.accepted_total);return report_;
    }
    // Read-only comparison with the current accepted portable preview. It does
    // not replace that result, advance either controller, or submit anything.
    void compare_current(const tire::Vec& force,const tire::Vec& torque,const tire::Vec& hop,
                         bool active,unsigned selection) noexcept {
        report_.comparison_available=false;
        if(!report_.evaluated||!report_.valid||!finite_vector(force)||!finite_vector(torque)||!finite_vector(hop))return;
        const auto difference=[](const auto& a,const auto& b){::bmx::Vec3 d{};for(unsigned i=0;i<3;++i)d[i]=a[i]-b[i];return norm(d);};
        const double f=difference(force,report_.force),t=difference(torque,report_.torque),h=difference(hop,report_.hop_impulse);
        if(!std::isfinite(f)||!std::isfinite(t)||!std::isfinite(h))return;
        report_.force_difference_norm=bounded(f);report_.torque_difference_norm=bounded(t);report_.hop_difference_norm=bounded(h);
        report_.current_selection=selection;report_.selection_matches=selection==report_.shadow_selection;
        report_.active_matches=active==report_.active;report_.comparison_available=true;
    }
private:
    bool enabled_{},same_lease_{},have_epoch_{},configured_{};
    Lease lease_{};
    std::uint64_t last_serial_{};
    double last_time_{},configured_mass_{};
    tire::Vec configured_inertia_{};
    ::bmx::Movement movement_{};
    Report report_{};
    static void count(unsigned& value) noexcept {if(value<65535)++value;}
    static bool valid_lease(const Lease& k) noexcept {
        return k.base&&k.client&&k.entity&&k.core&&k.context&&k.rig&&k.query_context&&k.world&&k.native_state&&
            k.body_index!=0xffffffffU&&k.token;
    }
    void remember(const Observation& o) noexcept {last_serial_=o.serial;last_time_=o.simulation_time;have_epoch_=true;}
    void begin_report() noexcept {
        const auto old=report_;report_={};report_.enabled=enabled_;
        report_.evaluated_total=old.evaluated_total;report_.accepted_total=old.accepted_total;
        report_.duplicate_total=old.duplicate_total;report_.rejected_total=old.rejected_total;report_.reset_total=old.reset_total;
    }
    const Report& reject(Issue issue,bool clear_output=true) noexcept {
        movement_.invalidate_sample();count(report_.reset_total);count(report_.rejected_total);
        report_.valid=false;report_.comparison_available=false;report_.issue=issue;
        if(clear_output){report_.force=report_.torque=report_.hop_impulse={};report_.within_caps=false;}
        return report_;
    }
    double bounded(double value) noexcept {
        const double result=std::clamp(value,-1e9,1e9);report_.diagnostic_clamped|=result!=value;return result;
    }
    ::bmx::Vec3 bounded(::bmx::Vec3 value) noexcept {for(auto& v:value)v=bounded(v);return value;}
    static ::bmx::Movement make_movement(const tire::Body& body) noexcept {
        ::bmx::Tuning tuning;
        const chassis_control::Tuning native;
        tuning.gravity=native.gravity;tuning.wheelbase=geometry::wheelbase;
        tuning.contact_gap=.03;tuning.force_limit=40000;tuning.torque_limit=4000;
        ::bmx::SimulationSettings settings;settings.seconds=chassis_control::verified_native_seconds;
        settings.mass=body.mass;settings.inertia_body=body.inertia_body;settings.inertia_orientation={0,0,0,1};
        const auto wheels=chassis_control::proposed_wheels();
        for(unsigned i=0;i<2;++i)settings.wheels[i]={wheels[i].radius,wheels[i].inertia,wheels[i].angular_speed};
        settings.hop_delay_seconds=0;settings.hop_impulse_steps=1;settings.hop_impulse_multiplier=1;
        return ::bmx::Movement(tuning,settings);
    }
};
}
