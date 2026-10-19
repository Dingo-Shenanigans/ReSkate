#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"
#include <algorithm>
#include <cmath>
namespace jishsfirstpersonmod {
struct StickSettings {
    bool enabled{},turn_character{},invert_y{};
    float yaw_speed=120,pitch_speed=90,deadzone=.15f;
    bool smooth_transitions=true;
    float mount_smoothing=70,dismount_smoothing=35;
};
struct StickAxes { float x{},y{}; };
inline float normalize_axis(short value) { return value<0 ? value/32768.f:value/32767.f; }
inline float wrap_heading(float heading) { return std::remainder(heading,360.f); }
inline StickAxes radial_stick(StickAxes input,float deadzone) {
    if(!std::isfinite(input.x) || !std::isfinite(input.y)) return {};
    const auto length=std::hypot(input.x,input.y);
    if(length<=deadzone) return {};
    const float scale=(std::min(length,1.f)-deadzone)/((1-deadzone)*length);
    return {input.x*scale,input.y*scale};
}
struct StickAim { bool capture{},active{}; float heading{},pitch{},body_heading{};bool manual{};};
class StickLookState {
    bool ready_{},seen_{},mode_{},center_facing_{};
    unsigned device_{};
    double time_{};
    float yaw_{},pitch_{},smooth_yaw_{},smooth_pitch_{},yaw_velocity_{},pitch_velocity_{},body_bias_{},center_offset_{};
public:
    void reset() { *this={}; }
    void recenter_facing(float camera_offset=180.f) { reset();center_facing_=true;center_offset_=camera_offset; }
    void seed(const StickSettings& settings,unsigned device,double now,float yaw,float pitch,float body_heading) {
        reset();ready_=seen_=true;device_=device;mode_=settings.turn_character;time_=now;
        yaw_=smooth_yaw_=yaw;pitch_=smooth_pitch_=std::clamp(pitch,-85.f,85.f);
        body_bias_=wrap_heading(body_heading-yaw);
    }
    void rebase_facing(float body_heading) { body_bias_=wrap_heading(body_heading-smooth_yaw_); }
    StickAim update(const StickSettings& settings,StickAxes axes,bool eligible,unsigned device,
                    double now,float camera_heading,float body_heading,
                    const first_person_vendor::first_person::Settings& spring,float camera_pitch=0) {
        if(!settings.enabled || !eligible || !device || !std::isfinite(now)) { reset();return {}; }
        if(!seen_ || device_!=device || mode_!=settings.turn_character || now<time_ || now-time_>.25) {
            const bool center=center_facing_;const float offset=center_offset_;
            reset();center_facing_=center;center_offset_=offset;seen_=true;device_=device;mode_=settings.turn_character;time_=now;
        }
        const auto input=radial_stick(axes,settings.deadzone);
        if(!ready_) {
            time_=now;
            if(input.x!=0 || input.y!=0) return {true,false};
            if(center_facing_ && settings.turn_character) {camera_heading=wrap_heading(body_heading+center_offset_);camera_pitch=0;}
            center_facing_=false;
            ready_=true;yaw_=smooth_yaw_=camera_heading;pitch_=smooth_pitch_=std::clamp(camera_pitch,-85.f,85.f);
            body_bias_=wrap_heading(body_heading-camera_heading);
        }
        const float dt=static_cast<float>(now-time_);time_=now;
        // Match the proven glance conventions: +Y looks up, +X looks right.
        yaw_=wrap_heading(yaw_-input.x*settings.yaw_speed*dt);
        pitch_=std::clamp(pitch_+input.y*settings.pitch_speed*dt*(settings.invert_y ? -1.f:1.f),-85.f,85.f);
        if(!spring.enabled) { smooth_yaw_=yaw_;smooth_pitch_=pitch_;yaw_velocity_=pitch_velocity_=0; }
        else {
            float yaw_error=wrap_heading(smooth_yaw_-yaw_),pitch_error=smooth_pitch_-pitch_;
            first_person_vendor::first_person::damp(yaw_error,yaw_velocity_,yaw_error>0 ? spring.right:spring.left,dt);
            first_person_vendor::first_person::damp(pitch_error,pitch_velocity_,pitch_error>0 ? spring.down:spring.up,dt);
            smooth_yaw_=wrap_heading(yaw_+yaw_error);smooth_pitch_=pitch_+pitch_error;
        }
        return {true,true,smooth_yaw_,smooth_pitch_,wrap_heading(smooth_yaw_+body_bias_),input.x!=0 || input.y!=0};
    }
};
inline float matrix_heading(const first_person_vendor::first_person::Matrix& matrix) {
    return std::atan2(matrix[8],matrix[10])*180.f/std::numbers::pi_v<float>;
}
inline first_person_vendor::first_person::Matrix aim_head_relative(first_person_vendor::first_person::Matrix pose,const StickAim& aim) {
    // StickAim heading uses mathematical yaw; LookAngles uses its inverse.
    return aim.active ? first_person_vendor::first_person::target(pose,{}, {aim.pitch,-aim.heading}):pose;
}
inline first_person_vendor::first_person::Matrix aim_camera(first_person_vendor::first_person::Matrix pose,const StickAim& aim) {
    if(!aim.active) return pose;
    using namespace first_person_vendor::first_person;
    // Absolute world-up yaw followed by pitch about the horizontal right axis.
    // Native head tilt must not turn horizontal stick movement into camera roll.
    const float yaw=wrap_heading(aim.heading)*std::numbers::pi_v<float>/360.f;
    const float pitch=std::clamp(aim.pitch,-85.f,85.f)*std::numbers::pi_v<float>/360.f;
    const auto rotation=multiply({0,std::sin(yaw),0,std::cos(yaw)},
                                 {std::sin(pitch),0,0,std::cos(pitch)});
    write(pose,rotation,{pose[12],pose[13],pose[14]});
    return pose;
}
inline first_person_vendor::first_person::Matrix face_heading(first_person_vendor::first_person::Matrix pose,float heading) {
    // Keep the native surface-up axis, target position and SIMD metadata.
    using namespace first_person_vendor::first_person;
    Vec3 up{pose[4],pose[5],pose[6]};const auto n=length(up);
    if(!std::isfinite(n) || n<.5f) return pose;
    for(auto& value:up) value/=n;
    const float radians=heading*std::numbers::pi_v<float>/180;
    Vec3 forward{std::sin(radians),0,std::cos(radians)};
    const float dot=forward[0]*up[0]+forward[1]*up[1]+forward[2]*up[2];
    for(unsigned i=0;i<3;++i) forward[i]-=dot*up[i];
    const auto f=length(forward);if(f<.01f) return pose;
    for(auto& value:forward) value/=f;
    const Vec3 right{up[1]*forward[2]-up[2]*forward[1],up[2]*forward[0]-up[0]*forward[2],up[0]*forward[1]-up[1]*forward[0]};
    for(unsigned i=0;i<3;++i) { pose[i]=right[i];pose[4+i]=up[i];pose[8+i]=forward[i]; }
    return pose;
}
}
