#pragma once
#include "bmx_visual.h"
#include <algorithm>
#include <array>
#include <cmath>
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
#include <bmx/pose_track.hpp>
#endif

namespace dingosdk::bmx::rider::observed {
using Vec=std::array<float,3>;
// Neutral-relative holder-local POSITION cues only. This holder is not a
// native skeleton ancestor. Native IK still owns lengths and closed contacts;
// no observed quaternion, source body pose or unbound IK weight is written.
struct Targets {
    bool available{};float strength{1},retarget_scale{},pole_weight{};
    Vec hip{},torso{};
    std::array<Vec,2> palms{},ankles{},arm_poles{},leg_poles{};
};
inline bool finite(Vec v) noexcept {for(float x:v)if(!std::isfinite(x))return false;return true;}
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec v,float s) noexcept {for(auto& x:v)x*=s;return v;}
inline float length(Vec v) noexcept {float n=0;for(float x:v)n+=x*x;return std::sqrt(n);}
inline bool eligible(const visual::Frame& f,bool selected) noexcept {
    return selected&&f.active&&f.entity&&f.native_body_token&&f.rider_mode==visual::RiderMode::riding&&
        f.rider_blend==1&&f.mount_progress==1&&f.movement_timing_available&&
        !f.movement_caught&&catalog::find(f.movement_id)&&
        std::isfinite(f.movement_progress)&&f.movement_progress>=0&&f.movement_progress<=1&&
        std::isfinite(f.movement_recovery_progress)&&f.movement_recovery_progress>=0&&f.movement_recovery_progress<=1&&
        !(f.movement_holding&&f.movement_recovering)&&
        (!(f.movement_holding||f.movement_recovering)||f.movement_progress==1)&&
        (f.movement_progress>0||f.movement_holding||f.movement_recovering);
}
// Stateless: accepted producer progress is the only clock. Rendering cannot
// advance it or retain a track across equipment, owner or rig replacement.
inline Targets sample(const visual::Frame& frame,float native_leg_length,bool selected) noexcept {
    Targets out;
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    if(!eligible(frame,selected)||!std::isfinite(native_leg_length)||native_leg_length<.15f||native_leg_length>2)return out;
    const auto id=static_cast<::bmx::TrickId>(static_cast<unsigned>(frame.movement_id));
    const auto* native=catalog::find(frame.movement_id);
    const auto* imported=::bmx::detail::source::catalog::find(id);
    if(!native||!imported||native->name!=imported->name||native->direction!=imported->direction||
       native->release_mask!=imported->release_mask)return out;
    const auto* track=::bmx::find_pose_track(id);if(!track||track->keys.empty()||
        (frame.movement_holding&&!track->hold_available))return out;
    const float weight=std::min(std::clamp(frame.movement_progress/.15f,0.f,1.f),
        frame.movement_recovering?std::clamp((1-frame.movement_recovery_progress)/.15f,0.f,1.f):1.f);
    if(weight==0)return out; // exact endpoint uses the unchanged native candidate
    const auto pose=::bmx::sample_posture_phase(*track,frame.movement_progress,frame.movement_holding,
        frame.movement_recovering,frame.movement_recovery_progress,weight);
    using B=::bmx::PoseBoneId;
    constexpr std::array<B,15> semantic{B::Hips,B::Chest,B::Head,
        B::LeftUpperArm,B::LeftLowerArm,B::LeftHand,B::RightUpperArm,B::RightLowerArm,B::RightHand,
        B::LeftUpperLeg,B::LeftLowerLeg,B::LeftFoot,B::RightUpperLeg,B::RightLowerLeg,B::RightFoot};
    std::array<Vec,15> positions{},neutral{};
    for(unsigned i=0;i<semantic.size();++i){
        const auto index=static_cast<unsigned>(semantic[i]);
        if(!(pose[index].available&::bmx::pose_position)||!(track->neutral[index].available&::bmx::pose_position))return out;
        positions[i]=pose[index].position;neutral[i]=track->neutral[index].position;
        if(!finite(positions[i])||!finite(neutral[i]))return out;
    }
    const float source_leg=.5f*(length(sub(neutral[10],neutral[9]))+length(sub(neutral[11],neutral[10]))+
        length(sub(neutral[13],neutral[12]))+length(sub(neutral[14],neutral[13])));
    if(!std::isfinite(source_leg)||source_leg<.15f||source_leg>2)return out;
    const float scale=native_leg_length/source_leg;
    if(!std::isfinite(scale)||scale<.5f||scale>1.5f)return out;
    const auto delta=[&](unsigned i){return mul(sub(positions[i],neutral[i]),scale);};
    out.hip=delta(0);out.torso=delta(1);
    out.palms={delta(5),delta(8)};out.ankles={delta(11),delta(14)};
    // Pole directions guide native-length IK only. Absolute source joint
    // positions/indices and source rotations never replace native transforms.
    out.arm_poles={sub(positions[4],positions[3]),sub(positions[7],positions[6])};
    out.leg_poles={sub(positions[10],positions[9]),sub(positions[13],positions[12])};
    if(length(out.hip)>.6f||length(out.torso)>.75f)return {};
    for(unsigned side=0;side<2;++side)
        if(length(out.palms[side])>1.5f||length(out.ankles[side])>1.5f||
           length(out.arm_poles[side])<.0001f||length(out.leg_poles[side])<.0001f)return {};
    // Position deltas already include weight in sample_posture_phase. Poles
    // are absolute directions, so their influence needs the same separate
    // envelope to converge to the existing native pole at both endpoints.
    out.available=true;out.retarget_scale=scale;out.pole_weight=weight;
#else
    (void)frame;(void)native_leg_length;(void)selected;
#endif
    return out;
}
} // namespace dingosdk::bmx::rider::observed
