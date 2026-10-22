#include "bmx_chassis_ride.h"
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
#include "bmx_standalone_shadow.h"
#endif
#include "bmx_native_chassis.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include "Engine/Game/BMX/bmx_landing_presentation.h"
#include "Engine/Game/BMX/bmx_rider_posture_presentation.h"
#include "Engine/Game/BMX/bmx_crank_presentation.h"
#include "bmx_wheel_surface.h"
#include "bmx_body_guard.h"
#include "bmx_collision_envelope.h"
#include "Engine/Game/BMX/chassis_transform.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <limits>
#include <numbers>

namespace dingosdk::bmx::chassis_ride {
namespace {
namespace control = chassis_control;
constexpr Vec com_from_root=geometry::com_from_root;
constexpr double tau = 2*std::numbers::pi;
constexpr double contact_skin = .03;
bool approach_requested(std::wstring_view expected) noexcept {
    wchar_t value[2]{};
    constexpr wchar_t exact_root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-ramp-test";
    return expected==exact_root&&GetEnvironmentVariableW(L"RESKATE_BMX_APPROACH_WITNESS",value,2)==1&&value[0]==L'1'&&
        user_data::runtime_development_selected(L"bmx-user-data-ramp-test",exact_root);
}

bool approach_normal_requested(std::wstring_view expected) noexcept {
    wchar_t value[2]{};
    return approach_requested(expected)&&GetEnvironmentVariableW(L"RESKATE_BMX_APPROACH_NORMAL",value,2)==1&&value[0]==L'1';
}
bool contact_compliance_requested(std::wstring_view expected) noexcept {
    wchar_t value[2]{};
    return approach_normal_requested(expected)&&GetEnvironmentVariableW(L"RESKATE_BMX_CONTACT_COMPLIANCE",value,2)==1&&value[0]==L'1';
}
bool landing_contact_compliance_requested(std::wstring_view expected) noexcept {
    wchar_t value[2]{};
    return contact_compliance_requested(expected)&&GetEnvironmentVariableW(L"RESKATE_BMX_LANDING_CONTACT_COMPLIANCE",value,2)==1&&value[0]==L'1';
}
static_assert(contact_compliance::maximum_deflection<collision_envelope::wheel_backup_recess,
    "private tire deflection must leave the unchanged native backup recess margin");
struct LastError { DWORD value=GetLastError(); ~LastError(){SetLastError(value);} };
Vec add(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
Vec mul(Vec a,float scale) noexcept {for(auto& v:a)v*=scale;return a;}
float dot(Vec a,Vec b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
float length(Vec a) noexcept {return std::sqrt(dot(a,a));}
tire::Vec wide(Vec v) noexcept {return {v[0],v[1],v[2]};}
Vec narrow(tire::Vec v) noexcept {return {static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};}
bool finite(Vec v,float limit=999990) noexcept {
    for(float x:v)if(!std::isfinite(x)||std::abs(x)>limit)return false;
    return true;
}
bool same_owner(const NativeFrame& a,const NativeFrame& b) noexcept {
    return a.available&&b.available&&a.client==b.client&&a.entity==b.entity&&a.core==b.core&&
        a.context==b.context&&a.rig==b.rig;
}
control::Controller new_controller() noexcept {
    control::Tuning tuning;tuning.contact_gap=contact_skin;
    return control::Controller(tuning);
}
struct State {
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    standalone_shadow::Observer shadow;
#endif
    bool started{},paused{},observed{},approach_diagnostics{},approach_normal{},contact_compliance{},landing_contact_compliance{};
    std::uintptr_t base{};
    NativeFrame owner;
    std::filesystem::path root;
    float root_height{};
    std::uint64_t token{},serial{},hop_event{};
    double native_time{},front_phase{},rear_phase{},crank_phase{};
    InputMapper mapper;
    TrickController tricks;
    landing_approach::Tracker landing_approach;
    landing_approach::SettleHoldTracker settle_hold;
    bail_policy::Policy bail;
    native_chassis::Frame previous_body;
    rider_posture::State rider_posture;
    crank_presentation::State crank_presentation;
    control::Controller controller=new_controller();
    std::array<tire::Wheel,2> wheels=control::proposed_wheels();
    Step last;
};
State& state(){static State value;return value;}
bool opted_root(std::filesystem::path* root=nullptr,std::wstring_view expected={}) {
    wchar_t value[2]{};
    if(GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_CHASSIS",value,2)!=1||value[0]!=L'1'||
       !user_data::development_requested())return false;
    if(!root)return user_data::runtime_development_selected({},expected);
    const auto path=user_data::override_root();if(!path)return false;
    if(root)*root=*path;
    return true;
}
void reset_controls(State& s) noexcept {
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    s.shadow.reset();
#endif
    s.mapper.reset();s.tricks.cancel();s.controller.reset();s.landing_approach.reset();s.settle_hold.reset();
    s.last.frame.landing_compression=0;
    s.last.pump={};s.last.pump_force={};
    s.last.query_geometry_fresh=s.last.approach_selected=s.last.pre_contact_prediction_available=false;s.last.approach={};s.last.approach_microseconds=0;
    s.last.approach_normal_selected=s.last.approach_normal_attempted=s.last.approach_normal_adopted=false;
    s.last.approach_normal_issue="unavailable_step";s.last.speculative_normal_mask=0;
    s.last.speculative_issues={"disabled","disabled"};s.last.speculative_normal_impulse={};s.last.speculative_normal_force={};
    s.last.speculative_linear_impulse={};s.last.speculative_angular_impulse={};
    s.last.landing_contact_compliance_selected=s.last.landing_contact_compliance_revoked=false;s.last.landing_compliance={};
    s.last.contact_compliance_selected=s.last.contact_compliance_revoked=false;s.last.compliant_normal_mask=s.last.compliance_attempted_mask=0;s.last.compliance_budget_fallback=false;
    s.last.compliance={};s.last.compliant_predicted_gap={};s.last.compliant_deflection_satisfied={};
    s.last.frame.air_extension_blend=0;s.last.frame.air_extension_available=true;
    s.rider_posture={};s.last.frame.rider_uphill_blend=s.last.frame.rider_gaze_counterpitch=0;
    s.crank_presentation=crank_presentation::suspend(s.crank_presentation);
    s.last.frame.rider_posture_available=false;
    s.bail.pause();s.previous_body={};
    for(auto& wheel:s.wheels)wheel.angular_speed=0;
    s.observed=false;
}
Step reject(State& s,std::uintptr_t base,const NativeFrame& rider,const char* issue) noexcept {
    native_chassis::park(base,rider);
    s.paused=true;reset_controls(s);s.last={};s.last.issue=issue;
    return s.last;
}
// A clear ray supplies no surface normal. Its basis is the measured wheel plane
// solely for airborne rotor reactions; it cannot provide contact/load/hop support.
bool wheel_contact(const native_chassis::Frame& body,const NativeFrame& rider,std::uintptr_t context,
                   Vec root,Quat rotation,unsigned wheel,float steering_angle,control::ContactSample& sample,
                   unsigned& rays,const char*& feature,float& edge_uncertainty,
                   wheel_surface::EdgeDiagnostic& edge_diagnostic,approach_surface::Surface& observed,const char*& issue) {
    // Queries describe the wheel at the last published steering deflection;
    // the next force solve chooses the following deflection independently.
    const auto up=chassis::rotate_unit(rotation,wheel==0?
        geometry::rotate_direction({0,1,0},geometry::steering_axis,steering_angle):Vec{0,1,0});
    const auto forward=chassis::rotate_unit(rotation,{0,0,1});
    const auto query_forward=chassis::rotate_unit(rotation,wheel==0?
        geometry::rotate_direction({0,0,1},geometry::steering_axis,steering_angle):Vec{0,0,1});
    const auto left=chassis::rotate_unit(rotation,wheel==0?
        geometry::rotate_direction({1,0,0},geometry::steering_axis,steering_angle):Vec{1,0,0});
    const auto hub=add(root,chassis::rotate_unit(rotation,geometry::wheel_hub(wheel==0,steering_angle)));
    sample.contact.status=tire::ContactStatus::clear;
    sample.contact.point=wide(sub(hub,mul(up,static_cast<float>(geometry::wheel_radius))));
    sample.contact.normal=wide(up);sample.contact.forward=wide(forward);
    sample.contact.surface_velocity={}; // first static-world experiment; moving surfaces unvalidated
    sample.separation=.25;
    const std::array<std::array<std::uint64_t,2>,1> ignored{{
        {body.world,(std::uint64_t{body.body_generation}<<32)|body.body_index}}};
    const auto query=[&](Vec from,Vec to) -> wheel_surface::RaySample {
        const auto hit=probe_local_surface(from,to,ignored,rider.entity);
        if(!hit.available||hit.world!=body.world||hit.context!=context) {
            issue="terrain_query_unavailable_or_world_changed";return {};
        }
        return {true,hit.hit,hit.position,hit.fraction,hit.hit_entity,hit.hit_body};
    };
    wheel_surface::Settings settings;settings.radius=static_cast<float>(geometry::wheel_radius);
    const auto surface=wheel_surface::search(hub,up,query_forward,left,query,settings);rays=surface.rays;
    feature=surface.feature==wheel_surface::Feature::edge?"edge":
        surface.feature==wheel_surface::Feature::plane?"plane":"none";
    edge_uncertainty=surface.edge_uncertainty;
    edge_diagnostic=surface.edge_diagnostic;
    if(!surface.available) {
        if(surface.issue==wheel_surface::Issue::penetration_limit)issue="wheel_penetration_limit";
        else if(surface.issue!=wheel_surface::Issue::unavailable)issue="terrain_query_or_surface_sample_invalid";
        return false;
    }
    if(!surface.hit)return true;
    observed={true,surface.feature==wheel_surface::Feature::plane,surface.point,surface.normal,forward,
        surface.separation,surface.entity,surface.body};
    sample.separation=surface.separation;
    sample.contact.point=wide(surface.point);sample.contact.normal=wide(surface.normal);
    sample.balance_normal=wide(surface.balance_normal);sample.balance_normal_available=surface.balance_normal_available;
    sample.verified_plane=surface.feature==wheel_surface::Feature::plane;
    if(sample.separation<=contact_skin)sample.contact.status=tire::ContactStatus::hit;
    return true;
}
tire::Body measured_body(const native_chassis::Frame& f,Quat rotation) noexcept {
    tire::Body body;
    body.mass=f.mass;body.inertia_body=wide(f.inertia);body.center_of_mass=wide(f.position);
    body.linear_velocity=wide(f.linear_velocity);body.angular_velocity=wide(f.angular_velocity);
    for(unsigned i=0;i<4;++i)body.orientation[i]=rotation[i];
    return body;
}
bool impact_contacts(State& s,const native_chassis::Frame& body,const NativeFrame& rider,
                     std::uintptr_t context,Vec root,Quat rotation,bail_policy::Sample& sample,unsigned& rays) {
    sample.owner={rider.client,rider.entity,rider.core,rider.context,rider.rig,body.world,
        body.body_index,body.body_generation,body.token};
    sample.serial=body.serial;sample.seconds=body.step_seconds;sample.mounted=true;
    sample.body_up=wide(chassis::rotate_unit(rotation,{0,1,0}));
    const auto current=measured_body(body,rotation);
    Quat old_rotation{};Vec old_root{};
    const auto& old=s.previous_body;
    const bool consecutive=s.observed&&old.available&&old.token==body.token&&old.serial+1==body.serial&&
        chassis::unit_quaternion(old.orientation,old_rotation)&&
        chassis::root_from_com(old.position,old_rotation,com_from_root,old_root);
    const auto previous=consecutive?measured_body(old,old_rotation):current;
    const std::array<std::array<std::uint64_t,2>,1> ignored{{
        {body.world,(std::uint64_t{body.body_generation}<<32)|body.body_index}}};
    const auto query=[&](Vec from,Vec to)->body_guard::Ray {
        const auto hit=probe_local_surface(from,to,ignored,rider.entity);
        if(!hit.available||hit.world!=body.world||hit.context!=context)return {};
        return {true,hit.hit,hit.position,hit.fraction};
    };
    for(unsigned i=0;i<collision_envelope::guards.size();++i) {
        const auto& guard=collision_envelope::guards[i];
        const auto center=add(root,chassis::rotate_unit(rotation,guard.center));
        const auto previous_center=consecutive?add(old_root,chassis::rotate_unit(old_rotation,guard.center)):center;
        const auto found=body_guard::search(center,guard.radius,narrow(tire::point_velocity(current,wide(center))),
            previous_center,narrow(tire::point_velocity(previous,wide(previous_center))),consecutive,query);
        rays+=found.rays;
        if(!found.available)return false;
        auto& contact=sample.contacts[sample.contact_count++];
        contact.region=guard.region;contact.available=true;contact.hit=found.hit;contact.query_serial=body.serial;
        contact.point=wide(found.point);contact.normal=wide(found.normal);contact.separation=found.separation;
        contact.closing_speed=found.closing_speed;contact.swept_crossing=found.swept_crossing;
    }
    if(consecutive) {
        double residual2=0;
        for(unsigned i=0;i<3;++i) {
            const double expected=old.linear_velocity[i]+s.last.force[i]*body.step_seconds/body.mass+
                s.last.hop_impulse[i]/body.mass+(i==1?-9.8*body.step_seconds:0);
            const double residual=(body.linear_velocity[i]-expected)*body.mass;residual2+=residual*residual;
        }
        sample.residual_available=true;sample.residual_impulse_ns=std::sqrt(residual2);
    }
    return true;
}
const char* control_issue(control::Issue issue) noexcept {
    switch(issue) {
    case control::Issue::none:return "ready";
    case control::Issue::unsupported_step:return "uncalibrated_native_step";
    case control::Issue::unavailable_contact:return "terrain_contact_unavailable";
    case control::Issue::penetration_limit:return "wheel_penetration_limit";
    case control::Issue::output_limit:return "chassis_force_or_torque_limit";
    default:return "invalid_chassis_control_sample";
    }
}
} // namespace

standalone_shadow::Report standalone_shadow_status() noexcept {
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    return state().shadow.status();
#else
    return {};
#endif
}
bool enabled() noexcept {LastError error;try{return opted_root();}catch(...){return false;}}
bool start(std::uintptr_t base,const NativeFrame& rider,const Vec& bike_root,float yaw,float root_height,const mount_pose::Seed* seed) noexcept {
    LastError error;
    try {
        auto& s=state();std::filesystem::path root;
        if(!opted_root(&root)||!rider.available||!rider.offboard||!finite(bike_root)||!std::isfinite(yaw)||
           !std::isfinite(root_height)||root_height<0||root_height>2)return false;
        if(seed && (!seed->ready || seed->root!=bike_root))return false;
        if(s.started)stop(s.base,s.owner);
        State next;next.started=true;next.base=base;next.owner=rider;next.root=std::move(root);next.root_height=root_height;
        next.approach_diagnostics=approach_requested(next.root.native());
        next.approach_normal=approach_normal_requested(next.root.native());
        next.contact_compliance=contact_compliance_requested(next.root.native());
        next.landing_contact_compliance=landing_contact_compliance_requested(next.root.native());
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
        wchar_t shadow_flag[2]{};
        next.shadow.set_enabled(GetEnvironmentVariableW(L"RESKATE_BMX_STANDALONE_SHADOW",shadow_flag,2)==1&&shadow_flag[0]==L'1');
#endif
        if(!native_chassis::acquire(base,rider,bike_root,yaw,seed))return false;
        s=std::move(next);
        return true;
    } catch(...) {return false;}
}
void pause(std::uintptr_t base,const NativeFrame& rider) noexcept {
    LastError error;auto& s=state();if(!s.started)return;
    native_chassis::park(base,rider);s.paused=true;reset_controls(s);s.last.ready=false;
    s.last.frame.active=false;s.last.issue="paused";
}
void stop(std::uintptr_t base,const NativeFrame& rider) noexcept {
    LastError error;auto& s=state();if(s.started)native_chassis::park(base,rider);
    s.started=false;s.paused=false;reset_controls(s);s.last={};
}
void detach() noexcept {
    LastError error;auto& s=state();
    s.started=false;s.paused=false;reset_controls(s);s.last={};
}
Step update(std::uintptr_t base,const NativeFrame& rider,const RawInput& raw,double native_h) noexcept {
    LastError error;auto& s=state();
    try {
        if(!s.started)return s.last;
        if(!opted_root(nullptr,s.root.native())||base!=s.base||!same_owner(s.owner,rider)||!rider.offboard)
            return reject(s,base,rider,"chassis_owner_or_development_gate_changed");
        if(!raw.available||!raw.focused||raw.menu_open)return reject(s,base,rider,"chassis_input_inhibited");
        if(!std::isfinite(native_h)||std::abs(native_h-control::verified_native_seconds)>1e-8)
            return reject(s,base,rider,"uncalibrated_native_step");
        if(s.paused) {
            if(!native_chassis::resume(base,rider))return reject(s,base,rider,"native_chassis_resume_rejected");
            s.paused=false;
        }
        const auto body=native_chassis::observe(base,rider);
        if(!body.available||!body.active)return reject(s,base,rider,body.issue);
        Quat rotation;Vec bike_root;
        if(!chassis::unit_quaternion(body.orientation,rotation)||!finite(body.position)||
           !finite(body.linear_velocity,50)||!finite(body.angular_velocity,40)||
           !chassis::root_from_com(body.position,rotation,com_from_root,bike_root))
            return reject(s,base,rider,"invalid_measured_chassis_pose");
        const auto rider_target=add(bike_root,chassis::rotate_unit(rotation,{0,s.root_height,0}));
        if(s.token&&body.token!=s.token)return reject(s,base,rider,"native_chassis_token_changed");
        if(s.observed) {
            if(body.serial==s.serial) {
                auto duplicate=s.last;duplicate.command_submitted=duplicate.hop_applied=false;
                duplicate.actions.hop_pressed=false;duplicate.trick_events={};
                duplicate.pump={};duplicate.pump_force={}; // no clock advance or repeated pump event
                duplicate.query_geometry_fresh=duplicate.approach_selected=duplicate.pre_contact_prediction_available=false;duplicate.approach={};duplicate.approach_microseconds=0;
    duplicate.approach_normal_selected=duplicate.approach_normal_attempted=duplicate.approach_normal_adopted=false;
    duplicate.approach_normal_issue="unavailable_step";duplicate.speculative_normal_mask=0;
    duplicate.speculative_issues={"disabled","disabled"};duplicate.speculative_normal_impulse={};duplicate.speculative_normal_force={};
    duplicate.speculative_linear_impulse={};duplicate.speculative_angular_impulse={};
    duplicate.landing_contact_compliance_selected=duplicate.landing_contact_compliance_revoked=false;duplicate.landing_compliance={};
    duplicate.contact_compliance_selected=duplicate.contact_compliance_revoked=false;duplicate.compliant_normal_mask=duplicate.compliance_attempted_mask=0;duplicate.compliance_budget_fallback=false;
    duplicate.compliance={};duplicate.compliant_predicted_gap={};duplicate.compliant_deflection_satisfied={};
                const auto status=native_chassis::status();duplicate.native_applications=status.applications;
                duplicate.hop_events=status.hop_events;return duplicate;
            }
            if(body.serial<s.serial||!std::isfinite(body.simulation_time)||
               std::abs(body.simulation_time-s.native_time-native_h)>1e-7)
                return reject(s,base,rider,"native_chassis_clock_discontinuity");
        }
        if(!std::isfinite(body.step_seconds)||std::abs(body.step_seconds-native_h)>1e-7)
            return reject(s,base,rider,"native_chassis_step_mismatch");
        std::uintptr_t context{};
        if(!memory::peek(rider.client+8,context)||!context)return reject(s,base,rider,"native_client_context_unavailable");
        bail_policy::Sample impact;unsigned guard_rays{};
        if(!impact_contacts(s,body,rider,context,bike_root,rotation,impact,guard_rays))
            return reject(s,base,rider,"body_guard_query_unavailable");
        const auto bail=s.bail.step(impact);
        if(!bail.valid)return reject(s,base,rider,"invalid_body_impact_sample");
        if(bail.bail) {
            s.controller.reset_pump(); // this branch submits no physical command
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
            s.shadow.reset();
#endif
            Step fallen;
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
            fallen.shadow=s.shadow.status();
#endif
            fallen.ready=true;fallen.issue="bail_contact";fallen.bail=bail;fallen.impact_sample=impact;
            fallen.body_guard_rays=guard_rays;fallen.observed_com=body.position;fallen.observed_quaternion=rotation;
            fallen.velocity=body.linear_velocity;fallen.angular_velocity=body.angular_velocity;
            fallen.serial=body.serial;fallen.body_token=body.token;
            fallen.native_step_seconds=native_h;fallen.native_simulation_seconds=body.simulation_time;
            const auto producer=native_chassis::status();fallen.native_applications=producer.applications;fallen.hop_events=producer.hop_events;
            fallen.frame=s.last.frame;fallen.frame.position=bike_root;fallen.frame.chassis_quaternion=rotation;
            return fallen; // no new force, trick credit, or rider pose submission
        }
        if(!finite(rider.position)||length(sub(rider_target,rider.position))>2)
            return reject(s,base,rider,"rider_chassis_separation_exceeded");
        std::array<control::ContactSample,2> contacts;
        std::array<approach_surface::Surface,2> queried_surfaces{};
        std::array<unsigned,2> contact_rays{};
        std::array<const char*,2> contact_features{"none","none"};
        std::array<float,2> contact_edge_uncertainty{};
        std::array<wheel_surface::EdgeDiagnostic,2> contact_edge_diagnostics{};
        const char* issue="ready";
        for(unsigned i=0;i<contacts.size();++i)
            // The observed native body belongs to the preceding submitted step.
            // Its current visual/query deflection is the last published angle;
            // the newly solved actuator angle is published for the next step.
            if(!wheel_contact(body,rider,context,bike_root,rotation,i,s.last.frame.steer,contacts[i],contact_rays[i],
                contact_features[i],contact_edge_uncertainty[i],contact_edge_diagnostics[i],queried_surfaces[i],issue))return reject(s,base,rider,issue);
        const auto measured=measured_body(body,rotation);
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
        // Diagnostic only: one disposable Movement evaluation for this exact
        // admitted native body/query epoch. The existing producer stays in control.
        wchar_t shadow_flag[2]{};
        s.shadow.set_enabled(GetEnvironmentVariableW(L"RESKATE_BMX_STANDALONE_SHADOW",shadow_flag,2)==1&&shadow_flag[0]==L'1');
        if(s.shadow.enabled()) {
            standalone_shadow::Observation observation;
            observation.lease={base,rider.client,rider.entity,rider.core,rider.context,rider.rig,context,
                body.world,body.native_state,body.body_index,body.body_generation,body.token};
            observation.serial=body.serial;observation.simulation_time=body.simulation_time;
            observation.seconds=body.step_seconds;observation.body=measured;observation.contacts=contacts;
            s.shadow.observe(raw,observation);
        }
#endif
        bool supported=false;std::array<bool,2> actual_wheel_support{};
        for(unsigned wheel=0;wheel<contacts.size();++wheel) {
            actual_wheel_support[wheel]=control::eligible_support(measured,contacts[wheel]);
            supported=supported||actual_wheel_support[wheel];
        }
        const auto actions=s.mapper.update(raw,{supported},native_h);
        control::Controls controls;
        // An acquired landing-only mode must never widen to all-ground when
        // its flag is revoked. Both selections then return to original rigid.
        const bool contact_compliance_selected=s.contact_compliance&&contact_compliance_requested(s.root.native())&&
            (!s.landing_contact_compliance||landing_contact_compliance_requested(s.root.native()));
        const bool landing_contact_compliance_selected=s.landing_contact_compliance&&contact_compliance_selected;
        controls.contact_compliance_available=contact_compliance_selected;
        controls.landing_contact_compliance_selected=landing_contact_compliance_selected;

        // No second clock: the existing owner/token/world/query checks above
        // qualify only an exact successor of the previously submitted step.
        controls.recovery_step_verified=s.observed&&body.token==s.token&&body.serial==s.serial+1&&
            std::abs(body.simulation_time-s.native_time-native_h)<=1e-7;
        controls.enabled=actions.active;controls.pedal=actions.pedal;controls.brake=actions.brake;
        controls.pump_operator_available=actions.active&&raw.available&&raw.focused&&!raw.menu_open;
        controls.ground_pump=actions.front_weight; // mapped LS Y, preserves the existing deadzone
        controls.pump_inhibited=actions.hop_pressed||raw.quick_hop||actions.preload>0;
        controls.steer=actions.steer;controls.manual=actions.manual;controls.nose_manual=actions.nose_manual;
        if(!supported) {
            controls.manual=std::max(0.0,-actions.air_pitch);
            controls.nose_manual=std::max(0.0,actions.air_pitch);
        }
        controls.air_yaw=actions.steer;controls.hop_strength=actions.hop_pressed?actions.hop_strength:0;
        // Preserve raw intent even before the mapper has rearmed an airborne
        // gesture; released catalog/spin/flip momentum stays reserved too.
        const auto& rotation_state=s.tricks.state();
        const double rotation_neutral=s.mapper.settings().deadzone+
            (1-s.mapper.settings().deadzone)*s.mapper.settings().neutral_threshold;
        controls.rotation_reserved=std::hypot(raw.left_x,raw.left_y)>rotation_neutral||
            std::hypot(raw.right_x,raw.right_y)>rotation_neutral||raw.left_shoulder||raw.right_shoulder||
            raw.left_trigger>s.mapper.settings().trigger_neutral||raw.right_trigger>s.mapper.settings().trigger_neutral||
            actions.handlebar!=TrickDirection::none||actions.tailwhip!=TrickDirection::none||
            actions.movement!=catalog::TrickId::none||rotation_state.bar_rotating||rotation_state.tail_rotating||
            rotation_state.bar_waiting_catch||rotation_state.tail_waiting_catch||
            rotation_state.movement_id!=catalog::TrickId::none||rotation_state.movement_holding||
            rotation_state.movement_recovering||rotation_state.pending_units>0||rotation_state.landing_pending;
        if(actions.hop_pressed) {
            if(s.hop_event==std::numeric_limits<std::uint64_t>::max())return reject(s,base,rider,"hop_event_counter_exhausted");
            controls.hop_event_id=++s.hop_event;
        }
        // Preview is a copy: optional actual-world witnesses cannot advance
        // pump/hop/balance bookkeeping twice in the committed controller.
        auto preview_controller=s.controller;
        auto solved=preview_controller.step(native_h,measured,s.wheels,contacts,controls);
        if(!solved.valid)return reject(s,base,rider,control_issue(solved.issue));
        const auto preview_result=solved;

        // Actual geometry observations are independent of optional force admission.
        // The default path retains the original preview response exactly.
        std::array<approach_surface::Result,2> approach_geometry{};std::uint64_t approach_microseconds{};
        const bool approach_selected=s.approach_diagnostics&&approach_requested(s.root.native());
        if(approach_selected&&solved.pre_contact_prediction_available) {
            const auto began=std::chrono::steady_clock::now();
            const auto epoch=[&]() noexcept {
                NoBailSkater owner;bool protected_now{};std::uintptr_t current_context{};
                std::uint64_t serial{};double time{};
                return approach_requested(s.root.native())&&inspect_no_bail_owner(rider.client,rider.entity,owner,protected_now)&&
                    owner.base==base&&owner.entity==rider.entity&&owner.core==rider.core&&owner.context==rider.context&&owner.rig==rider.rig&&
                    memory::peek(rider.client+8,current_context)&&current_context==context&&
                    read_native_clock_try(rider.entity,rider.core,serial,time)&&serial==body.serial&&time==body.simulation_time;
            };
            const std::array<std::array<std::uint64_t,2>,1> ignored{{
                {body.world,(std::uint64_t{body.body_generation}<<32)|body.body_index}}};
            const auto query=[&](Vec from,Vec to) -> approach_surface::RayHit {
                const auto bounded=probe_local_surface_bounded(from,to,2,ignored,rider.entity);
                const auto& hit=bounded.surface;
                approach_surface::RayHit result;result.native_casts=bounded.native_casts;
                result.elapsed_microseconds=bounded.elapsed_microseconds;result.budget_exhausted=bounded.budget_exhausted;
                if(!hit.available||hit.world!=body.world||hit.context!=context||hit.excluded_bodies<1)return result;
                result.available=true;result.hit=hit.hit;result.point=hit.position;result.fraction=hit.fraction;
                result.entity=hit.hit_entity;result.body=hit.hit_body;return result;
            };
            for(unsigned i=0;i<approach_geometry.size();++i)approach_geometry[i]=approach_surface::observe(true,
                controls.recovery_step_verified,solved.hop_applied||controls.hop_strength>0,native_h,
                solved.pre_contact_prediction,queried_surfaces[i],query,epoch);
            approach_microseconds=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-began).count());
        }
        const bool approach_normal_selected=s.approach_normal&&approach_normal_requested(s.root.native());
        bool approach_normal_attempted=false,approach_normal_adopted=false;
        const char* approach_normal_issue=approach_normal_selected?"no_qualified_current_approach":"disabled";
        std::array<control::SpeculativeNormal,2> speculative{};
        bool any_qualified=false;
        for(unsigned i=0;i<2;++i) {
            speculative[i]={approach_normal_selected,approach_normal_selected&&approach_geometry[i].qualified,
                contacts[i].contact.point,contacts[i].contact.normal,contacts[i].separation,solved.pre_contact_prediction};
            any_qualified=any_qualified||speculative[i].qualified;
        }
        // Default/no-proof/failed opt-in adopts the one original preview state.
        // A separate private solve may add only the coupled normal rows.
        auto committed_controller=preview_controller;
        if(approach_normal_selected&&any_qualified) {
            approach_normal_attempted=true;auto normal_controller=s.controller;
            auto normal=normal_controller.step(native_h,measured,s.wheels,contacts,controls,speculative);
            if(normal.valid&&approach_normal_requested(s.root.native())) {
                solved=normal;committed_controller=normal_controller;approach_normal_adopted=true;
                approach_normal_issue=normal.speculative_normal_mask?"coupled_normal_solve_adopted":"proof_refused_by_current_solve";
            } else approach_normal_issue=normal.valid?"opt_in_revoked":"normal_solve_rejected_original_preview_retained";
        }
        if(approach_normal_adopted&&!approach_normal_requested(s.root.native())) {
            solved=preview_result;committed_controller=preview_controller;approach_normal_adopted=false;
            approach_normal_issue="opt_in_revoked";
        }
        bool contact_compliance_revoked=false;
        const bool landing_contact_compliance_revoked=landing_contact_compliance_selected&&!landing_contact_compliance_requested(s.root.native());
        if(contact_compliance_selected&&(!contact_compliance_requested(s.root.native())||landing_contact_compliance_revoked)) {
            // Revocation selects a fresh original constitutive solve from the
            // same pre-step controller state, never the already-soft preview.
            controls.contact_compliance_available=false;controls.landing_contact_compliance_selected=false;auto fallback_controller=s.controller;
            const auto retained=approach_normal_requested(s.root.native())?speculative:std::array<control::SpeculativeNormal,2>{};
            auto fallback=fallback_controller.step(native_h,measured,s.wheels,contacts,controls,retained);
            if(!fallback.valid)return reject(s,base,rider,control_issue(fallback.issue));
            solved=fallback;committed_controller=fallback_controller;contact_compliance_revoked=true;
            approach_normal_adopted=solved.speculative_normal_mask!=0;
            approach_normal_issue="compliance_revoked_original_contact_model_retained";
        }
        s.controller=committed_controller;
        const auto force=narrow(solved.force),torque=narrow(solved.torque),hop=narrow(solved.hop_impulse);
        if(!native_chassis::submit(body,force,torque,hop,solved.hop_event_id))
            return reject(s,base,rider,"native_chassis_force_submission_rejected");
        
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
        s.shadow.compare_current(solved.force,solved.torque,solved.hop_impulse,actions.active,
            static_cast<unsigned>(actions.movement));
#endif
        s.wheels=solved.wheels;s.token=body.token;s.serial=body.serial;s.native_time=body.simulation_time;s.observed=true;
        // Only rotor/cadence presentation integrates. Native COM and quaternion
        // are never integrated, clamped to a plane, height-averaged or reseeded here.
        s.front_phase=std::remainder(s.front_phase+s.wheels[0].angular_speed*native_h,tau);
        s.rear_phase=std::remainder(s.rear_phase+s.wheels[1].angular_speed*native_h,tau);
        // Proposed visual gear ratio; no recovered drivetrain/timing claim.
        if(actions.pedal>0)s.crank_phase=std::remainder(s.crank_phase+s.wheels[1].angular_speed*native_h/2.6,tau);
        const auto forward=chassis::rotate_unit(rotation,{0,0,1});
        const float horizontal=std::hypot(forward[0],forward[2]);
        const double yaw=std::atan2(forward[0],forward[2]);
        const double travel=std::hypot(body.linear_velocity[0],body.linear_velocity[2])>.15f?
            std::atan2(body.linear_velocity[0],body.linear_velocity[2]):yaw;
        TrickInput trick;trick.enabled=actions.active;trick.grounded=supported;
        trick.owner=body.token;trick.device=raw.device;trick.barspin=actions.handlebar;trick.tailwhip=actions.tailwhip;
        trick.movement=actions.movement;
        trick.orientation_available=true;
        for(unsigned i=0;i<4;++i)trick.body_quaternion[i]=rotation[i];
        const auto neutral=s.mapper.settings().deadzone+(1-s.mapper.settings().deadzone)*s.mapper.settings().neutral_threshold;
        trick.bar_neutral=trick.rear_neutral=std::hypot(raw.right_x,raw.right_y)<=neutral;
        trick.body_yaw=yaw;trick.travel_yaw=travel;
        trick.landing_quality_available=true;
        const auto body_up=chassis::rotate_unit(rotation,{0,1,0});
        std::array<landing_approach::Support,2> actual_supports{};
        for(unsigned wheel=0;wheel<contacts.size();++wheel) {
            const auto& sample=contacts[wheel];
            if(!actual_wheel_support[wheel])continue;
            actual_supports[wheel]={true,sample.contact.point,sample.contact.normal,sample.contact.surface_velocity};
            trick.body_up_alignment=std::min(trick.body_up_alignment,double(dot(body_up,narrow(sample.contact.normal))));
            trick.support_up_alignment=std::min(trick.support_up_alignment,sample.contact.normal[1]);
            const auto point_velocity=tire::point_velocity(measured,sample.contact.point);
            double normal_speed=0;
            for(unsigned i=0;i<3;++i)normal_speed+=(point_velocity[i]-sample.contact.surface_velocity[i])*sample.contact.normal[i];
            trick.closing_normal_speed=std::max(trick.closing_normal_speed,-normal_speed);
        }
        // Retain the last actual unsupported observation. A native collision
        // may already have slowed/redirected this sample; classify conservatively
        // without claiming an exact manifold velocity or integrating body pose.
        const landing_approach::Motion observed_motion{body.token,body.serial,body.simulation_time,native_h,
            measured.center_of_mass,measured.linear_velocity,measured.angular_velocity,yaw,horizontal>.1f};
        const auto approach=s.landing_approach.observe(observed_motion,actual_supports,{0,-9.8,0});
        if(!approach.valid)trick.enabled=false; // cancel scoring only; physical outputs are unchanged
        if(approach.post_available) {
            trick.closing_normal_speed=approach.scoring_closing_speed;
            if(approach.approach_available&&approach.approach_travel_available)
                trick.travel_yaw=approach.approach_travel_yaw;
        }
        landing_approach::SettleSample settle;
        const auto& pending=s.tricks.state();
        settle.pending=pending.landing_pending;settle.available=true;
        settle.caught_frozen=pending.pending_units>0&&!pending.bar_rotating&&!pending.tail_rotating&&
            !pending.bar_waiting_catch&&!pending.tail_waiting_catch&&
            (!pending.pending_barspins||pending.bar_caught)&&(!pending.pending_tailwhips||pending.tail_caught)&&
            (!pending.pending_movements||pending.movement_caught);
        settle.neutral_controls=actions.active&&!actions.hop_pressed&&!raw.quick_hop&&
            landing_approach::settle_drive_neutral(raw.pedal,raw.brake,s.mapper.settings().trigger_neutral)&&
            std::hypot(raw.left_x,raw.left_y)<=neutral&&std::hypot(raw.right_x,raw.right_y)<=neutral&&
            raw.left_trigger<=s.mapper.settings().trigger_neutral&&raw.right_trigger<=s.mapper.settings().trigger_neutral&&
            !raw.left_shoulder&&!raw.right_shoulder&&actions.handlebar==TrickDirection::none&&
            actions.tailwhip==TrickDirection::none&&actions.movement==catalog::TrickId::none;
        settle.identity={rider.client,rider.entity,rider.core,rider.context,rider.rig,body.world,context};
        settle.motion=observed_motion;settle.quaternion=measured.orientation;
        for(unsigned i=0;i<2;++i)settle.wheels[i]={contacts[i].verified_plane,actual_wheel_support[i],
            contacts[i].contact.point,contacts[i].contact.normal,contacts[i].contact.surface_velocity,
            contacts[i].separation,solved.normal_load[i]};
        const auto hold=s.settle_hold.observe(settle);
        trick.landing_settle_hold=hold.hold;trick.landing_settle_hold_invalid=hold.cancel;
        Step step;
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
        step.shadow=s.shadow.status();
#endif
        step.trick_events=s.tricks.update(native_h,trick);step.tricks=s.tricks.state();
        step.bail=bail;step.impact_sample=impact;step.body_guard_rays=guard_rays;
        step.actual_support=supported;
        step.actual_wheel_support=actual_wheel_support;
        step.wheel_contacts_sampled=true;
        step.query_geometry_fresh=true;step.queried_surfaces=queried_surfaces;
        step.contact_compliance_selected=contact_compliance_selected;step.contact_compliance_revoked=contact_compliance_revoked;
        step.landing_contact_compliance_selected=landing_contact_compliance_selected;step.landing_contact_compliance_revoked=landing_contact_compliance_revoked;
        step.landing_compliance=solved.landing_compliance;
        step.compliant_normal_mask=solved.compliant_normal_mask;step.compliance_attempted_mask=solved.compliance_attempted_mask;
        step.compliance_budget_fallback=solved.compliance_budget_fallback;step.compliance=solved.compliance;
        step.compliant_predicted_gap=solved.compliant_predicted_gap;step.compliant_deflection_satisfied=solved.compliant_deflection_satisfied;
        step.approach_normal_selected=approach_normal_selected;step.approach_normal_attempted=approach_normal_attempted;
        step.approach_normal_adopted=approach_normal_adopted;step.approach_normal_issue=approach_normal_issue;
        step.speculative_normal_mask=solved.speculative_normal_mask;
        for(unsigned i=0;i<2;++i){step.speculative_issues[i]=control::speculative_name(solved.speculative_issues[i]);
            step.speculative_normal_impulse[i]=static_cast<float>(solved.speculative_normal_impulse[i]);
            step.speculative_normal_force[i]=static_cast<float>(solved.speculative_normal_force[i]);}
        step.speculative_linear_impulse=narrow(solved.speculative_linear_impulse);step.speculative_angular_impulse=narrow(solved.speculative_angular_impulse);
        step.approach_selected=approach_selected;step.approach=approach_geometry;step.approach_microseconds=approach_microseconds;
        step.pre_contact_prediction_available=solved.pre_contact_prediction_available;
        step.pre_contact_prediction=solved.pre_contact_prediction;
        step.landing_approach=approach;
        step.landing_body_alignment=static_cast<float>(trick.body_up_alignment);
        step.landing_support_alignment=static_cast<float>(trick.support_up_alignment);
        step.landing_closing_speed=static_cast<float>(trick.closing_normal_speed);
        step.ready=true;step.issue="ready";step.grounded=solved.grounded;step.control_active=actions.active;
        step.command_submitted=true;step.hop_applied=solved.hop_applied;step.actions=actions;
        step.contact_force_scale=static_cast<float>(solved.contact_scale);
        step.balance_support_mask=solved.balance_support_mask;
        step.balance_continuation_mask=solved.balance_continuation_mask;
        step.contact_rays=contact_rays;
        step.contact_features=contact_features;step.contact_edge_uncertainty=contact_edge_uncertainty;
        step.contact_edge_diagnostics=contact_edge_diagnostics;
        step.desired_support_angular_velocity=narrow(solved.desired_support_angular_velocity);
        step.balance_reference_up=narrow(solved.balance_reference_up);step.desired_balance_up=narrow(solved.desired_balance_up);
        step.requested_front_steer_angle=static_cast<float>(solved.requested_front_steer_angle);
        step.turning_lean_angle=static_cast<float>(solved.turning_lean_angle);step.steering_limited=solved.steering_limited;
        step.balance_torque=narrow(solved.balance_torque);step.air_control_torque=narrow(solved.air_control_torque);
        step.air_damping_torque=narrow(solved.air_damping_torque);
        step.pump=solved.pump;step.pump_force=narrow(solved.pump_force);
        step.air_rotation_reserved=solved.air_rotation_reserved;step.neutral_air_damping_active=solved.neutral_air_damping_active;
        step.motor_torque=narrow(solved.motor_torque);step.contact_torque=narrow(solved.contact_torque);
        step.observed_com=body.position;step.observed_quaternion=rotation;step.velocity=body.linear_velocity;
        step.angular_velocity=body.angular_velocity;step.force=force;step.torque=torque;step.hop_impulse=hop;
        step.target_rider_root=rider_target;
        step.speed=dot(body.linear_velocity,forward);step.serial=body.serial;step.body_token=body.token;
        step.native_step_seconds=native_h;step.native_simulation_seconds=body.simulation_time;
        const auto status=native_chassis::status();step.native_applications=status.applications;step.hop_events=status.hop_events;
        for(unsigned i=0;i<contacts.size();++i) {
            step.wheel_angular_speed[i]=static_cast<float>(s.wheels[i].angular_speed);
            step.wheel_separation[i]=static_cast<float>(contacts[i].separation);
            step.normal_load[i]=static_cast<float>(solved.normal_load[i]);
            step.wheel_recovery_velocity[i]=static_cast<float>(solved.recovery_velocity[i]);
            step.contact_hit[i]=contacts[i].contact.status==tire::ContactStatus::hit;
            if(step.contact_hit[i]) {
                // Read the original measured contact: Controller receives a
                // const sample and applies recovery only to its local copy.
                const auto relative=control::detail::sub(tire::point_velocity(measured,contacts[i].contact.point),
                    contacts[i].contact.surface_velocity);
                step.wheel_opening_speed[i]=static_cast<float>(control::detail::dot(relative,contacts[i].contact.normal));
                step.contact_points[i]=narrow(contacts[i].contact.point);step.contact_normals[i]=narrow(contacts[i].contact.normal);
                step.contact_balance_normals[i]=narrow(contacts[i].balance_normal);
                step.contact_balance_normal_available[i]=contacts[i].balance_normal_available;
                if(!step.has_ground)step.ground_height=step.contact_points[i][1];
                step.has_ground=true;
            }
        }
        auto& frame=step.frame;frame.active=true;frame.entity=rider.entity;frame.position=bike_root;
        frame.chassis_quaternion=rotation;frame.chassis_quaternion_available=true;frame.yaw=static_cast<float>(yaw);
        frame.steer=static_cast<float>(solved.front_steer_angle);frame.front_wheel_phase=static_cast<float>(s.front_phase);
        frame.rear_wheel_phase=static_cast<float>(s.rear_phase);
        crank_presentation::Observation crank_sample;
        crank_sample.identity={{rider.client,rider.entity,rider.core,rider.context,rider.rig,body.world},body.token};
        crank_sample.raw_phase=s.crank_phase;crank_sample.serial=body.serial;
        crank_sample.time=body.simulation_time;crank_sample.seconds=native_h;
        crank_sample.enabled=actions.active;crank_sample.pedaling=actions.pedal>0;
        crank_sample.reserved=actions.movement!=catalog::TrickId::none||
            step.tricks.movement_id!=catalog::TrickId::none||step.tricks.movement_phase!=0||
            step.tricks.movement_crank_angle!=0||step.tricks.tail_rotating||step.tricks.tail_waiting_catch||
            step.tricks.tail_release!=0||step.tricks.release_left_foot!=0||step.tricks.release_right_foot!=0||
            step.tricks.pending_units>0||step.tricks.landing_pending;
        const auto crank=crank_presentation::step(s.crank_presentation,crank_sample);
        s.crank_presentation=crank.state;
        frame.crank_phase=static_cast<float>(crank.state.initialized?crank.state.phase:s.crank_phase);
        frame.airborne=!solved.grounded;frame.preload=static_cast<float>(actions.preload);
        // Same accepted native step as the compression policy; never wall time
        // or the number of render callbacks. Duplicate samples returned above.
        frame.air_extension_blend=landing_presentation::step_air_extension(s.last.frame.air_extension_blend,
            native_h,frame.airborne).blend;
        frame.air_extension_available=true;
        frame.front_weight=static_cast<float>(actions.front_weight);frame.rear_weight=static_cast<float>(actions.rear_weight);
        frame.barspin=static_cast<float>(step.tricks.bar_angle+step.tricks.movement_bar_angle);
        frame.tailwhip=static_cast<float>(step.tricks.tail_angle+step.tricks.movement_tail_angle);
        frame.crank_phase+=static_cast<float>(step.tricks.movement_crank_angle);
        frame.barspin_release=static_cast<float>(step.tricks.bar_release);frame.tailwhip_release=static_cast<float>(step.tricks.tail_release);
        frame.movement_id=step.tricks.movement_id;frame.movement_phase=static_cast<float>(step.tricks.movement_phase);
        frame.movement_progress=static_cast<float>(step.tricks.movement_progress);
        frame.movement_timing_available=true;frame.native_body_token=body.token;
        frame.movement_holding=step.tricks.movement_holding;frame.movement_recovering=step.tricks.movement_recovering;
        frame.movement_caught=step.tricks.movement_caught;
        frame.movement_recovery_progress=static_cast<float>(step.tricks.movement_recovery_progress);
        frame.movement_bike_pitch=static_cast<float>(step.tricks.movement_bike_pitch);
        frame.movement_bike_roll=static_cast<float>(step.tricks.movement_bike_roll);
        frame.release_left_hand=static_cast<float>(step.tricks.release_left_hand);
        frame.release_right_hand=static_cast<float>(step.tricks.release_right_hand);
        frame.release_left_foot=static_cast<float>(step.tricks.release_left_foot);
        frame.release_right_foot=static_cast<float>(step.tricks.release_right_foot);
        frame.pedal_effort=static_cast<float>(actions.pedal);
        // Scoring retains first-touch severity. Presentation consumes it once;
        // later actual support uses only fresh post-contact closing speed.
        frame.landing_compression=landing_presentation::step(s.last.frame.landing_compression,
            native_h,body.serial,supported,approach).compression;
        // Only measured actual support/normal observations can create this pose
        // cue. No desired pitch, continuation balance normal, or wall time.
        rider_posture::Observation posture_sample;posture_sample.seconds=native_h;posture_sample.serial=body.serial;
        posture_sample.forward=forward;posture_sample.supported=actual_wheel_support;
        for(unsigned i=0;i<contacts.size();++i) {
            posture_sample.normal_available[i]=actual_wheel_support[i]&&contacts[i].contact.status==tire::ContactStatus::hit;
            posture_sample.normal[i]=narrow(contacts[i].contact.normal);
        }
        const auto posture=rider_posture::step(s.rider_posture,posture_sample);
        // A presentation rejection cannot park/force the physical bike.
        if(posture.valid) {
            s.rider_posture=posture.state;frame.rider_uphill_blend=posture.state.uphill;
            frame.rider_gaze_counterpitch=posture.state.counterpitch;frame.rider_posture_available=true;
        } else {s.rider_posture={};frame.rider_posture_available=false;}
        s.previous_body=body;
        s.last=step;return step;
    } catch(...) {return reject(s,base,rider,"chassis_bridge_exception");}
}
} // namespace dingosdk::bmx::chassis_ride
