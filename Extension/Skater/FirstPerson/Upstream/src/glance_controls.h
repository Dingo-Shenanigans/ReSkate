#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Input/controller_bindings.h"
#include <algorithm>
namespace jishsfirstpersonmod {
class GlanceControlsState {
    first_person_vendor::ControllerLookState held_;
    first_person_vendor::FirstPersonLookControls controls_{};
    std::array<bool,4> previous_{};
    unsigned device_{};
    bool initialized_{},toggle_{},armed_{};
    int selected_=-1;
    std::uint32_t shared_modifier_{};
public:
    void reset() { *this={}; }
    int selection() const { return toggle_ ? selected_:-1; }
    std::array<float,2> selection_angles() const {
        if(selected_<0 || selected_>3) return {};
        const float angle=controls_[unsigned(selected_)].angle;
        if(selected_==0) return {angle,0};
        if(selected_==1) return {-angle,0};
        if(selected_==2) return {0,-angle};
        return {0,angle};
    }
    bool restore_selection(int selection) {
        if(!initialized_ || !toggle_ || selection<0 || selection>3 || !controls_[unsigned(selection)].combo) return false;
        selected_=selection;armed_=false;previous_={};return true;
    }
    std::array<float,2> update(const first_person_vendor::FirstPersonLookControls& controls,
        first_person_vendor::ControllerInput input,bool inhibited,bool toggle,std::uint32_t shared_modifier=0) noexcept {
        bool changed=!initialized_ || toggle!=toggle_ || input.device!=device_ || shared_modifier!=shared_modifier_;
        for(unsigned i=0;i<4;++i) changed=changed || controls[i].combo!=controls_[i].combo || controls[i].angle!=controls_[i].angle;
        if(changed) { reset();initialized_=true;controls_=controls;device_=input.device;toggle_=toggle;shared_modifier_=shared_modifier; }
        if(!toggle) {
            if(!inhibited && input.available && shared_modifier) for(unsigned i=0;i<4;++i) {
                const auto chord=controls[i].combo;
                const auto direction=chord&~shared_modifier;
                if((chord&shared_modifier)==shared_modifier && direction && !(input.buttons&direction)) {
                    auto neutral=input;neutral.buttons&=~shared_modifier;
                    held_.held[i].update(controls[i],neutral,false);
                }
            }
            return held_.update(controls,input,inhibited);
        }
        if(inhibited || !input.available) { selected_=-1;armed_=false;previous_={};return {}; }
        std::uint32_t bound{};for(const auto& binding:controls) bound|=binding.combo;
        if(!armed_) {
            armed_=(input.buttons&(bound&~shared_modifier))==0;previous_={};return selection_angles();
        }
        int pressed=-1;unsigned count{};
        for(unsigned i=0;i<4;++i) {
            const auto combo=controls[i].combo;
            const bool full=combo && first_person_vendor::valid_controller_combo(combo) && (input.buttons&combo)==combo;
            if(full && !previous_[i]) { pressed=int(i);++count; }
            previous_[i]=full;
        }
        if(count==1) selected_=selected_==pressed ? -1:pressed;
        // Simultaneous new directions are ambiguous; consume their edges without
        // replacing the currently selected direction. Partial-chord release rearms.
        if(selected_<0) return {};
        const float angle=controls[unsigned(selected_)].angle;
        if(selected_==0) return {angle,0};
        if(selected_==1) return {-angle,0};
        if(selected_==2) return {0,-angle};
        return {0,angle};
    }
};
}
