#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/spring_policy.h"
#include "Extension/Skater/FirstPerson/Upstream/src/stick_runtime.h"
#include "Extension/Skater/FirstPerson/Upstream/src/player_state_contract.h"
#include <string_view>
namespace jishsfirstpersonmod {
inline bool camera_handoff_direction_only(const Config& config) {
    // Both look modes retain the composed head position; turning the character
    // changes facing ownership, never positional attachment.
    return !config.look_only && config.stick.enabled;
}
inline bool bail_camera_locked(const ComfortSnapshot& state,const CameraMovementSample& movement) {
    return movement.bail_recovery || (state.state_valid && (state.bailing || state.recovery ||
        state.wipeout_request || state.animation_wipeout || state.physics==player_state_contract::wipeout));
}
inline bool ordinary_jump_action(const CameraMovementSample& movement) {
    // Default samples expose only Falling. Live recognized stages use
    // the tracker, so a lifecycle reset cannot be bypassed by the raw air flag.
    return movement.jump_episode.stage==JumpStage::unknown ? movement.jumping:movement.jump_episode.ordinary;
}
inline const char* camera_action(const CameraMovementSample& movement) {
    if(movement.hippy.active) return "hippy_jump";
    if(movement.hippy.candidate) return "hippy_candidate";
    if(movement.mounting) return "mounting";
    if(ordinary_jump_action(movement)) return "jumping";
    return "none";
}
inline bool jump_camera_action(const CameraMovementSample& movement) {
    return movement.mounting || ordinary_jump_action(movement) || movement.hippy.active || movement.hippy.candidate;
}
inline bool native_camera_locked(const Config&,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    return movement.view_hold || bail_camera_locked(state,movement);
}
inline bool camera_overrides_allowed(const ComfortSnapshot& state,const CameraMovementSample& movement) {
    return movement.mode && movement.non_bail && state.state_valid && state.animation_valid &&
        !bail_camera_locked(state,movement) && !state.suppressed;
}
inline bool camera_look_allowed(const Config& config,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    if(!config.look_only && !config.third_person_bailing && bail_camera_locked(state,movement))
        return movement.mode==1 && movement.recovery_verified && movement.recovery_controllable &&
            state.state_valid && state.animation_valid && !state.suppressed;
    return camera_overrides_allowed(state,movement) && !(config.stick.enabled && movement.mounting) &&
        !(movement.mode==1 && !movement.custom_mode);
}
inline bool character_facing_allowed(const ComfortSnapshot& state,const CameraMovementSample& movement) {
    return camera_overrides_allowed(state,movement) && movement.mode==1 && movement.custom_mode==1 &&
        !movement.jumping && !movement.mounting && !movement.hippy.active && !movement.hippy.candidate;
}
inline bool board_free_look_allowed(const Config& config,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    return config.board.enabled && movement.mode==2 && movement.custom_mode==2 &&
        !movement.body_flip_camera_owned && camera_look_allowed(config,state,movement);
}
inline const char* head_attachment_reason(const Config& config,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    if(config.look_only) return "compatibility";
    if(bail_camera_locked(state,movement)) return "bail";
    // An action's explicit switch wins over foot attachment. Spring suppression
    // still applies independently, so disabling attachment does not enable Spring.
    if(jump_camera_action(movement)) return "none";
    if(config.disable_spring_on_foot && movement.mode==1) return "spring_on_foot";
    return "none";
}
inline bool head_position_attached(const Config& config,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    const std::string_view reason=head_attachment_reason(config,state,movement);
    return reason!="none" && reason!="compatibility";
}
inline Config effective_camera_config(Config config,bool active,const ComfortSnapshot& state,const CameraMovementSample& movement) {
    if(config.look_only) return config;
    return camera_config_for_state(config,active,movement.mode,head_position_attached(config,state,movement));
}
}
