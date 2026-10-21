#pragma once
#include "pose_track.hpp"

namespace bmx {
enum class RiderLimb : unsigned char {left_hand,right_hand,left_foot,right_foot,count};
// Literal observed IK controls. A positional IK weight alone does not establish
// hand-on-grip or foot-on-pedal contact; the target binding is separate evidence.
enum class IKContactBinding : unsigned char {unknown,left_grip,right_grip,left_pedal,right_pedal,other};
enum IKAvailability : unsigned char {ik_position_weight=1,ik_rotation_weight=2,ik_reference_weight=4,ik_target_selector=8};
struct IKCommand {
    float position_weight{},rotation_weight{},reference_weight{};
    int target_selector{};unsigned char available{};
    IKContactBinding binding{IKContactBinding::unknown};
};
struct IKKey {float seconds{};IKCommand command{};};
struct IKTrack {TrickId id{TrickId::none};RiderLimb limb{};std::span<const IKKey> keys{};};
using IKCommands=std::array<IKCommand,unsigned(RiderLimb::count)>;
std::span<const IKTrack> streets_observed_ik_tracks() noexcept;
inline IKCommand sample_ik(const IKTrack&track,double seconds) noexcept {
    IKCommand result{};if(track.keys.empty()||!std::isfinite(seconds))return result;
    const IKKey*a=&track.keys.front(),*b=a;double previous=-INFINITY;
    for(const auto&key:track.keys){if(!std::isfinite(key.seconds)||key.seconds<=previous)return result;previous=key.seconds;}
    if(seconds>=track.keys.back().seconds)a=b=&track.keys.back();
    else if(seconds>track.keys.front().seconds){auto next=std::upper_bound(track.keys.begin(),track.keys.end(),seconds,[](double t,const IKKey&key){return t<key.seconds;});b=&*next;a=&*(next-1);}
    const double span=b->seconds-a->seconds,t=span>0?std::clamp((seconds-a->seconds)/span,0.,1.):0;
    result=a->command;result.available&=b->command.available;
    if(!std::isfinite(a->command.position_weight)||!std::isfinite(b->command.position_weight))result.available&=~ik_position_weight;
    if(!std::isfinite(a->command.rotation_weight)||!std::isfinite(b->command.rotation_weight))result.available&=~ik_rotation_weight;
    if(!std::isfinite(a->command.reference_weight)||!std::isfinite(b->command.reference_weight))result.available&=~ik_reference_weight;
    if(result.available&ik_position_weight)result.position_weight=float(a->command.position_weight+t*(b->command.position_weight-a->command.position_weight));
    if(result.available&ik_rotation_weight)result.rotation_weight=float(a->command.rotation_weight+t*(b->command.rotation_weight-a->command.rotation_weight));
    if(result.available&ik_reference_weight)result.reference_weight=float(a->command.reference_weight+t*(b->command.reference_weight-a->command.reference_weight));
    // Selector and binding use the previous actual sample, never a fractional ID.
    return result;
}
inline IKCommands sample_ik(TrickId id,double seconds) noexcept {
    IKCommands result{};for(const auto&track:streets_observed_ik_tracks())if(track.id==id&&unsigned(track.limb)<result.size())result[unsigned(track.limb)]=sample_ik(track,seconds);return result;
}
}
