#include "Engine/Game/BMX/bmx_chassis_control.h"
#include "Engine/Game/BMX/bmx_approach_surface.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace c=dingosdk::bmx::chassis_control;
namespace t=dingosdk::bmx::tire;
namespace a=dingosdk::bmx::approach_surface;
using V=t::Vec;
unsigned assertions{},solves{};
constexpr double h=c::verified_native_seconds;
void check(bool ok,const char* why){++assertions;if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double x,double y,double tolerance=1e-7){return std::abs(x-y)<=tolerance;}
V add(V x,V y){for(unsigned i=0;i<3;++i)x[i]+=y[i];return x;}
V mul(V x,double n){for(auto& v:x)v*=n;return x;}
double dot(V x,V y){double n=0;for(unsigned i=0;i<3;++i)n+=x[i]*y[i];return n;}
double norm(V v){return std::sqrt(dot(v,v));}
struct Fixture {
    t::Body body=c::proposed_body();std::array<t::Wheel,2>wheels=c::proposed_wheels();
    std::array<c::ContactSample,2> samples{};c::Controls controls;c::Controller controller;
    Fixture(){
        body.center_of_mass={0,1,0};body.linear_velocity={0,-4,0};controls.recovery_step_verified=true;controls.rotation_reserved=true;
        for(auto& s:samples){s.contact.status=t::ContactStatus::clear;s.contact.point={0,0,0};s.separation=.04;s.verified_plane=true;}
    }
    c::Result preview(){++solves;auto copy=controller;return copy.step(h,body,wheels,samples,controls);}
    std::array<c::SpeculativeNormal,2> proof(unsigned mask=2){
        const auto p=preview();check(p.valid&&p.pre_contact_prediction_available,"preview supplies the exact existing precontact prediction");
        std::array<c::SpeculativeNormal,2> rows{};
        for(unsigned i=0;i<2;++i)rows[i]={bool(mask&(1u<<i)),bool(mask&(1u<<i)),samples[i].contact.point,samples[i].contact.normal,samples[i].separation,p.pre_contact_prediction};
        return rows;
    }
    c::Result run(const std::array<c::SpeculativeNormal,2>& rows){++solves;return controller.step(h,body,wheels,samples,controls,rows);}
};
t::Body response(const Fixture& f,const c::Result& r){
    auto b=f.body;b.linear_velocity=add(add(b.linear_velocity,{0,-9.8*h,0}),mul(add(r.linear_impulse,r.hop_impulse),1/b.mass));
    // All analytic fixtures use the identity orientation and diagonal inertia.
    for(unsigned i=0;i<3;++i)b.angular_velocity[i]+=r.angular_impulse[i]/b.inertia_body[i];
    return b;
}
double energy(t::Body b,const std::array<t::Wheel,2>& wheels){
    double e=.5*b.mass*dot(b.linear_velocity,b.linear_velocity);
    for(unsigned i=0;i<3;++i)e+=.5*b.inertia_body[i]*b.angular_velocity[i]*b.angular_velocity[i];
    for(const auto& w:wheels)e+=.5*w.inertia*w.angular_speed*w.angular_speed;
    return e;
}
void analytic_gap_and_gravity(){
    for(double mass:{20.,106.,200.})for(double gap:{.031,.04,.08}){
        Fixture f;f.body.mass=mass;f.samples[1].separation=gap;f.body.linear_velocity={3,-5,4};
        const auto before=f.body;const auto rows=f.proof();const auto r=f.run(rows);
        const auto predicted=-5-9.8*h,target=-gap/h;
        const auto expected=std::max(0.,(target-predicted)*mass);
        if(!(r.valid&&r.speculative_normal_mask==2&&near(r.contact_scale,1)))std::cerr<<"mass="<<mass<<" gap="<<gap<<" valid="<<r.valid<<" mask="<<r.speculative_normal_mask<<" frac="<<r.contact_scale<<" why="<<c::speculative_name(r.speculative_issues[1])<<" forceY="<<r.force[1]<<"\n";
        check(r.valid&&r.speculative_normal_mask==2&&near(r.contact_scale,1),"single COM-normal row is admitted within existing budgets");
        check(near(r.speculative_normal_impulse[1],expected,1e-5),"analytic impulse consumes one gravity step and allows remaining gap closure");
        check(near(response(f,r).linear_velocity[1],target,1e-7),"normal row targets negative gap/h rather than sticky zero velocity");
        check(!r.grounded&&r.balance_support_mask==0&&r.balance_continuation_mask==0,"speculation cannot create strict support or balance history");
        check(r.normal_load==std::array<double,2>{}&&r.normal_impulse==std::array<double,2>{},"speculative response is never reported as actual wheel load");
        check(r.longitudinal_impulse==std::array<double,2>{}&&r.lateral_impulse==std::array<double,2>{},"normal-only row supplies no traction even with lateral/longitudinal motion");
        check(near(r.force[0],0)&&near(r.force[2],0)&&norm(r.torque)<1e-8,"COM-normal fixture has no hidden tangent force or torque");
        check(near(r.speculative_normal_force[1],expected/h,1e-5)&&near(r.speculative_linear_impulse[1],expected,1e-5),"normal attribution is included in the shared submitted contact impulse");
        auto free=before;free.linear_velocity[1]-=9.8*h;
        check(energy(response(f,r),r.wheels)<=energy(free,f.wheels)+1e-7,"positive-gap normal response dissipates kinetic energy");
        check(f.body.linear_velocity==before.linear_velocity&&f.body.center_of_mass==before.center_of_mass&&f.body.orientation==before.orientation,"caller native pose and velocity observations remain untouched");
    }
}
void two_row_coupling(){
    Fixture f;f.body.linear_velocity={0,-3.7,0};f.samples[0].contact.point={0,0,.525};f.samples[1].contact.point={0,0,-.525};
    f.samples[0].separation=.04;f.samples[1].separation=.06;const auto r=f.run(f.proof(3));
    const double z=.525,diag=1/f.body.mass+z*z/f.body.inertia_body[0],off=1/f.body.mass-z*z/f.body.inertia_body[0];
    const double b0=-.04/h-(-3.7-9.8*h),b1=-.06/h-(-3.7-9.8*h),det=diag*diag-off*off;
    const double j0=(b0*diag-b1*off)/det,j1=(b1*diag-b0*off)/det;
    check(r.valid&&r.speculative_normal_mask==3&&near(r.contact_scale,1),"both speculative rows share the alternating coupled solve");
    check(near(r.speculative_normal_impulse[0],j0,1e-5)&&near(r.speculative_normal_impulse[1],j1,1e-5),"coupled impulses match independent two-point effective-mass matrix");
    const auto after=response(f,r);
    for(unsigned i=0;i<2;++i)check(near(t::point_velocity(after,f.samples[i].contact.point)[1],-f.samples[i].separation/h,1e-6),"both row endpoint velocities satisfy their separate negative gap targets");
    check(near(r.linear_impulse[1],j0+j1,1e-5)&&near(r.angular_impulse[0],z*(j1-j0),1e-5),"normal impulses account linear and angular momentum exactly once");
}
void shared_limits_and_brake_momentum(){
    Fixture f;f.body.linear_velocity={0,-40,0};f.samples[1].contact.point={1,0,-.8};
    const auto r=f.run(f.proof());
    check(r.valid&&r.speculative_normal_mask==2&&r.contact_scale>0&&r.contact_scale<1,"strong off-COM impact uses the existing common under-relaxation fraction");
    check(norm(r.force)<=40000&&norm(r.torque)<=4000,"speculative normal cannot bypass existing force/torque caps");
    check(norm(add(r.linear_impulse,mul(r.speculative_linear_impulse,-1)))<1e-6&&norm(add(r.angular_impulse,mul(r.speculative_angular_impulse,-1)))<1e-6,"capped speculative contribution exactly matches total contact momentum when no other actuation exists");
    check(t::point_velocity(response(f,r),f.samples[1].contact.point)[1]<-.04/h,"budget-limited unresolved closing is retained rather than repaired by a pose/velocity write");
    f={};f.samples[1].contact.point={0,0,-.1};f.body.linear_velocity={0,-4.5,6};f.controls.brake=.8;f.controls.pedal=.5;
    for(auto& w:f.wheels)w.angular_speed=45;
    const auto input_wheels=f.wheels;const auto input_body=f.body;const auto braked=f.run(f.proof());const auto after=response(f,braked);
    check(braked.valid&&braked.speculative_normal_mask==2&&near(braked.contact_scale,1),"braking/motor reaction and speculative row converge within the same contact budget");
    check(near(t::point_velocity(after,f.samples[1].contact.point)[1],-.04/h,1e-6),"normal row accounts its own brake-induced pitch in coupled iterations");
    double initial=input_body.inertia_body[0]*input_body.angular_velocity[0],final=after.inertia_body[0]*after.angular_velocity[0];
    for(unsigned i=0;i<2;++i){initial+=input_wheels[i].inertia*input_wheels[i].angular_speed;final+=braked.wheels[i].inertia*braked.wheels[i].angular_speed;}
    check(near(final-initial,braked.speculative_angular_impulse[0],1e-6),"motor/brakes conserve rotor+chassis angular momentum apart from the actual normal impulse moment");
    check(braked.brake_impulse[0]!=0&&braked.brake_impulse[1]!=0&&braked.longitudinal_impulse==std::array<double,2>{}&&braked.lateral_impulse==std::array<double,2>{},"clear wheels retain real braking but gain no speculative friction");
}
void unchanged_support_hop_pump(){
    Fixture f;f.samples[1].contact.status=t::ContactStatus::hit;f.samples[1].separation=0;f.samples[1].contact.point={0,0,-.4};
    f.samples[0].contact.point={0,0,.4};f.body.linear_velocity={0,-4,0};const auto r=f.run(f.proof(1));
    check(r.valid&&r.grounded&&r.speculative_normal_mask==1,"a real rear touch and speculative front response can coexist");
    check(r.balance_support_mask==2&&r.normal_load[0]==0&&r.speculative_normal_impulse[1]==0,"only the measured rear supplies balance/history/actual load");
    auto next=f.controller;auto rowless=next.step(h,f.body,f.wheels,f.samples,f.controls);++solves;
    check(rowless.speculative_normal_mask==0&&(rowless.balance_support_mask&1u)==0,"speculative row cannot warm-start or seed later front support/history");
    f={};f.controls.pump_operator_available=true;f.controls.ground_pump=-1;const auto airborne=f.run(f.proof());
    check(airborne.valid&&!airborne.pump.applied&&airborne.pump_force==V{}&&!airborne.grounded,"speculative response cannot arm airborne direct/landing pumping");
    f={};f.controls.hop_strength=.8;f.controls.hop_event_id=1;const auto hopped=f.run(f.proof());
    check(hopped.valid&&!hopped.hop_applied&&hopped.hop_impulse==V{}&&hopped.speculative_normal_mask==0&&hopped.speculative_issues[1]==c::SpeculativeIssue::hop_reserved,"reserved hop neither gains speculative ground eligibility nor a conflicting normal row");
}
void refusal_contract(){
    for(unsigned mode=0;mode<13;++mode){
        Fixture f;auto rows=f.proof();
        if(mode==0)rows[1].selected=false;
        if(mode==1)rows[1].qualified=false;
        if(mode==2)f.controls.recovery_step_verified=false;
        if(mode==3)f.controls.enabled=false;
        if(mode==4)f.samples[1].verified_plane=false;
        if(mode==5)rows[1].point[2]+=.001;
        if(mode==6)rows[1].normal[0]+=.001;
        if(mode==7)rows[1].separation+=.001;
        if(mode==8)rows[1].prediction.linear_velocity[1]+=.001;
        if(mode==9)f.samples[1].contact.surface_velocity={0,.1,0};
        if(mode==10){f.samples[1].separation=.03;rows=f.proof();}
        if(mode==11){f.samples[1].separation=.251;rows=f.proof();}
        if(mode==12){f.body.linear_velocity[1]=0;rows=f.proof();}
        const auto r=f.run(rows);check(r.valid&&r.speculative_normal_mask==0&&r.speculative_normal_impulse==std::array<double,2>{},"unqualified/changed/stale/nonstatic/noncrossing witness cannot add any row");
    }
    // A fresh finite-face miss is consumed as unqualified, never manufactured
    // from a later contact row. This exercises the actual approach policy too.
    Fixture f;const auto p=f.preview();a::Surface current{true,true,{0,0,0},{0,1,0},{0,0,1},.04,0,{11,22}};
    for(unsigned missing=1;missing<=5;++missing){unsigned casts{};
        const auto witness=a::observe(true,true,false,h,p.pre_contact_prediction,current,[&](a::Vec from,a::Vec to){
            ++casts;if(casts==missing)return a::RayHit{true,false};
            const auto fraction=-from[1]/(to[1]-from[1]);a::Vec point{};for(unsigned i=0;i<3;++i)point[i]=from[i]+fraction*(to[i]-from[i]);
            return a::RayHit{true,true,point,fraction,0,{11,22}};
        },[]{return true;});
        auto rows=f.proof();rows[1].qualified=witness.qualified;const auto r=f.run(rows);
        check(!witness.qualified&&r.speculative_normal_mask==0,"actual center/perimeter hole witness cannot enable normal actuation");
    }
}
int main(){analytic_gap_and_gravity();two_row_coupling();shared_limits_and_brake_momentum();unchanged_support_hop_pump();refusal_contract();
    std::cout<<"PASS "<<assertions<<" assertions / "<<solves<<" solves; parameter/refusal iterations are not independent native cases\n";}
