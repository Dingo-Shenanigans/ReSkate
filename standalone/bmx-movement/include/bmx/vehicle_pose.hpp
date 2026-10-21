#pragma once
#include "pose_track.hpp"

namespace bmx {
enum class VehiclePoseNode : unsigned char {
    spring,chassis,front_wheel,rear_wheel,headset,bottom_bracket,left_pedal,right_pedal,
    animated_vehicle_root,root_bike_joint,crank_target,left_pedal_target,right_pedal_target,count
};
inline constexpr unsigned vehicle_pose_node_count=unsigned(VehiclePoseNode::count);
inline constexpr std::array<std::string_view,vehicle_pose_node_count> vehicle_pose_node_names{
    "Spring Body","Chassis Body","Front Wheel Front","Back Wheel Back","Headset","BB Body",
    "Left Pedal Body","Right Pedal Body","_animatedVehicleRootJoint","_rootBikeJoint",
    "_BBAnimationTarget","_leftPedalAnimationTarget","_rightPedalAnimationTarget"};
struct VehiclePoseKey {float seconds{};PoseBone transform{};};
struct VehiclePoseTrack {
    TrickId id{TrickId::none};VehiclePoseNode node{};
    // Actual transform origins, expressed in the Chassis COM frame. These are
    // neither center-of-mass positions nor an assumed visual-mesh binding.
    PoseBone neutral{};std::span<const VehiclePoseKey> keys{};
    bool animation_control_target{};
};
using VehiclePose=std::array<PoseBone,vehicle_pose_node_count>;
std::span<const VehiclePoseTrack> streets_observed_vehicle_pose_tracks() noexcept;
inline PoseBone sample_vehicle_node(const VehiclePoseTrack&track,double seconds) noexcept {
    PoseBone result{};if(track.keys.empty()||!std::isfinite(seconds))return result;
    const VehiclePoseKey*a=&track.keys.front(),*b=a;double previous=-INFINITY;
    for(const auto&key:track.keys){if(!std::isfinite(key.seconds)||key.seconds<=previous)return result;previous=key.seconds;}
    if(seconds>=track.keys.back().seconds)a=b=&track.keys.back();
    else if(seconds>track.keys.front().seconds){auto next=std::upper_bound(track.keys.begin(),track.keys.end(),seconds,[](double t,const VehiclePoseKey&key){return t<key.seconds;});b=&*next;a=&*(next-1);}
    const double span=b->seconds-a->seconds,t=span>0?std::clamp((seconds-a->seconds)/span,0.,1.):0;
    result.available=a->transform.available&b->transform.available;
    for(const auto*bone:{&a->transform,&b->transform}){
        for(float value:bone->position)if(!std::isfinite(value))result.available&=~pose_position;
        double norm=0;for(float value:bone->rotation){if(!std::isfinite(value))result.available&=~pose_rotation;norm+=double(value)*value;}
        if(norm<1e-20)result.available&=~pose_rotation;
    }
    if(result.available&pose_position)for(unsigned j=0;j<3;++j)result.position[j]=float(a->transform.position[j]+t*(b->transform.position[j]-a->transform.position[j]));
    if(result.available&pose_rotation)result.rotation=pose_detail::narrow(pose_detail::slerp(pose_detail::quaternion(a->transform.rotation),pose_detail::quaternion(b->transform.rotation),t));
    return result;
}
inline VehiclePose sample_vehicle_pose(TrickId id,double seconds) noexcept {
    VehiclePose result{};for(const auto&track:streets_observed_vehicle_pose_tracks())if(track.id==id&&unsigned(track.node)<result.size())result[unsigned(track.node)]=sample_vehicle_node(track,seconds);return result;
}
// Rebase an observed relative part transform onto an independently modelled
// neutral geometry. Useful for handlebars, cranks and pedals. Physics receives
// no sampled body state; an engine may use its own articulated rigid bodies.
inline Vec3 articulate_vehicle_point(Vec3 point,const VehiclePoseTrack&track,
    const PoseBone&sample,const PoseFrame&chassis) noexcept {
    if((sample.available&track.neutral.available&(pose_position|pose_rotation))!=(pose_position|pose_rotation)){
        point=rotate(chassis.orientation,point);for(unsigned j=0;j<3;++j)point[j]+=chassis.position[j];return point;
    }
    const auto q0=pose_detail::quaternion(track.neutral.rotation),q=pose_detail::quaternion(sample.rotation);
    Vec3 neutral{track.neutral.position[0],track.neutral.position[1],track.neutral.position[2]};
    for(unsigned j=0;j<3;++j)point[j]-=neutral[j];
    point=rotate({-q0[0],-q0[1],-q0[2],q0[3]},point);point=rotate(q,point);
    for(unsigned j=0;j<3;++j)point[j]+=sample.position[j];
    point=rotate(chassis.orientation,point);for(unsigned j=0;j<3;++j)point[j]+=chassis.position[j];return point;
}
}
