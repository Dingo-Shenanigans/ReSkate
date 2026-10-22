#include "Engine/Game/BMX/bmx_ground_pump.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace p=dingosdk::bmx::ground_pump;
namespace c=dingosdk::bmx::chassis_control;
namespace t=dingosdk::bmx::tire;
constexpr double h=c::verified_native_seconds;
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double a,double b,double e=1e-7){return std::abs(a-b)<=e;}
p::Sample sample(){p::Sample s;s.enabled=s.operator_available=s.supported=true;return s;}
void arm(p::Controller& state,p::Sample& s){s.input=0;check(state.step(h,s).valid,"neutral arms a valid fresh pump operator");}
void flight(p::Controller& state,p::Sample& s,unsigned count){s.supported=false;for(unsigned i=0;i<count;++i)check(!state.step(h,s).applied,"flight submits no pump");s.supported=true;}

void direct_window_and_rate() {
    for(double dt:{.01,h,1./120}) {
        p::Controller state;auto s=sample();state.step(dt,s);s.input=-1;double total=0;
        for(unsigned i=0;i<200;++i){const auto r=state.step(dt,s);check(r.valid,"direct samples stay finite");total+=r.down_acceleration*dt;}
        check(near(total,11*.45,1e-10),"per-second pump integral respects .45 seconds across rates");
        check(!state.step(dt,s).applied,"held input cannot restart an exhausted window");
        s.input=0;state.step(dt,s);s.input=-.4;check(near(state.step(dt,s).down_acceleration,4.4),"release rearms a new direct window");
    }
    p::Controller cold;auto s=sample();s.input=-1;
    check(cold.step(h,s).status==p::Status::waiting_release,"held mount input does not bootstrap a pump window");
    arm(cold,s);s.input=-.3;check(!cold.step(h,s).applied,"direct down threshold is strict");
    s.input=-.3001;check(cold.step(h,s).applied,"fresh down input below the threshold applies");
}
void landing_windows_and_gates() {
    p::Controller state;auto s=sample();arm(state,s);flight(state,s,12);
    auto r=state.step(h,s);check(r.automatic_armed&&r.applied&&near(r.down_acceleration,4.4),"qualifying landing arms -.4 automatic pump");
    double total=r.down_acceleration*h;
    for(unsigned i=0;i<60;++i){r=state.step(h,s);check(!r.automatic_armed,"one landing cannot rearm itself while supported");total+=r.down_acceleration*h;}
    check(near(total,4.4*.3,1e-10),"automatic time integral is bounded to .3 seconds");
    state.reset();s=sample();arm(state,s);flight(state,s,9);
    check(!state.step(h,s).automatic_armed,"air intervals below .15 seconds do not arm automatic pump");
    for(unsigned mode=0;mode<3;++mode){state.reset();s=sample();arm(state,s);flight(state,s,12);
        if(mode==0)s.manual=.051;else if(mode==1)s.nose_manual=.051;else s.inhibited=true;
        r=state.step(h,s);check(!r.applied&&!r.automatic_armed,"manual, nose and hop/preload gates suppress first-touch automatic pump");
        s.manual=s.nose_manual=0;s.inhibited=false;check(!state.step(h,s).applied,"failed first-touch gate cannot arm later while grounded");}
    state.reset();s=sample();arm(state,s);flight(state,s,12);s.input=-.8;
    r=state.step(h,s);check(r.status==p::Status::automatic&&near(r.down_acceleration,4.4),"held flight input cannot lease a direct request through landing");
    s.input=0;state.step(h,s);s.input=-.8;r=state.step(h,s);
    check(r.status==p::Status::combined&&near(r.down_acceleration,8.8),"direct plus automatic takes strongest command rather than stacking");
    s.manual=.1;check(!state.step(h,s).applied,"manual cancels both active pump windows");
    s.manual=0;check(!state.step(h,s).applied,"manual release cannot replay a held canceled request");
}
void invalid_disabled_and_upward() {
    p::Controller state;auto s=sample();arm(state,s);s.input=-1;
    check(state.step(h,s).applied,"direct window begins");
    s.inhibited=true;check(!state.step(h,s).applied,"hop/preload cancels the active direct pump");
    s.inhibited=false;check(!state.step(h,s).applied,"held input cannot replay after hop inhibition");
    arm(state,s);s.input=-1;s.world_upward_speed=.50001;
    for(unsigned i=0;i<40;++i)check(state.step(h,s).status==p::Status::upward,"upward speed rejects while consuming native window time");
    s.world_upward_speed=0;check(!state.step(h,s).applied,"upward rejection cannot save a force burst for later");
    for(unsigned kind=0;kind<3;++kind){arm(state,s);s.input=-1;check(state.step(h,s).applied,"fresh valid request applies before interruption");
        if(kind==0)s.enabled=false;else if(kind==1)s.operator_available=false;else s.input=std::numeric_limits<double>::quiet_NaN();
        const auto r=state.step(h,s);check(!r.applied&&(kind<2?r.valid:!r.valid),"disable, missing operator and invalid input fail closed");
        s.enabled=s.operator_available=true;s.input=-1;check(!state.step(h,s).applied,"interrupted operator requires a new release");}
    for(double dt:{0.,-.01,.2,std::numeric_limits<double>::quiet_NaN()})check(!state.step(dt,s).valid,"invalid pump clock resets rather than accumulating");
    state.reset();s=sample();s.supported=false;for(unsigned i=0;i<60;++i)state.step(h,s);s.supported=true;
    check(!state.step(h,s).automatic_armed,"cold unsupported startup is not a known takeoff");
    state.reset();s=sample();arm(state,s);flight(state,s,12);state.reset();
    check(!state.step(h,s).automatic_armed,"owner/pause reset discards landing history");
}
struct Fixture {
    t::Body body=c::proposed_body();std::array<t::Wheel,2>wheels=c::proposed_wheels();std::array<c::ContactSample,2>contacts;
    Fixture(){body.center_of_mass={0,.9,-.05};for(unsigned i=0;i<2;++i){contacts[i].contact.status=t::ContactStatus::hit;contacts[i].contact.point={0,0,i==0?.525:-.525};}}
};
c::Controls operation(){c::Controls s;s.pump_operator_available=true;return s;}
void physical_accounting() {
    Fixture f;c::Controller state,baseline;auto s=operation();
    const auto ordinary=baseline.step(h,f.body,f.wheels,f.contacts);
    check(state.step(h,f.body,f.wheels,f.contacts,s).valid,"initial neutral native pump step is valid");s.ground_pump=-1;
    const auto r=state.step(h,f.body,f.wheels,f.contacts,s);
    check(r.valid&&r.pump.applied&&r.pump_force==t::Vec{0,-f.body.mass*11,0},"pump is a finite world-down COM component");
    check(near(r.normal_load[0]+r.normal_load[1],f.body.mass*(9.8+11),.003),"current tire loads include pump in the SAME prediction step");
    check(near(r.force[1],ordinary.force[1],.003),"flat tires counter pump load without a one-frame downward command");
    check(near(-9.8*h+r.linear_impulse[1]/f.body.mass,0,1e-6),"flat net response remains stationary under native gravity");
    check(std::abs(r.torque[0])<.003&&std::abs(r.torque[2])<.003,"COM pump introduces no unmatched direct torque");
    t::Vec expected=r.pump_force;
    for(unsigned i=0;i<2;++i){t::Basis basis;check(t::contact_basis(f.contacts[i].contact.normal,f.contacts[i].contact.forward,basis),"fixture contact basis exists");
        for(unsigned a=0;a<3;++a)expected[a]+=(basis.normal[a]*r.normal_impulse[i]+basis.forward[a]*r.longitudinal_impulse[i]+basis.lateral[a]*r.lateral_impulse[i])/h;}
    for(unsigned a=0;a<3;++a)check(near(expected[a],r.force[a],1e-7),"base pump plus solved contact equals submitted net force componentwise");
    check(f.body.center_of_mass==t::Vec{0,.9,-.05}&&f.body.linear_velocity==t::Vec{},"preparation never changes caller COM/pose/velocity");
    c::Controller light;auto b=f.body;b.mass*=.5;for(auto& i:b.inertia_body)i*=.5;s.ground_pump=0;light.step(h,b,f.wheels,f.contacts,s);s.ground_pump=-1;
    const auto lighter=light.step(h,b,f.wheels,f.contacts,s);check(lighter.valid&&near(lighter.pump_force[1],r.pump_force[1]*.5),"equivalent acceleration uses observed mass");
    c::Controller pedaling;s=operation();pedaling.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;s.pedal=1;
    check(near(pedaling.step(h,f.body,f.wheels,f.contacts,s).pump.down_acceleration,8.25),"active acceleration input uses .75 pump scale");
}
void physical_rejections_and_reset() {
    Fixture f;c::Controller state;auto s=operation();state.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;
    auto opening=f;opening.body.linear_velocity[1]=.06;
    const auto continuation=state.step(h,opening.body,opening.wheels,opening.contacts,s);
    check(continuation.valid&&!continuation.grounded&&continuation.balance_continuation_mask==3&&!continuation.pump.applied,
        "balance continuation does not authorize pump support");
    state.reset();s.ground_pump=0;state.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;
    auto skin=f;for(auto& x:skin.contacts)x.separation=.02;
    const auto speculative=state.step(h,skin.body,skin.wheels,skin.contacts,s);check(speculative.valid&&!speculative.grounded&&!speculative.pump.applied,"speculative skin contacts cannot pump");
    state.reset();s.ground_pump=0;state.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;s.hop_strength=1;s.hop_event_id=1;
    const auto hop=state.step(h,f.body,f.wheels,f.contacts,s);check(hop.valid&&hop.hop_applied&&!hop.pump.applied&&hop.pump_force==t::Vec{},"pump never weakens, stacks onto or leases an actual hop");
    s.hop_strength=0;check(!state.step(h,f.body,f.wheels,f.contacts,s).pump.applied,"held input does not replay immediately after hop");
    for(unsigned kind=0;kind<4;++kind){state.reset();s=operation();state.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;
        auto bad=f;double dt=h;if(kind==0)dt=0;else if(kind==1)bad.contacts[0].contact.status=t::ContactStatus::unavailable;
        else if(kind==2)bad.body.mass=std::numeric_limits<double>::quiet_NaN();else s.ground_pump=std::numeric_limits<double>::quiet_NaN();
        const auto rejected=state.step(dt,bad.body,bad.wheels,bad.contacts,s);check(!rejected.valid&&rejected.pump_force==t::Vec{},"invalid clock/contact/body/input emits no pump component");
        s.ground_pump=-1;check(!state.step(h,f.body,f.wheels,f.contacts,s).pump.applied,"failed chassis solve clears pump history before next valid step");}
    c::Tuning tuning;tuning.force_limit=100;c::Controller limited(tuning);s=operation();limited.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;
    const auto capped=limited.step(h,f.body,f.wheels,f.contacts,s);check(!capped.valid&&capped.issue==c::Issue::output_limit&&capped.force==t::Vec{},"common force budget rejects an unaffordable base pump instead of clipping components");
    check(!limited.step(h,f.body,f.wheels,f.contacts,s).pump.applied,"force-budget rejection cannot preserve a held pump lease");
    state.reset();s=operation();state.step(h,f.body,f.wheels,f.contacts,s);s.ground_pump=-1;state.reset_pump();
    check(!state.step(h,f.body,f.wheels,f.contacts,s).pump.applied,"bail-specific reset cancels direct history");
    c::Controller untouched,not_selected;s={};s.ground_pump=-1;
    const auto a=untouched.step(h,f.body,f.wheels,f.contacts),b=not_selected.step(h,f.body,f.wheels,f.contacts,s);
    check(a.valid&&b.valid&&a.force==b.force&&a.torque==b.torque&&a.normal_impulse==b.normal_impulse&&!b.pump.applied,"callers without fresh raw-operator opt-in retain unchanged force output");
}
void physical_landing_and_provisional_clock() {
    Fixture f;c::Controller state;auto s=operation();state.step(h,f.body,f.wheels,f.contacts,s);
    auto air=f;for(auto& x:air.contacts)x.contact.status=t::ContactStatus::clear;
    for(unsigned i=0;i<12;++i)check(!state.step(h,air.body,air.wheels,air.contacts,s).pump.applied,"native force preparation applies no airborne pump");
    const auto landed=state.step(h,f.body,f.wheels,f.contacts,s);
    check(landed.valid&&landed.pump.automatic_armed&&near(landed.pump.down_acceleration,4.4),"physical landing consumes the qualified automatic command");
    check(near(landed.normal_load[0]+landed.normal_load[1],f.body.mass*(9.8+4.4),.003)&&near(landed.force[1],f.body.mass*9.8,.003),
        "automatic landing load is balanced by current tire solve rather than a following-frame correction");
    state.reset();s=operation();s.recovery_step_verified=true;
    for(auto& x:f.contacts){x.separation=-.02;x.verified_plane=x.balance_normal_available=true;x.balance_normal={0,1,0};}
    check(state.step(h,f.body,f.wheels,f.contacts,s).valid,"verified pair establishes recovery history");
    f.body.linear_velocity[1]=.18;for(auto& x:f.contacts)x.separation=-.018;
    const auto first=state.step(h,f.body,f.wheels,f.contacts,s);
    check(first.valid&&first.balance_continuation_mask==3&&!first.grounded,"provisional recovery uses balance-only admission");
    for(auto& x:f.contacts)x.separation=-.016;
    const auto second=state.step(h,f.body,f.wheels,f.contacts,s);
    check(second.valid&&second.balance_continuation_mask==3&&!second.grounded&&!second.pump.applied&&near(second.pump.air_seconds,h,1e-12),
        "baseline and assisted provisional solves commit pump clock exactly once");
}
}
int main(){direct_window_and_rate();landing_windows_and_gates();invalid_disabled_and_upward();physical_accounting();physical_rejections_and_reset();physical_landing_and_provisional_clock();std::cout<<"BMX ground pump "<<checks<<" checks passed.\n";}
