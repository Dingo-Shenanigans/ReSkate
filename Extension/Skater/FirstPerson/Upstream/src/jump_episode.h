#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/player_state_contract.h"
#include "Extension/Skater/FirstPerson/Upstream/src/hippy_jump.h"
#include <string_view>
namespace jishsfirstpersonmod {
enum class JumpStage { unknown,ground,trajectory,falling,simulated_ragdoll,board };
inline const char* jump_stage_name(JumpStage stage) {
    switch(stage) {
    case JumpStage::ground:return "ground";
    case JumpStage::trajectory:return "trajectory";
    case JumpStage::falling:return "falling";
    case JumpStage::simulated_ragdoll:return "simulated_ragdoll";
    case JumpStage::board:return "board";
    default:return "unknown";
    }
}
struct JumpEpisodeState {
    bool ordinary{};
    JumpStage stage=JumpStage::unknown;
    const char* type="none";
    const char* reset_reason="none";
};
struct JumpEpisodeEvidence {
    std::array<std::uintptr_t,4> owner{};
    bool valid{},bail{},position_valid{};
    JumpStage stage=JumpStage::unknown;
    HippyJumpState hippy;
    first_person_vendor::first_person::Vec3 position{};
};
class JumpEpisodeTracker {
    bool ready_{};
    std::array<std::uintptr_t,4> owner_{};
    JumpStage previous_=JumpStage::unknown;
    first_person_vendor::first_person::Vec3 position_{};
    double time_{};
    JumpEpisodeState state_{};
public:
    void reset() { *this={}; }
    JumpEpisodeState update(const JumpEpisodeEvidence& evidence,double now) {
        using namespace first_person_vendor::first_person;
        const auto stop=[&](const char* reason) { reset();state_.stage=evidence.stage;state_.reset_reason=reason;return state_; };
        if(!evidence.valid || !evidence.owner[0]) return stop("ownership_lost");
        if(evidence.bail) return stop("bail_recovery");
        if(!evidence.position_valid || !std::isfinite(now)) return stop("position_invalid");
        for(float v:evidence.position) if(!std::isfinite(v)) return stop("position_invalid");
        const char* discontinuity="none";
        if(ready_) {
            if(owner_!=evidence.owner) discontinuity="owner_changed";
            else if(now<time_ || now-time_>.25) discontinuity="observation_gap";
            else if(length(subtract(evidence.position,position_))>5) discontinuity="teleport";
        }
        if(std::string_view(discontinuity)!="none") { reset();state_.reset_reason=discontinuity; }
        // Ground/board/unknown states end an episode, including a standalone ragdoll
        // after a lifecycle reset. Only a validated trajectory entry or Falling arms it.
        state_.stage=evidence.stage;
        if(evidence.stage==JumpStage::ground || evidence.stage==JumpStage::board || evidence.stage==JumpStage::unknown) {
            state_.ordinary=false;state_.type="none";
            state_.reset_reason=evidence.stage==JumpStage::ground ? "grounded":evidence.stage==JumpStage::board ? "board":"unknown_child";
        } else if(std::string_view(discontinuity)!="none") {
            state_.ordinary=false;state_.type="none";
        } else if(evidence.hippy.candidate || evidence.hippy.active) {
            state_.ordinary=false;state_.type=evidence.hippy.active ? "hippy_jump":"hippy_candidate";
            state_.reset_reason="none";
        } else {
            if(std::string_view(state_.type)=="hippy_jump" || std::string_view(state_.type)=="hippy_candidate") state_.ordinary=false;
            if(evidence.stage==JumpStage::falling || (evidence.stage==JumpStage::trajectory && ready_ && previous_==JumpStage::ground))
                state_.ordinary=true;
            state_.type=state_.ordinary ? "ordinary_jump":"none";state_.reset_reason="none";
        }
        ready_=true;owner_=evidence.owner;previous_=evidence.stage;position_=evidence.position;time_=now;
        return state_;
    }
};
}
