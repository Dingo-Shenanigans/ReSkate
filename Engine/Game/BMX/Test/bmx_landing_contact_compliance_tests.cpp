#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace c=dingosdk::bmx::chassis_control;
namespace p=dingosdk::bmx::landing_contact_compliance;
namespace t=dingosdk::bmx::tire;
unsigned checks{},solves{};
void check(bool value,const char* why){++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
void latch_policy(){
    p::Controller latch;p::Sample s{true,true,true,false,true,false};
    for(unsigned i=0;i<600;++i)check(!latch.step(s).active,"ordinary loaded riding cannot acquire landing exposure");
    s.automatic_armed=true;s.qualified_contact=false;check(!latch.step(s).active,"air/edge/unqualified pulse cannot be saved for later");
    s.automatic_armed=false;s.qualified_contact=true;check(!latch.step(s).active,"old rejected pulse is not deferred");
    s.automatic_armed=true;auto r=latch.step(s);check(r.active&&r.activated&&!r.released,"existing current qualified landing event activates once");
    check(!latch.step(s).activated,"repeated/provisional same pulse cannot activate twice in selected state");
    s.automatic_armed=false;s.qualified_contact=false;
    for(unsigned i=0;i<600;++i)check(latch.step(s).active,"loaded support blink or elapsed time cannot abruptly switch constitutive law");
    s.genuinely_unloaded=true;r=latch.step(s);check(!r.active&&r.released,"actual full unload releases exposure");
    s.genuinely_unloaded=false;s.qualified_contact=true;check(!latch.step(s).active,"release alone does not rearm without a new landing pulse");
    s.automatic_armed=true;check(latch.step(s).activated,"new qualified flight/landing event can rearm");
    for(unsigned invalid=0;invalid<3;++invalid){
        p::Controller next=latch;auto bad=s;
        if(invalid==0)bad.selected=false;
        if(invalid==1)bad.enabled=false;
        if(invalid==2)bad.verified_step=false;
        check(!next.step(bad).active,"flag/input/freshness loss clears exposure");
        bad=s;bad.automatic_armed=false;check(!next.step(bad).active,"invalidation never restores a previous pulse");
    }
    latch.reset();s.automatic_armed=false;check(!latch.step(s).active,"explicit owner/pause reset clears the latch");
    check(sizeof(p::Controller)<=sizeof(bool),"policy state remains a single bounded bit");
}
struct Fixture {
    c::Tuning tuning;t::Body body=c::proposed_body();std::array<t::Wheel,2>wheels=c::proposed_wheels();
    std::array<c::ContactSample,2> contacts{};c::Controls controls;
    Fixture(){
        tuning.balance_stiffness=tuning.balance_damping=0;tuning.contact_gap=.03;
        body.center_of_mass={0,.9,0};body.linear_velocity={0,-.2,0};
        controls.enabled=controls.pump_operator_available=controls.recovery_step_verified=true;
        controls.contact_compliance_available=controls.landing_contact_compliance_selected=true;
        for(unsigned i=0;i<2;++i){auto& s=contacts[i];s.contact.status=t::ContactStatus::hit;s.verified_plane=true;
            s.contact.point={0,0,i==0?.4:-.4};s.contact.normal={0,1,0};s.contact.forward={0,0,1};}
    }
    c::Result step(c::Controller& controller){++solves;return controller.step(c::verified_native_seconds,body,wheels,contacts,controls);}
    void air(){for(auto& s:contacts){s.contact.status=t::ContactStatus::clear;s.separation=.1;}body.linear_velocity={0,-.2,0};}
    void ground(){for(auto& s:contacts){s.contact.status=t::ContactStatus::hit;s.separation=.0003;}body.linear_velocity={0,-2.6,0};}
    void seed_flight(c::Controller& controller,unsigned frames=13){
        ground();auto r=step(controller);check(r.valid&&!r.landing_compliance.active&&r.compliant_normal_mask==0,"initial ground is exact original model");
        air();for(unsigned i=0;i<frames;++i){r=step(controller);check(r.valid&&!r.landing_compliance.active&&r.compliant_normal_mask==0,"flight has no compliant contact or early event");}
        ground();
    }
};
void controller_lifecycle(){
    Fixture f;c::Controller controller(f.tuning);f.seed_flight(controller);
    auto r=f.step(controller);check(r.valid&&r.pump.automatic_armed&&r.landing_compliance.activated&&r.landing_compliance.active&&r.compliant_normal_mask==3,"main landing event admits existing eligible rows without a separate support detector");
    for(auto& s:f.contacts)s.separation=.002;
    f.body.linear_velocity={0,1,0};
    r=f.step(controller);check(r.valid&&!r.grounded&&r.landing_compliance.active&&r.compliant_normal_mask==3,"opening force contacts keep model through loss of actual support");
    f.air();r=f.step(controller);check(r.valid&&!r.landing_compliance.active&&r.landing_compliance.released&&r.compliant_normal_mask==0,"both force-clear wheels outside domain discharge while law contributes no rows");
    f.ground();r=f.step(controller);check(r.valid&&!r.landing_compliance.active&&!r.pump.automatic_armed,"short gap does not manufacture another qualified landing");
    f.air();for(unsigned i=0;i<13;++i)check(f.step(controller).valid,"second observed flight valid");
    f.ground();r=f.step(controller);check(r.valid&&r.landing_compliance.activated,"second genuine event reactivates");
    controller.reset();r=f.step(controller);check(r.valid&&!r.landing_compliance.active&&r.compliant_normal_mask==0,"owner/pause reset begins original contact law");
    f.seed_flight(controller);check(f.step(controller).landing_compliance.active,"fresh event after owner reset");
    controller.reset_pump();r=f.step(controller);check(r.valid&&!r.landing_compliance.active,"bail no-command pump reset clears exposure");
    for(unsigned invalid=0;invalid<4;++invalid){
        c::Controller next(f.tuning);f.seed_flight(next);check(f.step(next).landing_compliance.active,"invalidation fixture activated");
        auto controls=f.controls;auto body=f.body;double h=c::verified_native_seconds;
        if(invalid==0)h=.01;
        if(invalid==1)body.mass=std::numeric_limits<double>::quiet_NaN();
        if(invalid==2)controls.enabled=false;
        if(invalid==3){controls.contact_compliance_available=false;controls.landing_contact_compliance_selected=false;}
        ++solves;auto rejected=next.step(h,body,f.wheels,f.contacts,controls);
        if(invalid<2)check(!rejected.valid,"invalid step/body rejected");
        else check(rejected.compliant_normal_mask==0,"input/flag invalidation emits no soft row");
        auto after=f.step(next);check(after.valid&&!after.landing_compliance.active&&after.compliant_normal_mask==0,"invalidated history does not lease an old event");
    }
}
void fallback_transaction(){
    Fixture f;f.tuning.force_limit=1000;c::Controller selected(f.tuning),original(f.tuning);
    f.seed_flight(selected);auto old_controls=f.controls;f.controls.contact_compliance_available=false;f.seed_flight(original);f.controls=old_controls;
    const auto requested=f.step(selected);f.controls.contact_compliance_available=false;const auto rigid=f.step(original);
    check(requested.valid&&requested.compliance_budget_fallback&&requested.compliant_normal_mask==0&&requested.compliance_attempted_mask==3,"low-cap landing attempts soft then retains original bounded response");
    check(requested.landing_compliance.active&&requested.landing_compliance.activated,"rigid fallback commits the same landing event once without discarding latch");
    check(requested.force==rigid.force&&requested.torque==rigid.torque&&requested.normal_load==rigid.normal_load&&requested.wheels[0].angular_speed==rigid.wheels[0].angular_speed,"fallback original physics remains exact");
    for(unsigned i=0;i<20;++i){
        f.controls.ground_pump=i<10?-.7:0;
        const auto a=f.step(selected),b=f.step(original);
        check(a.valid&&b.valid&&a.force==b.force&&a.torque==b.torque&&a.pump.automatic_remaining==b.pump.automatic_remaining&&a.pump.direct_seconds==b.pump.direct_seconds,"discarded attempt cannot advance pump/constitutive history twice");
        check(a.landing_compliance.active&&!a.landing_compliance.activated,"fallback latch retained while subsequent physical model explicitly disabled");
    }
}
void all_ground_preserved(){
    Fixture f;f.controls.landing_contact_compliance_selected=false;c::Controller soft(f.tuning);
    const auto r=f.step(soft);check(r.valid&&r.compliant_normal_mask==3&&!r.landing_compliance.active,"existing all-ground opt-in unchanged without separate landing selector");
    f.controls.contact_compliance_available=false;c::Controller original(f.tuning);auto old=f.step(original);
    check(old.valid&&old.compliant_normal_mask==0&&old.landing_compliance.phase==p::Phase::disabled,"ordinary default stays original rigid");
}
int main(){latch_policy();controller_lifecycle();fallback_transaction();all_ground_preserved();
    std::cout<<"PASS "<<checks<<" assertions / "<<solves<<" controller calls; authored policy/pure fixtures only, no native validation\n";}
