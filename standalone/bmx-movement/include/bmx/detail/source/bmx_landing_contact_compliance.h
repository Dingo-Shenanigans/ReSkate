#pragma once

// Authored private experiment selection, not a new landing/support detector.
// Consume ground_pump's existing qualified automatic_armed pulse. No clock,
// support, load, spring, force or scoring state is synthesized by this latch.
namespace bmx::detail::source::landing_contact_compliance {
enum class Phase {disabled,waiting,latched};
inline const char* name(Phase phase) noexcept {
    switch(phase) {
    case Phase::disabled:return "disabled";
    case Phase::waiting:return "waiting";
    case Phase::latched:return "latched";
    }
    return "disabled";
}
struct Sample {
    bool selected{},verified_step{},enabled{},automatic_armed{},qualified_contact{},genuinely_unloaded{};
};
struct Result {
    Phase phase{Phase::disabled};
    bool active{},activated{},released{};
};
class Controller {
    bool latched_{};
public:
    void reset() noexcept {latched_=false;}
    Result step(Sample sample) noexcept {
        Result result;
        if(!sample.selected) {reset();return result;}
        result.phase=Phase::waiting;
        if(!sample.verified_step||!sample.enabled) {reset();return result;}
        if(latched_&&sample.genuinely_unloaded) {latched_=false;result.released=true;}
        if(!latched_&&sample.automatic_armed&&sample.qualified_contact&&!sample.genuinely_unloaded) {
            latched_=true;result.activated=true;
        }
        result.active=latched_;
        if(latched_)result.phase=Phase::latched;
        return result;
    }
};
} // namespace bmx::detail::source::landing_contact_compliance
