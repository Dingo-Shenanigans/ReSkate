#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace c=dingosdk::bmx::chassis_control;
namespace t=dingosdk::bmx::tire;
constexpr double h=c::verified_native_seconds;
void check(bool condition,const char* why) {if(!condition){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double a,double b,double error=1e-7) {return std::abs(a-b)<=error;}
t::Vec add(t::Vec a,t::Vec b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
t::Vec sub(t::Vec a,t::Vec b) {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
t::Vec mul(t::Vec a,double s) {for(auto& x:a)x*=s;return a;}
double dot(t::Vec a,t::Vec b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
double length(t::Vec a) {return std::sqrt(dot(a,a));}
t::Vec cross(t::Vec a,t::Vec b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
void vector_near(t::Vec a,t::Vec b,const char* message,double error=1e-7) {check(length(sub(a,b))<=error,message);}
struct Fixture {
    t::Body body=c::proposed_body();
    std::array<t::Wheel,2> wheels=c::proposed_wheels();
    std::array<c::ContactSample,2> contacts;
    Fixture() {
        body.center_of_mass={0,.9,-.05};
        for(unsigned i=0;i<2;++i) {contacts[i].contact.status=t::ContactStatus::hit;contacts[i].contact.point={0,0,i==0?.525:-.525};}
    }
    void air() {for(auto& sample:contacts)sample.contact.status=t::ContactStatus::clear;}
    void rolling(double speed) {body.linear_velocity={0,0,speed};for(auto& w:wheels)w.angular_speed=speed/w.radius;}
};
// Independent explicit body-to-world matrix; used for inertia/momentum checks.
std::array<t::Vec,3> axes(t::Quat q) {
    double n=0;for(double v:q)n+=v*v;for(auto& v:q)v/=std::sqrt(n);
    const auto x=q[0],y=q[1],z=q[2],w=q[3];
    return {t::Vec{1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)},
        t::Vec{2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)},
        t::Vec{2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)}};
}
t::Vec tensor(const t::Body& b,t::Vec v,bool inverse) {
    const auto a=axes(b.orientation);t::Vec out{};
    for(unsigned i=0;i<3;++i)out=add(out,mul(a[i],dot(a[i],v)*(inverse?1/b.inertia_body[i]:b.inertia_body[i])));
    return out;
}
double energy(const t::Body& b,const std::array<t::Wheel,2>& wheels) {
    double e=.5*b.mass*dot(b.linear_velocity,b.linear_velocity)+.5*dot(b.angular_velocity,tensor(b,b.angular_velocity,false));
    for(const auto& w:wheels)e+=.5*w.inertia*w.angular_speed*w.angular_speed;
    return e;
}
}

namespace {
c::Tuning passive_brake_settings() {
    c::Tuning tuning;tuning.balance_stiffness=tuning.balance_damping=0;
    tuning.air_yaw_torque=tuning.air_pitch_torque=0;
    tuning.recovery_fraction=tuning.recovery_speed_limit=0;return tuning;
}
template<class Result>
t::Body advance_measured(const t::Body& before,const Result& result,t::Vec gravity) {
    auto after=before;
    after.linear_velocity=add(add(before.linear_velocity,mul(gravity,h)),mul(add(result.linear_impulse,result.hop_impulse),1/before.mass));
    after.angular_velocity=add(before.angular_velocity,tensor(before,result.angular_impulse,true));
    return after;
}
t::Vec total_momentum(const t::Body& body,const std::array<t::Wheel,2>& wheels,
                      const std::array<c::ContactSample,2>& contacts) {
    auto value=add(cross(body.center_of_mass,mul(body.linear_velocity,body.mass)),tensor(body,body.angular_velocity,false));
    for(unsigned i=0;i<2;++i) {
        t::Basis basis;check(t::contact_basis(contacts[i].contact.normal,contacts[i].contact.forward,basis),"oracle has valid measured axle basis");
        value=add(value,mul(basis.lateral,wheels[i].inertia*wheels[i].angular_speed));
    }
    return value;
}
void brake_budget_and_friction(const Fixture& fixture,const c::Result& result,c::Tuning tuning,double brake) {
    for(unsigned i=0;i<2;++i) {
        const auto limit=brake*(i==0?tuning.front_brake_torque:tuning.rear_brake_torque)*h;
        check(std::abs(result.brake_impulse[i])<=limit+1e-9,"actual signed brake impulse cannot exceed torque times ONE native step");
        const auto normal=result.normal_impulse[i];
        if(normal<1e-10) {
            check(std::abs(result.longitudinal_impulse[i])+std::abs(result.lateral_impulse[i])<1e-8,
                "zero load cannot invent brake traction");continue;
        }
        double ellipse=0;
        if(tuning.friction.longitudinal_friction>0)ellipse+=std::pow(result.longitudinal_impulse[i]/(tuning.friction.longitudinal_friction*normal),2);
        else check(std::abs(result.longitudinal_impulse[i])<1e-8,"zero longitudinal friction remains zero under braking");
        if(tuning.friction.lateral_friction>0)ellipse+=std::pow(result.lateral_impulse[i]/(tuning.friction.lateral_friction*normal),2);
        else check(std::abs(result.lateral_impulse[i])<1e-8,"zero lateral friction remains zero under braking");
        check(ellipse<=1+1e-7,"coupled brake/contact impulse stays inside the combined friction ellipse");
    }
    (void)fixture;
}
void brake_conservation(const Fixture& fixture,const c::Result& result,c::Tuning tuning,double brake) {
    check(result.valid,"conservation fixture produces a valid bounded brake result");
    const auto after=advance_measured(fixture.body,result,tuning.gravity);
    t::Vec linear{},external_moment{};
    for(unsigned i=0;i<2;++i) {
        const auto& sample=fixture.contacts[i];
        if(sample.contact.status!=t::ContactStatus::hit)continue;
        t::Basis basis;check(t::contact_basis(sample.contact.normal,sample.contact.forward,basis),"oracle contact basis valid");
        const auto impulse=add(add(mul(basis.normal,result.normal_impulse[i]),mul(basis.forward,result.longitudinal_impulse[i])),mul(basis.lateral,result.lateral_impulse[i]));
        linear=add(linear,impulse);external_moment=add(external_moment,cross(sample.contact.point,impulse));
    }
    const auto gravity_impulse=mul(tuning.gravity,fixture.body.mass*h);
    vector_near(mul(sub(after.linear_velocity,fixture.body.linear_velocity),fixture.body.mass),add(linear,gravity_impulse),
        "body linear response equals measured contact plus external gravity impulse",2e-6);
    const auto actual=sub(total_momentum(after,result.wheels,fixture.contacts),total_momentum(fixture.body,fixture.wheels,fixture.contacts));
    vector_near(actual,add(external_moment,cross(fixture.body.center_of_mass,gravity_impulse)),
        "full world angular momentum includes orbital COM, body tensor and BOTH absolute rotors",2e-6);
    auto free=fixture.body;free.linear_velocity=add(free.linear_velocity,mul(tuning.gravity,h));
    check(energy(after,result.wheels)<=energy(free,fixture.wheels)+1e-6,
        "stationary-ground brake/contact solve is passive relative to the gravity-predicted measured state");
    brake_budget_and_friction(fixture,result,tuning,brake);
}
void report_joint_brake_normal_residuals() {
    c::Controls command;command.brake=1;
    for(unsigned iterations:{48u,64u,96u,128u})for(double speed:{0.,.02,.8,2.4505030376230863}) {
        auto tuning=passive_brake_settings();tuning.brake_iterations=iterations;
        Fixture fixture;fixture.rolling(speed);c::Controller controller(tuning);
        const auto result=controller.step(h,fixture.body,fixture.wheels,fixture.contacts,command);
        check(result.valid,"normal residual diagnostic starts from a valid measured state");
        const auto after=advance_measured(fixture.body,result,tuning.gravity);
        std::cout<<"active brake iterations="<<iterations<<" speed="<<speed<<" vout="<<after.linear_velocity[2]
            <<" forceZ="<<result.force[2]<<" pitchrate="<<after.angular_velocity[0]<<" vn=";
        for(unsigned i=0;i<2;++i) {
            const auto normal_velocity=dot(t::point_velocity(after,fixture.contacts[i].contact.point),fixture.contacts[i].contact.normal);
            std::cout<<normal_velocity<<',';
            if(iterations>=96)check(std::abs(normal_velocity)<1e-6,"converged brake support has no material own-normal closing or opening residual");
        }
        if(iterations>=96)check(std::abs(after.angular_velocity[0])<1e-6,"converged flat braking does not inject pitch angular velocity");
        std::cout<<" load="<<result.normal_load[0]<<','<<result.normal_load[1]<<" lambda="<<result.brake_impulse[0]<<','<<result.brake_impulse[1]<<'\n';
    }
}
void direct_helper_clear_and_invalid_hit_contract() {
    Fixture fixture;fixture.air();fixture.wheels[0].angular_speed=8;
    auto body=fixture.body;auto wheel=fixture.wheels[0];auto contact=fixture.contacts[0].contact;
    contact.point[0]=std::numeric_limits<double>::quiet_NaN();contact.surface_velocity[0]=std::numeric_limits<double>::quiet_NaN();
    const auto free=c::coupled_brake::solve(h,body,wheel,contact,60,{});
    check(free.contact.valid&&wheel.angular_speed<fixture.wheels[0].angular_speed,
          "clear helper uses only wheel plane and correctly ignores non-geometric point/platform fields");
    vector_near(body.linear_velocity,fixture.body.linear_velocity,"clear helper cannot create a linear brake force");
    body=fixture.body;wheel=fixture.wheels[0];contact.status=t::ContactStatus::hit;
    const auto invalid=c::coupled_brake::solve(h,body,wheel,contact,60,{});
    check(!invalid.contact.valid&&body.linear_velocity==fixture.body.linear_velocity&&
          body.angular_velocity==fixture.body.angular_velocity&&wheel.angular_speed==fixture.wheels[0].angular_speed,
          "invalid actual hit is rejected without partially mutating native-ready body or wheel state");
}
void joint_brakes_have_low_speed_traction_and_static_hold() {
    const auto tuning=passive_brake_settings();c::Controls command;command.brake=1;
    for(double speed:{-.8,.8}) {
        Fixture fixture;fixture.rolling(speed);c::Controller controller(tuning);
        const auto result=controller.step(h,fixture.body,fixture.wheels,fixture.contacts,command);
        brake_conservation(fixture,result,tuning,1);
        check(-std::copysign(1.0,speed)*result.force[2]>280,
              "available60/45Nm brakes transmit nearly full torque through rolling contact below the old free-rotor clamp");
    }
    for(double speed:{-.02,.02}) {
        Fixture fixture;fixture.rolling(speed);c::Controller controller(tuning);
        double distance=0;
        for(unsigned i=0;i<12;++i) {
            const auto before=fixture.body;
            const auto result=controller.step(h,before,fixture.wheels,fixture.contacts,command);
            check(result.valid,"held low-speed brake remains valid");
            brake_budget_and_friction(fixture,result,tuning,1);
            const auto after=advance_measured(before,result,tuning.gravity);
            check(after.linear_velocity[2]*speed>=-1e-6,"brake constraint cannot reverse initially rolling forward or fakie travel");
            check(energy(after,result.wheels)<=energy(before,fixture.wheels)+1e-5,"held flat brake cannot add kinetic energy");
            distance+=std::abs(after.linear_velocity[2])*h;fixture.body=after;fixture.wheels=result.wheels;
        }
        check(std::abs(fixture.body.linear_velocity[2])<.001&&distance<.003,
              "low-speed held brake locks within.2s and millimeters instead of a free-rotor exponential coasting tail");
    }
    Fixture rest;c::Controller controller(tuning);
    for(unsigned i=0;i<20;++i) {
        const auto result=controller.step(h,rest.body,rest.wheels,rest.contacts,command);
        check(result.valid&&length(result.force)<1100&&std::abs(result.force[2])<1e-6,"static held brake cannot create longitudinal drive at rest");
        rest.body=advance_measured(rest.body,result,tuning.gravity);rest.wheels=result.wheels;
        check(std::abs(rest.body.linear_velocity[2])<1e-8,"stationary lock remains stationary without a velocity clamp");
    }
}
void brake_air_zero_friction_rotated_and_platform_cases() {
    auto tuning=passive_brake_settings();c::Controls command;command.brake=.8;
    Fixture fixture;fixture.body.center_of_mass={12,3,-7};
    fixture.body.orientation={.2,.3,-.1,.9};double n=0;for(double v:fixture.body.orientation)n+=v*v;for(auto& v:fixture.body.orientation)v/=std::sqrt(n);
    const auto frame=axes(fixture.body.orientation);
    fixture.body.linear_velocity=add(mul(frame[1],-1.2),add(mul(frame[2],2),mul(frame[0],.3)));
    fixture.body.angular_velocity={.2,-.3,.4};fixture.wheels[0].angular_speed=7;fixture.wheels[1].angular_speed=-4;
    for(unsigned i=0;i<2;++i) {
        auto& contact=fixture.contacts[i].contact;contact.normal=frame[1];contact.forward=frame[2];
        contact.point=add(sub(fixture.body.center_of_mass,mul(frame[1],.9)),mul(frame[2],i==0?.575:-.475));
    }
    tuning.friction={.7,.5};c::Controller rotated(tuning);
    const auto result=rotated.step(h,fixture.body,fixture.wheels,fixture.contacts,command);
    brake_conservation(fixture,result,tuning,.8);
    auto air=fixture;air.air();c::Controller airborne(tuning);
    const auto free=airborne.step(h,air.body,air.wheels,air.contacts,command);
    brake_conservation(air,free,tuning,.8);vector_near(free.force,{},"air braking has no chassis linear force");
    Fixture corotate;corotate.air();corotate.body.angular_velocity={5,0,0};for(auto& wheel:corotate.wheels)wheel.angular_speed=5;
    c::Controller lock(tuning);const auto no_relative=lock.step(h,corotate.body,corotate.wheels,corotate.contacts,command);
    check(no_relative.valid&&std::abs(no_relative.brake_impulse[0])+std::abs(no_relative.brake_impulse[1])<1e-12,
          "rigidly co-rotating wheel and chassis do not receive a false brake couple");
    for(auto friction:{t::Parameters{0,0},t::Parameters{0,.7},t::Parameters{.7,0}}) {
        Fixture ground;ground.rolling(.8);ground.body.linear_velocity[0]=.3;tuning.friction=friction;
        c::Controller controller(tuning);const auto r=controller.step(h,ground.body,ground.wheels,ground.contacts,command);
        brake_conservation(ground,r,tuning,.8);
        if(friction.longitudinal_friction==0)check(std::abs(r.force[2])<1e-7,"frictionless longitudinal tire cannot brake chassis travel");
        if(friction.lateral_friction==0)check(std::abs(r.force[0])<1e-7,"zero lateral grip does not leak into a brake force");
    }
    for(const auto torques:{std::array<double,2>{0,45},std::array<double,2>{60,0},std::array<double,2>{0,0}}) {
        Fixture ground;ground.rolling(.8);auto partial=tuning;partial.friction={.8,.6};
        partial.front_brake_torque=torques[0];partial.rear_brake_torque=torques[1];
        c::Controller controller(partial);const auto r=controller.step(h,ground.body,ground.wheels,ground.contacts,command);
        brake_conservation(ground,r,partial,.8);
        for(unsigned i=0;i<2;++i)if(torques[i]==0)check(r.brake_impulse[i]==0,"zero-torque wheel is a valid contact with no brake impulse");
        if(torques[0]==0&&torques[1]==0) {
            auto released=command;released.brake=0;c::Controller ordinary(partial);
            const auto coast=ordinary.step(h,ground.body,ground.wheels,ground.contacts,released);
            check(coast.valid&&r.force==coast.force&&r.torque==coast.torque&&r.normal_impulse==coast.normal_impulse&&
                  r.longitudinal_impulse==coast.longitudinal_impulse&&r.lateral_impulse==coast.lateral_impulse,
                  "both zero brake budgets preserve the exact original unbraked contact path");
            for(unsigned i=0;i<2;++i)check(r.wheels[i].angular_speed==coast.wheels[i].angular_speed,"zero budgets preserve original rotor outputs exactly");
        }
    }
    Fixture original;original.rolling(.8);auto moving=original;const t::Vec boost{2,0,4};
    moving.body.linear_velocity=add(moving.body.linear_velocity,boost);for(auto& sample:moving.contacts)sample.contact.surface_velocity=boost;
    tuning.friction={.8,.6};c::Controller a(tuning),b(tuning);
    const auto static_result=a.step(h,original.body,original.wheels,original.contacts,command);
    const auto moving_result=b.step(h,moving.body,moving.wheels,moving.contacts,command);
    check(static_result.valid&&moving_result.valid,"moving platform brake scenarios valid");
    vector_near(static_result.force,moving_result.force,"braking is invariant under shared ground/chassis translation",2e-7);
    vector_near(static_result.torque,moving_result.torque,"moving ground does not change brake reaction torque",2e-7);
    for(unsigned i=0;i<2;++i)check(near(static_result.wheels[i].angular_speed,moving_result.wheels[i].angular_speed)&&
        near(static_result.brake_impulse[i],moving_result.brake_impulse[i]),"platform shift leaves rotors and static brake multipliers unchanged");
    auto after=advance_measured(moving.body,moving_result,tuning.gravity);after.linear_velocity=sub(after.linear_velocity,boost);
    auto before=moving.body;before.linear_velocity=sub(before.linear_velocity,boost);
    check(energy(after,moving_result.wheels)<=energy(before,moving.wheels)+1e-5,"braking is passive in moving-ground rest frame");
}
struct StopMetrics {double distance{},minimum_speed{},final_speed{},seconds{};};
template<class Step>
StopMetrics flat_stop_replay(Step&& step) {
    Fixture fixture;fixture.rolling(2.4505030376230863);StopMetrics metrics;metrics.minimum_speed=2.4505030376230863;
    double distance=0;
    for(unsigned i=0;i<400;++i) {
        const auto before=fixture.body;const auto result=step(fixture);
        check(result.valid,"native-like flat velocity-contact stopping replay stays valid");
        const auto after=advance_measured(before,result,{0,-9.8,0});
        distance+=after.linear_velocity[2]*h;metrics.distance=std::max(metrics.distance,distance);
        metrics.minimum_speed=std::min(metrics.minimum_speed,after.linear_velocity[2]);
        metrics.final_speed=std::abs(after.linear_velocity[2]);metrics.seconds=(i+1)*h;
        fixture.body=after;fixture.wheels=result.wheels;
        check(after.linear_velocity[2]>=-1e-5,"forward stopping replay cannot create a reverse-travel overshoot");
        if(std::abs(after.linear_velocity[2])<1e-4&&length(after.angular_velocity)<.01)break;
    }
    return metrics;
}
void bounded_iterations_native_stop_and_motor_once() {
    c::Controls brake;brake.brake=1;StopMetrics ninety_six{},one_twenty_eight{};
    for(unsigned iterations:{1u,4u,12u,24u,32u}) {
        auto tuning=passive_brake_settings();tuning.contact_iterations=iterations;tuning.brake_iterations=iterations;
        Fixture fixture;fixture.rolling(.8);c::Controller controller(tuning);
        const auto result=controller.step(h,fixture.body,fixture.wheels,fixture.contacts,brake);
        check(result.valid,"each supported joint-constraint iteration count is bounded and valid");
        brake_budget_and_friction(fixture,result,tuning,1);
        fixture.air();c::Controls pedal;pedal.pedal=.4;c::Controller motor(tuning);
        const auto driven=motor.step(h,fixture.body,fixture.wheels,fixture.contacts,pedal);
        check(driven.valid&&near((driven.wheels[1].angular_speed-fixture.wheels[1].angular_speed)*fixture.wheels[1].inertia,30*h),
              "motor axle impulse is applied once and is not multiplied by contact iterations");
    }
    for(unsigned iterations:{48u,64u,96u,128u}) {
        auto tuning=passive_brake_settings();tuning.brake_iterations=iterations;c::Controller stopping(tuning);
        const auto stopped=flat_stop_replay([&](const Fixture& f){return stopping.step(h,f.body,f.wheels,f.contacts,brake);});
        if(iterations==96)ninety_six=stopped;
        if(iterations==128)one_twenty_eight=stopped;
        std::cout<<"joint stop active iterations="<<iterations<<" distance="<<stopped.distance<<" min_speed="<<stopped.minimum_speed<<" final="<<stopped.final_speed<<" seconds="<<stopped.seconds<<'\n';
    }
    check(ninety_six.distance<1.173,
          "joint brakes stop native2.4505mps velocity-contact scenario inside the original1.173m runout");
    check(std::abs(ninety_six.distance-one_twenty_eight.distance)<.025,
          "default96active-brake stopping distance is close to128iterations without hiding finite solver residual");
}
void group_capping_preserves_brake_contact_rotor_response() {
    Fixture fixture;fixture.body.linear_velocity={.6,-4.3,2};fixture.body.angular_velocity={.1,-.2,.05};
    fixture.wheels[0].angular_speed=5;fixture.wheels[1].angular_speed=-3;
    auto high=passive_brake_settings();high.friction={.8,.6};high.force_limit=high.torque_limit=1e6;
    auto low=high;low.force_limit=5000;low.torque_limit=200;
    c::Controls brake;brake.brake=1;c::Controller a(high),b(low);
    const auto full=a.step(h,fixture.body,fixture.wheels,fixture.contacts,brake);
    const auto capped=b.step(h,fixture.body,fixture.wheels,fixture.contacts,brake);
    check(full.valid&&capped.valid&&near(full.contact_scale,1)&&capped.contact_scale>0&&capped.contact_scale<1,
          "impact fixture genuinely exercises shared brake/contact fraction rather than coefficient clipping");
    const auto fraction=capped.contact_scale;
    vector_near(capped.linear_impulse,mul(full.linear_impulse,fraction),"capped linear contact and brake response share one fraction",1e-7);
    vector_near(capped.angular_impulse,mul(full.angular_impulse,fraction),"capped body angular reaction includes the same brake fraction",1e-7);
    for(unsigned i=0;i<2;++i) {
        check(near(capped.brake_impulse[i],fraction*full.brake_impulse[i],1e-8),"actual signed brake impulse is attenuated with contact wrench");
        check(near(capped.wheels[i].angular_speed,fixture.wheels[i].angular_speed+fraction*(full.wheels[i].angular_speed-fixture.wheels[i].angular_speed),1e-8),
              "shared cap includes ALL contact and brake rotor effects, not just the last contact spin");
    }
    brake_conservation(fixture,capped,low,1);
    Fixture powered=fixture;c::Controls both;both.pedal=.4;both.brake=1;
    auto motor_fixture=fixture;motor_fixture.air();c::Controls motor_cmd;motor_cmd.pedal=.4;
    c::Controller base_controller(high),full_controller(high),cap_controller(low);
    const auto base=base_controller.step(h,motor_fixture.body,motor_fixture.wheels,motor_fixture.contacts,motor_cmd);
    const auto driven_full=full_controller.step(h,powered.body,powered.wheels,powered.contacts,both);
    const auto driven_cap=cap_controller.step(h,powered.body,powered.wheels,powered.contacts,both);
    check(base.valid&&driven_full.valid&&driven_cap.valid&&driven_cap.contact_scale<1,"powered capped-brake fixture valid");
    const auto s=driven_cap.contact_scale;
    vector_near(sub(driven_cap.angular_impulse,base.angular_impulse),mul(sub(driven_full.angular_impulse,base.angular_impulse),s),
                "contact cap preserves one-time unscaled motor baseline while sharing brake response",1e-7);
    for(unsigned i=0;i<2;++i)check(near(driven_cap.wheels[i].angular_speed,base.wheels[i].angular_speed+s*(driven_full.wheels[i].angular_speed-base.wheels[i].angular_speed),1e-8),
          "powered capped wheel keeps motor once and scales the complete contact/brake delta");
}
}

int main() {
    report_joint_brake_normal_residuals();direct_helper_clear_and_invalid_hit_contract();
    joint_brakes_have_low_speed_traction_and_static_hold();brake_air_zero_friction_rotated_and_platform_cases();
    bounded_iterations_native_stop_and_motor_once();group_capping_preserves_brake_contact_rotor_response();
    std::cout<<"BMX coupled brake constraints, static lock, passivity, momentum, friction, bounded iterations and native-scenario stop tests passed.\n";
}
