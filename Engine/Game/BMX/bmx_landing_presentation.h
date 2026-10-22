#pragma once
#include "Engine/Game/BMX/bmx_landing_approach.h"

// Presentation only. The bridge owns token/serial/native-clock continuity and
// clears its previous presentation on pause, rejection or owner change. This
// stateless policy never changes a contact, force, trick or scoring observation.
namespace dingosdk::bmx::landing_presentation {
struct Result {
    bool valid{},advanced{};
    float compression{};
};
inline Result step(float previous,double native_seconds,std::uint64_t current_serial,
                   bool supported,const landing_approach::Evidence& evidence) noexcept {
    if(!std::isfinite(previous)||previous<0||previous>1||
       !std::isfinite(native_seconds)||native_seconds<0||native_seconds>.1)return {};
    // Repeated renders/paused native time cannot trigger or spend recovery.
    if(native_seconds==0)return {true,false,previous};
    if(!current_serial||!evidence.valid||supported!=evidence.post_available||
       (evidence.first_contact&&!supported))return {};
    double target=0;
    if(supported) {
        if(!evidence.contact_serial||evidence.contact_serial>current_serial||
           !std::isfinite(evidence.post_closing_speed)||evidence.post_closing_speed<0)return {};
        // A retained first-touch maximum remains authoritative for scoring,
        // but is an animation pulse only on that actual first-touch sample.
        double impact=evidence.post_closing_speed;
        if(evidence.first_contact) {
            if(evidence.contact_serial!=current_serial||
               !std::isfinite(evidence.scoring_closing_speed)||evidence.scoring_closing_speed<0)return {};
            impact=std::max(impact,evidence.scoring_closing_speed);
        }
        target=std::clamp(impact/6.0,0.0,1.0);
    }
    // Advancing air samples may recover the stored pose, never create a pulse.
    // Rider posing keeps this recovery continuous across support toggles.
    const double recovered=std::max(0.0,static_cast<double>(previous)-native_seconds*4);
    return {true,true,static_cast<float>(std::max(target,recovered))};
}
struct AirExtension {
    bool valid{},advanced{};
    float blend{};
};
// Native bridge presentation only: slew the existing4cm air posture over about
// .2 native seconds in either direction. No body position or airborne gate is
// changed, and the caller owns the same native clock/reset contract as above.
inline AirExtension step_air_extension(float previous,double native_seconds,bool airborne) noexcept {
    if(!std::isfinite(previous)||previous<0||previous>1||
       !std::isfinite(native_seconds)||native_seconds<0||native_seconds>.1)return {};
    if(native_seconds==0)return {true,false,previous};
    const double target=airborne?1:0;
    const double maximum_change=native_seconds*5;
    const double change=std::clamp(target-static_cast<double>(previous),-maximum_change,maximum_change);
    return {true,true,static_cast<float>(std::clamp(previous+change,0.0,1.0))};
}
}
