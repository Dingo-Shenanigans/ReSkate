#pragma once
#include "Engine/Game/Input/controller_bindings.h"
#include "Upstream/src/mod.h"
namespace dingosdk::first_person_integration {
inline bool action_held(std::uint32_t binding,const ControllerInput& input) {
    if(!binding || !valid_action_binding(binding)) return false;
    if(!keyboard_binding(binding)) return input.available && (input.buttons&binding)==binding;
    for(unsigned slot=0;slot<5;++slot) {
        const auto key=(binding&0xc0000000u)==keyboard_binding_tag ? (slot ? 0u:binding&255u):keyboard_key_from_id((binding>>(slot*6))&63u);
        if(!key) break;
        if(!(input.keys[key/64]&(std::uint64_t{1}<<(key%64)))) return false;
    }
    return true;
}
struct NativeGlanceInput {
    jishsfirstpersonmod::Config config;
    first_person_vendor::FirstPersonLookControls controls;
    first_person_vendor::ControllerInput input;
};
inline NativeGlanceInput glance_input(jishsfirstpersonmod::Config config,const ControllerBindingsModel& bindings,
                                     const ControllerInput& input,bool ready) {
    NativeGlanceInput result;result.config=config;result.config.toggle=0;
    result.input={ready,0,input.device ? input.device:16u,static_cast<first_person_vendor::ControllerStyle>(input.style)};
    result.config.board.enabled=false; // Held chords are resolved by native input, not vendor button bits.
    for(unsigned i=0;i<4;++i) {
        const auto binding=bindings.available && config.glances_enabled ? bindings.action_combos[i+1]:0u;
        result.config.look[i].combo=binding;
        result.controls[i]={binding ? 1u<<i:0u,config.look[i].angle};
        if(action_held(binding,input)) result.input.buttons|=1u<<i;
    }
    if(!config.glances_enabled) result.config.remember_marker_glance=false;
    return result;
}
}
