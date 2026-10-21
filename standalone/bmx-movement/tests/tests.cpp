#include <bmx/movement.hpp>
#include <bmx/grind.hpp>
#include <bmx/suspension.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>

namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
double speed(const bmx::Output&o){return std::hypot(o.body.linear_velocity[0],o.body.linear_velocity[2]);}
double length(bmx::Vec3 p){return std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);}
bool finite(bmx::Vec3 p){for(auto v:p)if(!std::isfinite(v))return false;return true;}
bmx::RawInput neutral(){bmx::RawInput r;r.available=true;r.focused=true;r.device=1;return r;}
bmx::Output arm(bmx::FlatWorld&w){auto r=neutral();bmx::Output o;for(int i=0;i<4;++i)o=w.step(r);return o;}
void rest_drive_brake(){
    bmx::FlatWorld w;auto r=neutral();auto initial=arm(w);auto o=initial;
    for(int i=0;i<600;++i){o=w.step(r);check(o.valid,"rest step valid");}
    check(speed(o)<.001,"resting bike does not manufacture horizontal motion");
    check(std::abs(o.body.center_of_mass[1]-initial.body.center_of_mass[1])<.015,"flat contact holds ride height");
    r.pedal=1;for(int i=0;i<180;++i){o=w.step(r);check(o.valid,"pedal step valid");}
    const double driven=speed(o);check(driven>1,"pedal torque propels the bike");
    r.pedal=0;r.brake=1;for(int i=0;i<180;++i){o=w.step(r);check(o.valid,"brake step valid");}
    check(speed(o)<driven*.2,"braking dissipates measured forward motion");
    check(finite(o.rider.head)&&finite(o.rider.hips),"rider targets remain finite while riding");
}
void gravity_and_hop(){
    bmx::FlatWorld falling;falling.reset({0,100,0});auto r=neutral();bmx::Output o;
    for(int i=0;i<30;++i){o=falling.step(r);check(o.valid,"clear air is valid");}
    check(std::abs(o.body.linear_velocity[1]+4.9)<.02,"gravity applied exactly once at 9.8 m/s2");
    check(!o.grounded,"clear air is not support");
    bmx::FlatWorld w;auto start=arm(w);r.quick_hop=true;unsigned hops=0;double top=start.body.center_of_mass[1];bool air=false,landed=false;
    for(int i=0;i<180;++i){o=w.step(r);check(o.valid,"hop integration valid");if(o.events.took_off)++hops;top=std::max(top,o.body.center_of_mass[1]);air=air||!o.grounded;if(air&&o.grounded)landed=true;}
    check(air&&top>start.body.center_of_mass[1]+.25,"hop produces a physical airborne arc");
    check(hops==1,"held hop cannot jump repeatedly after landing");check(landed,"hop returns to verified ground");
}
void selection_catalog(){
    std::set<unsigned> ids;
    for(const auto&e:bmx::catalog::entries){
        const unsigned id=static_cast<unsigned>(e.id);check(ids.insert(id).second,"catalog IDs unique");check(!e.name.empty(),"catalog entry named");
        bmx::FlatWorld w;auto r=neutral();arm(w);r.quick_hop=true;auto o=w.step(r);r.quick_hop=false;
        for(int i=0;i<5;++i)o=w.step(r);
        check(!o.grounded,"selection fixture is airborne");
        switch(id/10){case 0:r.left_shoulder=true;r.right_trigger=1;break;case 1:r.right_shoulder=true;r.left_trigger=1;break;
            case 2:r.left_shoulder=r.right_shoulder=true;break;case 3:r.left_shoulder=true;break;case 4:r.right_shoulder=true;break;
            case 5:r.left_trigger=r.right_trigger=1;break;case 6:r.left_trigger=1;break;case 7:r.right_trigger=1;break;}
        const double angle=(id%10)*3.14159265358979323846/4;
        r.right_x=std::sin(angle);r.right_y=std::cos(angle);o=w.step(r);
        bool selected=o.actions.movement==e.id;
        if(id==34||id==44)selected=selected&&o.tricks.bar_rotating;
        else if(id==52||id==56)selected=selected&&o.tricks.tail_rotating;
        else selected=selected&&o.tricks.movement_id==e.id;
        if(!selected){std::cerr<<"Selection mismatch for "<<e.name<<" expected "<<id<<" got "<<unsigned(o.tricks.movement_id)<<'\n';std::exit(1);}
        ++checks;
        r=neutral();
        for(int sample=0;sample<120;++sample){
            o=w.step(r);check(o.valid,"catalog follow-up remains valid");
            for(unsigned limb=0;limb<2;++limb){
                const auto distance=[](bmx::Vec3 a,bmx::Vec3 b){return length({a[0]-b[0],a[1]-b[1],a[2]-b[2]});};
                check(std::abs(distance(o.rider.shoulders[limb],o.rider.elbows[limb])-.34)<1e-5,"upper arm length preserved");
                check(std::abs(distance(o.rider.elbows[limb],o.rider.hands[limb])-.33)<1e-5,"forearm length preserved");
                check(std::abs(distance(o.rider.hips_joint[limb],o.rider.knees[limb])-.47)<1e-5,"thigh length preserved");
                check(std::abs(distance(o.rider.knees[limb],o.rider.feet[limb])-.45)<1e-5,"shin length preserved");
            }
        }
    }
    check(ids.size()==58,"all 58 authored selections present");
}
void host_contract(){
    bmx::FlatWorld w;const auto initial=arm(w);bmx::Movement module;
    std::array<bmx::ContactSample,2> unknown{};auto o=module.step(neutral(),initial.body,unknown);
    check(!o.valid,"unavailable host contacts rejected");
    check(length(o.force)==0&&length(o.torque)==0,"rejected sample cannot apply forces");
    auto invalid=initial.body;invalid.mass=std::numeric_limits<double>::quiet_NaN();
    o=module.step(neutral(),invalid,unknown);check(!o.valid,"nonfinite body rejected");
}
void turning_symmetry(){
    bmx::FlatWorld left,right;auto l=neutral(),r=neutral();arm(left);arm(right);l.pedal=r.pedal=1;
    bmx::Output lo,ro;
    for(int i=0;i<120;++i){lo=left.step(l);ro=right.step(r);}
    l.left_x=-.25;r.left_x=.25;
    for(int i=0;i<120;++i){lo=left.step(l);ro=right.step(r);check(lo.valid&&ro.valid,"sustained steering remains valid");}
    check(lo.bike_root[0]>0&&ro.bike_root[0]<0,"input right turns toward rider right with left-positive geometry");
    check(std::abs(lo.bike_root[0]+ro.bike_root[0])<1e-5&&std::abs(lo.bike_root[2]-ro.bike_root[2])<1e-5,"left and right turn trajectories mirror");
}

