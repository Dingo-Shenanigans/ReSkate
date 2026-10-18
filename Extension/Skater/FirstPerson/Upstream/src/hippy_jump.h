#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"
#include <cstdint>
namespace jishsfirstpersonmod {
struct HippyJumpEvidence {
    std::uintptr_t owner{};bool valid{},bail{},falling{},grounded{},on_board{};
    std::uint32_t physics{};first_person_vendor::first_person::Vec3 position{};
};
struct HippyJumpState { bool active{},candidate{};unsigned rising_samples{};float up_speed{}; };
class HippyJumpDetector {
    std::uintptr_t owner_{};bool ready_{};std::uint32_t physics_{};
    first_person_vendor::first_person::Vec3 position_{};double time_{},candidate_start_{};
    HippyJumpState state_{};
public:
    void reset() { *this={}; }
    HippyJumpState state() const { return state_; }
    HippyJumpState update(const HippyJumpEvidence& evidence,double now) {
        using namespace first_person_vendor::first_person;
        if(!evidence.valid || !evidence.owner || evidence.bail || !std::isfinite(now)) { reset();return state_; }
        for(auto value:evidence.position) if(!std::isfinite(value)) { reset();return state_; }
        const double dt=now-time_;
        if(!ready_ || owner_!=evidence.owner || dt<0 || dt>.25 || length(subtract(evidence.position,position_))>5) {
            reset();ready_=true;owner_=evidence.owner;physics_=evidence.physics;position_=evidence.position;time_=now;return state_;
        }
        if(dt==0) return state_;
        state_.up_speed=static_cast<float>((evidence.position[1]-position_[1])/dt);
        if(physics_==100 && evidence.physics==504 && !evidence.on_board) {
            state_.candidate=true;state_.rising_samples=0;candidate_start_=now;
        }
        if(state_.active && (evidence.on_board || evidence.grounded)) state_={};
        if(state_.candidate) {
            if(now-candidate_start_>.25 || evidence.on_board || evidence.physics!=504) {
                state_.candidate=false;state_.rising_samples=0;
            } else if(evidence.falling && state_.up_speed>.5f) {
                if(++state_.rising_samples>=2) { state_.active=true;state_.candidate=false; }
            } else state_.rising_samples=0;
        }
        physics_=evidence.physics;position_=evidence.position;time_=now;return state_;
    }
};
}
