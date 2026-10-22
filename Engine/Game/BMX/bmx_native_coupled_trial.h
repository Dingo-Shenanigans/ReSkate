#pragma once
#include "bmx_native_coupled_policy.h"
#include <algorithm>

namespace dingosdk::bmx::native_coupled::trial_policy {
namespace np=policy;
// This is a finite private native execution gate, not an approval/collision
// cache. Only the human-approved launcher sets both flags in the exact profile.
inline constexpr std::uint64_t duration_ms=10000,group_ms=100;
template<class Frame> bool qualified(const Frame& frame,bool active) noexcept {
    if(!frame.available||!np::valid_epoch(frame.epoch)||frame.active!=active||frame.detached||frame.parked==active)return false;
    for(unsigned role=0;role<2;++role){const auto& body=frame.roles[role];np::Quat q;
        if(!body.available||body.active!=active||body.detached||body.world!=frame.epoch.bodies[role].world||body.native_state!=frame.epoch.owner_world[6]||
           body.body_index!=frame.epoch.bodies[role].index||body.body_generation!=frame.epoch.bodies[role].generation||body.token!=frame.epoch.token||
           body.serial!=frame.epoch.serial||body.simulation_time!=frame.epoch.time||body.step_seconds!=frame.epoch.seconds||
           !np::finite(body.position)||!np::finite(body.linear_velocity,50)||!np::finite(body.angular_velocity,40)||!np::unit(body.orientation,q)||
           !std::isfinite(body.mass)||std::abs(body.mass-np::v2_roles[role].mass)>.0001f||frame.maximum_angular_speed[role]!=15||!frame.integrates_angular_accumulator[role])return false;
        for(unsigned axis=0;axis<3;++axis){const float value=body.inertia[axis],base=np::v2_roles[role].principal_moments[axis];
            if(!std::isfinite(value)||value<base-.0001f||value>150||((!active||!role)&&std::abs(value-base)>.0001f))return false;}
    }
    for(unsigned shape=0;shape<8;++shape)if(frame.categories[shape]!=0||frame.collides_with[shape]!=(active?0x101u:0u))return false;
    return true;
}
template<class Frame> bool same_actual(const Frame& a,const Frame& b) noexcept {
    if(a.epoch!=b.epoch||a.maximum_angular_speed!=b.maximum_angular_speed||a.integrates_angular_accumulator!=b.integrates_angular_accumulator||
       a.categories!=b.categories||a.collides_with!=b.collides_with)return false;
    for(unsigned role=0;role<2;++role){const auto& x=a.roles[role];const auto& y=b.roles[role];
        if(x.position!=y.position||x.orientation!=y.orientation||x.linear_velocity!=y.linear_velocity||x.angular_velocity!=y.angular_velocity||
           x.mass!=y.mass||x.inertia!=y.inertia||a.model_orientation[role]!=b.model_orientation[role])return false;}
    return true;
}
class Clock {
    bool used_{},active_{};std::uint64_t until_{};np::Epoch identity_;
public:
    bool begin(const np::Epoch& actual,std::uint64_t now,bool exact_profile,bool ride_flag,bool trial_flag) noexcept {
        if(used_||!exact_profile||!ride_flag||!trial_flag||!np::valid_epoch(actual)||now>std::numeric_limits<std::uint64_t>::max()-duration_ms)return false;
        used_=active_=true;identity_=actual;until_=now+duration_ms;return true;
    }
    bool accepts(const np::Epoch& actual,std::uint64_t now) const noexcept {
        return active_&&now<until_&&np::valid_epoch(actual)&&actual.owner_world==identity_.owner_world&&actual.bodies==identity_.bodies&&actual.token==identity_.token&&
            actual.serial>=identity_.serial&&actual.time>=identity_.time;
    }
    std::uint64_t deadline(const np::Epoch& actual,std::uint64_t now) const noexcept {
        return accepts(actual,now)&&now<=std::numeric_limits<std::uint64_t>::max()-group_ms?std::min(until_,now+group_ms):0;
    }
    void cancel() noexcept {active_=false;}
    bool used() const noexcept{return used_;}
};
}