void observed_grind_contract(){
    bmx::Body body;body.mass=100;body.inertia_body={25,30,20};body.center_of_mass={0,1,0};body.linear_velocity={0,0,5};
    bmx::GrindContact c;c.available=c.touching=true;c.point=body.center_of_mass;c.normal={0,1,0};c.tangent={0,0,1};
    const auto solved=bmx::solve_grind(body,c);
    check(solved.valid&&solved.active,"observed rail contact supports load");
    check(std::abs(solved.force[1]-980)<1e-8,"rail support balances measured mass times gravity");
    check(solved.speed>=0&&solved.speed<5&&solved.force[2]<0,"rail friction dissipates forward slip without reversal");
    auto moving=body;moving.linear_velocity[2]+=3;c.surface_velocity[2]=3;
    const auto shifted=bmx::solve_grind(moving,c);
    check(shifted.valid&&std::abs(shifted.speed-solved.speed)<1e-10&&length({shifted.force[0]-solved.force[0],shifted.force[1]-solved.force[1],shifted.force[2]-solved.force[2]})<1e-9,"rail response invariant to common platform velocity");
    c.available=false;const auto unavailable=bmx::solve_grind(body,c);
    check(!unavailable.valid&&length(unavailable.force)==0,"unavailable rail contact cannot apply forces");
    c.available=true;c.touching=false;const auto missing=bmx::solve_grind(body,c);
    check(missing.valid&&!missing.active&&length(missing.force)==0,"missing rail cannot attract bike");
    c.touching=true;c.separation=.05;const auto apart=bmx::solve_grind(body,c);
    check(apart.valid&&!apart.active&&length(apart.force)==0,"separated rail cannot snap bike");
    c.separation=0;c.surface_velocity={};body.linear_velocity={0,0,.001};
    const auto stop=bmx::solve_grind(body,c);
    check(stop.valid&&std::abs(stop.speed)<1e-10,"rail friction clamps at zero rather than reversing");
}

void configurable_fixed_step(){
    bmx::SimulationSettings setup;setup.seconds=.01;setup.mass=90;setup.inertia_body={6,8,6};
    setup.wheels={bmx::Wheel{.26,1.1,0},bmx::Wheel{.26,1.1,0}};
    bmx::Tuning tuning;tuning.gravity={0,-12.5,0};
    bmx::FlatWorld falling(tuning,setup);falling.reset({0,100,0});auto r=neutral();bmx::Output o;
    const double start=falling.body().center_of_mass[1];
    check(falling.fixed_seconds()==.01,"configured clock exposed without replacing legacy default");
    check(falling.body().mass==90&&falling.body().inertia_body==setup.inertia_body,"reference host adopts supplied mass and inertia");
    for(int i=0;i<50;++i){o=falling.step(r);check(o.valid&&!o.grounded,"100Hz free fall remains valid and airborne");}
    check(std::abs(o.body.linear_velocity[1]+6.25)<1e-10,"100Hz gravity integrated once over half a second");
    check(std::abs(o.body.center_of_mass[1]-(start-12.5*.01*.01*50*51/2))<1e-8,"100Hz semi-implicit trajectory matches analytic discrete free fall");
    bmx::FlatWorld riding(tuning,setup);o=arm(riding);const double floor=o.body.center_of_mass[1];
    for(int i=0;i<300;++i){o=riding.step(r);check(o.valid,"100Hz supported rest remains valid");}
    check(std::abs(o.body.center_of_mass[1]-floor)<.02&&speed(o)<.005,"100Hz contacts settle within5mm/s after configured-radius spawn clearance");
    r.quick_hop=true;o=riding.step(r);unsigned takeoffs=o.events.took_off?1:0;double top=o.body.center_of_mass[1];r=neutral();
    for(int i=0;i<4;++i)o=riding.step(r);
    r.left_shoulder=true;r.right_y=-1;o=riding.step(r);check(o.tricks.bar_rotating,"100Hz airborne neutral-rearmed barspin launches");r=neutral();
    for(int i=0;i<300;++i){o=riding.step(r);check(o.valid,"100Hz hop and trick recovery remain valid");takeoffs+=o.events.took_off?1:0;top=std::max(top,o.body.center_of_mass[1]);}
    check(takeoffs==1&&top>floor+.3&&o.grounded,"100Hz hop has one takeoff and returns to support");
    check(o.tricks.tally.landed_barspins==1,"100Hz time drives a complete caught and landed barspin");
    bmx::Movement host(tuning,setup);auto body=riding.body();body.mass=73;body.inertia_body={9,11,7};
    std::array<bmx::ContactSample,2> clear{};for(auto& c:clear)c.contact.status=bmx::ContactStatus::clear;
    const auto measured=host.step(neutral(),body,clear);check(measured.valid&&measured.body.mass==73&&measured.body.inertia_body==body.inertia_body,"host mass and inertia remain authoritative over reference initialization");
    bmx::Body rail_body;rail_body.mass=100;rail_body.center_of_mass={0,1,0};bmx::GrindContact rail;rail.available=rail.touching=true;rail.point=rail_body.center_of_mass;
    auto rail_result=bmx::solve_grind(rail_body,rail,{0,-12.5,0},.01);
    check(rail_result.valid&&std::abs(rail_result.force[1]-1250)<1e-8&&std::abs(rail_result.normal_impulse-12.5)<1e-8,"100Hz rail impulse and force use the same explicit clock");
    for(double seconds:{0.,-.01,.001,.1,std::numeric_limits<double>::quiet_NaN()}){
        auto bad=setup;bad.seconds=seconds;bmx::FlatWorld invalid(tuning,bad);const auto before=invalid.body();const auto refused=invalid.step(neutral());
        check(!refused.valid&&length(refused.force)==0&&length(refused.torque)==0,"invalid configured timestep is rejected without force");
        check(invalid.body().center_of_mass==before.center_of_mass,"invalid configured timestep cannot integrate the host");
        check(!bmx::solve_grind(rail_body,rail,{0,-12.5,0},seconds).valid,"rail solver rejects invalid timestep");
    }
    bmx::FlatWorld legacy;check(legacy.fixed_seconds()==bmx::fixed_step&&legacy.body().mass==106,"legacy construction retains60Hz and original initialization");
}

