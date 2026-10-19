#pragma once
#include "controls_context.h"
#include "Upstream/src/mod.h"
#include "Upstream/src/glance_controls.h"
#include "Upstream/src/marker_glance.h"
namespace dingosdk::first_person_integration {
// Teleport observations outlive a temporary native pose disarm, never a menu,
// disabled preference, changed bindings, world/device change, or bail.
class GlanceRuntime {
    jishsfirstpersonmod::GlanceControlsState glance_;
    jishsfirstpersonmod::MarkerGlanceMemory memory_;
    jishsfirstpersonmod::Config previous_;
    int last_=-1;
    bool initialized_{};
    unsigned device_{};std::uintptr_t world_{};
public:
    void disarm_pose() {glance_.reset();}
    std::array<float,2> step(const jishsfirstpersonmod::Config& config,
        const first_person_vendor::FirstPersonLookControls& controls,
        first_person_vendor::ControllerInput input,bool blocked,bool requested,
        const ControlsContext& context) {
        bool changed=!initialized_ || previous_.toggle_glances!=config.toggle_glances ||
            previous_.remember_marker_glance!=config.remember_marker_glance ||
            previous_.toggle!=config.toggle || previous_.look_only!=config.look_only;
        for(unsigned i=0;i<4;++i) changed=changed || previous_.look[i].combo!=config.look[i].combo ||
            previous_.look[i].angle!=config.look[i].angle;
        if(changed) {memory_.clear();last_=-1;glance_.reset();}
        previous_=config;initialized_=true;
        const bool remember=config.remember_marker_glance && config.toggle_glances && requested &&
            context.telemetry_valid && input.available && (context.teleport || (context.verified && !context.bail));
        if(!remember || device_!=input.device || world_!=context.world_context) last_=-1;
        device_=input.device;world_=context.world_context;
        memory_.observe(remember,context.teleport,last_,input.device,context.world_context,double(context.tick)*.001);
        if(context.teleport) last_=-1; // An expired/interrupted episode must not re-seed itself.
        auto angles=glance_.update(controls,input,blocked,config.toggle_glances,
            !context.on_foot && config.board.enabled ? config.board.modifier:0);
        const int restored=memory_.take(!blocked && !context.teleport);
        if(restored>=0 && glance_.restore_selection(restored)) angles=glance_.selection_angles();
        if(!context.teleport) last_=remember ? glance_.selection():-1;
        return angles;
    }
};
}
