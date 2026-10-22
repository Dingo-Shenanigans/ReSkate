#pragma once
#include "bmx_visual.h"
#include "bmx_world_transform.h"
#include "bmx_bike_contacts.h"
#include "bmx_rider_motion.h"
#include "bmx_observed_posture.h"
#include "Engine/Game/BMX/bmx_mount_transition.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace dingosdk::bmx::rider::pose {
// Authored offline candidate, not recovered Streets animation values.
namespace small_bike_stance {
inline constexpr float neutral_hip_height=0.9578988338f;
inline constexpr float neutral_hip_z=-0.2877711058f;
inline constexpr float chest_aim_height=1.476639786f;
inline constexpr float chest_aim_z=0.015f;
inline constexpr float elbow_pole_side=0.12f;
inline constexpr float elbow_pole_down=-1.70f;
inline constexpr float elbow_pole_back=-0.05f;
inline constexpr float wrist_forward_slope=0.30f;
inline constexpr float incline_body_response=.25f;
}

using Vec = std::array<float, 3>;
using Quat = std::array<float, 4>;
struct Bone { std::array<float, 4> scale{1,1,1,0}, rotation{0,0,0,1}, position{}; };
static_assert(sizeof(Bone) == 0x30);
struct Finger { int metacarpal{-1}; std::array<int,3> joints{-1,-1,-1}; };
struct Rig {
    std::vector<int> parents;
    std::vector<Quat> bind;
    int hips{}, head{};
    std::array<int,4> spine{};
    std::array<int,2> shoulders{}, neck{};
    std::array<int,3> left_arm{}, right_arm{}, left_leg{}, right_leg{};
    std::array<int,2> toe_base{-1,-1}; // left/right bind-plane calibration; these joints are never written
    std::array<Finger,5> left_fingers{}, right_fingers{}; // index, middle, ring, pinky, thumb
};
struct Transform { Vec position{}; Quat rotation{0,0,0,1}; float scale{1}; };
// Errors measure the current intended palm/ankle targets. A released limb must
// not be rejected merely because its target is deliberately away from the bike.
struct Result {
    bool ready{}; float hand_error{}, foot_error{}, hand_contact{1}, foot_contact{1};
    // Completed candidate diagnostics, not visual acceptance or physics state.
    unsigned posture_attempts{};float posture_scale{},posture_fit_adjustment{},posture_gaze{};
    bool posture_requested{},posture_fallback{};
    bool observed_requested{},observed_applied{};float observed_strength{};
    bool spring_requested{},spring_calibrated{},spring_applied{};float spring_fit_adjustment{};
};
struct EntryContacts {
    std::uintptr_t entity{};std::uint64_t token{};
    std::array<Vec,2> palms{},ankles{}; // world anchors from the first fresh native pose
    std::array<Vec,2> arm_poles{},leg_poles{};
    std::array<Quat,2> hands{},feet{};
};
struct SpringBinding {
    avatar_binding::Calibration root;
    avatar_binding::Epoch pose_epoch;
    Vec local_hip{},local_chest{};
    Quat local_hip_basis{0,0,0,1};
    bool proximal_ready{},completed_pose_write{},last_pose_applied{};
    float fit_adjustment{},hand_error{},foot_error{};
};
struct SpringTargets {Vec hip{},chest{};Quat hip_rotation{0,0,0,1},model_rotation{0,0,0,1};};
inline Vec add(Vec a,Vec b) { for(unsigned i=0;i<3;++i)a[i]+=b[i];return a; }
inline Vec sub(Vec a,Vec b) { for(unsigned i=0;i<3;++i)a[i]-=b[i];return a; }
inline Vec mul(Vec a,float b) { for(auto& v:a)v*=b;return a; }
inline float dot(Vec a,Vec b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vec cross(Vec a,Vec b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline float length(Vec a) { return std::sqrt(dot(a,a)); }
inline Vec unit(Vec a) { return mul(a,1/std::max(length(a),.000001f)); }
inline Quat multiply(Quat a,Quat b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline Quat inverse(Quat q) { return {-q[0],-q[1],-q[2],q[3]}; }
inline Quat normal(Quat q) { float n=0;for(float v:q)n+=v*v;if(n<.000001f)return {0,0,0,1};for(auto& v:q)v/=std::sqrt(n);return q; }
inline Vec blend(Vec a,Vec b,float amount) { if(amount<=0)return a;if(amount>=1)return b;return add(mul(a,1-amount),mul(b,amount)); }
inline Quat blend(Quat a,Quat b,float amount) {
    if(amount<=0)return a;
    if(amount>=1)return b;
    float d=0;for(unsigned i=0;i<4;++i)d+=a[i]*b[i];
    if(d<0)for(auto& v:b)v=-v;
    for(unsigned i=0;i<4;++i)a[i]=a[i]*(1-amount)+b[i]*amount;
    return normal(a);
}
inline Quat axis(unsigned i,float a) { Quat q{0,0,0,std::cos(a*.5f)};q[i]=std::sin(a*.5f);return q; }
inline Quat steerer(float angle) {
    const auto n=unit(geometry::steering_axis);const float sine=std::sin(angle*.5f);
    return {n[0]*sine,n[1]*sine,n[2]*sine,std::cos(angle*.5f)};
}
inline Vec turn(Quat q,Vec p) { const auto t=mul(cross({q[0],q[1],q[2]},p),2);return add(p,add(mul(t,q[3]),cross({q[0],q[1],q[2]},t))); }
// Matrix rows in Frostbite's LinearTransform are local +X/+Y/+Z world axes.
inline Quat orientation(Vec x,Vec y,Vec z) {
    const float t=x[0]+y[1]+z[2]; Quat q{};
    if(t>0) {const float s=std::sqrt(t+1)*2;q={(y[2]-z[1])/s,(z[0]-x[2])/s,(x[1]-y[0])/s,.25f*s};}
    else if(x[0]>y[1]&&x[0]>z[2]) {const float s=std::sqrt(1+x[0]-y[1]-z[2])*2;q={.25f*s,(x[1]+y[0])/s,(x[2]+z[0])/s,(y[2]-z[1])/s};}
    else if(y[1]>z[2]) {const float s=std::sqrt(1+y[1]-x[0]-z[2])*2;q={(x[1]+y[0])/s,.25f*s,(y[2]+z[1])/s,(z[0]-x[2])/s};}
    else {const float s=std::sqrt(1+z[2]-x[0]-y[1])*2;q={(x[2]+z[0])/s,(y[2]+z[1])/s,.25f*s,(x[1]-y[0])/s};}
    return normal(q);
}
inline Quat from_to(Vec a,Vec b) {
    a=unit(a);b=unit(b);auto v=cross(a,b);const float d=dot(a,b);
    if(d<-.9999f){v=unit(cross(a,std::abs(a[1])<.9f?Vec{0,1,0}:Vec{1,0,0}));return {v[0],v[1],v[2],0};}
    return normal({v[0],v[1],v[2],1+d});
}
inline std::vector<Transform> compose(std::span<const Bone> bones,const Rig& rig) {
    std::vector<Transform> out(rig.parents.size());
    for(std::size_t i=0;i<out.size();++i) {
        const auto& b=bones[i];Transform t{{b.position[0],b.position[1],b.position[2]},b.rotation,(b.scale[0]+b.scale[1]+b.scale[2])/3};
        if(rig.parents[i]>=0) {const auto& p=out[static_cast<std::size_t>(rig.parents[i])];t.position=add(p.position,turn(p.rotation,mul(t.position,p.scale)));t.rotation=normal(multiply(p.rotation,t.rotation));t.scale*=p.scale;}
        out[i]=t;
    }
    return out;
}
inline Quat bike_rotation(const visual::Frame& f) {
    if(f.chassis_quaternion_available) {
        Quat rotation{}; // solve() rejects invalid availability before any pose mutation.
        chassis::unit_quaternion(f.chassis_quaternion,rotation);return rotation;
    }
    return multiply(axis(1,f.yaw),multiply(axis(0,f.pitch),axis(2,f.roll)));
}
inline Vec world(const visual::Frame& f,Vec p) { return add(f.position,turn(bike_rotation(f),p)); }
inline Quat part_rotation(const visual::Frame& f) {
    if(f.movement_bike_pitch==0&&f.movement_bike_roll==0)return bike_rotation(f);
    return multiply(bike_rotation(f),multiply(axis(2,f.movement_bike_roll),axis(0,f.movement_bike_pitch)));
}
inline Vec part_world(const visual::Frame& f,Vec point) {
    if(f.movement_bike_pitch==0&&f.movement_bike_roll==0)return world(f,point);
    // Match world::detail::local_point: whole-bike visual articulation pivots
    // about the head tube after bars/cranks/whip. Rider torso remains measured.
    point=world::detail::rotate(point,0,f.movement_bike_pitch,contacts::steering_pivot);
    point=world::detail::rotate(point,2,f.movement_bike_roll,contacts::steering_pivot);
    return world(f,point);
}
inline Vec grip(const visual::Frame& f,bool left) {
    // Native skeleton +X is the rider's LEFT (the bind LeftHand is +0.476m).
    // Match the visual's exact steering pivot, including the grip's fore/aft offset.
    const auto p=geometry::rotate_steering(contacts::grip_center(left),f.steer+f.barspin);
    return part_world(f,p);
}
// The native hand +X points from wrist to knuckles; mirrored +/-Z is the
// palmar side. A wrist joint is not the point where a palm holds the tube.
inline Vec palm_bar_offset(bool left,float radius=contacts::grip_radius) { return {.055f,0,(left?1.f:-1.f)*(radius+.004f)}; }
inline Quat grip_hand_rotation(const visual::Frame& f,bool left) {
    const float slope=small_bike_stance::wrist_forward_slope;
    const float side=left?1.f:-1.f,c=std::cos(slope),s=std::sin(slope);
    // The recovered bar ends have backsweep and slight asymmetric upsweep.
    // Native hand +Y points inward across the palm, exactly along the tube.
    // Build the other axes perpendicular to that authored direction; fixing
    // them to bike +/-X leaves the fingers skewed across the real grip.
    const auto outward=unit(contacts::grip_axis(left));
    const auto forward=unit(sub(Vec{0,0,1},mul(outward,dot(outward,{0,0,1}))));
    const auto up=mul(unit(cross(forward,outward)),side);
    const auto distal=sub(mul(forward,c),mul(up,s));
    const auto across=mul(outward,-1);
    const auto local=orientation(distal,across,cross(distal,across));
    return multiply(multiply(part_rotation(f),steerer(f.steer+f.barspin)),local);
}
inline float hand_release(const visual::Frame& f) { return std::clamp(f.barspin_release,0.f,1.f); }
inline float foot_release(const visual::Frame& f) { return std::clamp(f.tailwhip_release,0.f,1.f); }
inline float hand_release(const visual::Frame& f,bool left) {
    return std::max(hand_release(f),f.movement_id==catalog::TrickId::none?0.f:
        std::clamp(left?f.release_left_hand:f.release_right_hand,0.f,1.f));
}
inline float foot_release(const visual::Frame& f,bool left) {
    return std::max(foot_release(f),f.movement_id==catalog::TrickId::none?0.f:
        std::clamp(left?f.release_left_foot:f.release_right_foot,0.f,1.f));
}
inline bool spring_capture_neutral(const visual::Frame& f) noexcept {
    return !f.airborne&&f.movement_id==catalog::TrickId::none&&f.movement_phase==0&&f.movement_progress==0&&
        !f.movement_holding&&!f.movement_recovering&&f.barspin==0&&f.tailwhip==0&&f.movement_bike_pitch==0&&f.movement_bike_roll==0&&f.preload==0&&
        f.front_weight==0&&f.rear_weight==0&&f.landing_compression==0&&f.pedal_effort==0&&f.steer==0&&f.air_extension_blend==0&&
        hand_release(f,true)==0&&hand_release(f,false)==0&&foot_release(f,true)==0&&foot_release(f,false)==0;
}
inline Quat hand_rotation(const visual::Frame& f,bool left) {
    auto unspun=f;unspun.barspin=0;unspun.movement_bike_pitch=unspun.movement_bike_roll=0;
    return blend(grip_hand_rotation(f,left),grip_hand_rotation(unspun,left),hand_release(f,left));
}
inline Vec palm_target(const visual::Frame& f,bool left,const observed::Targets* measured=nullptr) {
    const auto closed=grip(f,left);const float release=hand_release(f,left);if(release==0)return closed;
    auto unspun=f;unspun.barspin=0;
    auto free=add(grip(unspun,left),turn(bike_rotation(f),{left?.08f:-.08f,.12f,-.08f}));
    if(f.movement_id!=catalog::TrickId::none)free=blend(free,
        world(f,motion::sample(f).palms[left?0:1]),f.movement_phase);
    if(measured&&measured->available){auto anchored=unspun;anchored.movement_bike_pitch=anchored.movement_bike_roll=0;
        free=blend(free,add(grip(anchored,left),turn(bike_rotation(f),measured->palms[left?0:1])),measured->strength);}
    return blend(closed,free,release);
}
inline Vec wrist_target(const visual::Frame& f,bool left,const observed::Targets* measured=nullptr) {
    return sub(palm_target(f,left,measured),turn(hand_rotation(f,left),palm_bar_offset(left)));
}
inline Vec pedal(const visual::Frame& f,bool left) {
    // The supplied source's left platform starts low/forward. Separate native
    // platform parts translate with their spindle and keep a level sole.
    Vec p=add(geometry::pedal_point(left,f.crank_phase),
        mul(geometry::pedal(left).normal,geometry::ankle_from_sole));
    p=geometry::rotate_steering(p,f.tailwhip);
    return part_world(f,p);
}
inline Vec foot_target(const visual::Frame& f,bool left,const observed::Targets* measured=nullptr) {
    const auto closed=pedal(f,left);const float release=foot_release(f,left);if(release==0)return closed;
    // A released foot must not keep cycling or chase the frame through a whip.
    // The parked sole clears the upper frame; both knees fold toward the rider.
    // Symmetric free-leg targets do not inherit the source's asymmetric crank
    // phase or chase either platform while both feet are released.
    auto free=world(f,{left?.2792422f:-.2792422f,
        geometry::bottom_bracket[1]+geometry::ankle_from_sole+.43f,
        geometry::bottom_bracket[2]-.08f});
    if(f.movement_id!=catalog::TrickId::none)free=blend(free,
        world(f,motion::sample(f).ankles[left?0:1]),f.movement_phase);
    if(measured&&measured->available){auto unwhipped=f;unwhipped.tailwhip=0;unwhipped.movement_bike_pitch=unwhipped.movement_bike_roll=0;
        free=blend(free,add(pedal(unwhipped,left),turn(bike_rotation(f),measured->ankles[left?0:1])),measured->strength);}
    return blend(closed,free,release);
}
// Foot+X follows ankle-to-ball, which slopes down in the shipped bind pose.
// ToeBase is the level forefoot frame there. Applying the old level basis to
// Foot raised the bind ToeBase axis by28.1degrees. Convert the requested plane
// through the actual ToeBase bind rotation; leave native toe flex untouched.
// This is a bind-frame calibration, not a skinned shoe-sole measurement.
inline Quat foot_rotation(const Rig& rig,const visual::Frame& f,bool left) {
    const auto platform=multiply(part_rotation(f),steerer(f.tailwhip));
    const auto plane=multiply(blend(platform,bike_rotation(f),foot_release(f,left)),
        orientation({0,0,1},{0,-1,0},{1,0,0}));
    const int toe=rig.toe_base[left?0:1];
    return toe<0?plane:multiply(plane,inverse(rig.bind[static_cast<std::size_t>(toe)]));
}
inline void set_rotation(std::span<Bone> bones,const Rig& rig,int index,Quat world_rotation) {
    const auto g=compose(bones,rig);const int parent=rig.parents[static_cast<std::size_t>(index)];
    bones[static_cast<std::size_t>(index)].rotation=normal(parent<0?world_rotation:multiply(inverse(g[static_cast<std::size_t>(parent)].rotation),world_rotation));
}
inline void aim(std::span<Bone> bones,const Rig& rig,int index,int child,Vec target) {
    const auto g=compose(bones,rig);const auto& joint=g[static_cast<std::size_t>(index)];
    set_rotation(bones,rig,index,multiply(from_to(sub(g[static_cast<std::size_t>(child)].position,joint.position),sub(target,joint.position)),joint.rotation));
}
inline void limb(std::span<Bone> bones,const Rig& rig,const std::array<int,3>& chain,Vec target,Vec pole) {
    const auto g=compose(bones,rig);const Vec root=g[static_cast<std::size_t>(chain[0])].position;
    const float a=length(sub(g[static_cast<std::size_t>(chain[1])].position,root)),b=length(sub(g[static_cast<std::size_t>(chain[2])].position,g[static_cast<std::size_t>(chain[1])].position));
    if(a<.04f||b<.04f||a>1||b>1)return;
    const auto direction=unit(sub(target,root));const float reach=std::clamp(length(sub(target,root)),std::abs(a-b)+.0001f,a+b-.0001f);
    const float along=(a*a-b*b+reach*reach)/(2*reach),height=std::sqrt(std::max(0.f,a*a-along*along));
    auto side=sub(pole,mul(direction,dot(pole,direction)));
    if(length(side)<.0001f)side=cross(direction,std::abs(direction[1])<.9f?Vec{0,1,0}:Vec{1,0,0});
    const auto elbow=add(root,add(mul(direction,along),mul(unit(side),height)));
    aim(bones,rig,chain[0],chain[1],elbow);aim(bones,rig,chain[1],chain[2],add(root,mul(direction,reach)));
}
inline bool fit_pelvis_to_limbs(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,
    const std::array<Vec,4>* entry_targets=nullptr,float maximum_adjustment=.18f,const observed::Targets* measured=nullptr) {
    // Source geometry has a longer crank than the former prototype. Preserve
    // the requested posture whenever reachable; otherwise move the pelvis
    // into the four native-length limb reach balls. Never stretch a segment or
    // move the bike. This is a bounded presentation adjustment, not physics.
    const auto g=compose(bones,rig);const auto desired=g[static_cast<std::size_t>(rig.hips)].position;
    std::array<Vec,4> centers{};std::array<float,4> radii{};
    for(unsigned side=0;side<4;++side) {
        const auto& chain=side==0?rig.left_leg:side==1?rig.right_leg:side==2?rig.left_arm:rig.right_arm;
        const auto root=g[static_cast<std::size_t>(chain[0])].position;
        radii[side]=length(sub(g[static_cast<std::size_t>(chain[1])].position,root))+
            length(sub(g[static_cast<std::size_t>(chain[2])].position,g[static_cast<std::size_t>(chain[1])].position))-.001f;
        if(!std::isfinite(radii[side])||radii[side]<.08f||radii[side]>2)return false;
        const auto target=entry_targets?(*entry_targets)[side]:side<2?foot_target(frame,side==0,measured):wrist_target(frame,side==2,measured);
        centers[side]=sub(target,sub(root,desired));
    }
    auto fitted=desired;
    for(unsigned pass=0;pass<24;++pass)for(unsigned side=0;side<4;++side) {
        const auto delta=sub(fitted,centers[side]);const auto distance=length(delta);
        if(distance>radii[side])fitted=add(centers[side],mul(delta,radii[side]/distance));
    }
    if(length(sub(fitted,desired))>maximum_adjustment)return false;
    for(unsigned side=0;side<4;++side)
        if(length(sub(fitted,centers[side]))>radii[side]+.0005f)return false;
    if(fitted==desired)return true;
    const int parent=rig.parents[static_cast<std::size_t>(rig.hips)];if(parent<0)return false;
    const auto& p=g[static_cast<std::size_t>(parent)];
    const auto local=mul(turn(inverse(p.rotation),sub(fitted,p.position)),1/p.scale);
    for(unsigned i=0;i<3;++i)bones[static_cast<std::size_t>(rig.hips)].position[i]=local[i];
    return true;
}
inline bool close_hand(std::span<Bone> bones,const Rig& rig,const std::array<Finger,5>& fingers,
                       int hand,const visual::Frame& frame,bool left,const Quat* override_rotation=nullptr,
                       float tube_radius=contacts::grip_radius) {
    const float side=left?1.f:-1.f;
    const auto hand_q=override_rotation?*override_rotation:hand_rotation(frame,left);
    set_rotation(bones,rig,hand,hand_q);
    const float release=override_rotation?0.f:hand_release(frame,left);
    if(release==1)return true; // solve() already restored relaxed native finger bind rotations.
    const auto hand_transform=compose(bones,rig)[static_cast<std::size_t>(hand)];
    const auto append=[](const Transform& parent,const Bone& bone) {
        return Transform{add(parent.position,turn(parent.rotation,mul({bone.position[0],bone.position[1],bone.position[2]},parent.scale))),
            normal(multiply(parent.rotation,bone.rotation)),parent.scale*(bone.scale[0]+bone.scale[1]+bone.scale[2])/3};
    };
    for(std::size_t i=0;i<fingers.size();++i) {
        const auto& finger=fingers[i];if(finger.joints[0]<0)continue;
        const auto hand_position=hand_transform.position;
        auto parent=hand_transform;
        if(finger.metacarpal>=0)parent=append(parent,bones[static_cast<std::size_t>(finger.metacarpal)]);
        for(unsigned link=0;link<2;++link) {
            const auto index=static_cast<std::size_t>(finger.joints[link]);
            const auto child=static_cast<std::size_t>(finger.joints[link+1]);
            auto joint=append(parent,bones[index]);
            const auto next=append(joint,bones[child]);
            const auto local=turn(inverse(hand_q),sub(joint.position,hand_position));
            const auto radial=sub(local,palm_bar_offset(left,tube_radius));
            const float x=radial[0],z=side*radial[2],distance=std::hypot(x,z);
            const float radius=tube_radius+.002f;
            if(distance<=radius||!std::isfinite(distance))return false;
            // Follow successive tangents around the actual cylindrical grip.
            // Each native-length phalanx stays outside its surface. Direct tip
            // IK can meet a target while cutting through the handlebar.
            const float sine=radius/distance,cosine=std::sqrt(std::max(0.f,1-sine*sine));
            const Vec direction{(-x*cosine-z*sine)/distance,0,
                side*(-z*cosine+x*sine)/distance};
            joint.rotation=normal(multiply(from_to(sub(next.position,joint.position),turn(hand_q,direction)),joint.rotation));
            bones[index].rotation=normal(multiply(inverse(parent.rotation),joint.rotation));
            parent=joint;
        }
        const auto end=static_cast<std::size_t>(finger.joints[2]);
        const auto distal=append(parent,bones[end]);
        const auto radial=sub(turn(inverse(hand_q),sub(distal.position,hand_position)),palm_bar_offset(left,tube_radius));
        const auto tangent=unit(Vec{-side*radial[2],0,side*radial[0]});
        bones[end].rotation=normal(multiply(inverse(parent.rotation),multiply(hand_q,
            orientation(tangent,{0,1,0},cross(tangent,{0,1,0})))));
    }
    if(release>0)for(const auto& finger:fingers)for(int index:finger.joints)
        if(index>=0)bones[static_cast<std::size_t>(index)].rotation=blend(bones[static_cast<std::size_t>(index)].rotation,rig.bind[static_cast<std::size_t>(index)],release);
    return true;
}
inline std::vector<int> changed(const Rig& rig) {
    std::vector<int> result{rig.hips,rig.head};
    for(auto i:rig.spine)result.push_back(i);
    for(auto i:rig.shoulders)result.push_back(i);
    for(auto i:rig.neck)result.push_back(i);
    for(const auto chain:{rig.left_arm,rig.right_arm,rig.left_leg,rig.right_leg})for(auto i:chain)result.push_back(i);
    for(bool left:{false,true})for(const auto& finger:(left?rig.left_fingers:rig.right_fingers)) {
        if(finger.metacarpal>=0)result.push_back(finger.metacarpal);
        for(int i:finger.joints)if(i>=0)result.push_back(i);
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
struct PostureTargets {Vec hip{},torso{};float gaze{};};
inline PostureTargets posture_targets(const visual::Frame& frame,float scale) {
    PostureTargets out;if(!frame.rider_posture_available||scale==0)return out;
    const auto q=bike_rotation(frame);const auto up=turn(q,{0,1,0}),forward=turn(q,{0,0,1});
    const float v=rider_posture::smooth(.5f,.8191520443f,up[1])*
        rider_posture::smooth(.5f,.8191520443f,std::hypot(forward[0],forward[2]));
    const float released=std::max({frame.movement_phase,hand_release(frame,true),hand_release(frame,false),
        foot_release(frame,true),foot_release(frame,false)});
    const float neutral=1-released,air=frame.air_extension_available?frame.air_extension_blend:(frame.airborne?1.f:0.f);
    const float preload=frame.airborne?0.f:std::clamp(frame.preload,0.f,1.f);
    // Smaller-bike authored stance already supplies the neutral hip hinge.
    // Bound its additional incline body response to4.5mm hip/18.75mm chest;
    // preserve the horizon-gaze cue and existing compression contribution.
    // Retain the existing vertical impact recovery through support toggles.
    const float a=small_bike_stance::incline_body_response*scale*frame.rider_uphill_blend*(1-.5f*air)*neutral*v*(1-.6f*preload);
    const float compression=scale*frame.landing_compression*neutral*v;
    out.hip={0,-.018f*a,.010f*a-.004f*compression};
    out.torso={0,-.012f*a,std::min(.075f,.075f*a+.015f*compression)};
    out.gaze=scale*frame.rider_gaze_counterpitch*(1-.5f*air)*neutral*v;return out;
}
inline Quat fraction(Quat q,float share) {
    if(q[3]<0)for(auto& x:q)x=-x;
    const float half=std::acos(std::clamp(q[3],-1.f,1.f)),sine=std::sin(half);
    if(std::abs(sine)<.000001f)return {0,0,0,1};
    const float factor=std::sin(half*share)/sine;
    return normal({q[0]*factor,q[1]*factor,q[2]*factor,std::cos(half*share)});
}
inline float stabilize_gaze(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,float yaw,float cue) {
    const auto legacy=multiply(multiply(bike_rotation(frame),axis(1,yaw)),Quat{.5f,.5f,.5f,.5f});
    const auto forward=turn(legacy,{0,1,0});const float horizontal=std::hypot(forward[0],forward[2]);
    const float phi=horizontal>.0001f?std::atan2(forward[1],horizontal):0.f;
    // Correct only positive world elevation, stopping at the horizon.
    const float delta=std::min(cue,std::max(phi,0.f));
    if(delta<=.000001f||horizontal<=.0001f||
       rig.parents[static_cast<std::size_t>(rig.neck[0])]!=rig.spine[3]||
       rig.parents[static_cast<std::size_t>(rig.neck[1])]!=rig.neck[0]||
       rig.parents[static_cast<std::size_t>(rig.head)]!=rig.neck[1]) {
        set_rotation(bones,rig,rig.head,legacy);return 0;
    }
    const Vec target{forward[0]/horizontal*std::cos(phi-delta),std::sin(phi-delta),
        forward[2]/horizontal*std::cos(phi-delta)};
    const auto correction=from_to(forward,target);const auto original=compose(bones,rig);
    // Cervical joints branch above the arms. Preserve all local translations,
    // distribute 35/35/30 percent, and retain the legacy yaw/roll with a minimal
    // world-space correction. These are proposed weights, not decoded channels.
    set_rotation(bones,rig,rig.neck[0],multiply(fraction(correction,.35f),original[static_cast<std::size_t>(rig.neck[0])].rotation));
    set_rotation(bones,rig,rig.neck[1],multiply(fraction(correction,.70f),original[static_cast<std::size_t>(rig.neck[1])].rotation));
    set_rotation(bones,rig,rig.head,multiply(correction,legacy));return delta;
}
inline bool retained_neutral_anatomy(std::span<const Bone> bones,const Rig& rig,const visual::Frame& frame) {
    // Preserve the existing neutral-fixture anatomy bounds. Loaded/preloaded,
    // airborne and authored released poses have separate existing contracts.
    if(frame.preload!=0||frame.front_weight!=0||frame.rear_weight!=0||
       frame.barspin!=0||frame.tailwhip!=0||frame.movement_id!=catalog::TrickId::none||
       frame.movement_bike_pitch!=0||frame.movement_bike_roll!=0||frame.landing_compression!=0||frame.airborne||
       (frame.air_extension_available&&frame.air_extension_blend!=0)||frame.movement_phase!=0||
       hand_release(frame,true)!=0||hand_release(frame,false)!=0||
       foot_release(frame,true)!=0||foot_release(frame,false)!=0)return true;
    const auto g=compose(bones,rig);const auto inverse_q=inverse(bike_rotation(frame));
    const auto point=[&](int joint){return turn(inverse_q,sub(g[static_cast<std::size_t>(joint)].position,frame.position));};
    const auto angle=[](Vec a,Vec b){return std::acos(std::clamp(dot(unit(a),unit(b)),-1.f,1.f))*57.2957795f;};
    const float torso=angle(sub(point(rig.spine[3]),point(rig.spine[0])),{0,1,0});
    if(torso<=25||torso>=40||point(rig.hips)[1]<=1.09f)return false;
    for(const auto chain:{rig.left_arm,rig.right_arm}) {
        const auto shoulder=point(chain[0]),elbow=point(chain[1]),wrist=point(chain[2]);
        const auto forearm=sub(wrist,elbow);const float bend=angle(sub(shoulder,elbow),forearm);
        if(bend<=65||bend>=120||angle(forearm,{0,0,1})>=35||
           angle(forearm,turn(inverse_q,turn(g[static_cast<std::size_t>(chain[2])].rotation,{1,0,0})))>=35||
           elbow[1]>=shoulder[1]-.15f||std::abs(elbow[1]-wrist[1])>=.10f||std::abs(elbow[0])>=.35f)return false;
    }
    for(const auto chain:{rig.left_leg,rig.right_leg}) {
        const float knee=angle(sub(point(chain[0]),point(chain[1])),sub(point(chain[2]),point(chain[1])));
        if(knee<=50||knee>=176)return false;
    }
    return true;
}

// NEW AUTHORED SMALL-BIKE DESIGN. The former1.09m golden hip floor remains
// unchanged in retained_neutral_anatomy() for explicit comparison only.
// Bounds derive from native segment lengths and original50..176deg knees.
struct ProposedHipBand {bool available{};float low{},high{},seat_floor{};};
inline ProposedHipBand proposed_hip_band(std::span<const Bone> bones,const Rig& rig,const visual::Frame& frame,float hip_z,const observed::Targets* measured=nullptr) {
    const auto g=compose(bones,rig);const auto inverse_q=inverse(bike_rotation(frame));
    const auto hip=g[static_cast<std::size_t>(rig.hips)].position;
    const float half_width=.5f*length(sub(g[static_cast<std::size_t>(rig.left_leg[0])].position,g[static_cast<std::size_t>(rig.right_leg[0])].position));
    // Seat mesh top0.702625573m is measured from the supplied normalized mesh.
    // Native half pelvis width plus30mm is an explicit clearance proxy; it is
    // not a recovered Streets slider or proof of skinned human mesh clearance.
    const float seat_floor=.702625573f+half_width+.03f;float low=seat_floor,high=10;
    for(bool left:{true,false}) {
        const auto chain=left?rig.left_leg:rig.right_leg;
        const auto a=g[static_cast<std::size_t>(chain[0])].position,b=g[static_cast<std::size_t>(chain[1])].position,c=g[static_cast<std::size_t>(chain[2])].position;
        const float upper=length(sub(b,a)),lower=length(sub(c,b));
        const auto offset=turn(inverse_q,sub(a,hip)),target=turn(inverse_q,sub(foot_target(frame,left,measured),frame.position));
        const float horizontal=(target[0]-offset[0])*(target[0]-offset[0])+(target[2]-hip_z-offset[2])*(target[2]-hip_z-offset[2]);
        const float near2=upper*upper+lower*lower-2*upper*lower*std::cos(50.001f/57.2957795f);
        const float far2=upper*upper+lower*lower-2*upper*lower*std::cos(175.999f/57.2957795f);
        if(horizontal>=far2)return {false,low,high,seat_floor};
        low=std::max(low,target[1]+std::sqrt(std::max(0.f,near2-horizontal))-offset[1]);
        high=std::min(high,target[1]+std::sqrt(far2-horizontal)-offset[1]);
    }
    return {std::isfinite(low)&&std::isfinite(high)&&high-low>.006f,low,high,seat_floor};
}
inline bool proposed_small_bike_anatomy(std::span<const Bone> bones,const Rig& rig,const visual::Frame& frame) {
    // Preserve the existing neutral-fixture anatomy bounds. Loaded/preloaded,
    // airborne and authored released poses have separate existing contracts.
    if(frame.preload!=0||frame.front_weight!=0||frame.rear_weight!=0||
       frame.barspin!=0||frame.tailwhip!=0||frame.movement_id!=catalog::TrickId::none||
       frame.movement_bike_pitch!=0||frame.movement_bike_roll!=0||frame.landing_compression!=0||frame.airborne||
       (frame.air_extension_available&&frame.air_extension_blend!=0)||frame.movement_phase!=0||
       hand_release(frame,true)!=0||hand_release(frame,false)!=0||
       foot_release(frame,true)!=0||foot_release(frame,false)!=0)return true;
    const auto g=compose(bones,rig);const auto inverse_q=inverse(bike_rotation(frame));
    const auto point=[&](int joint){return turn(inverse_q,sub(g[static_cast<std::size_t>(joint)].position,frame.position));};
    const auto angle=[](Vec a,Vec b){return std::acos(std::clamp(dot(unit(a),unit(b)),-1.f,1.f))*57.2957795f;};
    const float torso=angle(sub(point(rig.spine[3]),point(rig.spine[0])),{0,1,0});
    if(torso<=25||torso>=40)return false;
    const auto band=proposed_hip_band(bones,rig,frame,point(rig.hips)[2]);
    if(!band.available||point(rig.hips)[1]<=band.low||point(rig.hips)[1]>=band.high)return false;
    for(const auto chain:{rig.left_arm,rig.right_arm}) {
        const auto shoulder=point(chain[0]),elbow=point(chain[1]),wrist=point(chain[2]);
        const auto forearm=sub(wrist,elbow);const float bend=angle(sub(shoulder,elbow),forearm);
        if(bend<=65||bend>=120||angle(forearm,{0,0,1})>=35||
           angle(forearm,turn(inverse_q,turn(g[static_cast<std::size_t>(chain[2])].rotation,{1,0,0})))>=35||
           elbow[1]>=shoulder[1]-.15f||std::abs(elbow[1]-wrist[1])>=.10f||std::abs(elbow[0])>=.35f)return false;
    }
    for(const auto chain:{rig.left_leg,rig.right_leg}) {
        const float knee=angle(sub(point(chain[0]),point(chain[1])),sub(point(chain[2]),point(chain[1])));
        if(knee<=50||knee>=176)return false;
    }
    return true;
}
// Authored articulated stance candidate. Rotate the native clavicles to
// keep the arm-chain origins lower while the hips extend. Aiming changes only
// joint rotations; each native shoulder-to-arm segment keeps its exact length.
// This is independent of gaze and never moves a grip, pedal or native root.
inline bool articulate_clavicles(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame) {
    for(unsigned side=0;side<2;++side) {
        const int arm=side==0?rig.left_arm[0]:rig.right_arm[0];
        if(rig.parents[static_cast<std::size_t>(arm)]!=rig.shoulders[side])return false;
    }
    const auto q=bike_rotation(frame);const auto down=turn(q,{0,-.04f,0});
    for(unsigned side=0;side<2;++side) {
        const int clavicle=rig.shoulders[side];
        const int arm=side==0?rig.left_arm[0]:rig.right_arm[0];
        const auto g=compose(bones,rig);
        aim(bones,rig,clavicle,arm,add(g[static_cast<std::size_t>(arm)].position,down));
    }
    return true;
}
inline Result solve_riding_candidate(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,float posture_scale,const observed::Targets* measured=nullptr,const SpringTargets* spring=nullptr) {
    Result result;
    // These are bounded posture cues, not copied Unity humanoid channels or
    // physics forces. Pedals stay driven by the actual bike crank phase. The
    // reference controller uses Preload/Grounded and an EngineRPS-scaled pedal
    // animation, so an independent wall-clock clip would drift from the bike.
    const float preload=frame.airborne?0.f:std::clamp(frame.preload,0.f,1.f);
    const float front=std::clamp(frame.front_weight,-1.f,1.f),rear=std::clamp(frame.rear_weight,-1.f,1.f);
    const float extension=.04f*(frame.air_extension_available?frame.air_extension_blend:(frame.airborne?1.f:0.f));
    auto movement=motion::sample(frame);
    if(measured&&measured->available){
        movement.hip_offset=blend(movement.hip_offset,measured->hip,measured->strength);
        movement.torso_offset=blend(movement.torso_offset,measured->torso,measured->strength);
    }
    auto posture=posture_targets(frame,posture_scale);
    Vec desired_hip{};
    const auto q=spring?spring->model_rotation:bike_rotation(frame);
    if(spring) {
        // Only the proximal presentation follows Spring. Contact targets below
        // continue using frame.position/chassis_quaternion from the actual bike.
        set_rotation(bones,rig,rig.hips,spring->hip_rotation);
        const auto current=compose(bones,rig);const int parent=rig.parents[static_cast<std::size_t>(rig.hips)];
        if(parent<0)return result;const auto& ancestor=current[static_cast<std::size_t>(parent)];
        const auto local=mul(turn(inverse(ancestor.rotation),sub(spring->hip,ancestor.position)),1/ancestor.scale);
        for(unsigned a=0;a<3;++a)bones[static_cast<std::size_t>(rig.hips)].position[a]=local[a];
        desired_hip=spring->hip;
        aim(bones,rig,rig.spine[0],rig.spine[3],spring->chest);
        if(!articulate_clavicles(bones,rig,frame))return result;
    } else {
    if((frame.movement_bike_pitch!=0||frame.movement_bike_roll!=0)&&
        hand_release(frame,true)==0&&hand_release(frame,false)==0) {
        // When authored hands retain the bike through an under-body flip, the
        // pelvis follows the bars' displacement instead of stretching the arms.
        // Orientation remains measured; released limbs stay independent.
        auto neutral=frame;neutral.movement_bike_pitch=neutral.movement_bike_roll=0;
        const auto displacement=mul(sub(add(grip(frame,true),grip(frame,false)),
            add(grip(neutral,true),grip(neutral,false))),.5f);
        const auto local_displacement=turn(inverse(bike_rotation(frame)),displacement);
        const float floating=.75f+.25f*std::min(foot_release(frame,true),foot_release(frame,false));
        movement.hip_offset=add(movement.hip_offset,mul(local_displacement,floating));
        movement.torso_offset=add(movement.torso_offset,mul(local_displacement,floating));
    }
    // Landing recovery and pedal motion are presentation cues only. The crank
    // remains the measured wheel/controller phase; no wall-clock locomotion.
    // A brief separating wheel sample cannot discard a still-decaying landing
    // recovery and rebound the hips by8cm. This never changes contact state.
    const float recovery=frame.landing_compression;
    const float pedal_bob=frame.airborne?0.f:.012f*frame.pedal_effort*std::sin(frame.crank_phase*2);
    // The selected smaller bike puts measured grips at0.953241m. Coordinate
    // its lower hip/chest requests with the unchanged native limb lengths;
    // the per-crank band and reach projection keep the contact solve bounded.
    float pelvis_height=small_bike_stance::neutral_hip_height-.20f*preload+extension-.08f*recovery+pedal_bob;
    const float weight_shift=.055f*front+.035f*rear;
    const auto hip_orientation=multiply(multiply(q,multiply(axis(1,movement.hip_yaw),axis(2,movement.hip_roll))),
        Quat{.5f,.5f,.5f,.5f}); // native +X up, +Y forward, +Z left
    set_rotation(bones,rig,rig.hips,hip_orientation);
    auto g=compose(bones,rig);const int parent=rig.parents[static_cast<std::size_t>(rig.hips)];
    if(parent<0)return result;
    const auto& p=g[static_cast<std::size_t>(parent)];
    const float desired_z=small_bike_stance::neutral_hip_z-.035f*preload+weight_shift+posture.hip[2]+movement.hip_offset[2];
    const auto hip_band=proposed_hip_band(bones,rig,frame,desired_z,measured);
    // Continuous support/release cues retain the authored loaded pose while
    // fading the extra neutral leg-band fit. A named trick at phase zero is
    // neutral; its identifier alone must not switch the pelvis target.
    const float air_cue=frame.air_extension_available?frame.air_extension_blend:(frame.airborne?1.f:0.f);
    const float contact_cue=1-std::max({frame.movement_phase,hand_release(frame,true),hand_release(frame,false),
        foot_release(frame,true),foot_release(frame,false)});
    const float fit_weight=(1-air_cue)*(1-recovery)*(1-preload)*(1-std::abs(front))*(1-std::abs(rear))*contact_cue;
    // Preserve the seat proxy during supported loading. Release/catalog
    // progression fades only this added lower-bound correction continuously.
    if(fit_weight>0&&!hip_band.available)return result;
    const float seat_fitted=std::max(pelvis_height,hip_band.seat_floor-posture.hip[1]-movement.hip_offset[1]);
    pelvis_height+=(seat_fitted-pelvis_height)*contact_cue;
    const float neutral_fitted=hip_band.available?std::clamp(pelvis_height,hip_band.low+.003f,hip_band.high-.003f):pelvis_height;
    pelvis_height+=(neutral_fitted-pelvis_height)*fit_weight;
    if(posture_scale>0&&hip_band.available) {
        const float fitted_drop=std::max(posture.hip[1],std::min(0.f,hip_band.low+.003f-pelvis_height));
        posture.hip[1]+=(fitted_drop-posture.hip[1])*fit_weight;
    }
    const auto local=mul(turn(inverse(p.rotation),sub(world(frame,add(
        add({0,pelvis_height,small_bike_stance::neutral_hip_z-.035f*preload+weight_shift},posture.hip),movement.hip_offset)),p.position)),1/p.scale);
    for(unsigned a=0;a<3;++a)bones[static_cast<std::size_t>(rig.hips)].position[a]=local[a];
    desired_hip=compose(bones,rig)[static_cast<std::size_t>(rig.hips)].position;
    aim(bones,rig,rig.spine[0],rig.spine[3],world(frame,
        add({0,small_bike_stance::chest_aim_height-.16f*preload+extension-.06f*recovery+pedal_bob*.5f,
            small_bike_stance::chest_aim_z-.03f*preload+.06f*front+.04f*rear},add(movement.torso_offset,posture.torso))));
    if(!articulate_clavicles(bones,rig,frame))return result;
    }
    if(!fit_pelvis_to_limbs(bones,rig,frame,nullptr,.18f,measured))return result;
    result.posture_fit_adjustment=length(sub(compose(bones,rig)[static_cast<std::size_t>(rig.hips)].position,desired_hip));
    if(spring)result.spring_fit_adjustment=result.posture_fit_adjustment;
    const auto forward=turn(q,{0,0,1});
    const auto pole=[&](Vec fallback,bool arm,unsigned side){
        return measured&&measured->available?blend(fallback,turn(q,arm?measured->arm_poles[side]:measured->leg_poles[side]),measured->strength*measured->pole_weight):fallback;};
    limb(bones,rig,rig.left_leg,foot_target(frame,true,measured),pole(forward,false,0));
    limb(bones,rig,rig.right_leg,foot_target(frame,false,measured),pole(forward,false,1));
    // Keep modest outward clearance below the shoulders. The coordinated
    // chest/hip stance makes the forearms reach forward, without rotating the
    // working grip basis or selecting a laterally flared elbow solution.
    limb(bones,rig,rig.left_arm,wrist_target(frame,true,measured),pole(turn(q,{small_bike_stance::elbow_pole_side,small_bike_stance::elbow_pole_down,small_bike_stance::elbow_pole_back}),true,0));
    limb(bones,rig,rig.right_arm,wrist_target(frame,false,measured),pole(turn(q,{-small_bike_stance::elbow_pole_side,small_bike_stance::elbow_pole_down,small_bike_stance::elbow_pole_back}),true,1));
    if(!close_hand(bones,rig,rig.left_fingers,rig.left_arm[2],frame,true)||
       !close_hand(bones,rig,rig.right_fingers,rig.right_arm[2],frame,false))return result;
    // Calibrate the native foot through its bind forefoot frame. The ankle
    // target remains the existing pedal proxy; native ToeBase flex stays owned
    // by animation, including released feet and walking/carrying.
    for(bool left:{false,true})
        set_rotation(bones,rig,left?rig.left_leg[2]:rig.right_leg[2],foot_rotation(rig,frame,left));
    result.posture_gaze=stabilize_gaze(bones,rig,frame,movement.head_yaw,posture.gaze);
    const auto g=compose(bones,rig);
    const auto palm=[&](bool left){const auto& hand=g[static_cast<std::size_t>(left?rig.left_arm[2]:rig.right_arm[2])];return add(hand.position,turn(hand.rotation,mul(palm_bar_offset(left),hand.scale)));};
    result.hand_error=std::max(length(sub(palm(true),palm_target(frame,true,measured))),length(sub(palm(false),palm_target(frame,false,measured))));
    result.foot_error=std::max(length(sub(g[static_cast<std::size_t>(rig.left_leg[2])].position,foot_target(frame,true,measured))),length(sub(g[static_cast<std::size_t>(rig.right_leg[2])].position,foot_target(frame,false,measured))));
    result.hand_contact=1-std::max(hand_release(frame,true),hand_release(frame,false));
    result.foot_contact=1-std::max(foot_release(frame,true),foot_release(frame,false));
    result.ready=std::isfinite(result.hand_error)&&std::isfinite(result.foot_error)&&result.hand_error<.18f&&result.foot_error<.18f;
    return result;
}
inline Result solve_entry(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,EntryContacts& contacts);
inline Result solve(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,EntryContacts* entry=nullptr,bool observed_selected=false,SpringBinding* spring_binding=nullptr) {
    Result result;
    if(frame.rider_mode!=visual::RiderMode::riding&&frame.rider_mode!=visual::RiderMode::carrying)return result;
    if(!std::isfinite(frame.rider_blend)||!std::isfinite(frame.mount_progress)||frame.mount_progress<0||frame.mount_progress>1)return result;
    if(!motion::valid(frame))return result;
    if(frame.chassis_quaternion_available) {
        Quat rotation;if(!chassis::unit_quaternion(frame.chassis_quaternion,rotation))return result;
    } else for(float a:{frame.yaw,frame.pitch,frame.roll})if(!std::isfinite(a))return result;
    if(rig.parents.empty()||rig.parents.size()>bones.size()||rig.bind.size()!=rig.parents.size())return result;
    for(std::size_t i=0;i<rig.parents.size();++i) {
        if(rig.parents[i]<-1||rig.parents[i]>=static_cast<int>(i))return result;
        float bind_norm=0;for(float q:rig.bind[i]){if(!std::isfinite(q))return result;bind_norm+=q*q;}
        if(bind_norm<.99f||bind_norm>1.01f)return result;
        const auto& bone=bones[i];float norm=0;
        for(float q:bone.rotation){if(!std::isfinite(q))return result;norm+=q*q;}
        if(norm<.9f||norm>1.1f)return result;
        for(unsigned a=0;a<3;++a)if(!std::isfinite(bone.position[a])||std::abs(bone.position[a])>1e6f||!std::isfinite(bone.scale[a])||bone.scale[a]<.0001f||bone.scale[a]>20)return result;
    }
    for(float p:frame.position)if(!std::isfinite(p)||std::abs(p)>1e6f)return result;
    for(float a:{frame.steer,frame.crank_phase,frame.barspin,frame.tailwhip,
        frame.preload,frame.front_weight,frame.rear_weight,frame.barspin_release,frame.tailwhip_release,
        frame.movement_bike_pitch,frame.movement_bike_roll})if(!std::isfinite(a))return result;
    const auto joints=changed(rig);
    for(int i:joints)if(i<0||static_cast<std::size_t>(i)>=rig.parents.size())return result;
    for(const auto chain:{rig.left_arm,rig.right_arm,rig.left_leg,rig.right_leg})
        if(rig.parents[static_cast<std::size_t>(chain[1])]!=chain[0]||rig.parents[static_cast<std::size_t>(chain[2])]!=chain[1])return result;
    for(bool left:{false,true}) {
        const int toe=rig.toe_base[left?0:1];
        if(toe<-1||toe>=static_cast<int>(rig.parents.size())||
           (toe>=0&&rig.parents[static_cast<std::size_t>(toe)]!=(left?rig.left_leg[2]:rig.right_leg[2])))return result;
    }
    for(bool left:{false,true})for(const auto& finger:(left?rig.left_fingers:rig.right_fingers)) {
        const bool absent=finger.joints[0]<0&&finger.joints[1]<0&&finger.joints[2]<0&&finger.metacarpal<0;
        if(absent)continue;
        for(int i:finger.joints)if(i<0||static_cast<std::size_t>(i)>=rig.parents.size())return result;
        if(rig.parents[static_cast<std::size_t>(finger.joints[1])]!=finger.joints[0]||
           rig.parents[static_cast<std::size_t>(finger.joints[2])]!=finger.joints[1])return result;
        const int hand=left?rig.left_arm[2]:rig.right_arm[2];
        if(finger.metacarpal>=0) {
            if(rig.parents[static_cast<std::size_t>(finger.metacarpal)]!=hand||
               rig.parents[static_cast<std::size_t>(finger.joints[0])]!=finger.metacarpal)return result;
        }else if(rig.parents[static_cast<std::size_t>(finger.joints[0])]!=hand)return result;
    }
    if(frame.rider_mode==visual::RiderMode::carrying) {
        // Native walking owns hips/spine/legs/left arm. Only the right primary
        // arm/fingers reach the carried top-tube proxy; no stale walking restore.
        for(int i:rig.right_arm)bones[static_cast<std::size_t>(i)].rotation=rig.bind[static_cast<std::size_t>(i)];
        for(const auto& finger:rig.right_fingers) {
            if(finger.metacarpal>=0)bones[static_cast<std::size_t>(finger.metacarpal)].rotation=rig.bind[static_cast<std::size_t>(finger.metacarpal)];
            for(int i:finger.joints)if(i>=0)bones[static_cast<std::size_t>(i)].rotation=rig.bind[static_cast<std::size_t>(i)];
        }
        const auto palm=part_world(frame,motion::carry_palm_local);
        const Vec distal{-1,0,0},across=unit(geometry::carry_tube_axis);
        const auto hold=multiply(part_rotation(frame),orientation(distal,across,cross(distal,across)));
        const auto hand_scale=compose(bones,rig)[static_cast<std::size_t>(rig.right_arm[2])].scale;
        const auto wrist=sub(palm,turn(hold,mul(palm_bar_offset(false,geometry::carry_tube_radius),hand_scale)));
        limb(bones,rig,rig.right_arm,wrist,turn(bike_rotation(frame),{-.25f,-1,-.15f}));
        if(!close_hand(bones,rig,rig.right_fingers,rig.right_arm[2],frame,false,&hold,geometry::carry_tube_radius))return result;
        const auto g=compose(bones,rig);const auto& hand=g[static_cast<std::size_t>(rig.right_arm[2])];
        result.hand_error=length(sub(add(hand.position,turn(hand.rotation,mul(palm_bar_offset(false,geometry::carry_tube_radius),hand.scale))),palm));
        result.hand_contact=.5f;result.foot_contact=0;
        result.ready=std::isfinite(result.hand_error)&&result.hand_error<.003f;return result;
    }
    if(frame.mount_token&&frame.mount_progress<1) {
        if(!entry||!frame.entity)return result;
        return solve_entry(bones,rig,frame,*entry);
    }
    const bool spring_requested=frame.spring_avatar.requested;
    SpringTargets spring_targets;const SpringTargets* spring=nullptr;
    if(spring_requested) {
        if(!spring_binding||frame.mount_progress!=1||frame.rider_blend!=1||!avatar_binding::valid(frame.spring_avatar.actual)||
           frame.entity!=frame.spring_avatar.actual.epoch.owner_world[1]||frame.native_body_token!=frame.spring_avatar.actual.epoch.token||
           !avatar_binding::close(frame.position,frame.spring_avatar.actual.bike_root)||
           !avatar_binding::close_rotation(bike_rotation(frame),frame.spring_avatar.actual.bike_orientation))return result;
        // Capture.neutral is the caller's independent quiet-input witness.
        // Corroborate it before touching any candidate; never synthesize it.
        if(!spring_binding->root.ready&&(!frame.spring_avatar.capture.requested||!frame.spring_avatar.capture.neutral||!spring_capture_neutral(frame)))return result;
        if(spring_binding->root.ready) {
            const auto root=avatar_binding::sample(spring_binding->root,frame.spring_avatar.actual);
            if(!root.ready||!spring_binding->proximal_ready||
               (spring_binding->completed_pose_write&&
                (frame.spring_avatar.actual.epoch.serial<spring_binding->pose_epoch.serial||frame.spring_avatar.actual.epoch.time<spring_binding->pose_epoch.time||
                 ((frame.spring_avatar.actual.epoch.serial==spring_binding->pose_epoch.serial)!=(frame.spring_avatar.actual.epoch.time==spring_binding->pose_epoch.time)))))return result;
            const auto& actual=frame.spring_avatar.actual;Quat q;
            if(!avatar_binding::np::unit(actual.spring_orientation,q))return result;
            spring_targets.hip=add(actual.spring_com,turn(q,spring_binding->local_hip));
            spring_targets.chest=add(actual.spring_com,turn(q,spring_binding->local_chest));
            spring_targets.hip_rotation=multiply(q,spring_binding->local_hip_basis);spring_targets.model_rotation=q;spring=&spring_targets;
        }
    }
    const float transition=std::clamp(frame.rider_blend,0.f,1.f);
    if(transition==0){result.ready=true;result.hand_contact=result.foot_contact=0;return result;}
    std::vector<Bone> original;
    if(transition<1) {original.reserve(joints.size());for(int i:joints)original.push_back(bones[static_cast<std::size_t>(i)]);}
    // Use the shipped bind orientation for the primary chain, preserving the live
    // character's proportions. Otherwise walking-animation twist survives every solve.
    const auto requested=posture_targets(frame,1);
    observed::Targets measured;
    if(observed::eligible(frame,observed_selected)){
        const auto g=compose(bones,rig);float leg_length=0;
        for(const auto chain:{rig.left_leg,rig.right_leg})
            leg_length+=.5f*(length(sub(g[static_cast<std::size_t>(chain[1])].position,g[static_cast<std::size_t>(chain[0])].position))+
                length(sub(g[static_cast<std::size_t>(chain[2])].position,g[static_cast<std::size_t>(chain[1])].position)));
        measured=observed::sample(frame,leg_length,observed_selected);
    }
    const bool posture_requested=spring_requested||measured.available||length(requested.hip)>0||length(requested.torso)>0||requested.gaze>0;
    if(!posture_requested) {
        for(int i:joints)bones[static_cast<std::size_t>(i)].rotation=rig.bind[static_cast<std::size_t>(i)];
        result=solve_riding_candidate(bones,rig,frame,0);
    } else {
        // One retained scratch allocation for at most three candidates. The
        // source pose stays untouched until a strict contact candidate wins.
        std::vector<Bone> candidate(bones.begin(),bones.end());
        for(float amount:{1.f,.5f,0.f}) {
            std::copy(bones.begin(),bones.end(),candidate.begin());
            for(int i:joints)candidate[static_cast<std::size_t>(i)].rotation=rig.bind[static_cast<std::size_t>(i)];
            auto scaled=measured;scaled.strength=amount;
            auto tried=solve_riding_candidate(candidate,rig,frame,amount,measured.available&&amount>0?&scaled:nullptr,spring);
            tried.observed_requested=measured.available;tried.observed_applied=measured.available&&amount>0;
            tried.observed_strength=tried.observed_applied?amount:0;
            const unsigned attempts=result.posture_attempts+1;
            result=tried;result.posture_requested=true;result.posture_attempts=attempts;
            result.posture_scale=amount;result.posture_fallback=attempts>1;
            if(tried.ready&&tried.hand_error<.003f&&tried.foot_error<.003f&&proposed_small_bike_anatomy(candidate,rig,frame)) {
                std::copy(candidate.begin(),candidate.end(),bones.begin());break;
            }
            result.ready=false;
        }
    }
    if(spring_requested) {
        result.spring_requested=true;
        // Even the neutral path uses the strict candidate loop. A failed Spring
        // contact solve must not silently become an unbound bike-follow pose.
        if(!result.ready||result.hand_error>=.003f||result.foot_error>=.003f)return Result{};
        if(!spring_binding->root.ready) {
            auto witness=frame.spring_avatar.capture;
            witness.mounted_geometry_verified=true;
            witness.neutral=witness.neutral&&spring_capture_neutral(frame);
            const auto calibration=avatar_binding::capture(frame.spring_avatar.actual,witness);
            if(!calibration.ready)return Result{};
            const auto actual=compose(bones,rig);const auto& hip=actual[static_cast<std::size_t>(rig.hips)];
            Quat checked_spring;
            if(!avatar_binding::np::unit(frame.spring_avatar.actual.spring_orientation,checked_spring))return Result{};
            const auto q=inverse(checked_spring);
            const auto com=frame.spring_avatar.actual.spring_com;
            spring_binding->root=calibration;
            spring_binding->local_hip=turn(q,sub(hip.position,com));
            spring_binding->local_chest=turn(q,sub(actual[static_cast<std::size_t>(rig.spine[3])].position,com));
            spring_binding->local_hip_basis=multiply(q,hip.rotation);
            spring_binding->proximal_ready=true;result.spring_calibrated=true;
        }
        result.spring_applied=spring!=nullptr;
        spring_binding->pose_epoch=frame.spring_avatar.actual.epoch;
        spring_binding->last_pose_applied=result.spring_applied;
        spring_binding->fit_adjustment=result.spring_fit_adjustment;
        spring_binding->hand_error=result.hand_error;spring_binding->foot_error=result.foot_error;
    }
    if(result.ready&&transition<1) {
        for(std::size_t i=0;i<joints.size();++i) {
            auto& bone=bones[static_cast<std::size_t>(joints[i])];
            bone.rotation=blend(original[i].rotation,bone.rotation,transition);
            if(joints[i]==rig.hips)for(unsigned a=0;a<3;++a)
                bone.position[a]=original[i].position[a]+(bone.position[a]-original[i].position[a])*transition;
        }
        // During entry/exit these are partial contacts, not completed IK claims.
        result.hand_contact*=transition;result.foot_contact*=transition;
    }
    return result;
}
// A mount uses the observed native stance and the supplied model's tube/pedals.
// It has no authored humanoid clip or guessed bone offsets. World-space anchors
// survive native root translation/rotation; IK keeps every native segment length.
inline Result solve_entry(std::span<Bone> bones,const Rig& rig,const visual::Frame& frame,EntryContacts& contacts) {
    const auto source=compose(bones,rig);
    if(contacts.entity!=frame.entity||contacts.token!=frame.mount_token) {
        contacts={};contacts.entity=frame.entity;contacts.token=frame.mount_token;
        for(unsigned i=0;i<2;++i) {
            const bool left=i==0;
            const auto& hand=source[static_cast<std::size_t>(left?rig.left_arm[2]:rig.right_arm[2])];
            const auto& foot=source[static_cast<std::size_t>(left?rig.left_leg[2]:rig.right_leg[2])];
            contacts.palms[i]=add(hand.position,turn(hand.rotation,mul(palm_bar_offset(left),hand.scale)));
            contacts.hands[i]=hand.rotation;contacts.ankles[i]=foot.position;contacts.feet[i]=foot.rotation;
            const auto& arm=left?rig.left_arm:rig.right_arm;const auto& leg=left?rig.left_leg:rig.right_leg;
            contacts.arm_poles[i]=sub(source[static_cast<std::size_t>(arm[1])].position,source[static_cast<std::size_t>(arm[0])].position);
            contacts.leg_poles[i]=sub(source[static_cast<std::size_t>(leg[1])].position,source[static_cast<std::size_t>(leg[0])].position);
        }
    }
    Result result;result.hand_contact=result.foot_contact=0;
    const float t=frame.mount_progress;
    if(t==0){result.ready=true;return result;}
    const std::vector<Bone> original(bones.begin(),bones.end());
    auto riding=original;auto target_frame=frame;target_frame.mount_token=0;target_frame.rider_blend=1;
    // The entry endpoint is a native mounted geometry candidate, never an
    // already-calibrated Spring receipt. Capture is admitted only after entry.
    target_frame.spring_avatar.requested=false;
    if(!solve(riding,rig,target_frame).ready)return result;
    const auto full=compose(riding,rig);const auto q=bike_rotation(frame);
    const float hands=mount_transition::phase(t,.05f,.42f),posture=mount_transition::phase(t,.12f,1.f);
    for(int index:changed(rig)) {
        float amount=posture;
        for(const auto& chain:{rig.left_arm,rig.right_arm})for(int joint:chain)if(index==joint)amount=hands;
        for(const auto& fingers:{rig.left_fingers,rig.right_fingers})for(const auto& finger:fingers) {
            if(index==finger.metacarpal)amount=hands;
            for(int joint:finger.joints)if(index==joint)amount=hands;
        }
        bones[static_cast<std::size_t>(index)].rotation=blend(original[static_cast<std::size_t>(index)].rotation,
            riding[static_cast<std::size_t>(index)].rotation,amount);
    }
    for(unsigned a=0;a<3;++a)bones[static_cast<std::size_t>(rig.hips)].position[a]=
        original[static_cast<std::size_t>(rig.hips)].position[a]+(riding[static_cast<std::size_t>(rig.hips)].position[a]-
            original[static_cast<std::size_t>(rig.hips)].position[a])*posture;
    std::array<Vec,2> palm_targets{},ankle_targets{},wrist_targets{},arm_poles{},leg_poles{};
    std::array<Quat,2> hand_rotations{},foot_rotations{};
    const auto up=turn(q,{0,1,0}),forward=turn(q,{0,0,1});
    float pedal_contact=1;
    for(unsigned i=0;i<2;++i) {
        const bool left=i==0,support=left==frame.mount_from_left;
        const auto& arm=left?rig.left_arm:rig.right_arm;
        palm_targets[i]=blend(contacts.palms[i],palm_target(frame,left),hands);
        const auto hand_q=blend(contacts.hands[i],hand_rotation(frame,left),hands);
        const auto wrist=sub(palm_targets[i],turn(hand_q,mul(palm_bar_offset(left),source[static_cast<std::size_t>(arm[2])].scale)));
        wrist_targets[i]=wrist;hand_rotations[i]=hand_q;
        arm_poles[i]=blend(contacts.arm_poles[i],turn(q,{left?.25f:-.25f,-1,-.2f}),hands);
        const auto endpoint=foot_target(frame,left),start=contacts.ankles[i];
        const float foot=mount_transition::phase(t,support?.45f:.34f,support?1.f:.88f);
        auto ankle=blend(start,endpoint,foot);
        if(!support&&foot>0&&foot<1) {
            // Raise before crossing, clear the measured top tube, then lower to
            // the far pedal. The retained ankle proxy accounts for the shoe sole.
            const float clearance=std::max({dot(start,up),dot(endpoint,up),
                dot(world(frame,geometry::carry_tube_center),up)+geometry::carry_tube_radius+geometry::ankle_from_sole+.02f});
            const auto raised_start=add(start,mul(up,clearance-dot(start,up)));
            const auto raised_end=add(endpoint,mul(up,clearance-dot(endpoint,up)));
            if(foot<.3f)ankle=blend(start,raised_start,mount_transition::smooth(foot/.3f));
            else if(foot<.75f)ankle=blend(raised_start,raised_end,mount_transition::smooth((foot-.3f)/.45f));
            else ankle=blend(raised_end,endpoint,mount_transition::smooth((foot-.75f)/.25f));
        }
        ankle_targets[i]=ankle;
        const auto& leg=left?rig.left_leg:rig.right_leg;
        leg_poles[i]=blend(contacts.leg_poles[i],forward,foot);
        foot_rotations[i]=blend(contacts.feet[i],full[static_cast<std::size_t>(leg[2])].rotation,foot);
        pedal_contact=std::min(pedal_contact,foot);
    }
    // Project the pelvis against all four actual limb lengths together, so
    // keeping the ground foot cannot silently pull an already caught grip away.
    // Side mounting needs more travel than the seated .18m posture correction;
    // .35m is a bounded presentation envelope, never an anatomical measurement.
    const std::array<Vec,4> targets{ankle_targets[0],ankle_targets[1],wrist_targets[0],wrist_targets[1]};
    if(!fit_pelvis_to_limbs(bones,rig,frame,&targets,.35f)) {
        std::copy(original.begin(),original.end(),bones.begin());return result;
    }
    for(unsigned i=0;i<2;++i) {
        const auto& arm=i==0?rig.left_arm:rig.right_arm;const auto& leg=i==0?rig.left_leg:rig.right_leg;
        limb(bones,rig,arm,wrist_targets[i],arm_poles[i]);set_rotation(bones,rig,arm[2],hand_rotations[i]);
        limb(bones,rig,leg,ankle_targets[i],leg_poles[i]);set_rotation(bones,rig,leg[2],foot_rotations[i]);
    }
    const auto actual=compose(bones,rig);
    for(unsigned i=0;i<2;++i) {
        const bool left=i==0;const auto& hand=actual[static_cast<std::size_t>(left?rig.left_arm[2]:rig.right_arm[2])];
        result.hand_error=std::max(result.hand_error,length(sub(add(hand.position,turn(hand.rotation,mul(palm_bar_offset(left),hand.scale))),palm_targets[i])));
        result.foot_error=std::max(result.foot_error,length(sub(actual[static_cast<std::size_t>(left?rig.left_leg[2]:rig.right_leg[2])].position,ankle_targets[i])));
    }
    // Unreachable intermediate targets clamp to native chain length. Do not
    // claim a planted/caught contact merely because its timeline has completed.
    result.hand_contact=hands==1&&result.hand_error<.003f?1.f:0.f;
    result.foot_contact=pedal_contact==1&&result.foot_error<.003f?1.f:0.f;
    result.ready=std::isfinite(result.hand_error)&&std::isfinite(result.foot_error);
    if(!result.ready)std::copy(original.begin(),original.end(),bones.begin());
    return result;
}
} // namespace dingosdk::bmx::rider::pose
