// Reuse the existing actual native-axis/hand anatomy fixtures verbatim.
#define main retained_rider_pose_test_main
#include "Extension/BMX/Test/bmx_rider_pose_tests.cpp"
#undef main
#include <bmx/pose_track.hpp>
#include <bit>
#include <iomanip>

namespace {
namespace observed=dingosdk::bmx::rider::observed;
unsigned checks{};
void expect(bool value,const char* text){++checks;check(value,text);}
Frame observed_frame(dingosdk::bmx::catalog::TrickId id) {
    Frame f;f.active=true;f.entity=0x1000;f.native_body_token=17;f.airborne=true;
    f.movement_id=id;f.movement_phase=.5f;f.movement_progress=.5f;f.movement_timing_available=true;
    const auto* entry=dingosdk::bmx::catalog::find(id);
    if(entry){f.release_left_hand=(entry->release_mask&1)?1.f:0.f;f.release_right_hand=(entry->release_mask&2)?1.f:0.f;
        f.release_left_foot=(entry->release_mask&4)?1.f:0.f;f.release_right_foot=(entry->release_mask&8)?1.f:0.f;}
    return f;
}
void raw_equal(const std::vector<p::Bone>& a,const std::vector<p::Bone>& b,const char* text){
    expect(a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(p::Bone))==0,text);
}
void catalog_and_phase_contract() {
    using namespace dingosdk::bmx;
    unsigned available=0,hold=0,recover=0;
    for(const auto& entry:catalog::entries){
        const auto id=static_cast<::bmx::TrickId>(static_cast<unsigned>(entry.id));
        const auto* target=::bmx::detail::source::catalog::find(id);
        expect(target&&entry.name==target->name&&entry.direction==target->direction&&entry.release_mask==target->release_mask,"IDs/names/directions/releases map explicitly");
        const auto* track=::bmx::find_pose_track(id);expect(track!=nullptr,"each accepted current catalog ID has an actual track");
        auto f=observed_frame(entry.id);const auto sample=observed::sample(f,.847004f,true);
        if(sample.available)++available;
        const auto again=observed::sample(f,.847004f,true);
        expect(sample.available==again.available&&sample.hip==again.hip&&sample.palms==again.palms,"duplicate render callbacks do not advance source time");
        f.movement_progress=1;f.movement_holding=true;
        const auto held=observed::sample(f,.847004f,true);
        if(held.available)++hold;
        if(!track->hold_available)expect(!held.available,"unobserved hold does not invent a loop");
        f.movement_holding=false;f.movement_recovering=true;f.movement_recovery_progress=.5f;
        if(observed::sample(f,.847004f,true).available)++recover;
    }
    expect(available>=40,"most catalog enter tracks are admitted by conservative source bounds");
    expect(hold>0,"genuinely observed hold tracks are usable");expect(recover>=40,"measured recovery paths are usable");
    std::cout<<"catalog tracks enter="<<available<<" observed_hold="<<hold<<" recovery="<<recover<<'\n';
}
void selection_and_rejection() {
    using namespace dingosdk::bmx;
    const auto good=observed_frame(catalog::TrickId::nothing);
    expect(observed::sample(good,.847004f,true).available,"Nothing measured posture can be selected");
    expect(!observed::sample(good,.847004f,false).available,"default-off selector never samples a track");
    for(unsigned i=0;i<13;++i){auto f=good;
        switch(i){case 0:f.active=false;break;case 1:f.entity=0;break;case 2:f.native_body_token=0;break;
        case 3:f.rider_mode=visual::RiderMode::carrying;break;case 4:f.mount_progress=.999f;break;
        case 5:f.rider_blend=.5f;break;case 6:f.movement_timing_available=false;break;case 7:f.movement_caught=true;break;
        case 8:f.movement_id=catalog::TrickId::none;break;case 9:f.movement_progress=std::numeric_limits<float>::quiet_NaN();break;
        case 10:f.movement_recovery_progress=1.1f;break;case 11:f.movement_holding=f.movement_recovering=true;break;
        case 12:f.movement_progress=0;break;}
        expect(!observed::sample(f,.847004f,true).available,"ineligible accepted context never supplies measured targets");
    }
    for(float size:{0.f,.1f,2.1f,std::numeric_limits<float>::infinity()})
        expect(!observed::sample(good,size,true).available,"unproved or extreme anatomy scale refuses sampling");
    auto initial=good;initial.movement_progress=0;
    expect(!observed::sample(initial,.847004f,true).available,"phase-zero starts retain exact neutral candidate");
    auto missing=good;missing.movement_timing_available=false;
    auto a=hands_fixture(),b=a;
    const auto ra=p::solve(a.bones,a.rig,missing,nullptr,false),rb=p::solve(b.bones,b.rig,missing,nullptr,true);
    expect(ra.ready==rb.ready,"missing accepted timing keeps legacy result");raw_equal(a.bones,b.bones,"missing timing preserves legacy pose bytes");
}
void native_rig_contacts_and_presentation() {
    using namespace dingosdk::bmx;
    unsigned applied=0,changed=0,closed=0;
    for(const auto id:{catalog::TrickId::nothing,catalog::TrickId::one_hander_left,catalog::TrickId::superman,catalog::TrickId::tuck_no,catalog::TrickId::cannon_ball})
    for(float progress:{.25f,.5f,.9f})for(float yaw:{0.f,1.3f}){
        auto f=observed_frame(id);f.movement_progress=progress;f.yaw=yaw;f.position={627,199,1062};
        auto before=hands_fixture();before.bones[1].position={627,199,1062,17};before.bones[1].rotation=p::axis(1,yaw);
        auto legacy=before,measured=before;
        const auto ra=p::solve(legacy.bones,legacy.rig,f,nullptr,false);
        const auto rb=p::solve(measured.bones,measured.rig,f,nullptr,true);
        if(rb.observed_applied&&rb.ready){
            ++applied;if(std::memcmp(legacy.bones.data(),measured.bones.data(),legacy.bones.size()*sizeof(p::Bone))!=0)++changed;
            expect(rb.hand_error<.003f&&rb.foot_error<.003f,"measured candidate must pass strict actual target errors");
            const auto g=p::compose(measured.bones,measured.rig),original=p::compose(before.bones,before.rig);
            for(const auto chain:{measured.rig.left_arm,measured.rig.right_arm,measured.rig.left_leg,measured.rig.right_leg})
                for(unsigned k=1;k<3;++k)
                    expect(std::abs(p::length(p::sub(g[static_cast<std::size_t>(chain[k])].position,g[static_cast<std::size_t>(chain[k-1])].position))-
                        p::length(p::sub(original[static_cast<std::size_t>(chain[k])].position,original[static_cast<std::size_t>(chain[k-1])].position)))<.0002f,"native segments retain length within world-float subtraction precision");
            for(bool left:{true,false})if(p::hand_release(f,left)==0){
                const auto& hand=g[static_cast<std::size_t>(left?measured.rig.left_arm[2]:measured.rig.right_arm[2])];
                expect(p::length(p::sub(p::add(hand.position,p::turn(hand.rotation,p::mul(p::palm_bar_offset(left),hand.scale))),p::grip(f,left)))<.003f,"closed hand remains on exact placed grip");++closed;
            }
            for(bool left:{true,false})if(p::foot_release(f,left)==0)
                expect(p::length(p::sub(g[static_cast<std::size_t>(left?measured.rig.left_leg[2]:measured.rig.right_leg[2])].position,p::pedal(f,left)))<.003f,"closed foot remains on actual crank pedal");
            for(std::size_t i=0;i<before.bones.size();++i){
                expect(before.bones[i].scale==measured.bones[i].scale,"all scale and padding lanes remain untouched");
                if(i!=static_cast<std::size_t>(before.rig.hips))expect(before.bones[i].position==measured.bones[i].position,"only existing hips local-position write is used");
            }
        }else if(rb.ready){expect(!rb.observed_applied&&rb.observed_strength==0,"refused observed candidate falls back without claiming a source pose");}
        (void)ra;
    }
    expect(applied>=4&&changed==applied,"actual native rig must visibly receive measured motion");
    expect(closed>0,"mixed-release tricks preserve a proved closed grip");
    std::cout<<"measured native rig candidates="<<applied<<" distinct="<<changed<<" closed_grips="<<closed<<'\n';
}
void endpoint_continuity() {
    using namespace dingosdk::bmx;
    float maximum=0;unsigned applied=0;
    for(const auto& entry:catalog::entries)for(bool recovering:{false,true}){
        const float epsilon=.000001f;
        auto f=observed_frame(entry.id);f.position={627,199,1062};
        f.movement_progress=recovering?1.f:epsilon;f.movement_recovering=recovering;
        f.movement_recovery_progress=recovering?1-epsilon:0;
        const auto smooth=[](float x){return x*x*(3-2*x);};
        const float release=recovering?1-smooth(f.movement_recovery_progress):std::min(1.f,f.movement_progress/.15f);
        f.movement_phase=entry.motion==catalog::Motion::pose?(recovering?1-smooth(f.movement_recovery_progress):smooth(f.movement_progress)):
            (recovering?0.f:std::sin(3.14159265f*f.movement_progress));
        f.release_left_hand=(entry.release_mask&1)?release:0;f.release_right_hand=(entry.release_mask&2)?release:0;
        f.release_left_foot=(entry.release_mask&4)?release:0;f.release_right_foot=(entry.release_mask&8)?release:0;
        auto a=hands_fixture();a.bones[1].position={627,199,1062,17};auto b=a;
        const auto old=p::solve(a.bones,a.rig,f,nullptr,false),now=p::solve(b.bones,b.rig,f,nullptr,true);
        expect(old.ready&&now.ready,"endpoint candidates preserve native admission");
        if(now.observed_applied)++applied;
        const auto before=p::compose(a.bones,a.rig),after=p::compose(b.bones,b.rig);
        for(std::size_t i=0;i<before.size();++i)maximum=std::max(maximum,p::length(p::sub(before[i].position,after[i].position)));
        expect(maximum<.001f,"enter and recovery influence approaches native baseline below1mm");
        if(recovering){f.movement_recovery_progress=1;f.movement_phase=0;
            f.release_left_hand=f.release_right_hand=f.release_left_foot=f.release_right_foot=0;
        }else {f.movement_progress=0;f.movement_phase=0;f.release_left_hand=f.release_right_hand=f.release_left_foot=f.release_right_foot=0;}
        a=hands_fixture();a.bones[1].position={627,199,1062,17};b=a;
        const auto endpoint_old=p::solve(a.bones,a.rig,f,nullptr,false),endpoint_new=p::solve(b.bones,b.rig,f,nullptr,true);
        expect(endpoint_old.ready==endpoint_new.ready&&!endpoint_new.observed_applied,"exact endpoints do not claim measured influence");
        raw_equal(a.bones,b.bones,"exact endpoints reproduce current native candidate bytes");
    }
    expect(applied==116,"all58 synthetic track endpoints exercise the measured envelope");
    std::cout<<"endpoint measured="<<applied<<" max_joint_difference_m="<<std::setprecision(9)<<maximum<<'\n';
}
void actual_accepted_phase_export() {
    using namespace dingosdk::bmx;
    constexpr double h=1.0/60;unsigned completed=0,legacy=0;
    for(const auto& entry:catalog::entries){
        TrickController controller;TrickInput in;in.enabled=true;in.owner=7;in.device=1;in.bar_neutral=in.rear_neutral=true;
        controller.update(h,in);in.grounded=false;expect(controller.update(h,in).took_off,"phase export begins from accepted takeoff");
        if(catalog::legacy_rotation(entry.id)){
            const auto direction=entry.direction<0?TrickDirection::left:TrickDirection::right;
            if(entry.family==catalog::Family::barspin){in.barspin=direction;in.bar_neutral=false;}
            else {in.tailwhip=direction;in.rear_neutral=false;}
            controller.update(h,in);
            expect(controller.state().movement_id==catalog::TrickId::none,"four legacy channels have no fabricated accepted catalog posture phase");++legacy;continue;
        }
        in.movement=entry.id;in.rear_neutral=false;controller.update(h,in);
        expect(controller.state().movement_id==entry.id,"accepted selection owns the posture ID");
        for(unsigned tick=0;tick<90&&controller.state().movement_progress<1;++tick)controller.update(h,in);
        expect(controller.state().movement_progress==1,"current controller reaches exact enter endpoint");
        in.movement=catalog::TrickId::none;in.rear_neutral=true;controller.update(h,in);
        expect(controller.state().movement_recovering&&controller.state().movement_recovery_progress==0,"accepted release begins recovery at zero");
        double previous=0;
        for(unsigned tick=1;tick<30&&!controller.state().movement_caught;++tick){
            controller.update(h,in);const auto& state=controller.state();
            if(state.movement_recovering){
                expect(state.movement_recovery_progress>=previous&&
                    std::abs(state.movement_recovery_progress-std::min(1.,tick*h/entry.recover_seconds))<1e-9,
                    "export is exact accepted linear recovery time, not an inverted easing or render clock");
                previous=state.movement_recovery_progress;
            }
        }
        expect(controller.state().movement_caught&&controller.state().movement_recovery_progress==0,"catch clears the presentation recovery export");++completed;
    }
    expect(completed==54&&legacy==4,"runtime route boundary is54catalog plus4legacy selections");
}
}
int main(){catalog_and_phase_contract();selection_and_rejection();native_rig_contacts_and_presentation();endpoint_continuity();actual_accepted_phase_export();
    expect(retained_rider_pose_test_main()==0,"existing rider fixture suite remains passing");
    std::cout<<checks<<" observed posture checks passed\n";return 0;}
