#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace c=dingosdk::bmx::chassis_control;
namespace t=dingosdk::bmx::tire;
namespace k=dingosdk::bmx::contact_compliance;
using V=t::Vec;constexpr double h=c::verified_native_seconds;
unsigned assertions{},solves{},backstops{};
void check(bool yes,const char* why){++assertions;if(!yes){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double x,double y,double tolerance=1e-7){return std::abs(x-y)<=tolerance;}
V add(V x,V y){for(unsigned i=0;i<3;++i)x[i]+=y[i];return x;}
V mul(V x,double n){for(auto& v:x)v*=n;return x;}
double dot(V x,V y){return c::detail::dot(x,y);}
double norm(V x){return std::sqrt(dot(x,x));}
void passive_scalar_domain(){
    for(double effective_mass:{5.,20.,74.084372133,106.,300.})for(double gap:{-.015,-.01,-.005,0.,.000348,.005})
        for(double velocity:{-20.,-5.,-2.6,-.5,-.01,0.,.03,.2,1.}){
            const double target=.15*std::max(0.,-gap)/h-std::max(0.,gap)/h;
            const auto r=k::solve(h,gap,.15,1/effective_mass,velocity,target);
            check(r.available&&r.impulse>=0,"valid scalar law is finite and unilateral");
            const double after=velocity+r.impulse/effective_mass,endgap=gap+h*after;
            check(endgap>=-k::maximum_deflection-1e-12,"raw compliant row enforces declared linear15mm deflection stop");
            const double before_energy=.5*effective_mass*velocity*velocity+.5*k::stiffness*std::pow(std::min(0.,gap),2);
            const double after_energy=.5*effective_mass*after*after+.5*k::stiffness*std::pow(std::min(0.,endgap),2);
            check(after_energy<=before_energy+1e-8,"normal kinetic plus declared spring energy cannot grow in passive scalar fixtures");
            if(r.backstop_active)++backstops;
            else if(r.impulse>0)check(near(after-target+r.gamma*r.impulse,0,1e-10),"soft row satisfies the implicit constitutive residual");
        }
    for(double gap:{-.015001,.005001,std::numeric_limits<double>::quiet_NaN()})check(!k::solve(h,gap,.15,.01,-2,0).available,"already-deep/outside-current-contact/unknown geometry does not soften original response");
    check(!k::solve(.01,0,.15,.01,-2,0).available,"uncalibrated step unavailable");
    for(double erp:{0.,1.,-1.,std::numeric_limits<double>::infinity()})check(!k::solve(h,0,erp,.01,-2,0).available,"invalid constitutive ERP unavailable");
    check(!k::solve(h,0,.15,0,-2,0).available,"missing effective mass unavailable");
}
struct ActualRearFixture {
    // Derived from native245 sample255/serial5651, selftestSHA7079132A….
    // Other base actuation is removed here by disabling balance/motor/pump,
    // subtracting one gravity step from the supplied precontact velocity.
    // This reproduces its exact contact stage, not the whole native trajectory.
    c::Tuning tuning;t::Body body;std::array<t::Wheel,2>wheels=c::proposed_wheels();
    std::array<c::ContactSample,2> samples{};c::Controls controls;
    ActualRearFixture(){
        tuning.balance_stiffness=tuning.balance_damping=0;
        body.mass=106;body.inertia_body={26.000001907348633,32,10};
        body.orientation={.028865518048405647,.9071575999259949,.06250887364149094,-.41511979699134827};
        body.center_of_mass={299.7231750488281,275.2583923339844,-481.6163330078125};
        body.linear_velocity={-1.7530947923660278,-3.1657511919736865+9.8*h,-1.5142029523849487};
        body.angular_velocity={-1.1062058733154783,-.002435600177787596,1.273524049062085};
        wheels[0].angular_speed=11.325804710388184;wheels[1].angular_speed=8.588080406188965;
        controls.enabled=false;controls.recovery_step_verified=true;
        samples[0].contact.status=t::ContactStatus::clear;samples[0].separation=.12727154791355133;
        auto& rear=samples[1];rear.contact.status=t::ContactStatus::hit;rear.verified_plane=true;
        rear.separation=.00034824071917682886;rear.contact.point={299.96875,274.306884765625,-481.40484619140625};
        rear.contact.normal={-.0012337459484115243,.9999948143959045,.0029671364463865757};
        rear.contact.forward={-.7495494484901428,.1373760998249054,-.6475362777709961};
        samples[0].contact.normal=rear.contact.normal;samples[0].contact.forward=rear.contact.forward;
    }
    c::Result step(c::Controller& controller,bool selected){controls.contact_compliance_available=selected;++solves;return controller.step(h,body,wheels,samples,controls);}
};
t::Body submitted(const ActualRearFixture& f,const c::Result& r){
    auto b=f.body;b.linear_velocity=add(add(b.linear_velocity,{0,-9.8*h,0}),mul(add(r.linear_impulse,r.hop_impulse),1/b.mass));
    b.angular_velocity=add(b.angular_velocity,c::detail::tensor(b,r.angular_impulse,true));return b;
}
void actual_rear_stage_and_coupling(){
    ActualRearFixture f;c::Controller original(f.tuning),candidate(f.tuning);
    const auto rigid=f.step(original,false);const auto soft=f.step(candidate,true);
    if(!(rigid.valid&&soft.valid&&soft.compliant_normal_mask==2&&!soft.compliance_budget_fallback))std::cerr<<"rigid="<<rigid.valid<<" soft="<<soft.valid<<" issue="<<int(soft.issue)<<" mask="<<soft.compliant_normal_mask<<" attempted="<<soft.compliance_attempted_mask<<" fallback="<<soft.compliance_budget_fallback<<" gap="<<soft.compliant_predicted_gap[1]<<" fraction="<<soft.contact_scale<<" normal="<<soft.normal_impulse[1]<<"\n";
    check(rigid.valid&&soft.valid&&soft.compliant_normal_mask==2&&!soft.compliance_budget_fallback,"only actual rear current plane uses the new constitutive row");
    check(near(rigid.normal_impulse[1],192.53410672565,.0001),"original controller contact stage reproduces actual245 normal impulse");
    check(near(norm(submitted(f,rigid).angular_velocity),3.87134461104,.0001),"original contact stage reproduces actual245 peak angular speed");
    check(near(soft.normal_impulse[1],140.73424874117,.0001),"implicit normal/tangent coupled contact matches independent actual-snapshot replay");
    check(near(norm(submitted(f,soft).angular_velocity),3.25964703148,.0001),"same-snapshot angular response is reduced by compliance, not changed inertia/caps");
    check(soft.compliant_deflection_satisfied[1]&&near(soft.compliant_predicted_gap[1],-.01266608314166,.000001),"actual-snapshot linear point prediction retains margin before unchanged30mm backup");
    const auto after=submitted(f,soft);t::Basis basis;t::contact_basis(f.samples[1].contact.normal,f.samples[1].contact.forward,basis);
    const auto vn=dot(t::point_velocity(after,f.samples[1].contact.point),basis.normal);
    check(std::abs(vn-soft.compliance[1].target_velocity+soft.compliance[1].gamma*soft.normal_impulse[1])<1e-6,"normal row corrects its own traction feedback in coupled iterations");
    check(soft.grounded==rigid.grounded&&soft.balance_support_mask==rigid.balance_support_mask&&soft.normal_load[0]==0&&soft.speculative_normal_mask==0&&!soft.hop_applied&&!soft.pump.applied,"strict actual support/history masks, speculative normal, hop and pump contracts are unchanged");
}
void cap_fallback_and_contracts(){
    ActualRearFixture f;f.tuning.force_limit=2000;c::Controller requested(f.tuning),original(f.tuning);
    const auto attempt=f.step(requested,true),rigid=f.step(original,false);
    check(attempt.valid&&attempt.compliance_budget_fallback&&attempt.compliant_normal_mask==0&&attempt.compliance_attempted_mask==2,"failed post-cap linear deflection selects original model, never a weaker hidden soft command");
    check(attempt.force==rigid.force&&attempt.torque==rigid.torque&&attempt.normal_load==rigid.normal_load&&attempt.wheels[1].angular_speed==rigid.wheels[1].angular_speed,"fallback preserves exact original capped momentum/rotor response");
    check(!attempt.compliant_deflection_satisfied[1]&&attempt.compliant_predicted_gap[1]<-.015,"attempted model's failed bound is retained honestly as attempted telemetry");
    const auto next=f.step(requested,false),expected=f.step(original,false);
    check(next.force==expected.force&&next.torque==expected.torque&&next.normal_load==expected.normal_load,"discarded soft preview cannot contaminate committed controller history");
    for(unsigned mode=0;mode<6;++mode){
        ActualRearFixture g;if(mode==0)g.samples[1].verified_plane=false;
        if(mode==1)g.samples[1].contact.surface_velocity={.1,0,0};
        if(mode==2)g.samples[1].separation=.006;
        if(mode==3)g.samples[1].separation=-.016;
        if(mode==4)g.controls.recovery_step_verified=false;
        if(mode==5)g.samples[1].contact.status=t::ContactStatus::clear;
        c::Controller yes(g.tuning),no(g.tuning);const auto a=g.step(yes,true),b=g.step(no,false);
        check(a.valid&&a.compliant_normal_mask==0&&!a.compliance_budget_fallback&&a.force==b.force&&a.torque==b.torque,"edge/moving/air/deep/stale geometry retains exact original contact response");
    }
}
void brake_motor_hop_and_early_normal(){
    c::Tuning tuning;tuning.balance_stiffness=tuning.balance_damping=0;
    auto body=c::proposed_body();body.center_of_mass={0,.9,0};body.linear_velocity={0,-2.5,3};body.angular_velocity={.3,0,0};
    auto wheels=c::proposed_wheels();for(auto& w:wheels)w.angular_speed=12;
    std::array<c::ContactSample,2> samples{};samples[0].contact.status=t::ContactStatus::clear;samples[0].separation=.1;
    samples[1].contact.status=t::ContactStatus::hit;samples[1].verified_plane=true;samples[1].contact.point={0,0,-.25};
    c::Controls controls;controls.recovery_step_verified=controls.contact_compliance_available=true;controls.brake=.6;controls.pedal=.2;
    c::Controller controller(tuning);const auto r=controller.step(h,body,wheels,samples,controls);++solves;
    check(r.valid&&r.compliant_normal_mask==2&&!r.compliance_budget_fallback,"compliance, actual brake and motor use the same converged/capped contact block");
    const V external{0,r.normal_impulse[1],r.longitudinal_impulse[1]};
    const auto expected=c::detail::cross(c::detail::sub(samples[1].contact.point,body.center_of_mass),external);
    auto combined=r.angular_impulse;
    for(unsigned i=0;i<2;++i)combined[0]+=wheels[i].inertia*(r.wheels[i].angular_speed-wheels[i].angular_speed);
    check(norm(c::detail::sub(combined,expected))<1e-6,"rotor+chassis momentum changes only by external normal/traction moment; motor/brakes remain internal");
    auto after=body;after.linear_velocity=add(add(body.linear_velocity,{0,-9.8*h,0}),mul(r.linear_impulse,1/body.mass));
    after.angular_velocity=add(body.angular_velocity,c::detail::tensor(body,r.angular_impulse,true));
    const double vn=t::point_velocity(after,samples[1].contact.point)[1];
    check(r.compliance[1].backstop_active||std::abs(vn-r.compliance[1].target_velocity+r.compliance[1].gamma*r.normal_impulse[1])<1e-5,"compliant normal includes its own brake/traction normal feedback");
    controls.brake=controls.pedal=0;controls.hop_strength=1;controls.hop_event_id=1;
    c::Controller hop_yes(tuning),hop_no(tuning);const auto yes=hop_yes.step(h,body,wheels,samples,controls);++solves;
    controls.contact_compliance_available=false;const auto no=hop_no.step(h,body,wheels,samples,controls);++solves;
    check(yes.valid&&no.valid&&yes.hop_applied&&yes.hop_impulse==no.hop_impulse&&yes.force==no.force&&yes.torque==no.torque,"actual grounded hop eligibility/impulse remains unchanged; opening wheels receive no soft pull");
    for(auto& s:samples){s.contact.status=t::ContactStatus::clear;s.separation=.04;s.verified_plane=true;s.contact.point={0,0,0};}
    body.center_of_mass={0,1,0};body.linear_velocity={0,-4,0};body.angular_velocity={};controls={};controls.recovery_step_verified=true;controls.rotation_reserved=true;
    c::Controller original(tuning),soft(tuning),preview(tuning);const auto p=preview.step(h,body,wheels,samples,controls);++solves;
    std::array<c::SpeculativeNormal,2> proof{};proof[1]={true,true,samples[1].contact.point,samples[1].contact.normal,.04,p.pre_contact_prediction};
    const auto early=original.step(h,body,wheels,samples,controls,proof);++solves;controls.contact_compliance_available=true;
    const auto retained=soft.step(h,body,wheels,samples,controls,proof);++solves;
    check(early.valid&&retained.valid&&retained.speculative_normal_mask==2&&retained.compliant_normal_mask==0&&early.force==retained.force&&early.torque==retained.torque,
        "validated early speculative normal response remains exactly unchanged outside actual near-plane contact");
}
void damped_vertical_landing(){
    c::Tuning tuning;tuning.balance_stiffness=tuning.balance_damping=0;c::Controller controller(tuning);
    auto body=c::proposed_body();auto wheels=c::proposed_wheels();body.center_of_mass={0,1.000348,0};body.linear_velocity={0,-2.6,0};
    c::Controls controls;controls.enabled=false;controls.recovery_step_verified=controls.contact_compliance_available=true;
    std::array<c::ContactSample,2> contacts{};for(auto& s:contacts){s.contact.status=t::ContactStatus::hit;s.verified_plane=true;s.contact.point={0,0,0};}
    double minimum_gap=0;unsigned fallback{};double last_energy=.5*body.mass*body.linear_velocity[1]*body.linear_velocity[1]+body.mass*9.8*(body.center_of_mass[1]-1);
    for(unsigned frame=0;frame<600;++frame){
        const double gap=body.center_of_mass[1]-1;for(auto& s:contacts)s.separation=gap;
        const auto r=controller.step(h,body,wheels,contacts,controls);++solves;check(r.valid,"vertical static-plane landing remains a valid force solve");
        if(r.compliance_budget_fallback)++fallback;
        body.linear_velocity[1]+=h*(-9.8+r.force[1]/body.mass);body.center_of_mass[1]+=h*body.linear_velocity[1];wheels=r.wheels;
        const double after_gap=body.center_of_mass[1]-1;minimum_gap=std::min(minimum_gap,after_gap);
        check(after_gap>=-.0150001,"multi-step pure integration stays inside declared15mm deflection");
        const double energy=.5*body.mass*body.linear_velocity[1]*body.linear_velocity[1]+k::stiffness*std::pow(std::min(0.,after_gap),2)+body.mass*9.8*after_gap;
        check(energy<=last_energy+1e-5,"kinetic + both spring energies + gravity potential dissipate through pure landing");last_energy=energy;
    }
    check(fallback==0&&minimum_gap<-.005,"constitutive law spreads the landing impulse rather than merely reverting every sample");
    check(near(body.center_of_mass[1]-1,-body.mass*9.8/(2*k::stiffness),.00005)&&std::abs(body.linear_velocity[1])<1e-5,"two tires settle to derived spring sag under gravity exactly once");
}
int main(){passive_scalar_domain();actual_rear_stage_and_coupling();cap_fallback_and_contracts();brake_motor_hop_and_early_normal();damped_vertical_landing();
    std::cout<<"PASS "<<assertions<<" assertions / "<<solves<<" controller solves / "<<backstops<<" scalar deflection-stop cases; no native validation\n";}
