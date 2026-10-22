#pragma once
#include "chassis_transform.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx::mount_transition {
using Vec=chassis::Vector;
using Quat=chassis::Quaternion;
inline constexpr float minimum_seconds=.8f;
inline constexpr float relative_speed_limit=1.2f;
// Longer approaches need native walking/stepping before the mount itself.
// Covers the existing 0.65m set-down offset without dragging a planted leg.
inline constexpr float maximum_entry_distance=.7f;
inline constexpr float maximum_pickup_distance=1.8f;
inline bool interaction_distance(float distance,bool pickup) noexcept {
    return std::isfinite(distance)&&distance>=0&&distance<=(pickup?maximum_pickup_distance:maximum_entry_distance);
}
struct BodyIdentity {
    std::array<std::uintptr_t,5> owner{}; // client/entity/core/context/rig
    std::uintptr_t world{},state{};
    std::uint32_t index=0xffffffffU,generation{};
    std::uint64_t token{};
};
inline bool remount_matches(const BodyIdentity& parked,const BodyIdentity& acquired) noexcept {
    return parked.token&&acquired.token==parked.token+1&&acquired.token&&parked.owner==acquired.owner&&
        parked.world&&parked.world==acquired.world&&parked.state&&parked.state==acquired.state&&
        parked.index!=0xffffffffU&&parked.index==acquired.index&&parked.generation==acquired.generation;
}
inline constexpr float angular_speed_limit=2.35619449f; // 135 degrees/s
inline constexpr float root_phase_end=.65f; // approach/alignment precedes final pedal settling
inline float smooth(float t) noexcept { t=std::clamp(t,0.f,1.f);return t*t*(3-2*t); }
inline float phase(float t,float from,float to) noexcept { return smooth((t-from)/(to-from)); }
inline float length(Vec v) noexcept { return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
inline Vec sub(Vec a,Vec b) noexcept { for(unsigned i=0;i<3;++i)a[i]-=b[i];return a; }
inline float dot(Quat a,Quat b) noexcept { float d=0;for(unsigned i=0;i<4;++i)d+=a[i]*b[i];return d; }
inline float angle(Quat a,Quat b) noexcept { return 2*std::acos(std::clamp(std::abs(dot(a,b)),0.f,1.f)); }
inline Quat shortest_arc(Quat a,Quat b,float t) noexcept {
    float d=dot(a,b);if(d<0){for(auto& v:b)v=-v;d=-d;}
    if(t<=0)return a;
    if(t>=1)return b;
    float wa=1-t,wb=t;
    if(d<.9995f){const float theta=std::acos(std::clamp(d,0.f,1.f)),s=std::sin(theta);
        wa=std::sin((1-t)*theta)/s;wb=std::sin(t*theta)/s;}
    Quat q;float n=0;for(unsigned i=0;i<4;++i){q[i]=a[i]*wa+b[i]*wb;n+=q[i]*q[i];}
    for(auto& v:q)v/=std::sqrt(n);
    return q;
}
// Native rider/root matrix only: reject scale, reflection, shear and nonfinite
// input. No mesh/skeleton basis correction belongs in the native motion lease.
inline bool native_rotation(const std::array<float,16>& m,Quat& output) noexcept {
    for(float v:m)if(!std::isfinite(v))return false;
    const Vec x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    const auto scalar=[](Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    if(std::abs(scalar(x,x)-1)>.002f||std::abs(scalar(y,y)-1)>.002f||std::abs(scalar(z,z)-1)>.002f||
       std::abs(scalar(x,y))>.002f||std::abs(scalar(x,z))>.002f||std::abs(scalar(y,z))>.002f||
       scalar(Vec{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]},z)<.998f)return false;
    Quat q;const float trace=m[0]+m[5]+m[10];
    if(trace>0){const float s=2*std::sqrt(trace+1);q={(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,s*.25f};}
    else if(m[0]>m[5]&&m[0]>m[10]){const float s=2*std::sqrt(1+m[0]-m[5]-m[10]);q={s*.25f,(m[4]+m[1])/s,(m[8]+m[2])/s,(m[6]-m[9])/s};}
    else if(m[5]>m[10]){const float s=2*std::sqrt(1+m[5]-m[0]-m[10]);q={(m[4]+m[1])/s,s*.25f,(m[9]+m[6])/s,(m[8]-m[2])/s};}
    else {const float s=2*std::sqrt(1+m[10]-m[0]-m[5]);q={(m[8]+m[2])/s,(m[9]+m[6])/s,s*.25f,(m[1]-m[4])/s};}
    return chassis::unit_quaternion(q,output);
}
struct Sample {
    bool ready{};
    Vec root{},velocity{};
    Quat rotation{0,0,0,1};
    float progress{},blend{},duration{},initial_distance{},initial_angle{};
    bool from_left{};
};
class Transition {
public:
    bool capture(Vec root,Quat rotation) noexcept {
        *this={};
        Quat checked;if(!chassis::finite(root)||!chassis::unit_quaternion(rotation,checked))return false;
        start_=root;rotation_=checked;captured_=true;return true;
    }
    // Initial duration bounds the added approach/alignment speed. A moving bike
    // contributes its separately measured rider-point velocity. Never infer that
    // velocity from COM alone: rotating the root-height offset also moves it.
    Sample sample(Vec target,Quat rotation,double seconds) noexcept {
        Quat checked;if(!captured_||!chassis::finite(target)||!chassis::unit_quaternion(rotation,checked)||
            !std::isfinite(seconds)||seconds<0||seconds>.1)return {};
        if(!started_) {
            const auto delta=sub(target,start_);
            distance_=length(delta);angle_=angle(rotation_,checked);
            if(distance_>maximum_entry_distance)return {};
            duration_=std::max({minimum_seconds,1.5f*distance_/(relative_speed_limit*root_phase_end),
                1.5f*angle_/(angular_speed_limit*root_phase_end)});
            const auto left=chassis::rotate_unit(checked,{1,0,0});
            from_left_=delta[0]*left[0]+delta[1]*left[1]+delta[2]*left[2]<=0;
            previous_target_=target;started_=true;
        } else if(seconds==0)return last_;
        else {
            const auto delta=sub(target,previous_target_);
            for(unsigned i=0;i<3;++i)target_velocity_[i]=delta[i]/static_cast<float>(seconds);
            if(length(target_velocity_)>28)return {};
            previous_target_=target;elapsed_=std::min(double(duration_),elapsed_+seconds);
        }
        const float t=static_cast<float>(elapsed_/duration_),root_t=std::min(1.f,t/root_phase_end),a=smooth(root_t),
            rate=6*root_t*(1-root_t)/(duration_*root_phase_end);
        Sample next;next.ready=true;next.progress=t;next.blend=a;next.duration=duration_;next.from_left=from_left_;
        next.initial_distance=distance_;next.initial_angle=angle_;
        next.rotation=shortest_arc(rotation_,checked,a);
        for(unsigned i=0;i<3;++i){const float delta=target[i]-start_[i];next.root[i]=start_[i]+delta*a;
            next.velocity[i]=target_velocity_[i]*a+delta*rate;}
        last_=next;return next;
    }
private:
    bool captured_{},started_{},from_left_{};
    Vec start_{},previous_target_{},target_velocity_{};
    Quat rotation_{0,0,0,1};
    double elapsed_{};float duration_{},distance_{},angle_{};
    Sample last_;
};
} // namespace dingosdk::bmx::mount_transition
