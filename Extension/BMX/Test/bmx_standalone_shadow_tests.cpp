#include "Extension/BMX/bmx_standalone_shadow.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace native=dingosdk::bmx;
namespace shadow=native::standalone_shadow;
namespace control=native::chassis_control;
namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
bool near(double a,double b,double tolerance=1e-8){return std::abs(a-b)<=tolerance;}
native::RawInput neutral(){native::RawInput r;r.available=true;r.focused=true;r.device=4;return r;}
shadow::Observation observation(){
    shadow::Observation o;o.lease={1,2,3,4,5,6,7,8,9,10,11,12};
    o.serial=100;o.simulation_time=10;o.seconds=control::verified_native_seconds;o.body=control::proposed_body();
    o.body.center_of_mass={0,.9,-.05};
    for(auto& contact:o.contacts){contact.contact.status=native::tire::ContactStatus::clear;
        contact.contact.normal={0,1,0};contact.contact.forward={0,0,1};contact.separation=.25;}
    return o;
}
void advance(shadow::Observation& o){++o.serial;o.simulation_time+=o.seconds;}
void flat_contacts(shadow::Observation& o){
    for(unsigned i=0;i<2;++i){
        auto& c=o.contacts[i];c.contact.status=native::tire::ContactStatus::hit;c.separation=0;
        const auto hub=native::geometry::wheel_hub(i==0);
        c.contact.point={hub[0],0,hub[2]};c.balance_normal={0,1,0};c.balance_normal_available=true;c.verified_plane=true;
    }
}
void conversions(){
    auto o=observation();o.body.mass=123;o.body.inertia_body={19,29,13};
    o.body.orientation={0,.6,0,.8};o.body.center_of_mass={17,18,19};
    o.body.linear_velocity={-4,5,6};o.body.angular_velocity={7,-8,9};
    const auto body=shadow::convert_body(o.body);
    check(body.mass==123&&body.inertia_body==o.body.inertia_body,"observed mass and principal inertia remain authoritative");
    check(body.inertia_orientation==::bmx::Quat{0,0,0,1},"native principal axes are body axes");
    check(body.orientation==o.body.orientation&&body.center_of_mass==o.body.center_of_mass,"body-to-world XYZW and COM are not reflected or offset");
    check(body.linear_velocity==o.body.linear_velocity&&body.angular_velocity==o.body.angular_velocity,"world velocities are copied without integration");
    auto raw=neutral();raw.left_x=.7;raw.left_y=-.6;raw.right_x=-.3;raw.right_y=.4;raw.pedal=.5;raw.brake=.2;
    raw.left_trigger=.8;raw.right_trigger=.9;raw.left_shoulder=true;raw.right_shoulder=true;raw.quick_hop=true;raw.menu_open=true;raw.focused=false;
    const auto input=shadow::convert_input(raw);
    check(input.left_x==.7&&input.left_y==-.6&&input.right_x==-.3&&input.right_y==.4,"stick axes preserve input convention");
    check(input.pedal==.5&&input.brake==.2&&input.left_trigger==.8&&input.right_trigger==.9,"analog inputs preserve values");
    check(input.device==4&&input.available&&!input.focused&&input.menu_open&&input.quick_hop&&input.left_shoulder&&input.right_shoulder,"input gates and modifiers are explicit");
    auto& c=o.contacts[0];c.contact.status=native::tire::ContactStatus::hit;c.contact.point={1,2,3};c.contact.normal={0,.8,.6};
    c.contact.forward={1,0,0};c.contact.surface_velocity={4,5,6};c.separation=-.01;
    c.balance_normal={0,1,0};c.balance_normal_available=true;c.verified_plane=true;
    const auto contact=shadow::convert_contact(c);
    check(contact.contact.status==::bmx::ContactStatus::hit&&contact.contact.point==c.contact.point&&contact.contact.normal==c.contact.normal,"hit point and force normal copied from query");
    check(contact.contact.forward==c.contact.forward&&contact.contact.surface_velocity==c.contact.surface_velocity&&contact.separation==c.separation,"query gap, forward and surface velocity preserved");
    check(contact.balance_normal==c.balance_normal&&contact.balance_normal_available&&contact.verified_plane,"balance and plane availability preserved");
    c.contact.status=native::tire::ContactStatus::clear;c.verified_plane=false;
    check(shadow::convert_contact(c).contact.status==::bmx::ContactStatus::clear&&!shadow::convert_contact(c).verified_plane,"clear queries never become inferred plane support");
    c.contact.status=native::tire::ContactStatus::unavailable;
    check(shadow::convert_contact(c).contact.status==::bmx::ContactStatus::unavailable,"unknown query remains unavailable");
    check(shadow::geometry_matches(),"pinned source and current native geometry agree");
}
void clock_and_reset(){
    shadow::Observer observer;auto o=observation();auto raw=neutral();raw.left_x=.8;
    check(!observer.observe(raw,o).evaluated&&observer.status().issue==shadow::Issue::disabled,"ordinary default never evaluates imported controller");
    observer.set_enabled(true);
    check(observer.observe(raw,o).valid&&!observer.status().active,"first held input requires neutral");
    advance(o);raw=neutral();check(observer.observe(raw,o).valid&&observer.status().active,"fresh neutral arms shadow");
    const auto count=observer.status().evaluated_total;raw.left_x=.9;
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate&&observer.status().evaluated_total==count,"same epoch cannot advance controller with changed input");
    advance(o);check(observer.observe(raw,o).valid&&observer.status().active,"exact successor admitted once");
    observer.reset();check(!observer.status().valid&&!observer.status().active,"lifecycle reset clears published control readiness");
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate,"lifecycle reset cannot replay consumed epoch");
    advance(o);check(observer.observe(raw,o).valid&&!observer.status().active,"lifecycle reset cancels held input until neutral");
    const auto old=o;++o.serial;++o.serial;o.simulation_time+=2*o.seconds;
    check(observer.observe(raw,o).issue==shadow::Issue::discontinuity,"skipped native epoch rejected");
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate,"refused advancing epoch is not retried");
    check(observer.observe(raw,old).issue==shadow::Issue::discontinuity,"old serial cannot roll observer fence backward");
    auto wrong_time=o;wrong_time.simulation_time+=o.seconds;
    check(observer.observe(raw,wrong_time).issue==shadow::Issue::discontinuity,"same serial with changed time cannot replace consumed epoch");
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate,"inconsistent pair preserves original epoch fence");
    advance(o);check(observer.observe(raw,o).valid&&!observer.status().active,"post-discontinuity successor starts gated");
    advance(o);raw=neutral();check(observer.observe(raw,o).valid&&observer.status().active,"post-discontinuity neutral rearms");
    advance(o);raw.device=5;raw.left_x=.8;
    check(observer.observe(raw,o).valid&&!observer.status().active,"changed controller device requires neutral without reusing old input history");
    advance(o);o.simulation_time+=.001;
    check(observer.observe(raw,o).issue==shadow::Issue::discontinuity,"fresh serial with wrong native time rejected");
    advance(o);o.seconds=.01;
    check(observer.observe(raw,o).issue==shadow::Issue::invalid_clock,"100Hz cannot masquerade as native60Hz");
    observer.set_enabled(false);check(!observer.observe(raw,o).evaluated&&!observer.status().enabled,"explicit runtime revoke stops evaluation");
}
void lease_replacement(){
    for(unsigned field=0;field<12;++field){
        shadow::Observer observer;observer.set_enabled(true);auto o=observation();auto raw=neutral();
        check(observer.observe(raw,o).active,"initial neutral before lease replacement");
        advance(o);raw.left_x=.8;check(observer.observe(raw,o).active,"old lease has warmed controls");
        switch(field){
        case 0:++o.lease.base;break;case 1:++o.lease.client;break;case 2:++o.lease.entity;break;
        case 3:++o.lease.core;break;case 4:++o.lease.context;break;case 5:++o.lease.rig;break;
        case 6:++o.lease.query_context;break;case 7:++o.lease.world;break;case 8:++o.lease.native_state;break;
        case 9:++o.lease.body_index;break;case 10:++o.lease.body_generation;break;case 11:++o.lease.token;break;
        }
        o.serial=1;o.simulation_time=0;
        const auto report=observer.observe(raw,o);shadow::Observer fresh;fresh.set_enabled(true);const auto expected=fresh.observe(raw,o);
        check(report.valid&&!report.active&&report.force==expected.force&&report.torque==expected.torque&&report.hop_impulse==expected.hop_impulse,"every owner/world/body/lease field resets full shadow history");
        advance(o);raw=neutral();check(observer.observe(raw,o).active,"replacement can rearm on its own fresh neutral");
    }
    shadow::Observer observer;observer.set_enabled(true);auto o=observation();o.lease.token=0;
    check(observer.observe(neutral(),o).issue==shadow::Issue::invalid_lease,"zero native token refused");
    o=observation();o.lease.native_state=0;
    check(observer.observe(neutral(),o).issue==shadow::Issue::invalid_lease,"missing native world state refused");
}
void rejection_and_budget(){
    shadow::Observer observer;observer.set_enabled(true);auto o=observation();auto raw=neutral();
    check(observer.observe(raw,o).valid,"baseline before invalid sample");
    advance(o);o.contacts[1].contact.status=native::tire::ContactStatus::unavailable;
    check(!observer.observe(raw,o).valid&&observer.status().issue==shadow::Issue::invalid_output,"unavailable auxiliary query refuses disposable trial");
    check(observer.status().force==::bmx::Vec3{}&&!observer.status().comparison_available,"failed output cannot publish stale wrench comparison");
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate,"rejected solver epoch is burned");
    advance(o);o.contacts[1].contact.status=native::tire::ContactStatus::clear;raw.pedal=1;
    check(observer.observe(raw,o).valid&&!observer.status().active,"refused controller history discarded and neutral required");
    advance(o);raw=neutral();check(observer.observe(raw,o).active,"neutral after refusal rearms");
    advance(o);o.body.mass=120;raw.left_x=.8;
    check(observer.observe(raw,o).valid&&!observer.status().active,"measured mass change rebuilds history with real mass");
    advance(o);raw=neutral();check(observer.observe(raw,o).active,"neutral after mass change rearms");
    advance(o);o.body.inertia_body[0]+=1;raw.left_x=.8;
    check(observer.observe(raw,o).valid&&!observer.status().active,"measured principal inertia change resets history");
    const double h=control::verified_native_seconds;
    check(shadow::assess_wrench({40000,0,0},{0,4000,0},{},h).within_caps,"exact force and torque boundaries accepted");
    check(!shadow::assess_wrench({40000,0,0},{},{.001,0,0},h).within_caps,"hop is included once as J/h in combined native force budget");
    check(!shadow::assess_wrench({40001,0,0},{},{-1,0,0},h).within_caps,"opposing hop cannot hide oversized raw force");
    check(shadow::assess_wrench({}, {}, {600,0,0},h).within_caps,"600Ns impulse boundary remains below combined force ceiling");
    check(!shadow::assess_wrench({}, {}, {600.01,0,0},h).within_caps,"hop impulse independent ceiling checked");
    check(!shadow::assess_wrench({}, {0,4000.01,0},{},h).within_caps,"torque independent ceiling checked");
    check(!shadow::assess_wrench({std::numeric_limits<double>::quiet_NaN(),0,0},{},{},h).finite,"nonfinite diagnostic wrench refused");
    check(!shadow::assess_wrench({std::numeric_limits<double>::max(),0,0},{},{std::numeric_limits<double>::max(),0,0},h).finite,"overflow in combined force refused");
}
void native_gravity_and_hop(){
    shadow::Observer observer;observer.set_enabled(true);auto o=observation();flat_contacts(o);auto raw=neutral();
    const auto supported=observer.observe(raw,o);
    check(supported.valid&&supported.grounded,"fresh real-shaped flat wheel queries support the measured body");
    check(near(supported.force[1],o.body.mass*9.8,o.body.mass*.1),"flat support uses native9.8 predictor and measured mass once");
    advance(o);raw.quick_hop=true;const auto hopped=observer.observe(raw,o);
    check(hopped.valid&&hopped.within_caps&&near(shadow::norm(hopped.hop_impulse),o.body.mass*4.5,.01),"native one-part hop is an impulse using measured mass");
    const auto evaluated=observer.status().evaluated_total;
    check(observer.observe(raw,o).issue==shadow::Issue::duplicate&&observer.status().evaluated_total==evaluated,"same body epoch never repeats hop evaluation");
    advance(o);check(observer.observe(raw,o).valid&&observer.status().hop_impulse==::bmx::Vec3{},"held quick-hop does not emit a second impulse");
    observer.reset();advance(o);
    check(observer.observe(raw,o).valid&&!observer.status().active&&observer.status().hop_impulse==::bmx::Vec3{},"reset while hop held requires neutral and cannot replay launch");
}
void comparison_and_current_controller(){
    shadow::Observer observer;observer.set_enabled(true);auto o=observation();auto raw=neutral();
    const auto before=o.body;const auto input_before=raw;
    check(observer.observe(raw,o).valid,"comparison baseline valid");
    check(observer.status().force==::bmx::Vec3{},"airborne shadow does not emit engine gravity a second time");
    const auto r=observer.status();observer.compare_current(r.force,r.torque,r.hop_impulse,r.active,r.shadow_selection);
    check(observer.status().comparison_available&&observer.status().active_matches&&observer.status().selection_matches&&
        observer.status().force_difference_norm==0&&observer.status().torque_difference_norm==0,"bounded comparison is observational");
    observer.compare_current({2,0,0},{0,3,0},{0,0,4},false,999);
    check(near(observer.status().force_difference_norm,2)&&near(observer.status().torque_difference_norm,3)&&
        near(observer.status().hop_difference_norm,4)&&!observer.status().active_matches&&!observer.status().selection_matches,"current producer deltas and selection mismatch exposed");
    observer.compare_current({std::numeric_limits<double>::quiet_NaN(),0,0},{},{},false,0);
    check(!observer.status().comparison_available,"failed comparison cannot retain an earlier comparison as current");
    control::Controller with_shadow,unchanged;auto wheels=control::proposed_wheels();control::Controls controls;
    for(unsigned tick=0;tick<20;++tick){
        advance(o);observer.observe(raw,o);
        const auto a=with_shadow.step(o.seconds,o.body,wheels,o.contacts,controls);
        const auto b=unchanged.step(o.seconds,o.body,wheels,o.contacts,controls);
        check(a.valid&&b.valid&&a.force==b.force&&a.torque==b.torque&&a.hop_impulse==b.hop_impulse,"shadow cannot alter existing native producer results");
    }
    check(o.body.mass==before.mass&&o.body.inertia_body==before.inertia_body&&o.body.orientation==before.orientation&&
        o.body.center_of_mass==before.center_of_mass&&raw.left_x==input_before.left_x,"observations and input remain immutable");
    for(unsigned i=0;i<66000;++i)observer.observe(raw,o);
    check(observer.status().duplicate_total==65535&&observer.status().evaluated_total<100,"diagnostic counters saturate and duplicates never evaluate");
}
}
int main(){conversions();clock_and_reset();lease_replacement();rejection_and_budget();native_gravity_and_hop();comparison_and_current_controller();
    std::cout<<checks<<" standalone shadow checks passed\n";}