std::array<bmx::ContactSample,2> ground_samples(const bmx::Output& frame,double radius){
    std::array<bmx::ContactSample,2> contacts{};
    for(unsigned i=0;i<2;++i){
        const auto hub=bmx::wheel_hub(frame,i==0);auto& c=contacts[i];
        c.separation=hub[1]-radius;c.contact.status=bmx::ContactStatus::hit;
        c.contact.point={hub[0],0,hub[2]};c.contact.normal={0,1,0};c.contact.forward={0,0,1};
        c.balance_normal_available=true;c.balance_normal={0,1,0};c.verified_plane=true;
    }
    return contacts;
}
void scheduled_hop_contract(){
    const auto profile=bmx::streets_observed_v1();
    bmx::FlatWorld fixture(profile.tuning,profile.simulation,profile.rider);
    auto frame=arm(fixture);const auto body=frame.body;const auto contacts=ground_samples(frame,.26);
    std::array<bmx::ContactSample,2> clear{};for(auto& c:clear)c.contact.status=bmx::ContactStatus::clear;
    bmx::Movement module(profile.tuning,profile.simulation,profile.rider);auto r=neutral();
    for(int i=0;i<3;++i)check(module.step(r,body,contacts).valid,"scheduled fixture arms on verified support");
    r.quick_hop=true;auto o=module.step(r,body,contacts);
    check(o.actions.hop_pressed&&length(o.hop_impulse)==0,"delayed hop reserves one input request without early impulse");
    r=neutral();double total=0;unsigned takeoffs=0;
    for(int tick=1;tick<=7;++tick){
        o=module.step(r,body,contacts);check(o.valid,"waiting hop keeps supported physics valid");
        if(tick<7)check(length(o.hop_impulse)==0,"hop respects seven complete100Hz wait intervals");
        else check(std::abs(o.hop_impulse[1]-body.mass*7.9/4)<1e-10,"first delayed hop part is one quarter of accepted impulse");
        total+=o.hop_impulse[1];takeoffs+=o.events.took_off?1:0;
    }
    auto airborne=body;airborne.center_of_mass[1]+=1;airborne.linear_velocity[1]=2;
    for(int i=0;i<3;++i){
        o=module.step(r,airborne,clear);check(o.valid&&!o.grounded,"scheduled hop continues through verified clear air");
        check(std::abs(o.hop_impulse[1]-body.mass*7.9/4)<1e-10,"remaining airborne hop part retains original accepted impulse");
        total+=o.hop_impulse[1];takeoffs+=o.events.took_off?1:0;
    }
    check(std::abs(total-body.mass*7.9)<1e-9&&takeoffs==1,"four parts conserve exact accepted hop impulse with one takeoff");
    check(length(module.step(r,airborne,clear).hop_impulse)==0,"completed hop emits no fifth part");
    // Cancellation is exercised both while waiting and after launch, using real
    // mapper transitions and invalid host observations rather than state access.
    for(bool launched:{false,true})for(unsigned cause=0;cause<9;++cause){
        bmx::Movement pending(profile.tuning,profile.simulation,profile.rider);r=neutral();
        for(int i=0;i<3;++i)pending.step(r,body,contacts);
        r.quick_hop=true;pending.step(r,body,contacts);r=neutral();
        if(launched)for(int i=0;i<7;++i)o=pending.step(r,body,contacts);
        auto canceled_body=body;auto canceled_contacts=contacts;std::uint64_t identity=1;
        switch(cause){
        case 0:r.focused=false;break;case 1:r.menu_open=true;break;case 2:r.available=false;break;
        case 3:r.device=2;break;case 4:canceled_body.mass=std::numeric_limits<double>::quiet_NaN();break;
        case 5:canceled_contacts={};break;case 6:identity=2;break;case 7:pending.reset();break;
        case 8:identity=0;break;
        }
        o=pending.step(r,canceled_body,canceled_contacts,identity);
        check(length(o.hop_impulse)==0,"control/body transition cancels pending or partially applied hop immediately");
        r=neutral();if(cause==3)r.device=2;if(!identity)identity=1;
        for(int i=0;i<20;++i){
            o=pending.step(r,body,contacts,identity);
            check(o.valid&&length(o.hop_impulse)==0,"canceled hop never replays after neutral rearm or support recovery");
        }
    }
    // Losing support exactly when the wait expires consumes the request; a
    // later landing cannot resurrect it.
    bmx::Movement missed(profile.tuning,profile.simulation,profile.rider);r=neutral();
    for(int i=0;i<3;++i)missed.step(r,body,contacts);
    r.quick_hop=true;missed.step(r,body,contacts);r=neutral();
    for(int i=0;i<9;++i)check(length(missed.step(r,airborne,clear).hop_impulse)==0,"unsupported delayed request cannot launch in air");
    for(int i=0;i<10;++i)check(length(missed.step(r,body,contacts).hop_impulse)==0,"expired unsupported hop cannot launch on a later landing");
    for(unsigned variant=0;variant<5;++variant){
        auto settings=profile.simulation;
        switch(variant){case 0:settings.hop_delay_seconds=-.01;break;case 1:settings.hop_delay_seconds=.51;break;
        case 2:settings.hop_delay_seconds=std::numeric_limits<double>::quiet_NaN();break;
        case 3:settings.hop_impulse_steps=0;break;case 4:settings.hop_impulse_steps=17;break;}
        bmx::Movement invalid(profile.tuning,settings,profile.rider);
        check(!invalid.step(neutral(),body,contacts).valid,"invalid hop scheduling settings are rejected without silent clamping");
    }
}
void observed_rider_profile(){
    const auto p=bmx::streets_observed_v1();
    check(p.simulation.seconds==.01&&p.tuning.gravity[1]==-12.5&&p.simulation.wheels[0].radius==.26,"observed profile applies clock gravity and tire radius explicitly");
    check(p.simulation.mass==106&&p.simulation.inertia_body==bmx::SimulationSettings{}.inertia_body,"observed profile keeps independent effective mass and inertia");
    check(p.rider.upper_arm==.277304&&p.rider.forearm==.227670&&p.rider.thigh==.417477&&p.rider.shin==.405345,"measured limb lengths remain independent of procedural motion scale");
    bmx::FlatWorld w(p.tuning,p.simulation,p.rider);auto o=arm(w);auto r=neutral();
    for(int i=0;i<200;++i)o=w.step(r);
    const double floor=o.body.center_of_mass[1];r.quick_hop=true;unsigned takeoffs=0,parts=0;double impulse=0,top=floor;
    for(int i=0;i<220;++i){
        o=w.step(r);check(o.valid,"observed profile hop is a valid reduced-body trajectory");
        takeoffs+=o.events.took_off?1:0;parts+=length(o.hop_impulse)>0?1:0;impulse+=o.hop_impulse[1];top=std::max(top,o.body.center_of_mass[1]);
    }
    check(parts==4&&takeoffs>=1&&std::abs(impulse-106*7.9)<1e-8,"held observed hop completes only its four scheduled parts despite touchdown contact transitions");
    check(top>floor+2&&top<floor+2.6&&o.grounded,"empirical observed hop has bounded height and returns to support");
    for(const auto& entry:bmx::catalog::entries){
        bmx::FlatWorld trick(p.tuning,p.simulation,p.rider);r=neutral();arm(trick);r.quick_hop=true;trick.step(r);r=neutral();
        for(int i=0;i<15;++i)o=trick.step(r);
        const unsigned id=static_cast<unsigned>(entry.id);
        switch(id/10){case 0:r.left_shoulder=true;r.right_trigger=1;break;case 1:r.right_shoulder=true;r.left_trigger=1;break;
        case 2:r.left_shoulder=r.right_shoulder=true;break;case 3:r.left_shoulder=true;break;case 4:r.right_shoulder=true;break;
        case 5:r.left_trigger=r.right_trigger=1;break;case 6:r.left_trigger=1;break;case 7:r.right_trigger=1;break;}
        const double angle=(id%10)*3.14159265358979323846/4;r.right_x=std::sin(angle);r.right_y=std::cos(angle);trick.step(r);r=neutral();
        const auto distance=[](bmx::Vec3 a,bmx::Vec3 b){return length({a[0]-b[0],a[1]-b[1],a[2]-b[2]});};
        for(int sample=0;sample<100;++sample){
            o=trick.step(r);check(o.valid&&finite(o.rider.head)&&finite(o.rider.hips),"measured rider targets remain finite through every authored selection");
            for(unsigned limb=0;limb<2;++limb){
                check(std::abs(distance(o.rider.shoulders[limb],o.rider.elbows[limb])-p.rider.upper_arm)<1e-5,"measured upper arm length preserved through retargeted motion");
                check(std::abs(distance(o.rider.elbows[limb],o.rider.hands[limb])-p.rider.forearm)<1e-5,"measured forearm length preserved through retargeted motion");
                check(std::abs(distance(o.rider.hips_joint[limb],o.rider.knees[limb])-p.rider.thigh)<1e-5,"measured thigh length preserved through retargeted motion");
                check(std::abs(distance(o.rider.knees[limb],o.rider.feet[limb])-p.rider.shin)<1e-5,"measured shin length preserved through retargeted motion");
            }
        }
    }
    auto bad=p.rider;bad.forearm=0;bmx::FlatWorld invalid(p.tuning,p.simulation,bad);
    check(!invalid.step(neutral()).valid,"unusable retargeting dimensions reject a physics frame");
}
void empirical_brake_profile(){
    const auto p=bmx::streets_observed_v1();auto fixed=p.tuning;
    fixed.brake_min_scale=fixed.brake_max_scale=1;fixed.brake_speed_gain=0;
    bmx::FlatWorld original(fixed,p.simulation,p.rider),fitted(p.tuning,p.simulation,p.rider);
    auto r=neutral();bmx::Output a,b;for(int i=0;i<200;++i){a=original.step(r);b=fitted.step(r);}
    r.pedal=1;
    for(int i=0;i<185;++i){
        a=original.step(r);b=fitted.step(r);
        check(a.valid&&b.valid&&a.body.center_of_mass==b.body.center_of_mass&&a.body.linear_velocity==b.body.linear_velocity&&a.torque==b.torque,
            "brake speed fit has no effect without a brake request");
    }
    check(speed(a)>4.5&&speed(a)<5,"brake comparison starts inside measured high-speed calibration range");
    r=neutral();r.brake=1;
    for(int i=0;i<25;++i){a=original.step(r);b=fitted.step(r);check(a.valid&&b.valid,"fixed and fitted high-speed braking stay valid");}
    check(speed(b)<speed(a)-.15,"observed speed fit dissipates more high-speed motion than fixed torque");
    for(int i=0;i<300;++i){
        b=fitted.step(r);check(b.valid&&b.body.linear_velocity[2]>-.005,"fitted braking stops forward travel without reverse overshoot");
    }
    check(speed(b)<.005,"fitted low-speed braking settles to rest");
    // The existing coupled contact/brake block is a kinetic-energy minimizer.
    // Exercise both directions, low speed and saturation with the fitted torques.
    namespace sc=bmx::detail::source::chassis_control;
    for(double velocity:{-20.,-4.7,-1.,-.001,0.,.001,1.,4.7,20.}){
        auto body=bmx::detail::source::chassis_control::proposed_body();body.center_of_mass={0,.9,0};body.linear_velocity={0,0,velocity};
        bmx::Wheel wheel{.26,.23,velocity/.26};
        bmx::detail::source::tire::Contact contact;contact.status=bmx::ContactStatus::hit;contact.point={0,0,.42};
        const auto energy=[](const bmx::Body& body,const bmx::Wheel& wheel){
            double result=0;for(unsigned j=0;j<3;++j)result+=.5*body.mass*body.linear_velocity[j]*body.linear_velocity[j]+
                .5*body.inertia_body[j]*body.angular_velocity[j]*body.angular_velocity[j];
            return result+.5*wheel.inertia*wheel.angular_speed*wheel.angular_speed;
        };
        const double before=energy(body,wheel);
        const double scale=std::clamp(std::abs(velocity)*p.tuning.brake_speed_gain,p.tuning.brake_min_scale,p.tuning.brake_max_scale);
        const auto solved=sc::coupled_brake::solve_tangent(.01,body,wheel,contact,106*12.5*.01/2,60*scale,{1,1});
        check(solved.contact.valid&&energy(body,wheel)<=before+1e-8,"bounded fitted brake torque cannot add chassis-plus-wheel kinetic energy");
    }
    bmx::FlatWorld fixture(p.tuning,p.simulation,p.rider);const auto frame=arm(fixture);const auto contacts=ground_samples(frame,.26);
    for(unsigned variant=0;variant<6;++variant){
        auto bad=p.tuning;switch(variant){case 0:bad.brake_min_scale=-.1;break;case 1:bad.brake_max_scale=.1;break;
        case 2:bad.brake_max_scale=11;break;case 3:bad.brake_speed_gain=-1;break;case 4:bad.brake_speed_gain=11;break;
        case 5:bad.brake_speed_gain=std::numeric_limits<double>::quiet_NaN();break;}
        bmx::Movement invalid(bad,p.simulation,p.rider);check(!invalid.step(neutral(),frame.body,contacts).valid,"invalid brake fit parameters reject the sample");
    }
    const bmx::Tuning legacy;check(legacy.brake_min_scale==1&&legacy.brake_max_scale==1&&legacy.brake_speed_gain==0,"legacy tuning keeps fixed brake torque by default");
}
void rejected_observation_breaks_history(){
    const auto p=bmx::streets_observed_v1();bmx::FlatWorld fixture(p.tuning,p.simulation,p.rider);
    const auto frame=arm(fixture);const auto contacts=ground_samples(frame,.26);
    for(bool missing_contact:{false,true}){
        bmx::Movement module(p.tuning,p.simulation,p.rider),fresh(p.tuning,p.simulation,p.rider);auto r=neutral();
        bmx::Output before,previous;for(int i=0;i<3;++i)before=module.step(r,frame.body,contacts);
        r.left_x=.8;for(int i=0;i<20;++i){previous=before;before=module.step(r,frame.body,contacts);}
        check(before.valid&&std::abs(before.steering_angle)>.1,"history rejection fixture starts with nonzero physical steering history");
        auto invalid_body=frame.body;auto invalid_contacts=contacts;
        if(missing_contact)invalid_contacts={};else invalid_body.mass=std::numeric_limits<double>::quiet_NaN();
        check(!module.step(r,invalid_body,invalid_contacts).valid,"intervening rejected observation breaks consecutive simulation proof");
        r=neutral();const auto resumed=module.step(r,frame.body,contacts);const auto cold=fresh.step(r,frame.body,contacts);
        check(resumed.valid&&cold.valid&&resumed.steering_angle==cold.steering_angle&&resumed.steering_angle==0,
            "resumed observation uses fresh steering history instead of carrying stale filtered deflection");
        // Wheel momentum persists through the rejected sample; reconstruct it
        // from the published phase increments for a fresh-controller comparison.
        auto wheels=p.simulation.wheels;
        wheels[0].angular_speed=(before.front_wheel_phase-previous.front_wheel_phase)/.01;
        wheels[1].angular_speed=(before.rear_wheel_phase-previous.rear_wheel_phase)/.01;
        bmx::detail::source::chassis_control::Controller cold_controller(p.tuning);
        bmx::detail::source::chassis_control::Controls command;command.pump_operator_available=true;
        const auto cold_force=cold_controller.step(.01,frame.body,wheels,contacts,command);
        check(cold_force.valid&&length({resumed.force[0]-cold_force.force[0],resumed.force[1]-cold_force.force[1],resumed.force[2]-cold_force.force[2]})<1e-7,
            "rejected body/contact gap cannot carry old support recovery into the next valid solve");
    }
}
void asymmetric_folded_rider_reach(){
    bmx::FlatWorld anchors;const auto frame=anchors.output();bmx::RiderSettings rider;
    const auto midpoint=[](bmx::Vec3 a,bmx::Vec3 b){return bmx::Vec3{(a[0]+b[0])*.5,(a[1]+b[1])*.5+.05,(a[2]+b[2])*.5};};
    const auto fit_axis=[](bmx::Vec3 left,bmx::Vec3 right,bmx::Vec3& axis,double& half_width){
        for(unsigned i=0;i<3;++i)axis[i]=left[i]-right[i];
        const double distance=length(axis);half_width=distance*.5;for(auto& component:axis)component/=distance;
    };
    const auto left_hand=bmx::grip_point(frame,true),right_hand=bmx::grip_point(frame,false);
    const auto left_foot=bmx::pedal_point(frame,true),right_foot=bmx::pedal_point(frame,false);
    rider.upper_arm=.20;rider.forearm=.40;rider.thigh=.21;rider.shin=.45;
    rider.chest=midpoint(left_hand,right_hand);rider.hips=midpoint(left_foot,right_foot);
    fit_axis(left_hand,right_hand,rider.shoulder_axis,rider.shoulder_half_width);
    fit_axis(left_foot,right_foot,rider.hip_joint_axis,rider.hip_joint_half_width);
    bmx::FlatWorld folded({}, {},rider);const auto o=arm(folded);
    check(o.valid,"custom asymmetric folded skeleton is a valid rider configuration");
    const auto distance=[](bmx::Vec3 a,bmx::Vec3 b){return length({a[0]-b[0],a[1]-b[1],a[2]-b[2]});};
    const auto folds_away=[](bmx::Vec3 start,bmx::Vec3 joint,bmx::Vec3 end){
        double dot=0;for(unsigned i=0;i<3;++i)dot+=(joint[i]-start[i])*(end[i]-start[i]);return dot<0;
    };
    for(unsigned side=0;side<2;++side){
        check(o.rider.hand_target_clamped[side]&&o.rider.foot_target_clamped[side],"near anchors project onto asymmetric limbs minimum reachable distance");
        check(std::abs(distance(o.rider.shoulders[side],o.rider.elbows[side])-.20)<1e-5&&
              std::abs(distance(o.rider.elbows[side],o.rider.hands[side])-.40)<1e-5,"folded arm preserves both lengths when forearm exceeds upper arm");
        check(std::abs(distance(o.rider.hips_joint[side],o.rider.knees[side])-.21)<1e-5&&
              std::abs(distance(o.rider.knees[side],o.rider.feet[side])-.45)<1e-5,"folded leg preserves both lengths when shin exceeds thigh");
        check(folds_away(o.rider.shoulders[side],o.rider.elbows[side],o.rider.hands[side])&&
              folds_away(o.rider.hips_joint[side],o.rider.knees[side],o.rider.feet[side]),"longer lower segments place folded joint behind start rather than stretching the limb");
    }
}
void passive_suspension_contract(){
    bmx::Body chassis,rider;chassis.mass=30;chassis.inertia_body={2,3,4};chassis.center_of_mass={0,.2,0};
    rider.mass=80;rider.inertia_body={6,8,6};rider.center_of_mass={.04,.8,.02};
    rider.orientation={std::sin(.075),0,0,std::cos(.075)};
    bmx::SuspensionSettings settings;settings.target_position={0,.8,0};
    for(unsigned axis=0;axis<3;++axis){settings.linear[axis]={1100,140,30000};settings.angular[axis]={500,50,5000};}
    const auto length_delta=[](bmx::Vec3 a,bmx::Vec3 b){return length({a[0]-b[0],a[1]-b[1],a[2]-b[2]});};
    const auto cross=[](bmx::Vec3 a,bmx::Vec3 b){return bmx::Vec3{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    bmx::SuspensionResult reference;
    for(double seconds:{1./240,.01,1./60,1./30}){
        const auto solved=bmx::solve_suspension(seconds,chassis,rider,settings);
        check(solved.valid,"implicit measured-body suspension admits supported fixed clocks");
        check(length_delta(solved.chassis_force,{-solved.rider_force[0],-solved.rider_force[1],-solved.rider_force[2]})<1e-9,"internal suspension force conserves pair linear momentum");
        const auto base_orbit=cross(chassis.center_of_mass,solved.chassis_force),rider_orbit=cross(rider.center_of_mass,solved.rider_force);
        bmx::Vec3 total_torque;for(unsigned i=0;i<3;++i)total_torque[i]=base_orbit[i]+rider_orbit[i]+solved.chassis_torque[i]+solved.rider_torque[i];
        check(length(total_torque)<1e-8,"off-center suspension reactions conserve total world angular momentum");
        check(solved.predicted_energy<=solved.energy_before+1e-6&&solved.actuator_work==0,"fixed passive suspension target cannot add total spring-plus-body energy");
        check(length(solved.rider_force)<=settings.maximum_linear_force+1e-8,"suspension respects explicit finite force bound");
        if(seconds==.01)reference=solved;
    }
    auto boosted_chassis=chassis,boosted_rider=rider;
    for(unsigned i=0;i<3;++i){const bmx::Vec3 shift{100,-50,25},boost{3,0,-2};boosted_chassis.center_of_mass[i]+=shift[i];boosted_rider.center_of_mass[i]+=shift[i];boosted_chassis.linear_velocity[i]+=boost[i];boosted_rider.linear_velocity[i]+=boost[i];}
    const auto boosted=bmx::solve_suspension(.01,boosted_chassis,boosted_rider,settings);
    check(boosted.valid&&length_delta(boosted.rider_force,reference.rider_force)<1e-7&&length_delta(boosted.chassis_torque,reference.chassis_torque)<1e-7,"suspension is invariant to common world translation and velocity");
    auto stretched=rider;stretched.center_of_mass[1]=2;auto bounded=settings;bounded.maximum_linear_force=50;
    const auto saturated=bmx::solve_suspension(.01,chassis,stretched,bounded);
    check(saturated.valid&&saturated.saturated&&length(saturated.rider_force)<=50+1e-8,"large spring displacement produces bounded finite reaction without teleportation");
    auto stop=settings;for(auto& drive:stop.linear)drive={};for(auto& drive:stop.angular)drive={};
    stop.linear_limits[1]={true,-.3,.3,{10000,250,50000}};
    auto outside=rider;outside.orientation={0,0,0,1};outside.center_of_mass={0,.8,0};
    const auto limited=bmx::solve_suspension(.01,chassis,outside,stop);
    check(limited.valid&&limited.limit_violation>.2&&limited.rider_force[1]<0,"measured excess travel activates a restoring soft stop");
    check(limited.predicted_energy<=limited.energy_before+1e-6,"soft travel stop dissipates rather than injecting energy");
    auto radial=stop;radial.linear_limits={};radial.linear_radial_limit={true,0,.3,{10000,250,50000}};
    outside.center_of_mass={.25,.45,0};
    const auto round_stop=bmx::solve_suspension(.01,chassis,outside,radial);
    check(round_stop.valid&&round_stop.limit_violation>.05,"limitedXYZ radial stop catches diagonal travel that fits inside an axis-aligned cube");
    check(round_stop.rider_force[0]<0&&round_stop.rider_force[1]<0&&round_stop.predicted_energy<=round_stop.energy_before+1e-6,"radial travel limit applies a finite dissipative restoring reaction");
    bmx::Body a=chassis,b=rider;a.center_of_mass={};b.center_of_mass={0,.2,0};b.orientation={0,0,0,1};
    bmx::SuspensionSettings acceleration;acceleration.linear[1]={30,2,100000,bmx::SuspensionDriveMode::acceleration};
    const auto low_mass=bmx::solve_suspension(.01,a,b,acceleration);a.mass*=3;b.mass*=3;
    const auto high_mass=bmx::solve_suspension(.01,a,b,acceleration);
    check(low_mass.valid&&high_mass.valid&&std::abs(high_mass.rider_force[1]/b.mass-low_mass.rider_force[1]/rider.mass)<1e-8,"acceleration-mode central drive responds consistently across a common mass scale");
    for(double seconds:{0.,-.01,.001,.1,std::numeric_limits<double>::quiet_NaN()})check(!bmx::solve_suspension(seconds,chassis,rider,settings).valid,"suspension rejects an unsupported or nonfinite timestep");
    auto invalid=settings;invalid.linear[1].damper=-1;check(!bmx::solve_suspension(.01,chassis,rider,invalid).valid,"negative suspension damping is rejected");
    auto invalid_body=rider;invalid_body.mass=std::numeric_limits<double>::quiet_NaN();check(!bmx::solve_suspension(.01,chassis,invalid_body,settings).valid,"suspension rejects nonfinite measured body state");
    bmx::FlatWorld world;auto frame=world.output();bmx::RiderSettings dimensions;bmx::RiderFrame measured;
    measured.neutral_center_from_bike_root={0,.8,0};measured.center_of_mass={0,.8,0};
    const auto neutral_pose=bmx::rider_pose(frame,dimensions,&measured),legacy_pose=bmx::rider_pose(frame,dimensions);
    check(length_delta(neutral_pose.head,legacy_pose.head)<1e-12&&length_delta(neutral_pose.hips,legacy_pose.hips)<1e-12,"neutral measured rider frame preserves the legacy skeleton template");
    measured.center_of_mass[1]-=.15;const auto compressed=bmx::rider_pose(frame,dimensions,&measured);
    check(std::abs(compressed.hips[1]-neutral_pose.hips[1]+.15)<1e-12&&std::abs(compressed.head[1]-neutral_pose.head[1]+.15)<1e-12,"measured rider compression translates proximal bones rather than the bike root");
    for(unsigned limb=0;limb<2;++limb)check(std::abs(length_delta(compressed.elbows[limb],compressed.hands[limb])-dimensions.forearm)<1e-5&&std::abs(length_delta(compressed.knees[limb],compressed.feet[limb])-dimensions.shin)<1e-5,"measured-frame retargeting retains limb lengths through suspension travel");
    bmx::Movement adapter;auto air=chassis;air.center_of_mass={0,100,0};
    std::array<bmx::ContactSample,2> clear{};for(auto& sample:clear)sample.contact.status=bmx::ContactStatus::clear;
    const bmx::ExternalWrench external{{10,-800,20},{5,6,7}};
    const auto forced=adapter.step(neutral(),air,clear,1,external);
    check(forced.valid&&length_delta(forced.force,external.force)<1e-8&&length_delta(forced.torque,external.torque)<1e-8,"host multi-body wrench enters current prediction and returned force exactly once with gravity separate");
    auto invalid_wrench=external;invalid_wrench.force[0]=std::numeric_limits<double>::quiet_NaN();
    check(!adapter.step(neutral(),air,clear,1,invalid_wrench).valid,"nonfinite multi-body reaction rejects the physics sample");
}
void principal_inertia_and_coupled_world(){
    namespace math=bmx::detail::source::tire::detail;
    bmx::Body inertia;inertia.inertia_body={2,3,5};const double root_half=std::sqrt(.5);
    inertia.inertia_orientation={0,0,root_half,root_half};
    const auto inverse_x=math::inverse_inertia(inertia,{1,0,0});
    check(std::abs(inverse_x[0]-1./3)<1e-12&&std::abs(inverse_x[1])<1e-12,"principal inertia orientation rotates tensor independently of body geometry");
    const bmx::Vec3 impulse{2,3,4};const auto local_response=math::inverse_inertia(inertia,impulse);
    inertia.orientation={0,std::sin(.35),0,std::cos(.35)};
    const auto transformed=math::inverse_inertia(inertia,bmx::rotate(inertia.orientation,impulse));
    const auto expected=bmx::rotate(inertia.orientation,local_response);
    check(length({transformed[0]-expected[0],transformed[1]-expected[1],transformed[2]-expected[2]})<1e-12,"full inertia response is covariant under a common world rotation");
    auto bad=inertia;bad.inertia_orientation={0,0,0,0};check(!bmx::detail::source::tire::valid(bad),"nonunit principal inertia orientation is rejected");
    const auto profile=bmx::streets_observed_v2();
    check(profile.movement.simulation.mass==30&&profile.coupled.rider_mass==80,"v2 explicitly separates selected non-Spring30kg and Spring80kg roles");
    check(profile.movement.tuning.hop_delta_speed==6.4&&profile.movement.simulation.predict_hop_parts,"v2 predicts actual nominal native hop parts without legacy7.9 velocity fit");
    bmx::CoupledWorld falling(profile);falling.reset({0,100,0});auto raw=neutral();
    const double mass=falling.body().mass+falling.rider_body().mass;
    const double initial_com=(falling.body().mass*falling.body().center_of_mass[1]+falling.rider_body().mass*falling.rider_body().center_of_mass[1])/mass;
    bmx::CoupledOutput o;
    for(int tick=0;tick<50;++tick){o=falling.step(raw);check(o.valid&&o.converged,"airborne coupled pair remains valid and converges at100Hz");}
    const double com=(o.bike.body.mass*o.bike.body.center_of_mass[1]+o.rider.mass*o.rider.center_of_mass[1])/mass;
    const double momentum=o.bike.body.mass*o.bike.body.linear_velocity[1]+o.rider.mass*o.rider.linear_velocity[1];
    check(std::abs(momentum/mass+6.25)<1e-8,"coupled internal reactions cancel while gravity accelerates total COM once");
    check(std::abs(com-initial_com+12.5*.01*.01*50*51/2)<1e-8,"coupled COM obeys independent semi-implicit free-fall trajectory");
    bmx::CoupledWorld riding(profile);for(int i=0;i<200;++i){o=riding.step(raw);check(o.valid&&o.converged,"coupled flat rest converges with native acceleration-drive gains");}
    check(std::abs(o.rider_relative_com[1]-.3987877644)<.003,"native-measured rider/chassis neutral COM gap matches without a pose offset fudge");
    const double floor=o.observed_chassis_com[1];unsigned parts=0,takeoffs=0,landings=0;double base_impulse=0,rider_impulse=0,top=floor;
    for(int tick=0;tick<300;++tick){
        raw=neutral();raw.quick_hop=tick==0;o=riding.step(raw);check(o.valid,"coupled nominal hop and landing remain finite");
        parts+=length(o.bike.hop_impulse)>0?1:0;takeoffs+=o.bike.events.took_off?1:0;landings+=o.bike.events.landed?1:0;
        base_impulse+=o.bike.hop_impulse[1];rider_impulse+=o.rider_hop_impulse[1];top=std::max(top,o.observed_chassis_com[1]);
    }
    check(parts==4&&takeoffs==1&&landings==1,"coupled hop predicts and delivers four real parts with one physical flight");
    check(std::abs(base_impulse-30*6.4*profile.movement.simulation.hop_impulse_multiplier)<1e-8&&
          std::abs(rider_impulse-80*6.4*profile.coupled.rider_hop_multiplier)<1e-8,"source hop velocity changes map to explicit per-body impulses without mass duplication");
    check(top>floor+1&&o.bike.grounded,"coupled nominal hop is a physical arc returning to actual wheel support");
}
void observed_combo_landing(){
    const auto p=bmx::streets_observed_v1();
    check(bmx::SimulationSettings{}.trick_settings.maximum_closing_speed==7,"legacy trick quality gate remains unchanged");
    for(bool tail:{false,true}){
        bmx::FlatWorld w(p.tuning,p.simulation,p.rider);auto r=neutral();bmx::Output o;
        for(int i=0;i<200;++i)o=w.step(r);
        unsigned landed=0,failed=0,canceled=0,caught=0,takeoffs=0;double minimum_gap=1;
        for(int tick=0;tick<300;++tick){
            r=neutral();r.quick_hop=tick==0;
            if(tick==20){
                if(tail){r.left_trigger=r.right_trigger=1;r.right_x=1;}
                else{r.left_shoulder=true;r.right_y=-1;}
            }
            o=w.step(r);check(o.valid,"full observed hop-trick-catch-landing sequence remains valid");
            check(length(o.force)<=p.tuning.force_limit+1e-6,"observed landing impulse respects the explicit host impact budget");
            for(bool front:{false,true})minimum_gap=std::min(minimum_gap,bmx::wheel_hub(o,front)[1]-.26);
            landed+=o.events.combo_landed?1:0;failed+=o.events.failed?1:0;canceled+=o.events.canceled?1:0;
            takeoffs+=o.events.took_off?1:0;
            caught+=(tail?o.events.tail_caught:o.events.bar_caught)?1:0;
        }
        if(landed!=1||failed||canceled)std::cerr<<"observed combo tail="<<tail<<" landed="<<landed<<" failed="<<failed<<" canceled="<<canceled<<" failure="<<bmx::trick_failure_name(o.tricks.failure)<<'\n';
        check(caught==1&&landed==1&&failed==0&&canceled==0,"normal observed hop trick catches and scores exactly once after settled support");
        check(o.tricks.tally.landed_combos==1&&o.tricks.tally.failed_combos==0&&o.tricks.tally.canceled_combos==0,"support rebound cannot duplicate or discard a valid observed combo");
        check((tail?o.tricks.tally.landed_tailwhips:o.tricks.tally.landed_barspins)==1,"landed observed combo records the selected barspin or tailwhip");
        check(takeoffs==1&&minimum_gap>=-1e-6,"anticipated flat impact avoids penetration and spurious rebound takeoff");
    }
    // A wider query horizon may anticipate impact but cannot qualify actual
    // support, admit a hop or award a landing before the wheel reaches ground.
    bmx::FlatWorld fixture(p.tuning,p.simulation,p.rider);auto frame=arm(fixture);frame.body.center_of_mass[1]+=.08;
    frame.bike_root[1]+=.08;frame.body.linear_velocity[1]=-7.6;const auto contacts=ground_samples(frame,.26);
    namespace sc=bmx::detail::source::chassis_control;
    for(const auto& contact:contacts)check(contact.separation>.005&&!sc::eligible_support(frame.body,contact),"positive anticipated wheel gap cannot qualify strict support");
    auto wheels=p.simulation.wheels;sc::Controller controller(p.tuning);sc::Controls command;command.hop_strength=1;command.hop_event_id=1;
    const auto anticipated=controller.step(.01,frame.body,wheels,contacts,command);
    check(anticipated.valid&&!anticipated.grounded&&!anticipated.hop_applied,"anticipated contact cannot launch a hop before actual support");
    bmx::Movement module(p.tuning,p.simulation,p.rider);auto neutral_raw=neutral();auto out=module.step(neutral_raw,frame.body,contacts);
    neutral_raw.quick_hop=true;out=module.step(neutral_raw,frame.body,contacts);
    check(out.valid&&!out.grounded&&length(out.hop_impulse)==0&&!out.events.combo_landed&&!out.events.landed,"anticipated contact cannot report real support or an early trick landing");
}
void physical_trick_presentation(){
    bmx::FlatWorld fixture;auto frame=fixture.output();const bmx::Vec3 point{.2,.8,.4};
    const auto physical=bmx::bike_point(frame,point,false),hub=bmx::wheel_hub(frame,true);
    frame.tricks.movement_bike_pitch=.7;frame.tricks.movement_bike_roll=-.8;
    const auto procedural=bmx::bike_point(frame,point,false);
    check(length({procedural[0]-physical[0],procedural[1]-physical[1],procedural[2]-physical[2]})>.1,"legacy presentation retains independent catalog tilt");
    frame.physical_trick_rotation=true;const auto coupled=bmx::bike_point(frame,point,false);
    check(coupled==physical&&frame.tricks.movement_bike_pitch==.7,"verified joint-driven chassis suppresses duplicate presentation tilt while retaining gameplay trick state");
    check(bmx::wheel_hub(frame,true)==hub,"physical wheel queries never consume procedural trick pitch or roll");
}

}
int main(){rest_drive_brake();gravity_and_hop();selection_catalog();host_contract();observed_grind_contract();turning_symmetry();configurable_fixed_step();scheduled_hop_contract();observed_rider_profile();empirical_brake_profile();rejected_observation_breaks_history();asymmetric_folded_rider_reach();passive_suspension_contract();principal_inertia_and_coupled_world();observed_combo_landing();physical_trick_presentation();std::cout<<"PASS: "<<checks<<" assertions (legacy60Hz, configurable100Hz, coupled100Hz, measured rider,58 selections, rail and host rejection)\n";}
