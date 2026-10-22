#pragma once
#include "bmx_standalone_coupled.h"
#include "Engine/Game/BMX/chassis_transform.h"

// Pure measured-frame conversion. No native calls, controller steps, pose
// integration, guessed body state, physical setters, or fall requests.
namespace dingosdk::bmx::coupled_ride_frame {
namespace np=native_coupled::policy;
using Vec=np::Vec;using Quat=np::Quat;
struct RiderWitness {
    bool available{};
    Vec center_of_mass{},linear_velocity{},angular_velocity{};
    Quat model_orientation{0,0,0,1};
    // A measured Spring-driven rig-root mapping using a caller's actual native
    // ground-to-root calibration. This is a target witness, not avatar adoption.
    Vec rig_root{},rig_point_velocity{};Quat rig_rotation{0,0,0,1};
    np::Epoch epoch;
};
struct Converted {
    bool available{};Vec bike_root{};Quat bike_rotation{0,0,0,1};
    std::array<::bmx::Body,2> bodies;
    std::array<std::array<std::uint64_t,2>,2> ignored_bodies{};
    RiderWitness spring;const char* issue="invalid_actual_pair";
};
inline Vec sub(Vec a,Vec b) noexcept{for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec add(Vec a,Vec b) noexcept{for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec cross(Vec a,Vec b) noexcept{return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
template<class ActualPair> Converted convert(const ActualPair& pair,float actual_native_root_height) noexcept {
    Converted out;
    if(!pair.available||!np::valid_epoch(pair.epoch)||!std::isfinite(actual_native_root_height)||actual_native_root_height<0||actual_native_root_height>2)return out;
    std::array<Quat,2> models{};
    for(unsigned role=0;role<2;++role){const auto& native=pair.roles[role];auto& body=out.bodies[role];Quat model,principal;
        if(!native.available||native.active!=pair.active||native.detached!=pair.detached||native.world!=pair.epoch.bodies[role].world||native.body_index!=pair.epoch.bodies[role].index||
            native.native_state!=pair.epoch.owner_world[6]||
            native.body_generation!=pair.epoch.bodies[role].generation||native.token!=pair.epoch.token||native.serial!=pair.epoch.serial||
            native.simulation_time!=pair.epoch.time||native.step_seconds!=pair.epoch.seconds||
            !np::unit(pair.model_orientation[role],model)||!np::unit(np::v2_roles[role].principal_to_model,principal)||
            !np::pose_matches(native.position,native.orientation,native.position,np::native_orientation(model,principal))||
            !np::finite(native.position)||!np::finite(native.linear_velocity,50)||!np::finite(native.angular_velocity,40))return out;
        body.mass=native.mass;
        for(unsigned i=0;i<3;++i){body.center_of_mass[i]=native.position[i];body.linear_velocity[i]=native.linear_velocity[i];
            body.angular_velocity[i]=native.angular_velocity[i];body.inertia_body[i]=native.inertia[i];}
        for(unsigned i=0;i<4;++i){body.orientation[i]=model[i];body.inertia_orientation[i]=principal[i];}
        if(!standalone_coupled::body_matches_role(body,role))return out;
        models[role]=model;
        out.ignored_bodies[role]={native.world,(std::uint64_t{native.body_generation}<<32)|native.body_index};
    }
    out.bike_rotation=models[0];
    out.bike_root=sub(pair.roles[0].position,np::rotate(out.bike_rotation,np::v2_roles[0].com_from_bike_root));
    Quat relative;if(!np::unit(np::rider_from_chassis,relative))return out;
    auto& spring=out.spring;spring.available=true;spring.epoch=pair.epoch;
    spring.center_of_mass=pair.roles[1].position;spring.linear_velocity=pair.roles[1].linear_velocity;
    spring.angular_velocity=pair.roles[1].angular_velocity;spring.model_orientation=models[1];
    spring.rig_rotation=np::multiply(spring.model_orientation,np::conjugate(relative));
    const auto calibration_local=np::rotate(np::conjugate(relative),sub(Vec{0,actual_native_root_height,0},np::v2_roles[1].com_from_bike_root));
    const auto world_offset=np::rotate(spring.model_orientation,calibration_local);
    spring.rig_root=add(spring.center_of_mass,world_offset);
    spring.rig_point_velocity=add(spring.linear_velocity,cross(spring.angular_velocity,world_offset));
    if(!np::finite(out.bike_root)||!np::finite(spring.rig_root)||!np::finite(spring.rig_point_velocity,50))return Converted{};
    out.available=true;out.issue="actual_principal_and_model_frames_bound";return out;
}

// Producer completion is not a commit until the following actual native body
// sample exists. No failed, cancelled, cross-owner, skipped or rewound epoch can
// be made valid by retaining an old pose or replaying the queued command.
enum class Pending {wait,next,reject};
inline Pending pending_status(const np::Epoch& queued,std::uint64_t hop,const np::Epoch& actual,const np::Receipt& receipt) noexcept {
    if(!np::valid_epoch(queued)||!np::valid_epoch(actual)||!standalone_coupled::same_identity(queued,actual)||
        receipt.epoch!=queued||receipt.token!=queued.token||receipt.serial!=queued.serial||receipt.hop_part!=hop||
        (receipt.state!=np::ReceiptState::queued&&receipt.state!=np::ReceiptState::submitted))return Pending::reject;
    if(actual==queued)return Pending::wait;
    if(queued.serial==std::numeric_limits<std::uint64_t>::max()||actual.serial!=queued.serial+1||
        std::abs(actual.time-queued.time-actual.seconds)>1e-7||actual.seconds!=queued.seconds||receipt.state!=np::ReceiptState::submitted)
        return Pending::reject;
    return Pending::next;
}
template<class ActualPair> bool same_observation(const ActualPair& a,const ActualPair& b) noexcept {
    if(!a.available||!b.available||a.epoch!=b.epoch||a.active!=b.active||a.parked!=b.parked||a.detached!=b.detached||
       a.maximum_angular_speed!=b.maximum_angular_speed||a.integrates_angular_accumulator!=b.integrates_angular_accumulator||
       a.categories!=b.categories||a.collides_with!=b.collides_with)return false;
    for(unsigned role=0;role<2;++role){const auto& x=a.roles[role];const auto& y=b.roles[role];
        if(x.available!=y.available||x.active!=y.active||x.detached!=y.detached||x.world!=y.world||x.native_state!=y.native_state||
           x.body_index!=y.body_index||x.body_generation!=y.body_generation||x.token!=y.token||x.serial!=y.serial||
           x.simulation_time!=y.simulation_time||x.step_seconds!=y.step_seconds||
           x.position!=y.position||x.orientation!=y.orientation||x.linear_velocity!=y.linear_velocity||x.angular_velocity!=y.angular_velocity||
           x.mass!=y.mass||x.inertia!=y.inertia||a.model_orientation[role]!=b.model_orientation[role])return false;}
    return true;
}
}
