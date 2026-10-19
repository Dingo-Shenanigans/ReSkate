#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"
#include <cstdint>
namespace jishsfirstpersonmod {
// Decay a handoff residual, rather than integrating ordinary native movement again.
class CameraTransition {
    using Matrix=first_person_vendor::first_person::Matrix;
    using Vec3=first_person_vendor::first_person::Vec3;
    Matrix last_{};
    Vec3 position_{},velocity_{},angle_{},angular_velocity_{};
    std::uintptr_t owner_{};
    int mode_{};
    std::array<std::uintptr_t,4> state_{};
    int blend_kind_{}; // 0: foot/board, 1: animation, 2: stick
    bool ready_{},blending_{},stick_{},attached_{},eligible_=true,held_{},foot_handoff_{};
    double time_{};
    bool mounting_{},mount_blend_{},foot_blend_{};
public:
    void reset() { *this={}; }
    Matrix apply(Matrix target,const first_person_vendor::first_person::Settings& settings,
                 std::uintptr_t owner,int mode,bool stick,double now,
                 std::array<std::uintptr_t,4> state={},
                 const first_person_vendor::first_person::Settings* animation=nullptr,
                 const first_person_vendor::first_person::Settings* look=nullptr,
                 bool attached=false,bool eligible=true,bool hold=false,bool mounting=false,bool direction_only=false) {
        using namespace first_person_vendor::first_person;
        if(!owner || !mode || !std::isfinite(now)) { reset();return target; }
        if(!ready_ || owner_!=owner || now<time_ || now-time_>.25 ||
           length(subtract({target[12],target[13],target[14]}, {last_[12],last_[13],last_[14]}))>5) {
            reset();ready_=true;owner_=owner;mode_=mode;stick_=stick;state_=state;attached_=attached;eligible_=eligible;held_=hold;mounting_=mounting;time_=now;last_=target;return target;
        }
        const bool mount_started=mounting && !mounting_;
        const bool dismounted=mode_==2 && mode==1;
        mounting_=mounting;
        if(dismounted || (hold && !mounting)) mount_blend_=false;
        if(hold && !mounting) {
            // A direct native action still owns its displayed-pose history.
            foot_handoff_=foot_handoff_ || mode_!=mode;
            foot_blend_=false;blending_=false;mode_=mode;stick_=stick;state_=state;
            attached_=attached;eligible_=eligible;held_=true;time_=now;last_=target;return target;
        }
        const bool animation_changed=state_!=state || attached_!=attached || held_ || eligible_!=eligible;
        const bool foot_changed=mode_!=mode || foot_handoff_;
        const bool changed=mount_started || (!mount_blend_ &&
            (foot_changed || (!foot_blend_ && (animation_changed || stick_!=stick))));
        if(changed) blend_kind_=(mount_started || mode_!=mode || foot_handoff_) ? 0:animation_changed ? 1:2;
        if(changed) foot_blend_=blend_kind_==0;
        if(mount_started) mount_blend_=true;
        attached_=attached;eligible_=eligible;held_=false;foot_handoff_=false;
        const auto& blend=blend_kind_==1 && animation ? *animation:
            blend_kind_==2 && look ? *look:settings;
        if(!blend.enabled) {
            mount_blend_=false;
            foot_blend_=false;blending_=false;mode_=mode;stick_=stick;state_=state;attached_=attached;eligible_=eligible;held_=hold;time_=now;last_=target;return target;
        }
        const auto inverse=conjugate(orientation(target));
        const bool rebase_handoff=!changed && foot_blend_ && (animation_changed || stick_!=stick);
        if(changed || rebase_handoff) {
            position_=rotate(inverse,subtract({last_[12],last_[13],last_[14]}, {target[12],target[13],target[14]}));
            angle_=rotation_vector(multiply(inverse,orientation(last_)));
            if(changed) { velocity_={};angular_velocity_={};time_=now; }
            blending_=true;mode_=mode;stick_=stick;state_=state;
        } else { mode_=mode;stick_=stick;state_=state; }
        // Mount entry carries position to the head; other attachments own it immediately.
        if(direction_only || (attached && !mount_blend_)) { position_={};velocity_={}; }
        const float dt=static_cast<float>(now-time_);time_=now;
        if(blending_) {
            const float average=(blend.up+blend.down+blend.left+blend.right)/4;
            damp(position_[0],velocity_[0],position_[0]>0 ? blend.left:blend.right,dt);
            damp(position_[1],velocity_[1],position_[1]>0 ? blend.down:blend.up,dt);
            damp(position_[2],velocity_[2],average,dt);
            damp(angle_[0],angular_velocity_[0],angle_[0]>0 ? blend.down:blend.up,dt);
            damp(angle_[1],angular_velocity_[1],angle_[1]>0 ? blend.right:blend.left,dt);
            damp(angle_[2],angular_velocity_[2],average,dt);
            if(length(position_)<.001f && length(angle_)<.1f*std::numbers::pi_v<float>/180) blending_=false;
            else {
                const auto rotation=orientation(target);
                write(target,normalized(multiply(rotation,from_rotation_vector(angle_))),
                      add({target[12],target[13],target[14]},rotate(rotation,position_)));
            }
        }
        // Keep the mount episode stable even if it settles before the lock releases.
        if(!blending_) foot_blend_=false;
        if(!blending_ && !mounting) mount_blend_=false;
        last_=target;return target;
    }
};
}
