#include <bmx/joint_actuation.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace bmx {
namespace {
bool unit(Quat q) noexcept {double norm=0;for(double x:q){if(!std::isfinite(x))return false;norm+=x*x;}return std::abs(norm-1)<1e-4;}
bool valid_command(const JointActuationCommand& c) noexcept {
    for(double x:c.target_position)if(!std::isfinite(x)||std::abs(x)>5)return false;
    for(double x:c.rider_inertia)if(!std::isfinite(x)||x<=0||x>1e6)return false;
    for(const auto* gains:{&c.linear_spring,&c.angular_spring})for(double x:*gains)if(!std::isfinite(x)||x<0||x>1e8)return false;
    for(const auto* gains:{&c.linear_damper,&c.angular_damper})for(double x:*gains)if(!std::isfinite(x)||x<0||x>1e7)return false;
    return unit(c.target_rotation)&&unit(c.rider_inertia_orientation)&&std::isfinite(c.radial_limit)&&c.radial_limit>=0&&c.radial_limit<=10&&c.acceleration_mask<=63;
}
Quat interpolate(Quat a,Quat b,double t) noexcept {
    double dot=0;for(unsigned i=0;i<4;++i)dot+=a[i]*b[i];
    if(dot<0){for(double& x:b)x=-x;dot=-dot;}
    Quat q{};
    if(dot>.9995)for(unsigned i=0;i<4;++i)q[i]=a[i]+t*(b[i]-a[i]);
    else {const double angle=std::acos(std::clamp(dot,0.,1.)),den=std::sin(angle);
        for(unsigned i=0;i<4;++i)q[i]=(std::sin((1-t)*angle)*a[i]+std::sin(t*angle)*b[i])/den;}
    double norm=0;for(double x:q)norm+=x*x;for(double& x:q)x/=std::sqrt(norm);return q;
}
}
bool valid_joint_actuation(const JointActuationTrack& track) noexcept {
    if(track.id>actuation_preload||track.keys.empty()||!valid_command(track.reference)||
        !std::isfinite(track.request_amplitude)||track.request_amplitude<=0||track.request_amplitude>1)return false;
    double last=-std::numeric_limits<double>::infinity();
    for(const auto& key:track.keys){if(!std::isfinite(key.seconds)||std::abs(key.seconds)>60||key.seconds<=last||!valid_command(key.command))return false;last=key.seconds;}
    return true;
}
JointActuationSample sample_joint_actuation(const JointActuationTrack& track,double seconds) noexcept {
    JointActuationSample result;if(!std::isfinite(seconds)||!valid_joint_actuation(track))return result;
    result.before_domain=seconds<track.keys.front().seconds;result.after_domain=seconds>track.keys.back().seconds;
    const JointActuationKey* a=&track.keys.front(),*b=a;
    if(seconds>=track.keys.back().seconds)a=b=&track.keys.back();
    else if(seconds>track.keys.front().seconds){const auto next=std::upper_bound(track.keys.begin(),track.keys.end(),seconds,[](double value,const JointActuationKey& key){return value<key.seconds;});b=&*next;a=&*(next-1);}
    const double t=b->seconds>a->seconds?(seconds-a->seconds)/(b->seconds-a->seconds):0;
    auto& c=result.command;c=a->command;
    for(unsigned i=0;i<3;++i){
        c.target_position[i]+=t*(b->command.target_position[i]-c.target_position[i]);
        c.rider_inertia[i]+=t*(b->command.rider_inertia[i]-c.rider_inertia[i]);
        for(auto pair:{std::pair{&c.linear_spring,&b->command.linear_spring},std::pair{&c.linear_damper,&b->command.linear_damper},
            std::pair{&c.angular_spring,&b->command.angular_spring},std::pair{&c.angular_damper,&b->command.angular_damper}})
            (*pair.first)[i]+=t*((*pair.second)[i]-(*pair.first)[i]);
    }
    c.target_rotation=interpolate(a->command.target_rotation,b->command.target_rotation,t);
    c.rider_inertia_orientation=interpolate(a->command.rider_inertia_orientation,b->command.rider_inertia_orientation,t);
    c.radial_limit+=t*(b->command.radial_limit-c.radial_limit);
    // Drive modes are discrete commands; the next mode takes effect at its key.
    if(t>=1)c.acceleration_mask=b->command.acceleration_mask;
    result.available=true;return result;
}
const JointActuationTrack* find_joint_actuation(std::uint16_t id,JointActuationPhase phase) noexcept {
    for(const auto& track:streets_observed_joint_actuation_tracks())if(track.id==id&&track.phase==phase)return &track;
    return nullptr;
}
}
