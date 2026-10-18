#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"

namespace jishsfirstpersonmod {
// Smooth only the relative glance: stock has already integrated the camera pose.
class LookSmoothing {
    first_person_vendor::first_person::FrameSpring spring_{};
    double frame_{};
    bool ready_{};
    bool selection_ready_{},linked_{};
    first_person_vendor::first_person::Settings selected_{};
public:
    void reset() { spring_.reset(); ready_=false; selection_ready_=false; }
    first_person_vendor::first_person::Matrix apply(first_person_vendor::first_person::Matrix pose,
        first_person_vendor::first_person::Settings settings,
        first_person_vendor::first_person::LookAngles look,double now,bool linked=true) {
        using namespace first_person_vendor::first_person;
        constexpr Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        settings.offset={}; settings.rotation={};
        if(selection_ready_ && linked!=linked_ &&
           (look.pitch!=0 || look.yaw!=0 || length(rotation_vector(spring_.state.rotation))>0.0001f)) {
            // Changing linkage during a glance takes effect after returning to
            // neutral, so a differently enabled destination cannot snap it.
            settings=selected_;
        } else { linked_=linked; selected_=settings; selection_ready_=true; }
        if(!settings.enabled) {
            spring_.reset(); ready_=false;
            return look.pitch==0 && look.yaw==0 ? pose : target(pose,{},look);
        }
        if(!ready_) {
            // Seed neutral, including when activation's first frame has a held look.
            update(spring_.state,identity,settings,now);
            frame_=now; spring_.begin(now); ready_=true;
        } else if(now!=frame_) {
            // Pauses reset timing without snapping a new target into place.
            if(now<frame_ || now-frame_>0.25) spring_.state.time=now;
            frame_=now; spring_.begin(now);
        }
        const auto relative=spring_.sample(identity,settings,{},look);
        const auto rotation=orientation(relative);
        if(rotation==Quat{0,0,0,1}) return pose;
        write(pose,normalized(multiply(orientation(pose),rotation)),{pose[12],pose[13],pose[14]});
        return pose;
    }
};
}
