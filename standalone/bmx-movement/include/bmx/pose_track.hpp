#pragma once
#include "movement.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>

namespace bmx {
// Named humanoid channels. Positions/rotations are holder-local, not parent-local.
// Hosts can retarget this neutral-relative posture onto their own skeleton and IK.
enum class PoseBoneId : unsigned {
    Hips,LeftUpperLeg,RightUpperLeg,LeftLowerLeg,RightLowerLeg,LeftFoot,RightFoot,
    Spine,Chest,Neck,Head,LeftShoulder,RightShoulder,LeftUpperArm,RightUpperArm,
    LeftLowerArm,RightLowerArm,LeftHand,RightHand,LeftToes,RightToes,LeftEye,
    RightEye,Jaw,LeftThumbProximal,LeftThumbIntermediate,LeftThumbDistal,
    LeftIndexProximal,LeftIndexIntermediate,LeftIndexDistal,LeftMiddleProximal,
    LeftMiddleIntermediate,LeftMiddleDistal,LeftRingProximal,LeftRingIntermediate,
    LeftRingDistal,LeftLittleProximal,LeftLittleIntermediate,LeftLittleDistal,
    RightThumbProximal,RightThumbIntermediate,RightThumbDistal,RightIndexProximal,
    RightIndexIntermediate,RightIndexDistal,RightMiddleProximal,RightMiddleIntermediate,
    RightMiddleDistal,RightRingProximal,RightRingIntermediate,RightRingDistal,
    RightLittleProximal,RightLittleIntermediate,RightLittleDistal,UpperChest,count
};
inline constexpr unsigned pose_bone_count=static_cast<unsigned>(PoseBoneId::count);
inline constexpr std::array<std::string_view,pose_bone_count> pose_bone_names{
    "Hips","LeftUpperLeg","RightUpperLeg","LeftLowerLeg","RightLowerLeg","LeftFoot","RightFoot",
    "Spine","Chest","Neck","Head","LeftShoulder","RightShoulder","LeftUpperArm","RightUpperArm",
    "LeftLowerArm","RightLowerArm","LeftHand","RightHand","LeftToes","RightToes","LeftEye",
    "RightEye","Jaw","LeftThumbProximal","LeftThumbIntermediate","LeftThumbDistal",
    "LeftIndexProximal","LeftIndexIntermediate","LeftIndexDistal","LeftMiddleProximal",
    "LeftMiddleIntermediate","LeftMiddleDistal","LeftRingProximal","LeftRingIntermediate",
    "LeftRingDistal","LeftLittleProximal","LeftLittleIntermediate","LeftLittleDistal",
    "RightThumbProximal","RightThumbIntermediate","RightThumbDistal","RightIndexProximal",
    "RightIndexIntermediate","RightIndexDistal","RightMiddleProximal","RightMiddleIntermediate",
    "RightMiddleDistal","RightRingProximal","RightRingIntermediate","RightRingDistal",
    "RightLittleProximal","RightLittleIntermediate","RightLittleDistal","UpperChest"};
enum PoseAvailability : unsigned char { pose_position=1,pose_rotation=2 };
struct PoseBone {
    std::array<float,3> position{};
    std::array<float,4> rotation{0,0,0,1};
    unsigned char available{};
};
using PoseSample=std::array<PoseBone,pose_bone_count>;
struct PoseKey {float seconds{};PoseSample delta{};};
struct PoseTrack {
    TrickId id{TrickId::none};
    PoseSample neutral{};
    std::span<const PoseKey> keys{};
    // Real recorded engine seconds relative to the observed fire. No flight or
    // video time warping is used. Keys may include negative pre-fire samples.
    float enter_end_seconds{},exit_end_seconds{};
    bool enter_end_observed{},exit_end_observed{};
    float exit_start_seconds{};
    bool exit_start_observed{},hold_available{};
};
struct PoseFrame {Vec3 position{};Quat orientation{0,0,0,1};};

namespace pose_detail {
inline Quat normalize(Quat q) noexcept {
    double n=0;for(auto x:q){if(!std::isfinite(x))return {0,0,0,1};n+=x*x;}
    if(n<1e-20)return {0,0,0,1};
    for(auto&x:q)x/=std::sqrt(n);
    return q;
}
inline Quat multiply(Quat a,Quat b) noexcept {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline Quat quaternion(const std::array<float,4>&q) noexcept{return normalize({q[0],q[1],q[2],q[3]});}
inline std::array<float,4> narrow(Quat q) noexcept {return {float(q[0]),float(q[1]),float(q[2]),float(q[3])};}
inline Quat slerp(Quat a,Quat b,double t) noexcept {
    a=normalize(a);b=normalize(b);double d=0;for(unsigned i=0;i<4;++i)d+=a[i]*b[i];
    if(d<0){for(auto&x:b)x=-x;d=-d;}
    if(d>.9995){for(unsigned i=0;i<4;++i)a[i]+=(b[i]-a[i])*t;return normalize(a);}
    const auto theta=std::acos(std::clamp(d,0.,1.));const auto den=std::sin(theta);
    for(unsigned i=0;i<4;++i)a[i]=(std::sin((1-t)*theta)*a[i]+std::sin(t*theta)*b[i])/den;
    return normalize(a);
}
}

// Samples only channels available at BOTH neighboring keys. A missing rotation
// is never converted to an observed identity rotation. Outside the key domain,
// the nearest actual endpoint is held; the caller controls recovery blending.
inline PoseSample sample_posture(const PoseTrack&track,double seconds,double weight=1) noexcept {
    PoseSample result{};
    if(track.keys.empty()||!std::isfinite(seconds)||!std::isfinite(weight))return result;
    double previous=-INFINITY;
    for(const auto&key:track.keys){if(!std::isfinite(key.seconds)||key.seconds<=previous)return result;previous=key.seconds;}
    weight=std::clamp(weight,0.,1.);
    const PoseKey* a=&track.keys.front(),*b=a;
    if(seconds>=track.keys.back().seconds)a=b=&track.keys.back();
    else if(seconds>track.keys.front().seconds){
        auto next=std::upper_bound(track.keys.begin(),track.keys.end(),seconds,[](double t,const PoseKey&k){return t<k.seconds;});
        b=&*next;a=&*(next-1);
    }
    const double span=b->seconds-a->seconds,t=span>0?std::clamp((seconds-a->seconds)/span,0.,1.):0;
    for(unsigned i=0;i<pose_bone_count;++i){
        const auto&n=track.neutral[i];const auto&x=a->delta[i];const auto&y=b->delta[i];auto&r=result[i];
        r.available=n.available&x.available&y.available;
        for(const auto*bone:{&n,&x,&y}){
            for(float value:bone->position)if(!std::isfinite(value))r.available&=~pose_position;
            double norm=0;for(float value:bone->rotation){if(!std::isfinite(value))r.available&=~pose_rotation;norm+=double(value)*value;}
            if(norm<1e-20)r.available&=~pose_rotation;
        }
        if(r.available&pose_position)for(unsigned j=0;j<3;++j)r.position[j]=float(n.position[j]+weight*(x.position[j]+t*(y.position[j]-x.position[j])));
        if(r.available&pose_rotation){
            auto delta=pose_detail::slerp(pose_detail::quaternion(x.rotation),pose_detail::quaternion(y.rotation),t);
            delta=pose_detail::slerp({0,0,0,1},delta,weight);
            r.rotation=pose_detail::narrow(pose_detail::normalize(pose_detail::multiply(delta,pose_detail::quaternion(n.rotation))));
        }
    }
    return result;
}
inline PoseSample posture_to_world(PoseSample pose,const PoseFrame&frame) noexcept {
    for(auto x:frame.position)if(!std::isfinite(x))return {};
    double norm=0;for(auto x:frame.orientation){if(!std::isfinite(x))return {};norm+=x*x;}
    if(norm<1e-20)return {};
    const auto q=pose_detail::normalize(frame.orientation);
    for(auto&bone:pose){
        if(bone.available&pose_position){const auto p=rotate(q,{bone.position[0],bone.position[1],bone.position[2]});
            for(unsigned j=0;j<3;++j)bone.position[j]=float(p[j]+frame.position[j]);}
        if(bone.available&pose_rotation)bone.rotation=pose_detail::narrow(pose_detail::normalize(pose_detail::multiply(q,pose_detail::quaternion(bone.rotation))));
    }
    return pose;
}
// Phase-controlled retargeting for arbitrary input holds. Progress values are
// linear elapsed/duration, not an easing weight. Only observed boundaries may
// be used. An observed hold freezes its endpoint; a complete repeating loop is
// not claimed from a short capture of the loop phase.
inline PoseSample sample_posture_phase(const PoseTrack&track,double enter_progress,
    bool holding,bool recovering,double recovery_progress,double weight=1) noexcept {
    if(!track.enter_end_observed||!std::isfinite(enter_progress)||!std::isfinite(recovery_progress))return {};
    double seconds=track.enter_end_seconds*std::clamp(enter_progress,0.,1.);
    if(holding)seconds=track.enter_end_seconds;
    if(recovering){
        if(!track.exit_start_observed||!track.exit_end_observed)return {};
        seconds=track.exit_start_seconds+(track.exit_end_seconds-track.exit_start_seconds)*std::clamp(recovery_progress,0.,1.);
    }
    return sample_posture(track,seconds,weight);
}
// Replace the 15 corresponding procedural joint positions when an observed
// posture is available. Other skeleton channels remain available to the host.
inline RiderPose posture_rider(const PoseSample&world,RiderPose fallback={}) noexcept {
    auto set=[&](PoseBoneId id,Vec3&target){const auto&b=world[static_cast<unsigned>(id)];if(b.available&pose_position)target={b.position[0],b.position[1],b.position[2]};};
    set(PoseBoneId::Hips,fallback.hips);set(PoseBoneId::Chest,fallback.chest);set(PoseBoneId::Head,fallback.head);
    const PoseBoneId arm[2][3]={{PoseBoneId::LeftUpperArm,PoseBoneId::LeftLowerArm,PoseBoneId::LeftHand},{PoseBoneId::RightUpperArm,PoseBoneId::RightLowerArm,PoseBoneId::RightHand}};
    const PoseBoneId leg[2][3]={{PoseBoneId::LeftUpperLeg,PoseBoneId::LeftLowerLeg,PoseBoneId::LeftFoot},{PoseBoneId::RightUpperLeg,PoseBoneId::RightLowerLeg,PoseBoneId::RightFoot}};
    for(unsigned i=0;i<2;++i){set(arm[i][0],fallback.shoulders[i]);set(arm[i][1],fallback.elbows[i]);set(arm[i][2],fallback.hands[i]);set(leg[i][0],fallback.hips_joint[i]);set(leg[i][1],fallback.knees[i]);set(leg[i][2],fallback.feet[i]);}
    return fallback;
}
// Dataset is built from observed controller captures. Empty slots and attempts
// without a verified native fire are deliberately absent.
std::span<const PoseTrack> streets_observed_pose_tracks() noexcept;
inline const PoseTrack* find_pose_track(TrickId id) noexcept {
    for(const auto&track:streets_observed_pose_tracks())if(track.id==id)return &track;
    return nullptr;
}
// A presentation clock for hosts that accept the library's selection event.
// The host owns grounded/busy eligibility; begin() never awards a trick.
// Change owner or disable playback on respawn/focus/rig replacement.
class PosturePlayer {
public:
    bool begin(TrickId id,std::uint64_t owner=1) noexcept {
        reset();if(!owner)return false;track_=find_pose_track(id);owner_=owner;
        return track_&&!track_->keys.empty();
    }
    void reset() noexcept {track_=nullptr;owner_=0;seconds_=0;}
    void advance(double seconds,std::uint64_t owner=1,bool enabled=true) noexcept {
        if(!enabled||owner!=owner_||!owner||!std::isfinite(seconds)||seconds<0||seconds>.1){reset();return;}
        if(!track_)return;
        seconds_+=seconds;if(seconds_>track_->keys.back().seconds+.15)reset();
    }
    bool active() const noexcept{return track_&&!track_->keys.empty();}
    double seconds() const noexcept{return seconds_;}
    TrickId id() const noexcept{return track_?track_->id:TrickId::none;}
    PoseSample local() const noexcept {
        if(!active())return {};
        return sample_posture(*track_,seconds_,std::clamp((track_->keys.back().seconds+.15-seconds_)/.15,0.,1.));
    }
    PoseSample world(const PoseFrame&frame) const noexcept{return posture_to_world(local(),frame);}
private:
    const PoseTrack* track_{};std::uint64_t owner_{};double seconds_{};
};
}
