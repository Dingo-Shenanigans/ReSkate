#include "bmx_coupled_ride.h"
#include "bmx_collision_envelope.h"
#include "bmx_body_guard.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <Windows.h>
#include <limits>

namespace dingosdk::bmx::coupled_ride {
namespace {
namespace nc=native_coupled;namespace cf=coupled_ride_frame;namespace sc=standalone_coupled;
using Vec=nc::Vec;using Quat=nc::Quat;using Contacts=std::array<chassis_control::ContactSample,2>;
struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}};
struct State {
    bool prepared{},active{},confirmed{},have_previous{};std::uintptr_t base{};NativeFrame owner;float root_height{};
    sc::Controller controller;std::optional<sc::Evaluation> pending;::bmx::CoupledOutput accepted;
    nc::Frame previous;std::array<bail_policy::Policy,2> bail;Result last;
};
State& state(){static auto* value=new State;return *value;}
bool owner_matches(const NativeFrame& a,const NativeFrame& b) noexcept {
    return a.available&&b.available&&a.client==b.client&&a.entity==b.entity&&a.core==b.core&&a.context==b.context&&a.rig==b.rig;
}
bool owner_matches(const NativeFrame& a,const nc::Frame& b) noexcept {
    return a.available&&a.timing_available&&b.available&&a.client==b.epoch.owner_world[0]&&a.entity==b.epoch.owner_world[1]&&
        a.core==b.epoch.owner_world[2]&&a.context==b.epoch.owner_world[3]&&a.rig==b.epoch.owner_world[4]&&
        a.serial==b.epoch.serial&&a.simulation_time==b.epoch.time&&a.seconds==b.epoch.seconds;
}
Vec narrow(const ::bmx::Vec3& v) noexcept{return {static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};}
std::array<double,3> wide(Vec v) noexcept{return {v[0],v[1],v[2]};}
Vec point_velocity(const ::bmx::Body& body,Vec point) noexcept {
    return cf::add(narrow(body.linear_velocity),cf::cross(narrow(body.angular_velocity),cf::sub(point,narrow(body.center_of_mass))));
}
Result reject(std::uintptr_t base,const NativeFrame& owner,const char* issue,const Result* attempted=nullptr) noexcept {
    auto& s=state();const auto before=nc::status();if(attempted)s.last=*attempted;else s.last.actual_pair_observed_this_update=false;
    s.last.native_before_cleanup=before;s.last.native_before_cleanup_available=true;nc::cancel();s.controller.invalidate();s.pending.reset();
    if(s.prepared)(void)nc::park(base,owner);
    s.active=false;s.have_previous=false;s.last.step.ready=false;s.last.step.command_submitted=false;s.last.step.frame.active=false;
    s.last.physics_active=false;s.last.issue=issue;s.last.step.issue=issue;return s.last;
}
bool wheel_query(const nc::Frame& pair,const cf::Converted& actual,const NativeFrame& owner,unsigned index,float steer,
    chassis_control::ContactSample& sample,chassis_ride::Step& step) {
    const auto q=actual.bike_rotation;
    const auto up=nc::policy::rotate(q,index==0?geometry::rotate_direction({0,1,0},geometry::steering_axis,steer):Vec{0,1,0});
    const auto forward=nc::policy::rotate(q,{0,0,1});
    const auto query_forward=nc::policy::rotate(q,index==0?geometry::rotate_direction({0,0,1},geometry::steering_axis,steer):Vec{0,0,1});
    const auto left=nc::policy::rotate(q,index==0?geometry::rotate_direction({1,0,0},geometry::steering_axis,steer):Vec{1,0,0});
    const auto hub=cf::add(actual.bike_root,nc::policy::rotate(q,geometry::wheel_hub(index==0,steer)));
    Vec bottom=hub;for(unsigned axis=0;axis<3;++axis)bottom[axis]-=up[axis]*static_cast<float>(geometry::wheel_radius);
    sample.contact.status=tire::ContactStatus::clear;sample.contact.point=wide(bottom);sample.contact.normal=wide(up);
    sample.contact.forward=wide(forward);sample.contact.surface_velocity={};sample.separation=.25;
    const auto query=[&](Vec from,Vec to)->wheel_surface::RaySample {
        const auto hit=probe_local_surface(from,to,actual.ignored_bodies,owner.entity);
        if(!hit.available||hit.world!=pair.epoch.owner_world[5]||hit.context!=pair.epoch.owner_world[7])return {};
        return {true,hit.hit,hit.position,hit.fraction,hit.hit_entity,hit.hit_body};
    };
    wheel_surface::Settings settings;settings.radius=static_cast<float>(geometry::wheel_radius);
    const auto surface=wheel_surface::search(hub,up,query_forward,left,query,settings);
    step.contact_rays[index]=surface.rays;step.contact_edge_uncertainty[index]=surface.edge_uncertainty;
    step.contact_edge_diagnostics[index]=surface.edge_diagnostic;
    step.contact_features[index]=surface.feature==wheel_surface::Feature::plane?"plane":surface.feature==wheel_surface::Feature::edge?"edge":"none";
    if(!surface.available)return false;if(!surface.hit)return true;
    sample.separation=surface.separation;sample.contact.point=wide(surface.point);sample.contact.normal=wide(surface.normal);
    sample.balance_normal=wide(surface.balance_normal);sample.balance_normal_available=surface.balance_normal_available;
    sample.verified_plane=surface.feature==wheel_surface::Feature::plane;
    if(surface.separation<=.03)sample.contact.status=tire::ContactStatus::hit;
    step.queried_surfaces[index]={true,sample.verified_plane,surface.point,surface.normal,forward,surface.separation,surface.entity,surface.body};
    return true;
}
Vec guard_center(const cf::Converted& actual,unsigned role,Vec authored) noexcept {
    if(!role)return cf::add(actual.bike_root,nc::policy::rotate(actual.bike_rotation,authored));
    Quat relative{0,0,0,1};(void)nc::policy::unit(nc::policy::rider_from_chassis,relative);
    const auto local=nc::policy::rotate(nc::policy::conjugate(relative),cf::sub(authored,nc::policy::v2_roles[1].com_from_bike_root));
    return cf::add(actual.spring.center_of_mass,nc::policy::rotate(actual.spring.model_orientation,local));
}
bool impact_query(State& s,const nc::Frame& pair,const cf::Converted& actual,const NativeFrame& owner,
    std::array<bail_policy::Sample,2>& samples,chassis_ride::Step& step) {
    cf::Converted previous;bool consecutive=false;
    if(s.have_previous&&sc::same_identity(s.previous.epoch,pair.epoch)&&s.previous.epoch.serial!=std::numeric_limits<std::uint64_t>::max()&&
        s.previous.epoch.serial+1==pair.epoch.serial){previous=cf::convert(s.previous,s.root_height);consecutive=previous.available;}
    for(unsigned role=0;role<2;++role){auto& sample=samples[role];const auto& body=pair.roles[role];
        sample.owner={owner.client,owner.entity,owner.core,owner.context,owner.rig,body.world,body.body_index,body.body_generation,body.token};
        sample.serial=pair.epoch.serial;sample.seconds=pair.epoch.seconds;sample.mounted=true;
        sample.body_up=wide(nc::policy::rotate(role?actual.spring.model_orientation:actual.bike_rotation,{0,1,0}));
        const auto query=[&](Vec from,Vec to)->body_guard::Ray {const auto hit=probe_local_surface(from,to,actual.ignored_bodies,owner.entity);
            if(!hit.available||hit.world!=pair.epoch.owner_world[5]||hit.context!=pair.epoch.owner_world[7])return {};
            return {true,hit.hit,hit.position,hit.fraction};};
        for(unsigned index=role?4u:0u;index<(role?8u:4u);++index){const auto& guard=collision_envelope::guards[index];
            const auto center=guard_center(actual,role,guard.center),old_center=consecutive?guard_center(previous,role,guard.center):center;
            const auto found=body_guard::search(center,guard.radius,point_velocity(actual.bodies[role],center),old_center,
                consecutive?point_velocity(previous.bodies[role],old_center):point_velocity(actual.bodies[role],center),consecutive,query);
            step.body_guard_rays+=found.rays;if(!found.available)return false;
            auto& contact=sample.contacts[sample.contact_count++];contact.region=guard.region;contact.available=true;contact.hit=found.hit;
            contact.query_serial=pair.epoch.serial;contact.point=wide(found.point);contact.normal=wide(found.normal);contact.separation=found.separation;
            contact.closing_speed=found.closing_speed;contact.swept_crossing=found.swept_crossing;
        }
    }
    return true;
}
void measured_pose(const nc::Frame& pair,const cf::Converted& actual,Result& result) noexcept {
    auto& step=result.step;const auto& body=pair.roles[0];
    step.observed_com=body.position;step.observed_quaternion=actual.bike_rotation;step.velocity=body.linear_velocity;step.angular_velocity=body.angular_velocity;
    step.target_rider_root=actual.spring.rig_root;step.body_token=pair.epoch.token;step.serial=pair.epoch.serial;
    step.native_step_seconds=pair.epoch.seconds;step.native_simulation_seconds=pair.epoch.time;
    const auto forward=nc::policy::rotate(actual.bike_rotation,{0,0,1});step.speed=static_cast<float>(nc::policy::length(body.linear_velocity));
    float signed_speed{};for(unsigned axis=0;axis<3;++axis)signed_speed+=body.linear_velocity[axis]*forward[axis];step.speed=signed_speed;
    auto& frame=step.frame;frame.active=true;frame.position=actual.bike_root;frame.entity=pair.epoch.owner_world[1];
    frame.chassis_quaternion=actual.bike_rotation;frame.chassis_quaternion_available=true;frame.native_body_token=pair.epoch.token;
    frame.yaw=std::atan2(forward[0],forward[2]);
    // Physics presentation is explicit while the native Spring-to-avatar/IK
    // binding is unfinished. Never disguise the old rigid chassis avatar as Spring.
    frame.rider_mode=visual::RiderMode::none;
}
void present(State& s,const nc::Frame& pair,const cf::Converted& actual,const Contacts& contacts,Result& result) noexcept {
    measured_pose(pair,actual,result);auto& step=result.step;auto& frame=step.frame;
    step.ready=true;step.control_active=true;step.issue="actual_coupled_physics_avatar_binding_unverified";
    for(unsigned i=0;i<2;++i){const auto& c=contacts[i];step.contact_hit[i]=c.contact.status==tire::ContactStatus::hit;
        step.actual_wheel_support[i]=step.contact_hit[i]&&c.separation<=.03;step.actual_support|=step.actual_wheel_support[i];
        step.wheel_separation[i]=static_cast<float>(c.separation);
        if(step.contact_hit[i]){step.contact_points[i]=narrow(c.contact.point);step.contact_normals[i]=narrow(c.contact.normal);
            if(!step.has_ground)step.ground_height=step.contact_points[i][1];step.has_ground=true;}
    }
    step.grounded=step.actual_support;frame.airborne=!step.grounded;step.wheel_contacts_sampled=step.query_geometry_fresh=true;
    if(s.confirmed){const auto& output=s.accepted.bike;frame.steer=static_cast<float>(output.steering_angle);
        frame.front_wheel_phase=static_cast<float>(output.front_wheel_phase);frame.rear_wheel_phase=static_cast<float>(output.rear_wheel_phase);
        frame.crank_phase=static_cast<float>(output.crank_phase);frame.pedal_effort=static_cast<float>(output.actions.pedal);
        frame.preload=static_cast<float>(output.actions.preload);frame.front_weight=static_cast<float>(output.actions.front_weight);frame.rear_weight=static_cast<float>(output.actions.rear_weight);
        frame.barspin=static_cast<float>(output.tricks.bar_angle+output.tricks.movement_bar_angle);frame.tailwhip=static_cast<float>(output.tricks.tail_angle+output.tricks.movement_tail_angle);
        const auto id=static_cast<catalog::TrickId>(output.tricks.movement_id);
        if(catalog::valid(id)){frame.movement_id=id;frame.movement_phase=static_cast<float>(output.tricks.movement_phase);frame.movement_progress=static_cast<float>(output.tricks.movement_progress);}
        frame.release_left_hand=static_cast<float>(output.tricks.release_left_hand);frame.release_right_hand=static_cast<float>(output.tricks.release_right_hand);
        frame.release_left_foot=static_cast<float>(output.tricks.release_left_foot);frame.release_right_foot=static_cast<float>(output.tricks.release_right_foot);
        // The supplied module exposes recovery phase/release, but no normalized
        // recovery clock. Do not manufacture the newer host presentation field.
        frame.movement_timing_available=!output.tricks.movement_recovering;frame.movement_holding=output.tricks.movement_holding;
        frame.movement_recovering=output.tricks.movement_recovering;frame.movement_caught=output.tricks.movement_caught;
        // Physical trick rotation is already present in actual native poses.
        frame.movement_bike_pitch=frame.movement_bike_roll=0;
    }
}
}
bool selected() noexcept {LastError error;return nc::controller_trial::selected();}
bool prepare(std::uintptr_t base,const NativeFrame& owner,const mount_pose::Seed& seed,float root_height) noexcept {
    LastError error;auto& s=state();try {
        if(!selected()||!owner.available||!owner.offboard||!std::isfinite(root_height)||root_height<0||root_height>2)return false;
        if(s.prepared){s.last.issue="already_prepared_restart_or_explicit_resume_required";return false;}
        if(!nc::acquire(base,owner,seed)){s.last.issue="paired_acquire_not_ready";return false;}
        s.prepared=true;s.base=base;s.owner=owner;s.root_height=root_height;s.controller.reset();
        for(auto& policy:s.bail)policy=bail_policy::Policy{};s.last.prepared=true;
        s.last.issue="genuine_pair_prepared_keyframed_avatar_entry_pending";return true;
    }catch(...){(void)reject(base,owner,"coupled_prepare_exception");return false;}
}
bool activate_prepared(std::uintptr_t base,const NativeFrame& owner) noexcept {
    LastError error;auto& s=state();try {
        if(!selected()||!s.prepared||s.active||base!=s.base||!owner_matches(s.owner,owner)||!owner.offboard)return false;
        if(!nc::controller_trial::selected()){s.last.issue="private_coupled_trial_two_flags_or_profile_not_selected";return false;}
        if(!nc::controller_trial::activate(base,owner)){s.last.issue="paired_dynamic_admission_refused";return false;}
        s.active=true;s.last.physics_active=true;s.last.issue="coupled_physics_started_avatar_binding_still_unverified";return true;
    }catch(...){(void)reject(base,owner,"coupled_start_exception");return false;}
}
bool start(std::uintptr_t base,const NativeFrame& owner,const mount_pose::Seed& seed,float height) noexcept {
    return prepare(base,owner,seed,height)&&activate_prepared(base,owner);
}
Result update(std::uintptr_t base,const NativeFrame& owner,const RawInput& input) noexcept {
    LastError error;auto& s=state();try {
        if(!s.active)return s.last;
        if(!selected()||base!=s.base||!owner_matches(s.owner,owner)||!owner.offboard||!input.available||!input.focused||input.menu_open)
            return reject(base,owner,"coupled_owner_or_input_gate_changed");
        const auto pair=nc::observe(base,owner);if(!pair.available||!pair.active||!owner_matches(owner,pair))return reject(base,owner,"coupled_actual_pair_not_current");
        const auto actual=cf::convert(pair,s.root_height);if(!actual.available)return reject(base,owner,actual.issue);
        if(nc::policy::length(cf::sub(actual.spring.center_of_mass,pair.roles[0].position))>2)
            return reject(base,owner,"coupled_actual_body_separation_exceeded_two_metres");
        if(!nc::policy::finite(owner.position)||nc::policy::length(cf::sub(actual.spring.center_of_mass,owner.position))>2)
            return reject(base,owner,"coupled_actual_avatar_to_spring_separation_exceeded_two_metres");
        Result result;result.prepared=result.physics_active=true;result.actual_pair=pair;result.actual_pair_observed_this_update=true;result.actual_spring=actual.spring;
        if(s.pending){const auto receipt=nc::status().receipt;const auto& queued=*s.pending;
            const auto advance=cf::pending_status(queued.epoch,queued.command.hop_part,pair.epoch,receipt);
            if(advance==cf::Pending::reject)return reject(base,owner,"coupled_pending_receipt_or_next_epoch_rejected");
            if(advance==cf::Pending::wait){
                result.step=s.last.step;measured_pose(pair,actual,result);result.step.command_submitted=false;result.step.force={};result.step.torque={};result.step.hop_impulse={};
                result.step.wheel_contacts_sampled=result.step.query_geometry_fresh=false;result.step.actual_support=false;result.step.actual_wheel_support={};
                result.step.contact_hit={};result.step.has_ground=result.step.grounded=false;result.step.trick_events={};
                result.issue="waiting_for_next_actual_native_body";result.step.issue=result.issue;s.last=result;return result;}
            sc::Observation actual_post{};actual_post.epoch=pair.epoch;actual_post.bodies=actual.bodies;
            actual_post.maximum_angular_speed=pair.maximum_angular_speed;actual_post.integrates_angular_accumulator=pair.integrates_angular_accumulator;
            if(!s.controller.acknowledge(receipt,actual_post))return reject(base,owner,"coupled_producer_or_actual_inertia_receipt_not_confirmed");
            s.accepted=queued.output;s.confirmed=true;s.pending.reset();result.controller_committed=true;
        }
        Contacts contacts{};const float steer=s.confirmed?static_cast<float>(s.accepted.bike.steering_angle):0;
        for(unsigned i=0;i<2;++i)if(!wheel_query(pair,actual,owner,i,steer,contacts[i],result.step))return reject(base,owner,"coupled_wheel_query_unavailable");
        std::array<bail_policy::Sample,2> impact{};
        if(!impact_query(s,pair,actual,owner,impact,result.step))return reject(base,owner,"coupled_body_query_unavailable");
        const auto after=nc::observe(base,owner);const auto fresh=read_native_frame(base,owner.client);
        if(!cf::same_observation(pair,after)||!fresh.offboard||!owner_matches(owner,fresh)||!owner_matches(fresh,after))return reject(base,owner,"coupled_query_owner_body_or_epoch_changed");
        for(unsigned role=0;role<2;++role){const auto bail=s.bail[role].step(impact[role]);
            if(!bail.valid)return reject(base,owner,"coupled_invalid_actual_impact_sample");
            if(bail.bail){result.step.bail=bail;result.step.impact_sample=impact[role];nc::cancel();
                // Native Spring angular/linear-to-ragdoll momentum transfer is
                // unproved. Keep this explicit and park BOTH rather than launch
                // a native rider through the old one-body handoff.
                (void)nc::park(base,fresh);s.controller.invalidate();s.pending.reset();s.active=false;result.physics_active=false;
                result.issue="coupled_fall_momentum_handoff_unverified_both_roles_parked";result.step.issue=result.issue;s.last=result;return result;}
        }
        present(s,pair,actual,contacts,result);
        sc::Observation observation;observation.epoch=pair.epoch;observation.bodies=actual.bodies;observation.contacts=contacts;
        observation.maximum_angular_speed=pair.maximum_angular_speed;observation.integrates_angular_accumulator=pair.integrates_angular_accumulator;
        const auto evaluated=s.controller.evaluate(input,observation);result.evaluated=true;
        if(!evaluated.valid)return reject(base,fresh,"coupled_controller_invalid_unconverged_or_over_budget");
        result.command=evaluated.command;result.command_available=true;
        if(!nc::controller_trial::submit(pair,evaluated.command))return reject(base,fresh,"coupled_paired_command_not_queued",&result);
        s.pending=evaluated;result.queued=true;result.step.command_submitted=true;result.step.force=evaluated.command.roles[0].force;
        result.step.torque=evaluated.command.roles[0].torque;result.step.hop_impulse=evaluated.command.roles[0].hop_impulse;
        result.issue="actual_coupled_physics_queued_avatar_binding_unverified";s.previous=pair;s.have_previous=true;s.last=result;return result;
    }catch(...){return reject(base,owner,"coupled_ride_exception");}
}
void stop(std::uintptr_t base,const NativeFrame& owner) noexcept {LastError error;auto& s=state();(void)reject(base,owner,"coupled_stopped");s.prepared=false;s.confirmed=false;s.last={};}
Result status() noexcept{return state().last;}
}
