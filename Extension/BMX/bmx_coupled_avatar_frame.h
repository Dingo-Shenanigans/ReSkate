#pragma once
#include "bmx_visual.h"
#include "bmx_native_motion.h"

namespace dingosdk::bmx::coupled_avatar_frame {
namespace ab=avatar_binding;namespace np=native_coupled::policy;
template<class Pair> ab::Observation observe(const Pair& pair) noexcept {
    ab::Observation out;if(!pair.available||!np::valid_epoch(pair.epoch))return out;
    std::array<np::Quat,2> model{};
    for(unsigned role=0;role<2;++role){const auto& body=pair.roles[role];np::Quat principal;
        if(!body.available||body.world!=pair.epoch.bodies[role].world||body.body_index!=pair.epoch.bodies[role].index||
           body.body_generation!=pair.epoch.bodies[role].generation||body.token!=pair.epoch.token||body.serial!=pair.epoch.serial||
           body.simulation_time!=pair.epoch.time||body.step_seconds!=pair.epoch.seconds||!np::finite(body.position)||
           !np::finite(body.linear_velocity,50)||!np::finite(body.angular_velocity,40)||!np::unit(pair.model_orientation[role],model[role])||
           !np::unit(np::v2_roles[role].principal_to_model,principal)||
           !np::pose_matches(body.position,body.orientation,body.position,np::native_orientation(model[role],principal)))return out;
    }
    out.epoch=pair.epoch;out.spring_com=pair.roles[1].position;out.spring_velocity=pair.roles[1].linear_velocity;
    out.spring_angular_velocity=pair.roles[1].angular_velocity;out.spring_orientation=model[1];out.bike_orientation=model[0];
    out.bike_root=ab::sub(pair.roles[0].position,np::rotate(model[0],np::v2_roles[0].com_from_bike_root));
    out.available=true;if(!ab::valid(out))return {};return out;
}
inline bool owner_epoch(const NativeFrame& native,const np::Epoch& e) noexcept {
    return native.available&&native.timing_available&&native.offboard&&native.motion_supported&&np::valid_epoch(e)&&
        native.client==e.owner_world[0]&&native.entity==e.owner_world[1]&&native.core==e.owner_world[2]&&native.context==e.owner_world[3]&&
        native.rig==e.owner_world[4]&&native.serial==e.serial&&native.simulation_time==e.time&&native.seconds==e.seconds;
}
inline bool same_native(const NativeFrame& a,const NativeFrame& b,const np::Epoch& e) noexcept {
    return owner_epoch(a,e)&&owner_epoch(b,e)&&a.position==b.position&&a.transform==b.transform&&a.standable==b.standable;
}
inline bool forward_epoch(const np::Epoch& before,const np::Epoch& now) noexcept {
    return ab::same_identity(before,now)&&(before==now||(now.serial>before.serial&&now.time>before.time));
}
template<class Pair> bool same_pair(const Pair& a,const Pair& b) noexcept {
    if(!a.available||!b.available||a.epoch!=b.epoch||a.active!=b.active||a.parked!=b.parked||a.detached!=b.detached||
       a.categories!=b.categories||a.collides_with!=b.collides_with||a.maximum_angular_speed!=b.maximum_angular_speed||
       a.integrates_angular_accumulator!=b.integrates_angular_accumulator)return false;
    for(unsigned role=0;role<2;++role){const auto& x=a.roles[role];const auto& y=b.roles[role];
        if(x.available!=y.available||x.world!=y.world||x.body_index!=y.body_index||x.body_generation!=y.body_generation||
           x.token!=y.token||x.serial!=y.serial||x.simulation_time!=y.simulation_time||x.step_seconds!=y.step_seconds||
           x.position!=y.position||x.orientation!=y.orientation||x.linear_velocity!=y.linear_velocity||x.angular_velocity!=y.angular_velocity||
           x.mass!=y.mass||x.inertia!=y.inertia||a.model_orientation[role]!=b.model_orientation[role])return false;}
    return true;
}
inline ab::NativeRoot root_witness(const ab::Observation& actual,const NativeFrame& native,bool enclosing_reads_revalidated) noexcept {
    ab::NativeRoot out;if(!enclosing_reads_revalidated||!ab::valid(actual)||!owner_epoch(native,actual.epoch))return out;
    np::Quat basis;if(!mount_transition::native_rotation(native.transform,basis)||!np::finite(native.position))return out;
    out.available=out.coherent_epoch=true;out.epoch=actual.epoch;out.position=native.position;out.transform=native.transform;return out;
}
}
