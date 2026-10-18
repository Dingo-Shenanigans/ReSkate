#include "Extension/Skater/FirstPerson/Upstream/src/camera_transition.h"
#include "Extension/Skater/FirstPerson/Upstream/src/camera_policy.h"
#undef near
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace first_person_vendor::first_person;
using namespace jishsfirstpersonmod;
void check(bool ok,const char* message) { if(!ok) { std::cerr<<message<<"\n";std::exit(1); } }
bool near(const Matrix& a,const Matrix& b) {
    for(unsigned i=0;i<16;++i) {
        if(i==3 || i==7 || i==11 || i==15) continue; // metadata belongs to destination
        if(std::abs(a[i]-b[i])>.00001f) return false;
    }
    return true;
}
int main() {
    Matrix base{};base[0]=base[5]=base[10]=base[15]=1;
    auto destination=target(base,{}, {0,90});destination[13]=1;
    Settings spring;spring.enabled=true;spring.up=spring.down=spring.left=spring.right=70;
    Settings handoff=spring,look=spring;
    const std::array<std::uintptr_t,4> ground{100,10,0,0},air{200,10,0,0},special{504,20,30,40};
    CameraTransition blend;
    auto sample=[&](Matrix pose,int mode,double time,auto state,bool stick=false) {
        return blend.apply(pose,handoff,1,mode,stick,time,state,&spring,&look);
    };
    check(sample(base,2,0,ground)==base,"initial pose was blended");
    check(near(sample(destination,2,.016,air),base),"riding state change snapped");
    const auto progressing=sample(destination,2,.032,air);
    check(!near(progressing,base) && !near(progressing,destination),"animation blend did not progress");
    check(near(sample(destination,2,.032,air),progressing),"same frame integrated twice");
    auto moved=destination;moved[12]+=.2f;moved[3]=.125f;
    const auto following=sample(moved,2,.032,air);
    check(std::abs(following[12]-progressing[12]-.2f)<.00001f && following[3]==.125f,"native motion or metadata changed");
    check(near(sample(base,1,.048,special),following),"special entry snapped");
    const auto entering=sample(base,1,.064,special);
    check(near(sample(destination,2,.080,ground),entering),"reversed handoff snapped");
    for(int i=6;i<300;++i) sample(destination,2,i*.016,ground);
    check(near(sample(destination,2,4.8,ground),destination),"blend failed to settle");
    blend.reset();sample(base,1,0,special);
    const std::array<std::uintptr_t,4> normal_foot{504,20,31,41};
    check(near(sample(destination,1,.016,normal_foot),base),"special exit within foot mode snapped");
    check(near(sample(base,1,.032,special),base),"interrupted special transition snapped");
    blend.reset();handoff.enabled=false;
    sample(base,2,0,ground);
    check(near(sample(destination,2,.016,air),base),"animation depended on foot/board switch");
    check(sample(base,1,.032,special)==base,"disabled foot/board smoothing blended");
    handoff.enabled=true;spring.enabled=false;blend.reset();sample(base,2,0,ground);
    check(sample(destination,2,.016,air)==destination,"disabled spring blended animation");
    spring.enabled=true;spring.up=spring.down=spring.left=spring.right=0;
    blend.reset();sample(base,2,0,ground);
    check(sample(destination,2,.016,air)==destination,"zero strength blended animation");
    spring=handoff;blend.reset();sample(base,2,0,ground);
    check(near(sample(destination,1,.016,special),base),"foot/board blend required active stick");
    check(sample(destination,0,.032,special)==destination,"bail/ineligible pose blended");
    check(sample(base,2,.048,ground)==base,"bail reused stale residual");
    for(int reset_kind=0;reset_kind<5;++reset_kind) {
        blend.reset();sample(base,2,1,ground);sample(destination,2,1.016,air);
        auto pose=base;double time=1.032;std::uintptr_t owner=1;
        if(reset_kind==0) owner=2;
        if(reset_kind==1) time=.5;
        if(reset_kind==2) time=2;
        if(reset_kind==3) pose[12]=10;
        if(reset_kind==4) time=std::numeric_limits<double>::quiet_NaN();
        check(blend.apply(pose,handoff,owner,2,false,time,ground,&spring,&look)==pose,"lifecycle reset blended");
    }
    auto at=[&](float strength) {
        CameraTransition test;auto cfg=spring;cfg.up=cfg.down=cfg.left=cfg.right=strength;
        test.apply(base,handoff,1,2,false,0,ground,&cfg);
        test.apply(destination,handoff,1,2,false,.016,air,&cfg);
        return test.apply(destination,handoff,1,2,false,.116,air,&cfg)[13];
    };
    check(at(70)<at(35),"higher strength did not soften animation transition");
    blend.reset();sample(base,1,0,special);look.enabled=false;
    check(sample(destination,1,.016,special,true)==destination,"stick transition ignored look control");
    blend.reset();
    auto left=target(base,{}, {0,179}),right=target(base,{}, {0,-179});
    sample(left,2,0,ground);sample(right,2,.016,air);
    const auto wrapped=sample(right,2,.116,air);
    check(length(rotation_vector(multiply(conjugate(orientation(left)),orientation(wrapped))))<.04f,
        "rotation took long path across yaw wrap");
    blend.reset();sample(base,2,0,ground);
    check(sample(destination,2,.016,ground)==destination,"ordinary native movement was smoothed twice");
    // Final attached poses include their glance and pitch, even when custom look is suspended.
    spring=handoff;look=handoff;blend.reset();
    auto locked=target(base,{}, {25,70});locked[12]=.3f;locked[13]=2;
    auto resumed=target(base,{}, {-15,-40});resumed[12]=.7f;resumed[13]=2.5f;resumed[3]=.125f;
    auto action=[&](Matrix pose,int mode,double time,auto state,bool attached,bool eligible,bool hold,bool stick=false) {
        return blend.apply(pose,handoff,1,mode,stick,time,state,&spring,&look,attached,eligible,hold);
    };
    action(base,1,0,normal_foot,true,true,false,true);
    check(action(locked,1,.016,special,true,false,true)==locked,"action attachment did not own its native pose");
    const auto first=action(resumed,1,.032,normal_foot,false,true,false);
    check(near(first,locked) && first[3]==.125f,"attached exit lost last displayed pose or metadata");
    const auto second=action(resumed,1,.048,normal_foot,false,true,false);
    check(!near(second,locked) && !near(second,resumed),"attached exit did not decay toward native pose");
    check(near(action(resumed,1,.048,normal_foot,false,true,false),second),"attached exit integrated twice");
    // A chained reverse lock replaces the residual rather than adding another layer.
    auto chained=target(locked,{}, {-20,20});chained[13]=3;
    action(chained,1,.064,special,true,false,true);
    check(near(action(resumed,1,.080,normal_foot,false,true,false),chained),"chained action reused an old residual");
    blend.reset();action(locked,1,0,special,true,false,true);
    const auto attached_exit=action(resumed,1,.016,normal_foot,true,true,false);
    check(length(rotation_vector(multiply(conjugate(orientation(locked)),orientation(attached_exit))))<.00001f &&
        attached_exit[12]==resumed[12] && attached_exit[13]==resumed[13],
        "landing with attachment active lost displayed direction or smoothed head position");
    blend.reset();action(locked,1,0,normal_foot,true,true,false);
    check(near(action(resumed,1,.016,normal_foot,false,true,false),locked),"same-state attachment change snapped");
    blend.reset();action(locked,1,0,normal_foot,true,false,true);
    check(near(action(resumed,1,.016,normal_foot,true,true,false),locked)==false,
        "attached position was incorrectly retained instead of following current head");
    // Mounting may change foot/board mode during the lock; its independent strengths still govern release.
    blend.reset();action(locked,1,0,normal_foot,true,true,false);
    action(locked,2,.016,ground,true,false,true);
    handoff.enabled=false;
    check(action(resumed,2,.032,ground,false,true,false)==resumed,"mount lock forgot disabled foot/board smoothing");
    handoff.enabled=true;spring.enabled=false;
    blend.reset();action(locked,1,0,special,true,false,true);
    check(action(resumed,1,.016,normal_foot,false,true,false)==resumed,"disabled camera Spring blended action exit");
    spring.enabled=true;spring.up=spring.down=spring.left=spring.right=0;
    blend.reset();action(locked,1,0,special,true,false,true);
    check(action(resumed,1,.016,normal_foot,false,true,false)==resumed,"zero-strength action exit was not immediate");
    // Mount starts before the board mode changes: preserve the displayed stick view.
    spring=handoff;spring.enabled=true;look=spring;blend.reset();
    auto mount=[&](Matrix pose,int mode,double time,bool mounting,bool eligible) {
        return blend.apply(pose,handoff,1,mode,!mounting && mode==1,time,
            mode==1 ? normal_foot:ground,&spring,&look,true,eligible,!eligible,mounting);
    };
    mount(base,1,0,false,true);
    check(near(mount(locked,1,.016,true,false),base),"mount entry snapped from stick view");
    const auto mount_progress=mount(locked,1,.032,true,false);
    check(!near(mount_progress,base) && !near(mount_progress,locked),"locked mount did not blend position and direction");
    check(near(mount(locked,2,.032,true,false),mount_progress),"board mode restarted mount blend");
    check(near(mount(locked,2,.032,false,true),mount_progress),"mount release restarted blend");
    auto moving_head=locked;moving_head[12]+=.2f;
    const auto moving_mount=mount(moving_head,2,.032,false,true);
    check(std::abs(moving_mount[12]-mount_progress[12]-.2f)<.00001f,"mount failed to follow moving target");
    for(int i=3;i<400;++i) mount(moving_head,2,i*.016,false,true);
    check(near(mount(moving_head,2,6.4,false,true),moving_head),"mount failed to settle at head");
    blend.reset();mount(base,1,0,false,true);mount(locked,1,.016,true,false);
    const auto before_reverse=mount(locked,2,.032,true,false);
    const auto reversed=blend.apply(resumed,handoff,1,1,true,.048,normal_foot,&spring,&look,false,true,false,false);
    check(near(reversed,before_reverse),"rapid dismount snapped or reused stale mount residual");
    blend.reset();handoff.enabled=false;mount(base,1,0,false,true);
    check(mount(locked,1,.016,true,false)==locked,"disabled mount smoothing blended");
    handoff.enabled=true;blend.reset();mount(base,1,0,false,true);mount(locked,1,.016,true,false);
    check(blend.apply(resumed,handoff,2,2,false,.032,ground,&spring,&look,true,false,true,true)==resumed,
        "mount retained another owner's camera");
    blend.reset();mount(base,1,0,false,true);mount(locked,1,.016,true,false);
    check(blend.apply(resumed,handoff,1,1,false,.032,special,&spring,&look,true,false,true,false)==resumed,
        "another locked action retained mount blend");
    blend.reset();mount(base,1,0,false,true);mount(locked,1,.016,true,false);
    for(int i=2;i<400;++i) mount(locked,2,i*.016,true,false);
    check(near(mount(locked,2,6.4,false,true),locked),"settled mount restarted at release");
    auto ordinary=locked;ordinary[12]+=.1f;
    check(near(mount(ordinary,2,6.416,false,true),ordinary),"ordinary movement smoothed after mounting");
    // Dismount can rearm character lock a frame after the mode changes.
    // Native state and stick changes must not replace its handoff strength.
    auto dismount_strength=[&](float strength,bool delayed_stick) {
        CameraTransition test;auto cfg=handoff;cfg.enabled=true;
        cfg.up=cfg.down=cfg.left=cfg.right=strength;
        Settings disabled;disabled.enabled=false;
        test.apply(base,cfg,1,2,false,0,ground,&disabled,&disabled);
        test.apply(locked,cfg,1,1,!delayed_stick,.016,special,&disabled,&disabled,true);
        const auto next=test.apply(locked,cfg,1,1,true,.032,normal_foot,&disabled,&disabled,true);
        check(!near(next,locked),"dismount blend replaced by disabled animation/look smoothing");
        return test.apply(locked,cfg,1,1,true,.132,normal_foot,&disabled,&disabled,true);
    };
    const auto slow_dismount=dismount_strength(90,true);
    const auto fast_dismount=dismount_strength(10,true);
    const auto remaining=[&](const Matrix& pose) {
        return length(rotation_vector(multiply(conjugate(orientation(locked)),orientation(pose))));
    };
    check(remaining(slow_dismount)>remaining(fast_dismount),"character-lock dismount ignored handoff strength");
    check(near(slow_dismount,dismount_strength(90,false)),"delayed stick activation restarted dismount blend");
    CameraTransition rearming;Settings transition_settings=handoff;transition_settings.enabled=true;
    Settings no_look;no_look.enabled=false;
    rearming.apply(base,transition_settings,1,2,false,0,ground,&no_look,&no_look);
    const auto before_rearm=rearming.apply(locked,transition_settings,1,1,false,.016,special,&no_look,&no_look);
    check(near(rearming.apply(resumed,transition_settings,1,1,true,.016,normal_foot,&no_look,&no_look),before_rearm),
        "new character-lock target snapped during dismount smoothing");
    // Both look modes must keep the composed head position. Turning the
    // character changes facing ownership, never positional attachment.
    // Camera smoothing must not add a positional residual to the
    // already configured camera/head target, including during mount locks.
    for(const bool turn_character:{false,true}) for(const float strength:{10.f,90.f}) {
        CameraTransition head_locked;Settings cfg=handoff;cfg.enabled=true;
        Config camera;camera.stick.enabled=true;camera.stick.turn_character=turn_character;
        cfg.up=cfg.down=cfg.left=cfg.right=strength;
        auto apply_head=[&](Matrix pose,int mode,double time,bool mounting,bool stick,auto state) {
            const auto result=head_locked.apply(pose,cfg,1,mode,stick,time,state,&spring,&look,
                mounting,true,false,mounting,camera_handoff_direction_only(camera));
            for(unsigned axis=12;axis<=14;++axis)
                check(std::abs(result[axis]-pose[axis])<.00001f,"look-mode smoothing displaced configured head position");
            return result;
        };
        apply_head(base,1,0,false,true,normal_foot);
        auto pose=locked;
        const auto entry=apply_head(pose,1,.016,true,false,special);
        check(length(rotation_vector(multiply(conjugate(orientation(base)),orientation(entry))))<.00001f,
            "head position fix removed directional mount smoothing");
        for(int i=2;i<20;++i) {
            pose[12]+=.1f;pose[13]+=.02f;
            apply_head(pose,2,i*.016,i<5,false,ground);
        }
        pose=resumed;
        apply_head(pose,1,.320,false,false,special);
        apply_head(pose,1,.336,false,true,normal_foot);
        for(int i=22;i<50;++i) {pose[14]+=.1f;apply_head(pose,1,i*.016,false,true,normal_foot);}
    }
    std::cout<<"Non-bail camera state transitions and reset policy passed\n";
}
