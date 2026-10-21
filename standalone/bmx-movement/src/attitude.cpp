#include <bmx/attitude.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace bmx {
namespace {
namespace m=detail::source::tire::detail;
double signed_angle(Vec3 from,Vec3 to,Vec3 axis) noexcept {
    const double denominator=std::sqrt(m::dot(from,from)*m::dot(to,to));
    if(denominator<1e-12)return 0;
    const double angle=std::acos(std::clamp(m::dot(from,to)/denominator,-1.,1.))*180/std::numbers::pi;
    return m::dot(m::cross(from,to),axis)<0?-angle:angle;
}
}
bool valid_attitude_pid(const AttitudePIDSettings& s) noexcept {
    for(double value:{s.proportional,s.integral,s.derivative,s.maximum_output,s.ground_pitch_output,s.air_pitch_output,
        s.ground_roll_output,s.air_roll_output,s.ground_mass_factor,s.air_mass_factor,s.air_multiplier,
        s.pitch_multiplier,s.ground_pitch_control,s.air_pitch_control,s.force_multiplier,s.normal_lerp,s.maximum_pitch_angle,s.maximum_torque})
        if(!std::isfinite(value)||value<0||value>1e6)return false;
    return std::isfinite(s.pitch_setpoint_degrees)&&std::abs(s.pitch_setpoint_degrees)<=180&&
        s.maximum_output<=100&&s.maximum_pitch_angle<=180&&s.maximum_torque>0&&s.ground_pitch_control<=1&&s.air_pitch_control<=1;
}
void AttitudePID::reset() noexcept {target_normal_={0,1,0};integral_={};last_error_={};}
AttitudePIDResult AttitudePID::step(double seconds,const Body& body,Vec3 wanted_normal,bool grounded,const AttitudePIDSettings& s) noexcept {
    AttitudePIDResult out;
    if(!std::isfinite(seconds)||seconds<minimum_fixed_step||seconds>maximum_fixed_step||!valid_attitude_pid(s)||
        !detail::source::tire::valid(body)||!m::unit(wanted_normal,wanted_normal))return out;
    out.valid=true;if(!s.enabled)return out;
    const auto up=rotate(body.orientation,{0,1,0}),right=rotate(body.orientation,{1,0,0}),forward=rotate(body.orientation,{0,0,1});
    const double sign=m::dot(wanted_normal,up)<0?-1:1;
    const double blend=std::clamp(seconds*s.normal_lerp,0.,1.);
    target_normal_=m::mul(m::add(target_normal_,m::mul(m::sub(wanted_normal,target_normal_),blend)),sign);
    const auto roll_axis=m::cross(target_normal_,forward),pitch_axis=m::cross(right,target_normal_);
    const double roll_angle=signed_angle(right,roll_axis,forward);
    const double pitch_angle=signed_angle(forward,pitch_axis,m::mul(right,-1));
    out.angle_degrees={pitch_angle,0,roll_angle};
    const std::array<double,2> error{s.pitch_setpoint_degrees-pitch_angle,-roll_angle};
    const double mass_factor=grounded?s.ground_mass_factor:s.air_mass_factor;
    const double factor=s.force_multiplier*(grounded?1:s.air_multiplier);
    std::array<double,2> output{};
    for(unsigned i=0;i<2;++i){
        if(i==0&&std::abs(pitch_angle)>=s.maximum_pitch_angle)continue;
        integral_[i]+=error[i]*seconds;
        const double derivative=(error[i]-last_error_[i])/seconds;last_error_[i]=error[i];
        const double value=s.proportional*error[i]*(grounded?1:mass_factor)+s.integral*integral_[i]+s.derivative*derivative*mass_factor;
        const double limited=std::clamp(value,-s.maximum_output,s.maximum_output);out.saturated=out.saturated||limited!=value;
        output[i]=limited*(i==0?(grounded?s.ground_pitch_output:s.air_pitch_output):(grounded?s.ground_roll_output:s.air_roll_output));
    }
    // The native controller omits pitch outside its80 degree controllable range.
    out.local_torque={std::abs(pitch_angle)<s.maximum_pitch_angle?output[0]*s.pitch_multiplier*(grounded?s.ground_pitch_control:s.air_pitch_control)*factor:0,0,-output[1]*factor};
    const double size=std::sqrt(m::dot(out.local_torque,out.local_torque));
    if(size>s.maximum_torque){out.local_torque=m::mul(out.local_torque,s.maximum_torque/size);out.saturated=true;}
    out.error_degrees={error[0],0,error[1]};out.torque=rotate(body.orientation,out.local_torque);return out;
}
}
