#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/stick_look.h"
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Input/controller_bindings.h"
namespace jishsfirstpersonmod {
struct BoardLookSettings {
    bool continue_auto_center=true;
    bool enabled=false,center_on_modifier=true,center_on_stick=false,keep_native_input=true,invert_y{};
    std::uint32_t modifier=0x100;
    float yaw_speed=330,pitch_speed=270,deadzone=.10f;
    float center_smoothing=35,center_pitch{},look_smoothing=12;
    float yaw_limit=85,pitch_limit=60;
};
struct StickInputPolicy {
    std::uint32_t mask{};
    StickAxes axes{};
    StickInputPolicy(std::uint32_t binding=0,StickAxes held={}):mask(binding),axes(held) {}
    bool captures(std::uint32_t buttons) const { return mask && (mask==~0u || (buttons&mask)==mask); }
};
inline short stick_short(float value) {
    value=std::clamp(value,-1.f,1.f);
    return static_cast<short>(std::lround(value*(value<0 ? 32768.f:32767.f)));
}
// Relative view angles stay with the skater; native input never uses camera damping.
class BoardLookState {
    bool ready_{},armed_{},holding_{},capturing_{},centering_{},modifier_centering_{};
    unsigned device_{};
    std::uint32_t modifier_{};
    double time_{};
    float yaw_{},pitch_{},target_yaw_{},target_pitch_{},yaw_velocity_{},pitch_velocity_{},center_pitch_{};
    StickAxes held_native_{};
public:
    void reset() { *this={}; }
    StickAxes native_axes(const BoardLookSettings& settings) const { return settings.keep_native_input ? held_native_:StickAxes{}; }
    StickAim update(const BoardLookSettings& settings,StickAxes axes,bool eligible,
                    first_person_vendor::ControllerInput input,double now) {
        if(!settings.enabled || !eligible || !input.available || !input.device || !std::isfinite(now)) { reset();return {}; }
        if(!ready_ || device_!=input.device || modifier_!=settings.modifier || now<time_ || now-time_>.25) {
            reset();ready_=true;device_=input.device;modifier_=settings.modifier;time_=now;
            pitch_=target_pitch_=center_pitch_=settings.center_pitch;
        }
        const float dt=static_cast<float>(now-time_);time_=now;
        const float shift=settings.center_pitch-center_pitch_;center_pitch_=settings.center_pitch;
        pitch_+=shift;target_pitch_+=shift;
        const float low=std::max(-85.f,settings.center_pitch-settings.pitch_limit);
        const float high=std::min(85.f,settings.center_pitch+settings.pitch_limit);
        const auto clamp=[&]() {
            yaw_=std::clamp(yaw_,-settings.yaw_limit,settings.yaw_limit);
            target_yaw_=std::clamp(target_yaw_,-settings.yaw_limit,settings.yaw_limit);
            pitch_=std::clamp(pitch_,low,high);target_pitch_=std::clamp(target_pitch_,low,high);
        };
        clamp();
        const bool held=settings.modifier && (input.buttons&settings.modifier)==settings.modifier;
        if(!held) armed_=true;
        const bool capture=held && armed_;
        const auto stick=radial_stick(axes,settings.deadzone);
        const bool deflected=stick.x!=0 || stick.y!=0;
        if(capture && !capturing_) {
            held_native_=axes;
            if(!(settings.continue_auto_center && centering_ && modifier_centering_)) {
                centering_=modifier_centering_=false;target_yaw_=yaw_;target_pitch_=pitch_;yaw_velocity_=pitch_velocity_=0;
            }
        }
        if(capture) {
            if(holding_ && deflected && centering_) {
                target_yaw_=yaw_;target_pitch_=pitch_;yaw_velocity_=pitch_velocity_=0;
                centering_=modifier_centering_=false;
            }
            // Native position is latched before requiring neutral camera input.
            if(!holding_ && !deflected) holding_=true;
            // A modifier-release return survives re-capture until actual look input.
            if(!(settings.continue_auto_center && centering_ && modifier_centering_)) {
                centering_=holding_ && !deflected && settings.center_on_stick;modifier_centering_=false;
            }
            if(holding_ && deflected) {
                target_yaw_-=stick.x*settings.yaw_speed*dt;
                target_pitch_+=stick.y*settings.pitch_speed*dt*(settings.invert_y ? -1.f:1.f);
            }
        } else {
            holding_=false;
            if(capturing_) {
                // Modifier release has priority regardless of the physical stick.
                if(!(settings.continue_auto_center && centering_ && modifier_centering_)) {
                    centering_=modifier_centering_=settings.center_on_modifier;
                    target_yaw_=yaw_;target_pitch_=pitch_;yaw_velocity_=pitch_velocity_=0;
                }
            }
            if(!settings.center_on_modifier) centering_=modifier_centering_=false;
        }
        capturing_=capture;clamp();
        if(centering_) { target_yaw_=0;target_pitch_=settings.center_pitch; }
        const float strength=centering_ ? settings.center_smoothing:capture ? settings.look_smoothing:0;
        float yaw_error=yaw_-target_yaw_,pitch_error=pitch_-target_pitch_;
        first_person_vendor::first_person::damp(yaw_error,yaw_velocity_,strength,dt);
        first_person_vendor::first_person::damp(pitch_error,pitch_velocity_,strength,dt);
        yaw_=target_yaw_+yaw_error;pitch_=target_pitch_+pitch_error;clamp();
        if(centering_ && std::abs(yaw_)<.001f && std::abs(pitch_-settings.center_pitch)<.001f &&
            std::abs(yaw_velocity_)<.01f && std::abs(pitch_velocity_)<.01f) {
            yaw_=target_yaw_=0;pitch_=target_pitch_=settings.center_pitch;
            yaw_velocity_=pitch_velocity_=0;centering_=modifier_centering_=false;
        }
        return {capture,true,yaw_,pitch_,0,capture && holding_ && deflected};
    }
};
}
