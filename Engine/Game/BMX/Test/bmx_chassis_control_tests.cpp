#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

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
t::Body response(const t::Body& before,const c::Result& r) {
    auto b=before;
    b.linear_velocity=add(add(before.linear_velocity,{0,-9.8*h,0}),mul(add(r.linear_impulse,r.hop_impulse),1/b.mass));
    b.angular_velocity=add(before.angular_velocity,tensor(before,r.angular_impulse,true));return b;
}
double energy(const t::Body& b,const std::array<t::Wheel,2>& wheels) {
    double e=.5*b.mass*dot(b.linear_velocity,b.linear_velocity)+.5*dot(b.angular_velocity,tensor(b,b.angular_velocity,false));
    for(const auto& w:wheels)e+=.5*w.inertia*w.angular_speed*w.angular_speed;
    return e;
}
void support_and_gravity() {
    Fixture f;c::Controller controller;
    const auto r=controller.step(h,f.body,f.wheels,f.contacts);
    check(r.valid&&r.grounded,"two verified wheel supports produce a valid grounded solve");
    check(near(r.force[1],f.body.mass*9.8,.002),"rest support force balances native gravity exactly once");
    check(length(response(f.body,r).linear_velocity)<.00001&&length(response(f.body,r).angular_velocity)<.00001,
        "rest contact iterations leave negligible linear and angular drift");
    check(near(r.normal_load[0]+r.normal_load[1],f.body.mass*9.8,.002)&&r.normal_load[0]>0&&r.normal_load[1]>0,
        "contact loads are solved normal impulse per native step, not assumed half-weight");
    check(r.normal_load[1]>r.normal_load[0],"rearward measured COM gives greater rear load");
    auto light=f;light.body.mass/=2;for(auto& moment:light.body.inertia_body)moment/=2;
    const auto lighter=controller.step(h,light.body,light.wheels,light.contacts);
    check(lighter.valid&&near(lighter.force[1],light.body.mass*9.8,.002),"measured native mass and inertia replace proposed first-tune defaults");
    f.air();f.body.linear_velocity={1,2,3};f.body.angular_velocity={.2,-.4,.7};
    f.body.orientation={.5,.5,.5,.5};
    for(auto& sample:f.contacts) {sample.contact.normal={0,0,1};sample.contact.forward={1,0,0};}
    const auto air=controller.step(h,f.body,f.wheels,f.contacts);
    check(air.valid&&!air.grounded,"known-clear wheels are a valid airborne state");
    vector_near(air.force,{},"air cannot submit native gravity for a second time");
    vector_near(air.torque,{},"air has no automatic upright or angular damping torque");
    vector_near(response(f.body,air).angular_velocity,f.body.angular_velocity,"air coast preserves measured angular momentum");
}
void steering_and_platforms() {
    // Independent reflected/platform initial states need equivalent fresh steering history.
    Fixture f;f.rolling(5);c::Controls cmd;cmd.steer=1;
    c::Controller controller;const auto right=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(right.valid&&right.front_steer_angle<0&&right.force[0]<0&&right.torque[1]<0,
        "rider-right front steering creates negative native yaw and lateral force, not a yaw setter");
    cmd.steer=-1;const auto left=c::Controller{}.step(h,f.body,f.wheels,f.contacts,cmd);
    check(left.valid&&left.front_steer_angle>0&&left.force[0]>0&&left.torque[1]>0,"rider-left steering physically mirrors yaw response");
    check(near(right.force[0],-left.force[0])&&near(right.torque[1],-left.torque[1]),"steering forces respect left/right reflection");
    const t::Vec boost{3,0,7};auto moving=f;
    moving.body.linear_velocity=add(moving.body.linear_velocity,boost);
    for(auto& sample:moving.contacts)sample.contact.surface_velocity=boost;
    cmd.steer=1;const auto platform=c::Controller{}.step(h,moving.body,moving.wheels,moving.contacts,cmd);
    check(platform.valid,"moving support velocity is accepted");
    vector_near(platform.force,right.force,"moving-ground contact and speed-sensitive steering use relative velocity");
    vector_near(platform.torque,right.torque,"shared platform/body boost leaves balance and contact torque invariant");
}
// The fork's axis is tilted: independently project its rotated heading onto the
// actual support plane, then compare centripetal demand against available lean.
// This checks the riding outcome, rather than expecting the clamp's formula.
double projected_steering(t::Vec forward,t::Vec axis,double deflection,t::Vec normal) {
    axis=mul(axis,1/length(axis));
    const auto turned=add(add(mul(forward,std::cos(deflection)),mul(cross(axis,forward),std::sin(deflection))),
        mul(axis,dot(axis,forward)*(1-std::cos(deflection))));
    const auto projected=sub(turned,mul(normal,dot(turned,normal)));
    const auto unit=mul(projected,1/length(projected));
    return std::atan2(dot(normal,cross(forward,unit)),dot(forward,unit));
}
void steering_matches_lean_envelope() {
    c::Tuning tuning;
    for(double yaw:{0.,1.2,-2.3})for(double side:{-1.,1.})for(double speed:{-12.,-8.,-2.,0.,2.,8.,12.}) {
        Fixture f;f.rolling(speed);f.body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        const auto a=axes(f.body.orientation);
        const auto turn=[&](t::Vec v){return add(add(mul(a[0],v[0]),mul(a[1],v[1])),mul(a[2],v[2]));};
        f.body.center_of_mass=turn(f.body.center_of_mass);f.body.linear_velocity=turn(f.body.linear_velocity);
        for(auto& sample:f.contacts){sample.contact.point=turn(sample.contact.point);sample.contact.forward=a[2];}
        c::Controls input;input.steer=side;c::Controller controller;
        const auto solved=controller.step(h,f.body,f.wheels,f.contacts,input);
        check(solved.valid&&solved.grounded,"ground turn with either travel direction and world heading remains valid");
        const auto steer_axis=turn({dingosdk::bmx::geometry::steering_axis[0],dingosdk::bmx::geometry::steering_axis[1],
            dingosdk::bmx::geometry::steering_axis[2]});
        const double heading=projected_steering(a[2],steer_axis,solved.front_steer_angle,{0,1,0});
        const double lateral_acceleration=speed*speed*std::tan(heading)/tuning.wheelbase;
        const double required_lean=std::atan2(std::abs(lateral_acceleration),9.8);
        check(required_lean<=tuning.lean_limit+1e-10,"actual caster-projected curvature is achievable within the riding lean envelope");
        check(near(std::abs(solved.turning_lean_angle),required_lean,1e-10),"balance target follows the same projected turn as the physical front tire");
        check(solved.front_steer_angle*side<=0&&std::abs(solved.front_steer_angle)<=std::abs(solved.requested_front_steer_angle),
            "limit preserves steering direction and never amplifies input");
        if(std::abs(speed)<=2)check(!solved.steering_limited&&solved.front_steer_angle==solved.requested_front_steer_angle,
            "tight low-speed maneuvering keeps the existing full fork deflection");
        if(std::abs(speed)>=8) {
            const double prior_heading=projected_steering(a[2],steer_axis,solved.requested_front_steer_angle,{0,1,0});
            const double old_lean=std::atan2(speed*speed*std::abs(std::tan(prior_heading))/tuning.wheelbase,9.8);
            check(old_lean>tuning.lean_limit+.15&&solved.steering_limited,
                "counterexample demonstrates the old tire turn demanded materially more lean than its balance controller allowed");
            check(required_lean>=tuning.lean_limit-1e-8,"high-speed limit retains all turning authority supported by the existing lean envelope");
        }
        auto air=f;air.air();c::Controller flight;const auto airborne=flight.step(h,air.body,air.wheels,air.contacts,input);
        check(airborne.valid&&!airborne.steering_limited&&airborne.front_steer_angle==airborne.requested_front_steer_angle,
            "airborne bar deflection and air-yaw input are not restricted by the ground-turn envelope");
    }
    // Same demand and response under a moving platform boost.
    Fixture stationary;stationary.rolling(8);c::Controls input;input.steer=1;
    const auto base=c::Controller{}.step(h,stationary.body,stationary.wheels,stationary.contacts,input);
    auto moving=stationary;const t::Vec boost{3,1,11};moving.body.linear_velocity=add(moving.body.linear_velocity,boost);
    for(auto& sample:moving.contacts)sample.contact.surface_velocity=boost;
    const auto platform=c::Controller{}.step(h,moving.body,moving.wheels,moving.contacts,input);
    check(base.valid&&platform.valid&&near(base.front_steer_angle,platform.front_steer_angle)&&
        near(base.turning_lean_angle,platform.turning_lean_angle),"lean-limited steering remains relative to measured support velocity");
    vector_near(base.force,platform.force,"moving support boost leaves bounded-turn tire forces invariant");
    vector_near(base.torque,platform.torque,"moving support boost leaves bounded-turn torques invariant");
}
void sustained_turns_remain_ridable() {
    // Independent semi-implicit rigid-body bench on an exact horizontal plane.
    // Advance only the returned impulses, native gravity and Euler gyro term;
    // there is no upright/height correction or backup capsule collision here.
    // Run115's controller overturned completely at8m/s in this same10s case.
    for(double speed:{-8.,8.})for(double steering:{-1.,1.})for(double yaw:{0.,1.2}) {
        auto body=c::proposed_body();body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        body.center_of_mass={0,.9,-.05};body.linear_velocity=mul(axes(body.orientation)[2],speed);
        auto wheels=c::proposed_wheels();for(auto& wheel:wheels)wheel.angular_speed=speed/wheel.radius;
        c::Tuning tuning;tuning.contact_gap=.03;c::Controller controller(tuning);
        double last_steer=0,min_up=1,min_separation=0,max_angular_speed=0;
        unsigned unsupported=0;
        for(unsigned tick=0;tick<600;++tick) {
            const auto a=axes(body.orientation);
            const auto turn=[&](t::Vec v){return add(add(mul(a[0],v[0]),mul(a[1],v[1])),mul(a[2],v[2]));};
            std::array<c::ContactSample,2> contacts{};
            for(unsigned wheel=0;wheel<2;++wheel) {
                const auto model_hub=dingosdk::bmx::geometry::wheel_hub(wheel==0,static_cast<float>(last_steer));
                const auto hub=add(body.center_of_mass,turn({model_hub[0],model_hub[1]-.9,model_hub[2]+.05}));
                contacts[wheel].contact.point={hub[0],0,hub[2]};contacts[wheel].contact.normal={0,1,0};
                contacts[wheel].contact.forward=a[2];contacts[wheel].separation=hub[1]-dingosdk::bmx::geometry::wheel_radius;
                contacts[wheel].contact.status=contacts[wheel].separation<=.03?t::ContactStatus::hit:t::ContactStatus::clear;
                min_separation=std::min(min_separation,contacts[wheel].separation);
            }
            c::Controls input;input.steer=steering*std::min(tick/60.,1.);
            const auto output=controller.step(h,body,wheels,contacts,input);
            check(output.valid,"sustained forward/fakie turn keeps all contact/controller samples valid");
            unsupported+=!output.grounded;wheels=output.wheels;last_steer=output.front_steer_angle;
            const auto angular_momentum=tensor(body,body.angular_velocity,false);
            body.linear_velocity=add(body.linear_velocity,add(mul(output.linear_impulse,1/body.mass),{0,-9.8*h,0}));
            body.center_of_mass=add(body.center_of_mass,mul(body.linear_velocity,h));
            body.angular_velocity=add(body.angular_velocity,tensor(body,
                sub(output.angular_impulse,mul(cross(body.angular_velocity,angular_momentum),h)),true));
            auto q=body.orientation;const t::Vec omega{q[0],q[1],q[2]},half_step=mul(body.angular_velocity,h*.5);
            const auto vector_delta=add(mul(half_step,q[3]),cross(half_step,omega));
            q={q[0]+vector_delta[0],q[1]+vector_delta[1],q[2]+vector_delta[2],q[3]-dot(half_step,omega)};
            double norm=0;for(double component:q)norm+=component*component;
            for(auto& component:q)component/=std::sqrt(norm);
            body.orientation=q;
            min_up=std::min(min_up,axes(q)[1][1]);max_angular_speed=std::max(max_angular_speed,length(body.angular_velocity));
        }
        check(min_up>.90&&unsupported==0&&min_separation>-.015&&max_angular_speed<1.2,
            "ten-second mirrored forward/fakie8m/s turns retain wheel support and stay upright without a pose repair");
        check(length(body.linear_velocity)>7.5&&std::abs(body.angular_velocity[1])>.5,
            "stable turning comes from usable curvature while retaining travel speed, not stopping or disabling steering");
    }
}
void steering_release_settles_without_countersteer_wobble() {
    // Reuse the independent world-tensor/gyro integration from the sustained
    // turn fixture. Real tire geometry is re-queried at the production gap;
    // the only height seed is the initial nonpenetrating placement.
    for(double speed:{-8.,8.})for(double steering:{-1.,1.})for(double yaw:{0.,1.2}) {
        auto body=c::proposed_body();body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        body.center_of_mass={0,.9,-.05};body.linear_velocity=mul(axes(body.orientation)[2],speed);
        double initial_gap=1e9;
        for(unsigned wheel=0;wheel<2;++wheel) {
            const auto hub=dingosdk::bmx::geometry::wheel_hub(wheel==0);
            initial_gap=std::min(initial_gap,hub[1]-dingosdk::bmx::geometry::wheel_radius);
        }
        body.center_of_mass[1]-=initial_gap;
        auto wheels=c::proposed_wheels();for(auto& wheel:wheels)wheel.angular_speed=speed/wheel.radius;
        c::Tuning tuning;c::Controller controller(tuning);
        double last_steer=0,min_up=1,min_gap=0,max_rate=0,opposite_lean=0,settled_after=-1,release_time=-1;
        unsigned unsupported=0,settled_ticks=0;
        for(unsigned tick=0;tick<360;++tick) {
            const auto a=axes(body.orientation);
            const auto turn=[&](t::Vec v){return add(add(mul(a[0],v[0]),mul(a[1],v[1])),mul(a[2],v[2]));};
            std::array<c::ContactSample,2> contacts{};
            for(unsigned wheel=0;wheel<2;++wheel) {
                const auto hub_model=dingosdk::bmx::geometry::wheel_hub(wheel==0,static_cast<float>(last_steer));
                const auto hub=add(body.center_of_mass,turn({hub_model[0],hub_model[1]-.9,hub_model[2]+.05}));
                auto& sample=contacts[wheel];sample.contact.point={hub[0],0,hub[2]};
                sample.contact.normal=sample.balance_normal={0,1,0};sample.contact.forward=a[2];
                sample.verified_plane=sample.balance_normal_available=true;
                sample.separation=hub[1]-dingosdk::bmx::geometry::wheel_radius;
                sample.contact.status=sample.separation<=tuning.contact_gap?t::ContactStatus::hit:t::ContactStatus::clear;
                min_gap=std::min(min_gap,sample.separation);
            }
            const double elapsed=tick*h;c::Controls input;input.recovery_step_verified=true;
            input.steer=elapsed<2.5?steering*std::min(elapsed,1.):0;
            if(elapsed>=2.5&&release_time<0)release_time=elapsed;
            const auto output=controller.step(h,body,wheels,contacts,input);
            check(output.valid,"steering release keeps the physical contact/controller solve valid");
            unsupported+=!output.grounded;wheels=output.wheels;last_steer=output.front_steer_angle;
            const auto momentum=tensor(body,body.angular_velocity,false);
            body.linear_velocity=add(body.linear_velocity,add(mul(output.linear_impulse,1/body.mass),{0,-9.8*h,0}));
            body.center_of_mass=add(body.center_of_mass,mul(body.linear_velocity,h));
            body.angular_velocity=add(body.angular_velocity,tensor(body,
                sub(output.angular_impulse,mul(cross(body.angular_velocity,momentum),h)),true));
            auto q=body.orientation;const t::Vec vector{q[0],q[1],q[2]},half_step=mul(body.angular_velocity,h*.5);
            const auto delta=add(mul(half_step,q[3]),cross(half_step,vector));
            q={q[0]+delta[0],q[1]+delta[1],q[2]+delta[2],q[3]-dot(half_step,vector)};
            double norm=0;for(double component:q)norm+=component*component;
            for(auto& component:q)component/=std::sqrt(norm);body.orientation=q;
            const auto after=axes(q);const auto planar=t::Vec{after[2][0],0,after[2][2]};
            const auto forward=mul(planar,1/length(planar));
            const double lean=std::atan2(dot(cross({0,1,0},forward),after[1]),after[1][1]);
            min_up=std::min(min_up,after[1][1]);max_rate=std::max(max_rate,length(body.angular_velocity));
            if(release_time>=0) {
                opposite_lean=std::max(opposite_lean,steering*lean);
                if(std::abs(lean)<.01&&length(body.angular_velocity)<.1) {
                    ++settled_ticks;if(settled_ticks==30&&settled_after<0)settled_after=elapsed+h-release_time-29*h;
                } else settled_ticks=0;
            }
        }
        check(opposite_lean<.05,"8m/s steering release must not overshoot upright by more than .05rad");
        check(settled_after>=0&&settled_after<2.5,"released steering settles within2.5seconds without a pose repair");
        check(min_up>.90&&unsupported==0&&min_gap>-.015&&max_rate<1.2,
            "release preserves the original steady-turn upright, support, penetration and angular bounds");
        check(length(body.linear_velocity)>7.5,"release stability retains riding speed instead of stopping the bike");
    }
}
void supported_steering_history_contract() {
    Fixture f;c::Controls input;c::Controller controller;
    check(controller.step(h,f.body,f.wheels,f.contacts,input).valid,"neutral step establishes a valid published bar angle");
    input.steer=1;
    const auto turn=controller.step(h,f.body,f.wheels,f.contacts,input);
    check(turn.valid&&!turn.steering_limited&&turn.front_steer_angle<0&&
        std::abs(turn.front_steer_angle)<std::abs(turn.requested_front_steer_angle)*.2,
        "warm neutral-to-full steering advances a mechanical command instead of snapping to full bars");
    input.steer=-1;
    const auto reversed=controller.step(h,f.body,f.wheels,f.contacts,input);
    check(reversed.valid&&reversed.front_steer_angle>turn.front_steer_angle&&
        std::abs(reversed.front_steer_angle-turn.front_steer_angle)<.04,
        "warm reversal advances toward the new request in a bounded step");
    input.steer=0;
    const auto released=controller.step(h,f.body,f.wheels,f.contacts,input);
    check(released.valid&&std::abs(released.front_steer_angle)<.04,
        "centering the stick returns the same physical steering trajectory");

    // The turn envelope outranks rate limiting when available curvature shrinks.
    c::Controller envelope;input.steer=1;
    const auto low=envelope.step(h,f.body,f.wheels,f.contacts,input);
    auto fast=f;fast.rolling(20);input.steer=-1;
    const auto limited=envelope.step(h,fast.body,fast.wheels,fast.contacts,input);
    check(low.valid&&limited.valid&&limited.steering_limited&&limited.front_steer_angle<0&&
        limited.requested_front_steer_angle>0&&std::abs(limited.turning_lean_angle)<=.470000001,
        "reversal envelope clips the filtered candidate without flipping to the raw request sign");
    check(std::abs(limited.front_steer_angle-low.front_steer_angle)>.1,
        "changing speed/lean envelope takes priority over slew continuity rather than promising an impossible bound");

    // Deliberate air controls preserve the existing raw bars, forces and torques.
    auto air=f;air.air();air.body.angular_velocity={.3,.2,-.1};
    input.steer=-.6;input.air_yaw=.4;input.manual=.3;input.rotation_reserved=true;
    const auto warm_air=controller.step(h,air.body,air.wheels,air.contacts,input);
    const auto cold_air=c::Controller{}.step(h,air.body,air.wheels,air.contacts,input);
    check(warm_air.valid&&cold_air.valid&&near(warm_air.front_steer_angle,cold_air.front_steer_angle),
        "air bar tracking stays raw even after supported steering history");
    vector_near(warm_air.force,cold_air.force,"supported history cannot alter deliberate airborne force");
    vector_near(warm_air.torque,cold_air.torque,"supported history cannot alter deliberate airborne torque");
    vector_near(warm_air.air_control_torque,cold_air.air_control_torque,"air pitch/yaw law stays independent from ground steering");
    check(length(warm_air.balance_torque)==0,"no supported upright target is retained into real air");
    input={};input.steer=1;
    const auto recontact=controller.step(h,f.body,f.wheels,f.contacts,input);
    check(recontact.valid&&recontact.front_steer_angle>0&&
        recontact.front_steer_angle<warm_air.front_steer_angle,
        "recontact starts from the actual last raw air-published bars rather than stale ground steering");

    // An established full turn is distinct from the two-step near-neutral
    // reversal above. Reflect identical histories in forward/fakie travel and
    // rotated world frames; then reverse without resetting the controller.
    for(double speed:{-8.,8.})for(double side:{-1.,1.})for(double yaw:{0.,1.2}) {
        Fixture warm;warm.rolling(speed);
        warm.body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        const auto a=axes(warm.body.orientation);
        const auto rotate=[&](t::Vec v){return add(add(mul(a[0],v[0]),mul(a[1],v[1])),mul(a[2],v[2]));};
        warm.body.center_of_mass=rotate(warm.body.center_of_mass);
        warm.body.linear_velocity=rotate(warm.body.linear_velocity);
        for(auto& sample:warm.contacts){sample.contact.point=rotate(sample.contact.point);sample.contact.forward=a[2];}
        c::Controller established;c::Controls command;
        check(established.step(h,warm.body,warm.wheels,warm.contacts,command).valid,"mirrored warm reversal seeds neutral history");
        c::Result prior;
        command.steer=side;
        for(unsigned tick=0;tick<60;++tick)prior=established.step(h,warm.body,warm.wheels,warm.contacts,command);
        auto centered_controller=established;
        command.steer=-side;
        const auto changing=established.step(h,warm.body,warm.wheels,warm.contacts,command);
        check(prior.valid&&changing.valid&&prior.front_steer_angle*side<0&&changing.front_steer_angle*side<0&&
            changing.requested_front_steer_angle*side>0&&
            std::abs(changing.front_steer_angle)<std::abs(prior.front_steer_angle)&&
            std::abs(changing.front_steer_angle-prior.front_steer_angle)<.04,
            "warm full reversal preserves actual steering continuity across reflected speed and yaw fixtures");
        const auto steer_axis=rotate({dingosdk::bmx::geometry::steering_axis[0],dingosdk::bmx::geometry::steering_axis[1],
            dingosdk::bmx::geometry::steering_axis[2]});
        const double heading=projected_steering(a[2],steer_axis,changing.front_steer_angle,{0,1,0});
        const double expected_lean=std::atan(-speed*speed*std::tan(heading)/(9.8*c::Tuning{}.wheelbase));
        check(near(changing.turning_lean_angle,expected_lean,1e-10),
            "warm reversal derives banking from its achieved physical tire angle rather than its opposite raw request");
        command.steer=0;
        const auto centering=centered_controller.step(h,warm.body,warm.wheels,warm.contacts,command);
        check(centering.valid&&centering.front_steer_angle*side<0&&
            std::abs(centering.front_steer_angle)<std::abs(prior.front_steer_angle),
            "warm full release continues toward center without snapping the established tire turn");
        command.steer=-side;
        c::Result arrived;
        for(unsigned tick=0;tick<90;++tick)arrived=established.step(h,warm.body,warm.wheels,warm.contacts,command);
        check(arrived.valid&&arrived.front_steer_angle*side>0&&
            std::abs(arrived.turning_lean_angle)<=c::Tuning{}.lean_limit+1e-10,
            "warm reversal eventually reaches the requested mirrored turn within the original physical lean bound");
    }

    // Disabled, unavailable, wrong-clock and explicit reset cannot leak history.
    for(unsigned reset_case=0;reset_case<4;++reset_case) {
        c::Controller subject;input={};subject.step(h,f.body,f.wheels,f.contacts,input);
        input.steer=1;subject.step(h,f.body,f.wheels,f.contacts,input);
        if(reset_case==0)subject.reset();
        if(reset_case==1){auto unavailable=f;unavailable.contacts[0].contact.status=t::ContactStatus::unavailable;
            check(!subject.step(h,unavailable.body,unavailable.wheels,unavailable.contacts,input).valid,"unavailable contact refuses the step");}
        if(reset_case==2)check(!subject.step(h*2,f.body,f.wheels,f.contacts,input).valid,"wrong native clock refuses the step");
        if(reset_case==3){auto disabled=input;disabled.enabled=false;
            check(subject.step(h,f.body,f.wheels,f.contacts,disabled).valid,"disabled controls retain a valid passive solve");}
        input.steer=-1;
        const auto restarted=subject.step(h,f.body,f.wheels,f.contacts,input);
        const auto fresh=c::Controller{}.step(h,f.body,f.wheels,f.contacts,input);
        check(restarted.valid&&fresh.valid&&near(restarted.front_steer_angle,fresh.front_steer_angle),
            "cleared steering history retains explicit legacy cold-step behavior");
    }
}

void physical_drive_and_brake() {
    Fixture f;c::Controller controller;c::Controls cmd;cmd.pedal=1;
    const auto driven=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(driven.valid&&driven.force[2]>0&&driven.wheels[1].angular_speed>0,"rear axle pedal torque produces forward ground traction");
    f.air();const auto motor=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(motor.valid&&motor.wheels[0].angular_speed==0&&motor.wheels[1].angular_speed>0,"pedaling drives only the rear free rotor");
    check(near(motor.wheels[1].angular_speed*motor.wheels[1].inertia/h,75)&&near(motor.angular_impulse[0]/h,-75),
        "default75Nm pedal torque produces measured rotor acceleration and an equal opposite chassis reaction");
    vector_near(motor.force,{},"airborne pedaling cannot invent a propulsion force");
    check(near(motor.angular_impulse[0]+motor.wheels[1].inertia*motor.wheels[1].angular_speed,0),
        "free-wheel motor torque has an equal opposite chassis angular impulse");
    for(double speed:{-5.,0.,5.}) {
        f=Fixture{};f.rolling(speed);cmd={};cmd.brake=1;
        const auto stopped=controller.step(h,f.body,f.wheels,f.contacts,cmd);
        check(stopped.valid,"braking rolling/fakie/rest state is valid");
        const auto after=response(f.body,stopped);
        check(std::abs(after.linear_velocity[2])<=std::abs(speed)+1e-7&&after.linear_velocity[2]*speed>=-1e-7,
            "brakes slow forward and fakie travel without reversing at rest");
        check(energy(after,stopped.wheels)<=energy(f.body,f.wheels)+1e-6,"ground braking cannot add chassis-plus-rotor kinetic energy");
    }
    f=Fixture{};f.air();f.body.orientation={.5,.5,.5,.5};f.body.angular_velocity={.3,.4,.5};
    const auto a=axes(f.body.orientation);for(auto& sample:f.contacts){sample.contact.normal=a[1];sample.contact.forward=a[2];}
    f.wheels[0].angular_speed=20;f.wheels[1].angular_speed=-10;cmd={};cmd.brake=1;
    const auto brake=controller.step(h,f.body,f.wheels,f.contacts,cmd);check(brake.valid,"tilted-axis airborne braking is valid");
    const auto chassis_delta=brake.angular_impulse;
    t::Vec rotor_delta{};for(unsigned i=0;i<2;++i)rotor_delta=add(rotor_delta,mul(a[0],f.wheels[i].inertia*(brake.wheels[i].angular_speed-f.wheels[i].angular_speed)));
    vector_near(add(chassis_delta,rotor_delta),{},"rotated-axis braking preserves total chassis plus rotor angular momentum");
}
void hop_events_and_manual_pitch() {
    Fixture f;c::Controller controller;c::Controls cmd;cmd.hop_event_id=1;cmd.hop_strength=1;
    auto r=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(r.valid&&r.hop_applied&&r.hop_event_id==1&&near(r.hop_impulse[1],106*4.5),"new grounded event produces a bounded one-shot COM hop impulse");
    vector_near(r.angular_impulse,{},"COM hop does not invent pitch torque");
    check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"held event never repeats even if caller presents grounded state again");
    cmd.hop_event_id=2;cmd.hop_strength=.5;r=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(r.valid&&r.hop_applied&&near(r.hop_impulse[1],106*2.25),"hop strength scales actual impulse");
    cmd.hop_event_id=1;check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"older event IDs cannot replay");
    cmd.hop_event_id=3;cmd.enabled=false;check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"disabled input suppresses hop");
    cmd.enabled=true;check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"suppressed event cannot resurrect on focus return");
    f.air();cmd.hop_event_id=4;check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"air event cannot jump");
    f=Fixture{};check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"held airborne event cannot replay on landing");
    cmd.hop_event_id=5;f.body.linear_velocity[1]=.2;
    const auto separating=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(separating.valid&&!separating.grounded&&!separating.hop_applied,"separating near-floor ray hit is not grounded support for another hop");
    f.body.linear_velocity={};check(!controller.step(h,f.body,f.wheels,f.contacts,cmd).hop_applied,"separating event is consumed before later support returns");
    cmd={};cmd.manual=1;const auto manual=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    cmd={};cmd.nose_manual=1;const auto nose=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(manual.valid&&nose.valid&&manual.torque[0]<0&&nose.torque[0]>0,"manual lifts front and nose manual lowers it using actual pitch torque");
    f.air();cmd={};cmd.air_yaw=1;r=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(r.valid&&near(r.torque[1],-200),"air rider-right input applies negative native yaw torque");
    cmd={};cmd.manual=1;r=controller.step(h,f.body,f.wheels,f.contacts,cmd);
    check(r.valid&&near(r.torque[0],-160),"air manual input gives front-up pitch torque without automatic orientation targets");
}
void bounded_impact_contacts() {
    Fixture f;f.body.orientation={std::sin(.026),0,0,std::cos(.026)};
    f.body.linear_velocity={1,-4.3,3};f.body.angular_velocity={.15,-.2,.05};
    f.wheels[0].angular_speed=0;f.wheels[1].angular_speed=4;
    f.contacts[1].contact.status=t::ContactStatus::clear;
    c::Tuning settings;settings.balance_stiffness=settings.balance_damping=0;settings.friction={.8,.6};
    c::Controller bounded(settings);const auto impact=bounded.step(h,f.body,f.wheels,f.contacts);
    check(impact.valid&&impact.contact_scale>0&&impact.contact_scale<1,
        "a single-wheel4.3m/s landing returns a bounded contact response instead of stopping the ride");
    check(length(impact.force)<=40000&&length(impact.torque)<4000&&length(impact.torque)>3900,
        "torque-dominated front impact retains native40kN/4kNm limits with float conversion margin");
    check(response(f.body,impact).linear_velocity[1]<-.1,
        "bounded contact leaves remaining closing velocity for native collision; it does not fake a complete stop");
    t::Vec external{},external_moment{},rotor_change{};
    for(unsigned i=0;i<2;++i) {
        t::Basis basis;check(t::contact_basis(f.contacts[i].contact.normal,f.contacts[i].contact.forward,basis),"test contact basis is valid");
        const auto impulse=add(mul(basis.normal,impact.normal_impulse[i]),
            add(mul(basis.forward,impact.longitudinal_impulse[i]),mul(basis.lateral,impact.lateral_impulse[i])));
        external=add(external,impulse);
        external_moment=add(external_moment,cross(sub(f.contacts[i].contact.point,f.body.center_of_mass),impulse));
        rotor_change=add(rotor_change,mul(basis.lateral,f.wheels[i].inertia*(impact.wheels[i].angular_speed-f.wheels[i].angular_speed)));
        if(impact.normal_impulse[i]>0)check(std::hypot(impact.longitudinal_impulse[i]/(.8*impact.normal_impulse[i]),
            impact.lateral_impulse[i]/(.6*impact.normal_impulse[i]))<=1+1e-10,"scaled tangential impulses remain inside the same friction ellipse");
        else check(impact.longitudinal_impulse[i]==0&&impact.lateral_impulse[i]==0,"zero normal impulse has no traction");
    }
    vector_near(impact.linear_impulse,external,"bounded chassis linear momentum equals reported contact impulse");
    vector_near(add(impact.angular_impulse,rotor_change),external_moment,
        "bounded contact and rotor changes preserve total angular momentum about measured COM");
    auto gravity_predicted=f.body;gravity_predicted.linear_velocity[1]-=9.8*h;
    check(energy(response(f.body,impact),impact.wheels)<=energy(gravity_predicted,f.wheels)+1e-6,
        "bounded unassisted impact is dissipative after accounting native gravity once");
    auto generous=settings;generous.force_limit=generous.torque_limit=1e6;
    const auto full=c::Controller(generous).step(h,f.body,f.wheels,f.contacts);
    check(full.valid&&full.contact_scale==1,"comparison unbounded impact solves its complete contact wrench");
    vector_near(impact.linear_impulse,mul(full.linear_impulse,impact.contact_scale),"shared fraction scales all contact linear response");
    vector_near(impact.angular_impulse,mul(full.angular_impulse,impact.contact_scale),"shared fraction scales contact angular response");
    for(unsigned i=0;i<2;++i)check(near(impact.wheels[i].angular_speed,
        f.wheels[i].angular_speed+impact.contact_scale*(full.wheels[i].angular_speed-f.wheels[i].angular_speed)),
        "wheel spin is attenuated by exactly the contact fraction");
    c::Controls drive;drive.pedal=.7;drive.brake=.3;
    const auto powered=bounded.step(h,f.body,f.wheels,f.contacts,drive);
    const auto powered_full=c::Controller(generous).step(h,f.body,f.wheels,f.contacts,drive);
    auto clear=f.contacts;for(auto& contact:clear)contact.contact.status=t::ContactStatus::clear;
    auto motor_only=drive;motor_only.brake=0;
    const auto actuated=bounded.step(h,f.body,f.wheels,clear,motor_only);
    check(powered.valid&&powered_full.valid&&actuated.valid&&powered.contact_scale<1,"powered impact and actuator baseline are valid");
    vector_near(powered.angular_impulse,add(actuated.angular_impulse,
        mul(sub(powered_full.angular_impulse,actuated.angular_impulse),powered.contact_scale)),
        "motor-only axle reaction is retained separately from limited brake/contact torque");
    for(unsigned i=0;i<2;++i)check(near(powered.wheels[i].angular_speed,actuated.wheels[i].angular_speed+
        powered.contact_scale*(powered_full.wheels[i].angular_speed-actuated.wheels[i].angular_speed)),
        "limited brake/contact response retains the once-only motor rotor change");
    for(unsigned i=0;i<2;++i)check(near(powered.brake_impulse[i],powered.contact_scale*powered_full.brake_impulse[i]),
        "brake axle impulses share the contact/body/rotor attenuation fraction");
    auto moved=f;const t::Vec platform{3,.2,-2};moved.body.linear_velocity=add(moved.body.linear_velocity,platform);
    for(auto& contact:moved.contacts)contact.contact.surface_velocity=platform;
    const auto relative=bounded.step(h,moved.body,moved.wheels,moved.contacts);
    check(relative.valid&&near(relative.contact_scale,impact.contact_scale),"impact fraction is Galilean invariant with supplied moving ground");
    vector_near(relative.linear_impulse,impact.linear_impulse,"moving platform leaves relative impact impulse unchanged");
    f=Fixture{};f.body.linear_velocity[1]=-4.3;settings.force_limit=2500;
    const auto force_limited=c::Controller(settings).step(h,f.body,f.wheels,f.contacts);
    check(force_limited.valid&&force_limited.contact_scale<1&&length(force_limited.force)<2500&&length(force_limited.force)>2400,
        "two-wheel impact can be force-limited with the same normal/traction fraction");
    check(near(force_limited.normal_load[0]+force_limited.normal_load[1],force_limited.force[1]),
        "reported normal loads describe applied bounded force rather than the unconstrained target");
    f.air();settings={};settings.torque_limit=10;c::Controls yaw;yaw.air_yaw=1;
    const auto rejected=c::Controller(settings).step(h,f.body,f.wheels,f.contacts,yaw);
    check(!rejected.valid&&rejected.issue==c::Issue::output_limit&&rejected.force==t::Vec{}&&rejected.torque==t::Vec{},
        "external actuator torque over budget still rejects atomically instead of silently changing controls");
}
struct MotorBench {
    t::Body body=c::proposed_body();
    std::array<t::Wheel,2> wheels=c::proposed_wheels();
    std::array<c::ContactSample,2> contacts;
    MotorBench(){for(auto& sample:contacts){sample.contact.status=t::ContactStatus::clear;sample.separation=1;}}
    void rolling(double speed){body.linear_velocity={0,0,speed};for(auto& wheel:wheels)wheel.angular_speed=speed/wheel.radius;}
};
double motor_rear_torque(const MotorBench& b,const c::Result& r){return (r.wheels[1].angular_speed-b.wheels[1].angular_speed)*b.wheels[1].inertia/h;}
void motor_cadence_taper(){
    c::Controls pedal;pedal.pedal=1;c::Controller controller;
    for(double speed:{-8.,-1.,0.,3.,6.}){
        MotorBench b;b.rolling(speed);const auto r=controller.step(h,b.body,b.wheels,b.contacts,pedal);
        check(r.valid&&near(motor_rear_torque(b,r),75),"full75Nm pedal torque applies through6m/s and when recovering fakie");
    }
    for(double speed:{6.1,7.,7.99,8.,12.,27.}){
        MotorBench b;b.rolling(speed);const auto r=controller.step(h,b.body,b.wheels,b.contacts,pedal);
        const double expected=75*std::clamp((8-speed)/2,0.0,1.0);
        check(r.valid&&near(motor_rear_torque(b,r),expected),"only motor torque tapers linearly6..8m/s and vanishes above cutoff");
        check(r.force==t::Vec{},"free-wheel taper never invents a drag/propulsion/velocity clamp force");
    }
    MotorBench lifted;lifted.wheels[1].angular_speed=7/lifted.wheels[1].radius;
    auto r=controller.step(h,lifted.body,lifted.wheels,lifted.contacts,pedal);
    check(r.valid&&near(motor_rear_torque(lifted,r),37.5),"rear cadence tapers torque even while measured COM is stationary");
    lifted.wheels[1].angular_speed=9/lifted.wheels[1].radius;
    r=controller.step(h,lifted.body,lifted.wheels,lifted.contacts,pedal);
    check(r.valid&&near(motor_rear_torque(lifted,r),0),"unsupported rear wheel above cutoff receives no more motor acceleration");
    MotorBench bench;
    for(unsigned i=0;i<36000;++i){
        r=controller.step(h,bench.body,bench.wheels,bench.contacts,pedal);check(r.valid,"ten-minute lifted-wheel actuation stays within finite bounds");
        // Externally measured COM is held by this bench; only the modelled
        // internal rotor/chassis angular impulse is advanced, not a bike pose.
        const auto before=bench.wheels[1].angular_speed;bench.wheels=r.wheels;
        bench.body.angular_velocity[0]+=r.angular_impulse[0]/bench.body.inertia_body[0];
        check(bench.wheels[1].angular_speed>=before&&bench.wheels[1].angular_speed*bench.wheels[1].radius<=8+1e-10,
            "bounded positive motor torque approaches cadence cutoff without spin overshoot or clamps");
    }
    check(bench.wheels[1].angular_speed*bench.wheels[1].radius>7.99,"taper permits usable cadence near its cutoff");
    check(std::abs(bench.body.angular_velocity[0])<1,"internal angular reaction also stays bounded");
    c::Controls coast;MotorBench fast;fast.rolling(20);r=controller.step(h,fast.body,fast.wheels,fast.contacts,coast);
    check(r.valid&&r.force==t::Vec{}&&r.torque==t::Vec{}&&r.wheels[1].angular_speed==fast.wheels[1].angular_speed,
        "coasting above cutoff is preserved rather than slowed or clamped");
    c::Controls brake;brake.brake=1;r=controller.step(h,fast.body,fast.wheels,fast.contacts,brake);
    check(r.valid&&r.wheels[1].angular_speed<fast.wheels[1].angular_speed&&near(motor_rear_torque(fast,r),-45),
        "high-speed rear braking remains unchanged by motor taper");
    check(near((r.wheels[0].angular_speed-fast.wheels[0].angular_speed)*fast.wheels[0].inertia/h,-60)&&r.force==t::Vec{},
        "front brake applies its sixty-Nm rotor couple without inventing airborne linear drag");
    MotorBench ground;ground.rolling(7);
    for(unsigned i=0;i<2;++i){ground.contacts[i].contact.status=t::ContactStatus::hit;ground.contacts[i].contact.point={0,0,i==0?.525:-.525};ground.contacts[i].separation=0;}
    ground.body.center_of_mass={0,.9,-.05};const auto base=controller.step(h,ground.body,ground.wheels,ground.contacts,pedal);
    const t::Vec platform{3,1,11};for(unsigned a=0;a<3;++a)ground.body.linear_velocity[a]+=platform[a];
    for(auto& sample:ground.contacts)sample.contact.surface_velocity=platform;
    const auto moving=controller.step(h,ground.body,ground.wheels,ground.contacts,pedal);
    check(base.valid&&moving.valid,"ground and moving-platform taper samples are valid");
    for(unsigned a=0;a<3;++a)check(near(base.force[a],moving.force[a],1e-7)&&near(base.torque[a],moving.torque[a],1e-7),
        "ground-relative speed and cadence preserve Galilean-invariant forces");
    for(unsigned invalid=0;invalid<4;++invalid){c::Tuning bad;
        if(invalid==0)bad.pedal_fade_start=bad.pedal_cutoff;
        if(invalid==1)bad.pedal_fade_start=-1;
        if(invalid==2)bad.pedal_cutoff=std::numeric_limits<double>::infinity();
        if(invalid==3)bad.pedal_cutoff=std::numeric_limits<double>::quiet_NaN();
        const auto rejected=c::Controller(bad).step(h,fast.body,fast.wheels,fast.contacts,pedal);
        check(!rejected.valid&&rejected.force==t::Vec{}&&rejected.torque==t::Vec{},"unordered/negative/nonfinite taper tuning rejects atomically");
    }
    std::cout<<"Scratch motor taper tests PASS: low-speed torque,6..8m/s falloff, ten-minute free cadence, coast/brake, Galilean invariance and invalid tuning.\n";
}
void actual_support_and_speculative_gap() {
    c::Tuning tuning;tuning.balance_stiffness=tuning.balance_damping=0;tuning.friction={0,0};
    for(double gap:{.010,.029}) {
        Fixture f;for(auto& sample:f.contacts)sample.separation=gap;
        c::Controller controller(tuning);c::Controls hop;hop.hop_event_id=1;hop.hop_strength=1;
        const auto r=controller.step(h,f.body,f.wheels,f.contacts,hop);
        check(r.valid&&!r.grounded&&!r.hop_applied,"ray skin above actual support cannot ground or hop");
        vector_near(r.force,{},"stationary positive gap falls under native gravity instead of hovering at ray skin");
        check(r.normal_impulse[0]==0&&r.normal_impulse[1]==0&&r.longitudinal_impulse[0]==0&&r.lateral_impulse[0]==0,
            "no closing collision gives no speculative normal load or phantom traction");
        check(!c::eligible_support(f.body,f.contacts[0]),"shared input/trick helper rejects ray-skin-only support");
        f.contacts[0].separation=f.contacts[1].separation=0;
        check(!controller.step(h,f.body,f.wheels,f.contacts,hop).hop_applied,"held skin-only hop cannot resurrect on actual contact");
    }
    Fixture f;for(auto& sample:f.contacts)sample.separation=.010;f.body.linear_velocity[1]=-4;
    const auto closing=c::Controller(tuning).step(h,f.body,f.wheels,f.contacts);
    check(closing.valid&&!closing.grounded&&closing.force[1]>0,"fast closing positive gap has speculative collision force without actual grounded state");
    const auto after=response(f.body,closing);
    for(const auto& sample:f.contacts) {
        const auto v=t::point_velocity(after,sample.contact.point);
        check(v[1]>=-.010/h-.00001&&v[1]<-.5,"positive-gap target permits closing through the gap rather than a premature zero-velocity stop");
    }
    f=Fixture{};f.contacts[0].separation=c::actual_support_tolerance;
    check(c::eligible_support(f.body,f.contacts[0]),"explicit five-millimeter support tolerance accepts a valid resting wheel");
    f.contacts[0].separation=c::actual_support_tolerance+.00001;
    check(!c::eligible_support(f.body,f.contacts[0]),"support tolerance boundary is separate from the three-centimeter ray skin");
    f.contacts[0].separation=-10;check(!c::eligible_support(f.body,f.contacts[0]),"pathological penetration cannot ground input before controller rejection");
    f=Fixture{};f.body.linear_velocity[1]=.051;check(!c::eligible_support(f.body,f.contacts[0]),"separating wheel cannot supply another grounded hop");
    f.contacts[0].contact.surface_velocity={0,.051,0};check(c::eligible_support(f.body,f.contacts[0]),"support helper uses ground-relative point velocity");
    f.contacts[0].contact.status=t::ContactStatus::unavailable;check(!c::eligible_support(f.body,f.contacts[0]),"unavailable surface never becomes support");
}
Fixture concave_ramp(double theta,double rate) {
    // An analytical1.05m contact span, not an assertion about the model wheelbase.
    // Radius follows the selected wheel, with COM offset derived consistently.
    constexpr double R=6,r=dingosdk::bmx::geometry::wheel_radius,rho=R-r,L=1.05;
    constexpr double com_above_hub=.9-r;
    const double delta=std::asin(L/(2*rho));
    const t::Vec center{0,R,0},normal{0,std::cos(theta),-std::sin(theta)},forward{0,std::sin(theta),std::cos(theta)};
    Fixture f;f.body.orientation={-std::sin(theta/2),0,0,std::cos(theta/2)};
    const auto radial=mul(normal,-1);
    f.body.center_of_mass=add(add(add(center,mul(radial,rho*std::cos(delta))),mul(normal,com_above_hub)),mul(forward,-.05));
    f.body.linear_velocity=mul(add(mul(forward,rho*std::cos(delta)-com_above_hub),mul(radial,.05)),rate);
    f.body.angular_velocity={-rate,0,0};
    for(unsigned i=0;i<2;++i) {
        const auto angle=theta+(i==0?delta:-delta);
        auto& contact=f.contacts[i].contact;
        contact.normal={0,std::cos(angle),-std::sin(angle)};contact.forward={0,std::sin(angle),std::cos(angle)};
        contact.point=add(center,mul(contact.normal,-R));f.wheels[i].angular_speed=rate*rho/r;
    }
    return f;
}
t::Vec external_balance(const Fixture& f,const c::Result& r) {
    t::Vec rotor{},contact_moment{};
    for(unsigned i=0;i<2;++i) {
        t::Basis basis;check(t::contact_basis(f.contacts[i].contact.normal,f.contacts[i].contact.forward,basis),"ramp basis valid");
        const auto J=add(mul(basis.normal,r.normal_impulse[i]),add(mul(basis.forward,r.longitudinal_impulse[i]),mul(basis.lateral,r.lateral_impulse[i])));
        rotor=add(rotor,mul(basis.lateral,f.wheels[i].inertia*(r.wheels[i].angular_speed-f.wheels[i].angular_speed)));
        contact_moment=add(contact_moment,cross(sub(f.contacts[i].contact.point,f.body.center_of_mass),J));
    }
    return mul(sub(add(r.angular_impulse,rotor),contact_moment),1/h);
}
void edge_force_normal_does_not_command_body_roll() {
    constexpr double a=.7071067811865475244;
    // Exercise the original .34m corner and the selected bike radius. These
    // are explicit radial-edge geometries; support/depth/torque bounds stay fixed.
    for(double radius:{.34,dingosdk::bmx::geometry::wheel_radius})for(double yaw:{0.,1.2,-2.3})for(double side:{-1.,1.}) {
        const double factor=radius/.34;
        Fixture f;for(auto& wheel:f.wheels)wheel.radius=radius;
        f.body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        const auto q=f.body.orientation;
        const auto turn=[&](t::Vec v){return add(add(mul(axes(q)[0],v[0]),mul(axes(q)[1],v[1])),mul(axes(q)[2],v[2]));};
        f.body.center_of_mass=turn({0,.9,-.05});f.body.linear_velocity=turn({0,-2,3});
        f.contacts[0].contact.point=turn({0,0,.601978*factor});
        f.contacts[1].contact.point=turn({-.24*side*factor,.10*factor,-.601978*factor});
        f.contacts[1].contact.normal=turn({a*side,a,0});
        f.contacts[1].separation=std::hypot(.24*factor,.24*factor)-radius;
        for(auto& sample:f.contacts)sample.contact.forward=turn({0,0,1});
        for(auto& wheel:f.wheels)wheel.angular_speed=3/wheel.radius;
        c::Tuning settings;settings.friction={0,0};
        c::Controller legacy(settings);const auto radial=legacy.step(h,f.body,f.wheels,f.contacts);
        check(radial.valid&&radial.grounded&&radial.balance_support_mask==3,"flat front and actual rear edge remain supported");
        const auto wrong_target=turn({0,0,-side*1600*std::sin(std::numbers::pi/8)});
        vector_near(radial.balance_torque,wrong_target,"counterexample exposes22.5deg radial-edge target at neutral input",1e-7);
        for(auto& sample:f.contacts) {sample.balance_normal={0,1,0};sample.balance_normal_available=true;}
        c::Controller corrected(settings);const auto face=corrected.step(h,f.body,f.wheels,f.contacts);
        check(face.valid&&face.grounded&&face.balance_support_mask==radial.balance_support_mask&&face.contact_scale==1,
            "measured face reference changes neither strict support eligibility nor force budgets");
        vector_near(face.balance_reference_up,{0,1,0},"independent horizontal stair face is the neutral riding-up reference");
        vector_near(face.balance_torque,{},"horizontal face cannot command side roll from a radial edge normal",1e-7);
        vector_near(external_balance(f,face),{},"all remaining angular impulse is actual radial contact/rotor momentum",1e-5);
        t::Vec J{};for(unsigned i=0;i<2;++i)J=add(J,mul(f.contacts[i].contact.normal,face.normal_impulse[i]));
        vector_near(face.linear_impulse,J,"normal impulse still follows real radial geometry; no force normal was flattened",1e-7);
        vector_near(face.torque,add(add(face.balance_torque,face.air_control_torque),add(face.motor_torque,face.contact_torque)),
            "diagnostic torque decomposition reproduces the actually submitted wrench",1e-7);
        f.contacts[1].contact.normal=turn({std::sin(std::numbers::pi/4+.02)*side,std::cos(std::numbers::pi/4+.02),0});
        const auto next=corrected.step(h,f.body,f.wheels,f.contacts);
        check(next.valid&&next.balance_support_mask==3,"moving radial contact is still genuine support");
        vector_near(next.desired_support_angular_velocity,{},"edge radial sweep is not fictitious terrain curvature",1e-8);
    }
    Fixture plain,explicit_face;
    for(auto& sample:explicit_face.contacts){sample.balance_normal={0,1,0};sample.balance_normal_available=true;}
    c::Controller old_plane,new_plane;
    for(unsigned i=0;i<3;++i) {
        const auto a0=old_plane.step(h,plain.body,plain.wheels,plain.contacts);
        const auto a1=new_plane.step(h,explicit_face.body,explicit_face.wheels,explicit_face.contacts);
        check(a0.valid&&a1.valid&&a0.force==a1.force&&a0.torque==a1.torque&&a0.normal_impulse==a1.normal_impulse&&
            a0.longitudinal_impulse==a1.longitudinal_impulse&&a0.lateral_impulse==a1.lateral_impulse&&
            a0.wheels[0].angular_speed==a1.wheels[0].angular_speed&&a0.wheels[1].angular_speed==a1.wheels[1].angular_speed,
            "ordinary plane/default input retains identical physical outputs and history");
    }
    for(t::Vec invalid:{t::Vec{},t::Vec{0,-1,0},t::Vec{std::numeric_limits<double>::quiet_NaN(),1,0}}) {
        Fixture f;f.contacts[1].balance_normal_available=true;f.contacts[1].balance_normal=invalid;
        c::Controller controller;check(!controller.step(h,f.body,f.wheels,f.contacts).valid,"invalid or opposite face reference fails closed");
    }
    Fixture air;air.air();for(auto& sample:air.contacts){sample.balance_normal_available=true;sample.balance_normal={};}
    c::Controls cmd;cmd.pedal=.7;cmd.air_yaw=.6;cmd.manual=.3;c::Controller flight;
    const auto result=flight.step(h,air.body,air.wheels,air.contacts,cmd);
    check(result.valid&&result.balance_torque==t::Vec{}&&result.contact_torque==t::Vec{},"clear air acquires no face target/contact torque");
    vector_near(result.air_control_torque,{-48,-120,0},"air input remains actual pitch/yaw torque rather than balance assistance");
    vector_near(result.motor_torque,{-52.5,0,0},"separate motor reaction is reported exactly once");
}
void curved_ramp_balance_and_history() {
    for(double rate:{.8,-.8}) {
        c::Controller controller;auto f=concave_ramp(.3,rate);
        const auto seed=controller.step(h,f.body,f.wheels,f.contacts);
        check(seed.valid&&seed.grounded&&seed.desired_support_angular_velocity==t::Vec{},"new ramp contact seeds normals without inventing a normal rate");
        f=concave_ramp(.3+rate*h,rate);const auto follow=controller.step(h,f.body,f.wheels,f.contacts);
        check(follow.valid&&follow.grounded,"exact forward/fakie rolling quarter-pipe geometry is supported");
        vector_near(follow.desired_support_angular_velocity,{-rate,0,0},"normal transport follows the actual curved ramp in either travel direction",1e-8);
        vector_near(external_balance(f,follow),{},"matching ramp pitch rate is not opposed by grounded angular damping",1e-5);
        f=concave_ramp(.3+2*rate*h,rate);f.contacts[0].contact.status=t::ContactStatus::clear;
        const auto one=controller.step(h,f.body,f.wheels,f.contacts);
        check(one.valid&&one.grounded,"one-wheel curved support remains a valid measured contact");
        vector_near(one.desired_support_angular_velocity,{},"support-count change seeds fresh normals rather than differentiating a new support plane",1e-8);
        f=concave_ramp(.3+3*rate*h,rate);const auto two=controller.step(h,f.body,f.wheels,f.contacts);
        vector_near(two.desired_support_angular_velocity,{},"returning wheel resets transport for the changed support topology",1e-8);
        f.air();const auto air=controller.step(h,f.body,f.wheels,f.contacts);
        check(air.valid&&!air.grounded&&air.desired_support_angular_velocity==t::Vec{},"clear lip removes curvature target immediately");
        vector_near(air.torque,{},"ramp exit does not auto-upright or damp the airborne bike");
        f=concave_ramp(.8,rate);const auto recontact=controller.step(h,f.body,f.wheels,f.contacts);
        check(recontact.valid&&recontact.desired_support_angular_velocity==t::Vec{},"landing seeds new history rather than replaying lip curvature");
        controller.reset();check(controller.step(h,f.body,f.wheels,f.contacts).desired_support_angular_velocity==t::Vec{},"mount/reset clears ramp history");
        f=concave_ramp(.8+rate*h,rate);check(controller.step(h,f.body,f.wheels,f.contacts).valid,"history warms after reset");
        f=concave_ramp(.8,-rate);const auto reversed=controller.step(h,f.body,f.wheels,f.contacts);
        check(reversed.valid&&reversed.desired_support_angular_velocity==t::Vec{},"signed travel reversal clears stale normal transport");
        f=concave_ramp(.8-rate*h,-rate);const auto reverse_follow=controller.step(h,f.body,f.wheels,f.contacts);
        vector_near(reverse_follow.desired_support_angular_velocity,{rate,0,0},"new fakie/forward direction establishes its own normal history",1e-8);
        f.contacts[0].contact.status=t::ContactStatus::unavailable;check(!controller.step(h,f.body,f.wheels,f.contacts).valid,"unavailable ramp query rejects safely");
        f=concave_ramp(1,-rate);check(controller.step(h,f.body,f.wheels,f.contacts).desired_support_angular_velocity==t::Vec{},"query failure clears normal history");
        f=concave_ramp(1.4,-rate);const auto seam=controller.step(h,f.body,f.wheels,f.contacts);
        check(seam.valid&&seam.desired_support_angular_velocity==t::Vec{},"discontinuous surface normals are not converted into a curvature-rate spike");
        f=concave_ramp(1.4-rate*h,-rate);check(controller.step(h,f.body,f.wheels,f.contacts).valid,"normal history rewarmed after seam");
        c::Controls hop;hop.hop_event_id=1;hop.hop_strength=1;
        f=concave_ramp(1.4-2*rate*h,-rate);const auto jumped=controller.step(h,f.body,f.wheels,f.contacts,hop);
        check(jumped.valid&&jumped.hop_applied,"supported ramp can produce a real hop event");
        f=concave_ramp(1.4-3*rate*h,-rate);check(controller.step(h,f.body,f.wheels,f.contacts).desired_support_angular_velocity==t::Vec{},
            "hop clears ramp transport even if the following query still touches the departure skin");
        c::Controls disabled;disabled.enabled=false;
        f=concave_ramp(1.4-4*rate*h,-rate);check(controller.step(h,f.body,f.wheels,f.contacts,disabled).valid,"disabled-control measured step remains bounded");
        f=concave_ramp(1.4-5*rate*h,-rate);check(controller.step(h,f.body,f.wheels,f.contacts).desired_support_angular_velocity==t::Vec{},
            "disabled input cannot preserve normal transport for a later focus return");
    }
    c::Controller flat;Fixture f;f.rolling(4);f.body.angular_velocity={-.8,0,0};
    f.contacts[0].contact.point=f.contacts[1].contact.point={0,0,-.05};
    check(flat.step(h,f.body,f.wheels,f.contacts).valid,"fixed-plane damping seed is valid");
    const auto fixed=flat.step(h,f.body,f.wheels,f.contacts);
    check(fixed.valid&&fixed.desired_support_angular_velocity==t::Vec{}&&external_balance(f,fixed)[0]>100,
        "fixed flat normal retains existing angular damping rather than treating every body rotation as ramp motion");
    c::Tuning settings;settings.balance_stiffness=0;c::Controller transition(settings);
    f=Fixture{};f.rolling(.6);check(transition.step(h,f.body,f.wheels,f.contacts).valid,"flat support-mask seed valid");
    f.body.orientation={std::sin(.05),0,0,std::cos(.05)};f.contacts[1].contact.status=t::ContactStatus::clear;
    f.body.linear_velocity={0,-.6*std::sin(.1),.6*std::cos(.1)};
    for(auto& sample:f.contacts) {sample.contact.normal={0,std::cos(.1),std::sin(.1)};sample.contact.forward={0,-std::sin(.1),std::cos(.1)};}
    const auto new_plane=transition.step(h,f.body,f.wheels,f.contacts);
    check(new_plane.valid&&new_plane.grounded&&new_plane.desired_support_angular_velocity==t::Vec{},
        "small seam coinciding with front-only support cannot create a capped curvature target");
    vector_near(external_balance(f,new_plane),{},"support-topology change cannot inject a normal-transport balance kick",1e-5);
}
void loaded_balance_continuity() {
    Fixture f;f.body.orientation={0,0,std::sin(.04),std::cos(.04)};
    c::Controller controller;
    const auto seed=controller.step(h,f.body,f.wheels,f.contacts);
    check(seed.valid&&seed.grounded&&seed.balance_support_mask==3&&seed.balance_continuation_mask==0&&
        seed.normal_load[0]>0&&seed.normal_load[1]>0,"genuine loaded contact seeds balance history");
    const auto expected_balance=external_balance(f,seed);
    check(length(expected_balance)>10,"tilted fixture has a measurable ordinary balance response");
    f.body.linear_velocity[1]=.05691;
    for(auto& sample:f.contacts)sample.separation=.000852;
    check(!c::eligible_support(f.body,f.contacts[0])&&!c::eligible_support(f.body,f.contacts[1]),
        "Run74 small-opening contact still fails the unchanged strict support helper");
    for(unsigned i=0;i<8;++i) {
        c::Controls hop;hop.hop_event_id=1;hop.hop_strength=1;
        const auto continued=controller.step(h,f.body,f.wheels,f.contacts,hop);
        check(continued.valid&&!continued.grounded&&!continued.hop_applied&&continued.balance_support_mask==3&&
            continued.balance_continuation_mask==3&&continued.normal_load[0]>0&&continued.normal_load[1]>0,
            "fresh near hits with positive preceding solved loads sustain balance, never hop/grounded status");
        vector_near(external_balance(f,continued),expected_balance,
            "small solver opening does not toggle the existing balance torque off",1e-5);
    }
    check(controller.consumed_hop_event_id()==1,"balance-only contact consumes unsupported hop without replay");
    c::Controller cold;
    for(unsigned i=0;i<2;++i) {
        const auto no_history=cold.step(h,f.body,f.wheels,f.contacts);
        check(no_history.valid&&!no_history.grounded&&no_history.normal_load[0]>0&&
            no_history.balance_support_mask==0&&no_history.balance_continuation_mask==0,
            "positive speculative load alone cannot bootstrap balance without prior genuine support");
        vector_near(external_balance(f,no_history),{},"no-history opening has no automatic upright assist",1e-5);
    }
    for(unsigned failure=0;failure<7;++failure) {
        Fixture invalid=f;invalid.body.linear_velocity={};for(auto& sample:invalid.contacts)sample.separation=0;
        c::Controller guarded;check(guarded.step(h,invalid.body,invalid.wheels,invalid.contacts).valid,"history seed valid");
        invalid.body.linear_velocity[1]=.05691;for(auto& sample:invalid.contacts)sample.separation=.000852;
        if(failure==0)for(auto& sample:invalid.contacts)sample.separation=c::actual_support_tolerance+.000001;
        if(failure==1)invalid.body.linear_velocity[1]=9.8*h+.000001;
        if(failure==2)invalid.air();
        if(failure==3)guarded.reset();
        if(failure==4)check(!guarded.step(0,invalid.body,invalid.wheels,invalid.contacts).valid,"invalid native step clears history");
        if(failure==5) {
            c::Controls disabled;disabled.enabled=false;
            check(guarded.step(h,invalid.body,invalid.wheels,invalid.contacts,disabled).valid,"disabled step remains valid");
        }
        if(failure==6)for(auto& sample:invalid.contacts) {
            sample.contact.normal={std::sin(.3),std::cos(.3),0};
            invalid.body.linear_velocity=mul(sample.contact.normal,.05691);
        }
        const auto rejected=guarded.step(h,invalid.body,invalid.wheels,invalid.contacts);
        check(rejected.valid&&!rejected.grounded&&rejected.balance_support_mask==0&&rejected.balance_continuation_mask==0,
            "far gap, departure beyond one gravity step, clear, reset, invalid/disabled and discontinuous normal cannot carry balance");
        vector_near(external_balance(invalid,rejected),{},"rejected continuation keeps ordinary zero automatic air assist",1e-5);
    }
    Fixture unloaded=f;unloaded.body.linear_velocity[1]=.049;
    for(auto& sample:unloaded.contacts)sample.separation=.005;
    c::Controller no_load;const auto empty=no_load.step(h,unloaded.body,unloaded.wheels,unloaded.contacts);
    check(empty.valid&&empty.grounded&&empty.normal_load[0]==0&&empty.normal_load[1]==0,
        "strict near support can be geometrically valid without a solved load");
    unloaded.body.linear_velocity[1]=.05691;for(auto& sample:unloaded.contacts)sample.separation=.000852;
    check(no_load.step(h,unloaded.body,unloaded.wheels,unloaded.contacts).balance_support_mask==0,
        "zero preceding load cannot carry balance even after genuine support");
    Fixture reverse;reverse.rolling(2);c::Controller direction;
    check(direction.step(h,reverse.body,reverse.wheels,reverse.contacts).valid,"forward history seeded");
    reverse.rolling(-2);reverse.body.linear_velocity[1]=.05691;
    for(auto& sample:reverse.contacts)sample.separation=.000852;
    const auto reversed=direction.step(h,reverse.body,reverse.wheels,reverse.contacts);
    check(reversed.valid&&!reversed.grounded&&reversed.balance_support_mask==0&&
        reversed.desired_support_angular_velocity==t::Vec{},"travel reversal must reestablish genuine support");
    c::Controller one_wheel;Fixture single;single.contacts[0].contact.status=t::ContactStatus::clear;
    check(one_wheel.step(h,single.body,single.wheels,single.contacts).normal_load[1]>0,"rear-only history is loaded");
    single.contacts[0].contact.status=t::ContactStatus::hit;single.body.linear_velocity[1]=.05691;
    for(auto& sample:single.contacts)sample.separation=.000852;
    const auto one=one_wheel.step(h,single.body,single.wheels,single.contacts);
    check(one.valid&&!one.grounded&&one.balance_support_mask==2&&one.balance_continuation_mask==2,
        "one wheel's history never grants continuation to a newly observed wheel");
    c::Controller curve;auto ramp=concave_ramp(.3,.8);
    check(curve.step(h,ramp.body,ramp.wheels,ramp.contacts).valid,"curved loaded seed valid");
    ramp=concave_ramp(.3+.8*h,.8);
    ramp.body.linear_velocity=add(ramp.body.linear_velocity,mul({0,std::cos(.3+.8*h),-std::sin(.3+.8*h)},.05691));
    const auto transported=curve.step(h,ramp.body,ramp.wheels,ramp.contacts);
    check(transported.valid&&!transported.grounded&&transported.balance_continuation_mask==3,
        "continuous loaded curved contact may retain balance during small solver opening");
    vector_near(transported.desired_support_angular_velocity,{-.8,0,0},
        "normal transport stays tied to continuous measured contacts during balance-only carry",1e-8);
}

// Frozen measured inputs, not a native re-integration or a future-trajectory
// assertion. These19 rows cover the true zero-load bounce and later loaded gap.
struct RecoveryRow161 {
    std::uint64_t serial; t::Vec com; t::Quat rotation; t::Vec velocity,angular_velocity;
    std::array<t::Vec,2> points,normals;std::array<double,2> gap,wheel_speed;std::array<bool,2> hit;
};
const std::array<RecoveryRow161,19> recovery161{{
        {4144ULL,{299.51837158203125,275.36981201171875,-481.78497314453125},{-0.055312693119049072,0.89978551864624023,-0.12567606568336487,-0.4141642153263092},{-2.0557084083557129,-4.0525798797607422,-1.7676571607589722},{-0.50376641750335693,-0.00090535799972712994,0.57432419061660767},{{{299.15597534179688,274.30807495117188,-482.09268188476562},{0,0,0}}},{{{-0.0012432624353095889,0.99999487400054932,0.0029513826593756676},{0,0,0}}},{0.0054607135243713856,0.25},{9.9782390594482422,7.1258563995361328},{true,false}},
        {4145ULL,{299.48440551757812,275.3221435546875,-481.814208984375},{-0.048964597284793854,0.90155595541000366,-0.11222352087497711,-0.4149760901927948},{-2.0383052825927734,-2.8594205379486084,-1.7549399137496948},{1.1582567691802979,-0.0065605868585407734,-1.3781753778457642},{{{299.105712890625,274.30816650390625,-482.13555908203125},{0,0,0}}},{{{-0.0012362211709842086,0.99999487400054932,0.0029574010986834764},{0,0,0}}},{-0.02771206758916378,0.25},{13.139865875244141,7.1258563995361328},{true,false}},
        {4146ULL,{299.45068359375,275.29132080078125,-481.84323120117188},{-0.034300584346055984,0.90486782789230347,-0.08079306036233902,-0.41654545068740845},{-2.0232236385345459,-1.8488044738769531,-1.7412652969360352},{2.7054047584533691,-0.0073839155957102776,-3.1934027671813965},{{{299.03567504882812,274.30825805664062,-482.19549560546875},{0,0,0}}},{{{-0.0012220157077535987,0.99999487400054932,0.0029667331837117672},{0,0,0}}},{-0.022340869531035423,0.18945248425006866},{16.148172378540039,7.1258563995361328},{true,false}},
        {4147ULL,{299.41696166992188,275.25778198242188,-481.87225341796875},{-0.019588705152273178,0.90707689523696899,-0.049267169088125229,-0.41761291027069092},{-2.0232236385345459,-2.0121376514434814,-1.7412652969360352},{2.7047533988952637,-0.0067461738362908363,-3.193958044052124},{{{298.96759033203125,274.308349609375,-482.25372314453125},{0,0,0}}},{{{-0.0012141673360019922,0.99999481439590454,0.0029742487240582705},{0,0,0}}},{-0.016451023519039154,0.11224230378866196},{16.148172378540039,7.1258563995361328},{true,false}},
        {4148ULL,{299.38323974609375,275.22152709960938,-481.90127563476562},{-0.0048468019813299179,0.90818023681640625,-0.017684243619441986,-0.41817757487297058},{-2.0232236385345459,-2.1754710674285889,-1.7412652969360352},{2.704113245010376,-0.0060235960409045219,-3.1945037841796875},{{{298.90176391601562,274.3084716796875,-482.31008911132812},{0,0,0}}},{{{-0.0012111413525417447,0.99999481439590454,0.0029780126642435789},{0,0,0}}},{-0.010225376114249229,0.034726984798908234},{16.148172378540039,7.1258563995361328},{true,false}},
        {4149ULL,{299.35140991210938,275.18637084960938,-481.92837524414062},{0.0070651951245963573,0.90836703777313232,0.0052009741775691509,-0.41808173060417175},{-1.9090276956558228,-2.1101326942443848,-1.6261172294616699},{1.8971855640411377,-0.02792835421860218,-2.4466533660888672},{{{298.84701538085938,274.30853271484375,-482.3553466796875},{299.76113891601562,274.30715942382812,-481.57229614257812}}},{{{-0.0012107602087780833,0.99999481439590454,0.0029803325887769461},{-0.00049316848162561655,0.99999558925628662,0.0029240595176815987}}},{-0.012117453850805759,-0.029238123446702957},{16.148172378540039,7.1258563995361328},{true,true}},
        {4150ULL,{299.32049560546875,275.18399047851562,-481.954833984375},{0.0073000695556402206,0.90844482183456421,0.0034341141581535339,-0.41792699694633484},{-1.8550711870193481,-0.14223466813564301,-1.5875773429870605},{-0.2045685350894928,-0.019297594204545021,0.062956251204013824},{{{298.81658935546875,274.30859375,-482.38006591796875},{299.73092651367188,274.30718994140625,-481.5966796875}}},{{{-0.00049827253678813577,0.99999558925628662,0.0029237126000225544},{-0.0012151686241850257,0.99999481439590454,0.0029789230320602655}}},{-0.016542777419090271,-0.030000880360603333},{9.2527761459350586,7.3617701530456543},{true,true}},
        {4151ULL,{299.28945922851562,275.187744140625,-481.98153686523438},{0.0070797563530504704,0.90845459699630737,0.0027933791279792786,-0.41791430115699768},{-1.8614615201950073,0.22566591203212738,-1.6028130054473877},{-0.05881626158952713,-0.0014109268086031079,0.056152638047933578},{{{298.78631591796875,274.30862426757812,-482.40625},{299.700439453125,274.3072509765625,-481.622802734375}}},{{{-0.0012142970226705074,0.99999481439590454,0.0029788562096655369},{-0.0012142970226705074,0.99999481439590454,0.0029788562096655369}}},{-0.013734877109527588,-0.025575818493962288},{7.0981531143188477,7.0978484153747559},{true,true}},
        {4152ULL,{299.25845336914062,275.19082641601562,-482.00827026367188},{0.0068322843872010708,0.90846049785614014,0.0020972948987036943,-0.41790959239006042},{-1.8610308170318604,0.18423616886138916,-1.6033755540847778},{-0.06347954273223877,-0.00030858625541441143,0.061889063566923141},{{{298.75564575195312,274.30868530273438,-482.43243408203125},{299.67001342773438,274.30728149414062,-481.64898681640625}}},{{{-0.00049826735630631447,0.99999558925628662,0.0029237167909741402},{-0.0012147659435868263,0.99999481439590454,0.002978453878313303}}},{-0.011659897863864899,-0.021730532869696617},{7.0852537155151367,7.0852146148681641},{true,true}},
        {4153ULL,{299.22744750976562,275.19345092773438,-482.03500366210938},{0.0066375085152685642,0.90846508741378784,0.0015032330993562937,-0.41790536046028137},{-1.8602426052093506,0.15735158324241638,-1.6036404371261597},{-0.054999221116304398,-0.00025859096786007285,0.051027283072471619},{{{298.725341796875,274.3087158203125,-482.45864868164062},{299.63949584960938,274.30734252929688,-481.67520141601562}}},{{{-0.0012151686241850257,0.99999481439590454,0.0029789227992296219},{-0.0012147659435868263,0.99999481439590454,0.0029784536454826593}}},{-0.0098896818235516548,-0.01846516877412796},{7.1052694320678711,7.105072021484375},{true,true}},
        {4154ULL,{299.19644165039062,275.1956787109375,-482.06173706054688},{0.0064838221296668053,0.90846854448318481,0.00098409107886254787,-0.41790172457695007},{-1.8595232963562012,0.13347040116786957,-1.6039731502532959},{-0.048891507089138031,-0.0001884004013845697,0.04279014840722084},{{{298.69448852539062,274.30877685546875,-482.48492431640625},{299.60888671875,274.307373046875,-481.70147705078125}}},{{{-0.00049826421309262514,0.99999558925628662,0.002923719584941864},{-0.0012151687406003475,0.99999481439590454,0.0029789230320602655}}},{-0.0084250485524535179,-0.015688082203269005},{7.1199097633361816,7.1198816299438477},{true,true}},
        {4155ULL,{299.16546630859375,275.19757080078125,-482.08847045898438},{0.0063735460862517357,0.90847128629684448,0.00054421881213784218,-0.41789835691452026},{-1.8587313890457153,0.11437392234802246,-1.6043072938919067},{-0.042426351457834244,-0.00017854548059403896,0.034082043915987015},{{{298.66409301757812,274.30880737304688,-482.51126098632812},{299.5782470703125,274.30743408203125,-481.72781372070312}}},{{{-0.001212631119415164,0.99999481439590454,0.0029782899655401707},{-0.0012142970226705074,0.99999481439590454,0.0029788559768348932}}},{-0.0071125980466604233,-0.013368668034672737},{7.1358523368835449,7.1357097625732422},{true,true}},
        {4156ULL,{299.13449096679688,275.19918823242188,-482.11520385742188},{0.0062923040241003036,0.90847331285476685,0.00015147945669014007,-0.41789540648460388},{-1.858022928237915,0.097037941217422485,-1.6047201156616211},{-0.038743607699871063,-0.00013023338397033513,0.028552809730172157},{{{298.63336181640625,274.30886840820312,-482.53765869140625},{299.54751586914062,274.30746459960938,-481.75421142578125}}},{{{-0.0012151686241850257,0.99999481439590454,0.0029789230320602655},{-0.0012141823535785079,0.99999481439590454,0.0029777726158499718}}},{-0.0061055216938257217,-0.011354519054293633},{7.1452584266662598,7.1452374458312988},{true,true}},
        {4157ULL,{299.10354614257812,275.2005615234375,-482.1419677734375},{0.0062434999272227287,0.90847486257553101,-0.00018736031779553741,-0.41789287328720093},{-1.8572767972946167,0.083017118275165558,-1.6051411628723145},{-0.034493949264287949,-0.00010452557762619108,0.022313334047794342},{{{298.60263061523438,274.30889892578125,-482.5640869140625},{299.51678466796875,274.30752563476562,-481.7806396484375}}},{{{-0.0012147659435868263,0.99999481439590454,0.002978453878313303},{-0.0012151687406003475,0.99999481439590454,0.0029789227992296219}}},{-0.0051899994723498821,-0.0096760597079992294},{7.1562995910644531,7.1562786102294922},{true,true}},
        {4158ULL,{299.07260131835938,275.20175170898438,-482.16873168945312},{0.0062212818302214146,0.90847593545913696,-0.0004857672902289778,-0.41789057850837708},{-1.8565375804901123,0.071023471653461456,-1.6056032180786133},{-0.031419072300195694,-8.4984560089651495e-05,0.017387121915817261},{{{298.57186889648438,274.3089599609375,-482.590576171875},{299.48602294921875,274.30755615234375,-481.80712890625}}},{{{-0.0012151686241850257,0.99999481439590454,0.0029789230320602655},{-0.0012141824699938297,0.99999481439590454,0.0029777726158499718}}},{-0.0044270632788538933,-0.0082112252712249756},{7.1646685600280762,7.1646537780761719},{true,true}},
        {4159ULL,{299.0416259765625,275.2027587890625,-482.1954345703125},{0.0061882324516773224,0.90847563743591309,-0.0007310781511478126,-0.41789138317108154},{-1.8583976030349731,0.060882154852151871,-1.603043794631958},{-0.025085218250751495,0.00028619627119041979,0.015904251486063004},{{{298.541015625,274.30899047851562,-482.61703491210938},{299.45492553710938,274.3076171875,-481.83355712890625}}},{{{-0.0012141823535785079,0.99999481439590454,0.0029777728486806154},{-0.00049786211457103491,0.99999558925628662,0.0029240630101412535}}},{-0.0037556816823780537,-0.0070212460123002529},{7.1723170280456543,7.1723051071166992},{true,true}},
        {4160ULL,{299.0106201171875,275.20364379882812,-482.22210693359375},{0.0061388923786580563,0.90847516059875488,-0.00094238616293296218,-0.41789275407791138},{-1.8602045774459839,0.052989546209573746,-1.6005501747131348},{-0.020560970529913902,0.00033170965616591275,0.015974905341863632},{{{298.51016235351562,274.30905151367188,-482.64352416992188},{299.42431640625,274.30764770507812,-481.86004638671875}}},{{{-0.0012141824699938297,0.99999481439590454,0.0029777726158499718},{-0.00121476617641747,0.99999481439590454,0.0029784541111439466}}},{-0.0032062772661447525,-0.0058919000439345837},{7.1761870384216309,7.1762752532958984},{true,true}},
        {4161ULL,{298.97958374023438,275.20437622070312,-482.24874877929688},{0.0060825408436357975,0.9084746241569519,-0.0011017823126167059,-0.41789427399635315},{-1.8619743585586548,0.044550057500600815,-1.5980908870697021},{-0.014549970626831055,0.00031714807846583426,0.01413594838231802},{{{298.479248046875,274.30908203125,-482.6700439453125},{299.3934326171875,274.30770874023438,-481.88656616210938}}},{{{-0.0012141824699938297,0.99999481439590454,0.0029777726158499718},{-0.0012151686241850257,0.99999481439590454,0.0029789230320602655}}},{-0.0027179985772818327,-0.0050679291598498821},{7.1840133666992188,7.1840062141418457},{true,true}},
        {4162ULL,{298.94851684570312,275.20501708984375,-482.27532958984375},{0.0060104201547801495,0.9084741473197937,-0.0012303334660828114,-0.41789600253105164},{-1.863722562789917,0.038527876138687134,-1.595736026763916},{-0.010396387428045273,0.00031614620820619166,0.014308307319879532},{{{298.44833374023438,274.30914306640625,-482.696533203125},{299.36248779296875,274.30776977539062,-481.91305541992188}}},{{{-0.0012151687406003475,0.99999481439590454,0.0029789230320602655},{-0.0012147660600021482,0.99999481439590454,0.0029784541111439466}}},{-0.002321233507245779,-0.0043049943633377552},{7.1875567436218262,7.1875524520874023},{true,true}},
}};
Fixture recovery_fixture(const RecoveryRow161& row,bool verified_plane=true) {
    Fixture f;f.body.center_of_mass=row.com;f.body.orientation=row.rotation;
    f.body.linear_velocity=row.velocity;f.body.angular_velocity=row.angular_velocity;
    const auto forward=axes(row.rotation)[2];
    for(unsigned i=0;i<2;++i) {
        f.wheels[i].angular_speed=row.wheel_speed[i];f.contacts[i].separation=row.gap[i];
        f.contacts[i].contact.status=row.hit[i]?t::ContactStatus::hit:t::ContactStatus::clear;
        f.contacts[i].contact.point=row.points[i];f.contacts[i].contact.normal=row.hit[i]?row.normals[i]:axes(row.rotation)[1];
        f.contacts[i].contact.forward=forward;f.contacts[i].balance_normal=row.normals[i];
        f.contacts[i].balance_normal_available=row.hit[i];f.contacts[i].verified_plane=verified_plane&&row.hit[i];
    }
    return f;
}
void recovery_balance_run161() {
    c::Result lost_front;lost_front.valid=true;lost_front.balance_continuation_mask=2;
    lost_front.normal_load={0,500};
    check(!c::detail::recovery_loaded_pair(lost_front),
        "rear-only continuation may not adopt an assisted result that unloaded the unmasked strict front wheel");
    lost_front.normal_load={300,0};
    check(!c::detail::recovery_loaded_pair(lost_front),"lost rear load also forces the existing baseline result/state fallback");
    lost_front.normal_load={300,500};
    check(c::detail::recovery_loaded_pair(lost_front),"valid assisted loaded pair is eligible for atomic controller/result adoption");
    lost_front.normal_load[0]=std::numeric_limits<double>::infinity();
    check(!c::detail::recovery_loaded_pair(lost_front),"nonfinite assisted load cannot pass the pair adoption gate");
    c::Controller guarded,old_policy;unsigned restored=0;
    for(const auto& row:recovery161) {
        const auto f=recovery_fixture(row),old=recovery_fixture(row,false);
        c::Controls control;control.recovery_step_verified=row.serial>recovery161.front().serial;
        const auto got=guarded.step(h,f.body,f.wheels,f.contacts,control);
        const auto base=old_policy.step(h,old.body,old.wheels,old.contacts,control);
        check(got.valid&&base.valid,"measured161 fixed-input controller evaluations remain valid");
        check(got.grounded==base.grounded&&!got.hop_applied&&got.hop_impulse==t::Vec{},
            "recovery balance never grants strict support or a new hop on recorded161 inputs");
        for(unsigned i=0;i<2;++i)check(c::eligible_support(f.body,f.contacts[i])==c::eligible_support(old.body,old.contacts[i]),
            "actual per-wheel eligibility stays independent of the new plane-admission metadata");
        if(row.serial>=4146&&row.serial<=4148) {
            check(got.balance_support_mask==0&&got.balance_continuation_mask==0&&got.balance_torque==t::Vec{},
                "true initial161 bounce receives no recovery assistance, even though the first wheel remains penetrating");
            vector_near(got.force,base.force,"unloaded initial bounce forces retain baseline arithmetic");
            vector_near(got.torque,base.torque,"unloaded initial bounce torques retain baseline arithmetic");
        }
        if(row.serial>=4151&&row.serial<=4157) {
            check(base.balance_support_mask==0&&base.balance_torque==t::Vec{}&&
                got.balance_support_mask==3&&got.balance_continuation_mask==3&&length(got.balance_torque)>1&&
                got.normal_load[0]>0&&got.normal_load[1]>0,"loaded seven-step161 dropout retains measured-plane balance without touching support eligibility");
            ++restored;
        }
    }
    check(restored==7,"all seven previously zero-balance loaded recovery samples are covered");
    const auto seed=[&](c::Controller& controller) {
        for(unsigned index:{5u,6u}) {const auto f=recovery_fixture(recovery161[index]);c::Controls control;control.recovery_step_verified=true;
            check(controller.step(h,f.body,f.wheels,f.contacts,control).valid,"two actual loaded same-plane samples seed the rejection fixture");}
    };
    for(unsigned failure=0;failure<19;++failure) {
        c::Controller controller;seed(controller);auto f=recovery_fixture(recovery161[7]);
        c::Controls control;control.recovery_step_verified=true;
        if(failure==0)control.recovery_step_verified=false;
        if(failure==1)controller.reset();
        if(failure==2) {auto missing=f;missing.contacts[0].contact.status=t::ContactStatus::unavailable;
            check(!controller.step(h,missing.body,missing.wheels,missing.contacts,control).valid,"unavailable query clears recovery history");}
        if(failure==3)check(!controller.step(0,f.body,f.wheels,f.contacts,control).valid,"paused/invalid native step clears recovery history");
        if(failure==4)f.contacts[1].verified_plane=false;
        if(failure==5)f.contacts[1].contact.status=t::ContactStatus::clear;
        if(failure==6)f.contacts[1].separation=.001;
        if(failure==7)f.contacts[1].separation=-.031;
        if(failure==8)f.contacts[1].contact.point[1]+=.003;
        if(failure==9) {f.contacts[1].contact.normal={.004,std::sqrt(1-.004*.004),0};f.contacts[1].balance_normal=f.contacts[1].contact.normal;}
        if(failure==10)control.hop_event_id=1,control.hop_strength=1;
        if(failure==11)control.manual=.1;
        if(failure==12)control.air_yaw=.1;
        if(failure==13) {f.body.linear_velocity[0]*=-1;f.body.linear_velocity[2]*=-1;}
        if(failure==14)f.body.orientation={1,0,0,0};
        if(failure==15)f.body.linear_velocity[1]+=.026;
        if(failure==16)f.contacts[1].contact.surface_velocity[0]=.001;
        if(failure==17)control.enabled=false;
        if(failure==18)control.steer=.1;
        const auto rejected=controller.step(h,f.body,f.wheels,f.contacts,control);
        check(rejected.valid&&rejected.balance_support_mask==0&&rejected.balance_continuation_mask==0,
            "missing epoch/history/plane/topology, changed gap/face/platform, hop/control/reversal or upside-down state cannot inherit new recovery balance");
    }
    c::Controller cold;const auto f=recovery_fixture(recovery161[7]);c::Controls control;control.recovery_step_verified=true;
    const auto no_history=cold.step(h,f.body,f.wheels,f.contacts,control);
    check(no_history.valid&&no_history.normal_load[0]>0&&no_history.normal_load[1]>0&&no_history.balance_support_mask==0,
        "current positive loads alone cannot bootstrap measured161 balance without actual prior support");
    Fixture bounded;for(auto& sample:bounded.contacts) {
        sample.separation=-.02;sample.verified_plane=sample.balance_normal_available=true;sample.balance_normal={0,1,0};
    }
    c::Controller duration;control={};control.recovery_step_verified=true;
    auto previous=duration.step(h,bounded.body,bounded.wheels,bounded.contacts,control);
    check(previous.valid&&previous.grounded,"synthetic negative-gap loaded support seeds a finite recovery budget");
    for(unsigned i=1;i<=13;++i) {
        bounded.body.linear_velocity[1]=previous.recovery_velocity[0];
        for(auto& sample:bounded.contacts)sample.separation=-.02+i*.00001;
        const auto current=duration.step(h,bounded.body,bounded.wheels,bounded.contacts,control);
        check(current.valid&&!current.grounded&&current.normal_load[0]>0&&current.normal_load[1]>0,
            "finite-budget fixture remains loaded and outside strict support");
        check((i<=12&&current.balance_continuation_mask==3)||(i==13&&current.balance_support_mask==0),
            "new recovery admission stops after0.2native seconds even with coherent loaded penetrating planes");
        previous=current;
    }
}


void neutral_air_damping_passivity() {
    // Independent full-tensor energy check, including asymmetric principal
    // moments and nontrivial world orientation. A cap scales the whole vector.
    unsigned cases=0;
    for(const auto q:{t::Quat{0,0,0,1},t::Quat{.5,.5,.5,.5},t::Quat{.2,-.4,.1,std::sqrt(.79)}})
    for(const auto moments:{t::Vec{26,32,10},t::Vec{.01,1000,.3},t::Vec{200,2,40}})
    for(const auto omega:{t::Vec{.764,0,0},t::Vec{1,-2,3},t::Vec{-30,20,-10},t::Vec{}})
    for(double dt:{1e-6,1./240,1./120,h,1./30,.1})
    for(double rate:{0.,.1,2.75,6.,10.})for(double cap:{0.,1.,160.}) {
        auto body=c::proposed_body();body.orientation=q;body.inertia_body=moments;body.angular_velocity=omega;
        const auto torque=c::detail::neutral_air_torque(body,dt,rate,cap);
        check(dot(omega,torque)<=1e-7,"neutral-air torque never adds instantaneous angular power");
        check(length(torque)<=cap+1e-7,"neutral-air torque respects the shared air authority norm cap");
        const auto next=add(omega,tensor(body,mul(torque,dt),true));
        const double before=.5*dot(omega,tensor(body,omega,false));
        const double after=.5*dot(next,tensor(body,next,false));
        check(after<=before+1e-7*std::max(1.,before),"implicit full-tensor air damping cannot increase fixed-tensor kinetic energy");
        check(dot(omega,next)>=-1e-7&&length(next)<=length(omega)+1e-7,
            "implicit damping never reverses or accelerates free angular velocity");
        ++cases;
    }
    check(cases==3240,"full-tensor damper boundary grid executed");
    auto body=c::proposed_body();body.angular_velocity={1,2,3};
    for(double bad:{-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        vector_near(c::detail::neutral_air_torque(body,bad,2.75,160),{},"bad damper time has no torque");
        vector_near(c::detail::neutral_air_torque(body,h,bad,160),{},"bad damper rate has no torque");
        vector_near(c::detail::neutral_air_torque(body,h,2.75,bad),{},"bad damper cap has no torque");
    }
}
void neutral_air_damping_intent_lifetime() {
    c::Controls proof;proof.recovery_step_verified=true;
    const auto qualify=[&](c::Controller& controller) {
        Fixture ground;const auto seed=controller.step(h,ground.body,ground.wheels,ground.contacts,proof);
        check(seed.valid&&seed.grounded&&seed.normal_load[0]>0&&seed.normal_load[1]>0,
            "strict current normal loads establish an observed departure");
    };
    Fixture air;air.air();air.body.angular_velocity={.7,-.4,.3};
    c::Controller unobserved;const auto unknown=unobserved.step(h,air.body,air.wheels,air.contacts,proof);
    check(unknown.valid&&!unknown.neutral_air_damping_active,"a controller first seen in air never invents prior ground qualification");
    c::Controller neutral;qualify(neutral);
    const auto damped=neutral.step(h,air.body,air.wheels,air.contacts,proof);
    check(damped.valid&&damped.neutral_air_damping_active&&!damped.air_rotation_reserved,
        "known-clear neutral departure gets passive rate damping");
    vector_near(damped.force,{},"neutral air damping adds no translation, gravity or hop impulse");
    vector_near(damped.air_control_torque,{},"passive damping is not attributed to player air controls");
    check(!damped.hop_applied&&damped.hop_impulse==t::Vec{}&&damped.normal_load==std::array<double,2>{},
        "air damping cannot invent support, normal loads or a hop");
    check(dot(air.body.angular_velocity,damped.air_damping_torque)<0,"actual controller applies a dissipative air torque");
    for(unsigned channel=0;channel<6;++channel) {
        c::Controller flight;qualify(flight);auto command=proof;
        if(channel==0)command.manual=.5;
        if(channel==1)command.nose_manual=.5;
        if(channel==2)command.steer=.5;
        if(channel==3)command.air_yaw=.5;
        if(channel==4)command.rotation_reserved=true; // adapter catalog/raw gesture reservation
        if(channel==5)command.brake=.5; // deliberate airborne axle reaction
        const auto active=flight.step(h,air.body,air.wheels,air.contacts,command);
        check(active.valid&&active.air_rotation_reserved&&!active.neutral_air_damping_active,
            "each deliberate flip/yaw/catalog input reserves the entire airborne interval");
        for(unsigned i=0;i<3;++i) {
            const auto released=flight.step(h,air.body,air.wheels,air.contacts,proof);
            check(released.valid&&released.air_rotation_reserved&&!released.neutral_air_damping_active,
                "releasing a deliberate airborne input cannot brake the continuing spin or flip");
        }
        // A merely speculative hit is not a landing and cannot clear intent.
        auto skin=air;skin.contacts[0].contact.status=t::ContactStatus::hit;skin.contacts[0].separation=.02;
        const auto speculative=flight.step(h,skin.body,skin.wheels,skin.contacts,proof);
        check(speculative.valid&&!speculative.grounded&&speculative.air_rotation_reserved,
            "speculative query contact cannot clear deliberate flight reservation");
        qualify(flight);
        check(flight.step(h,air.body,air.wheels,air.contacts,proof).neutral_air_damping_active,
            "a genuine strict loaded recontact admits a new neutral departure");
    }
    // Manual/gesture intent on the last grounded sample survives stick release
    // exactly at liftoff; ordinary ground steering is not erased by air rearm.
    for(unsigned channel=0;channel<4;++channel) {
        c::Controller flight;Fixture ground;auto launch=proof;
        if(channel==0)launch.manual=.5;if(channel==1)launch.nose_manual=.5;if(channel==2)launch.rotation_reserved=true;
        if(channel==3)launch.brake=.5;
        check(flight.step(h,ground.body,ground.wheels,ground.contacts,launch).valid,"ground launch intent fixture valid");
        const auto first=flight.step(h,air.body,air.wheels,air.contacts,proof);
        check(first.air_rotation_reserved&&!first.neutral_air_damping_active,"last grounded intentional pose survives release at takeoff");
    }
    // Real tire axle response: B/Circle braking a spinning free rotor creates
    // body pitch. After release that intentionally generated momentum must
    // coast under the unchanged tire solve, never become neutral damping.
    {
        Fixture spinning;spinning.air();spinning.wheels[0].angular_speed=spinning.wheels[1].angular_speed=20;
        c::Controller flight;qualify(flight);auto brake=proof;brake.brake=1;
        const auto stopped=flight.step(h,spinning.body,spinning.wheels,spinning.contacts,brake);
        check(stopped.valid&&stopped.air_rotation_reserved&&!stopped.neutral_air_damping_active,
            "air brake explicitly reserves the continuing flight");
        spinning.body=response(spinning.body,stopped);spinning.wheels=stopped.wheels;
        check(length(spinning.body.angular_velocity)>.01,"air brake fixture actually transfers rotor momentum into body rotation");
        const auto before=spinning.body.angular_velocity;
        for(unsigned i=0;i<3;++i) {
            const auto released=flight.step(h,spinning.body,spinning.wheels,spinning.contacts,proof);
            check(released.valid&&released.air_rotation_reserved&&!released.neutral_air_damping_active,
                "released air brake cannot rearm the damper against its own induced pitch");
            vector_near(released.air_damping_torque,{},"released air brake retains zero passive damping torque");
            spinning.body=response(spinning.body,released);spinning.wheels=released.wheels;
            vector_near(spinning.body.angular_velocity,before,"air-brake-induced rotation coasts after release",1e-6);
        }
    }
    for(unsigned failure=0;failure<5;++failure) {
        c::Controller flight;qualify(flight);auto command=proof;auto rejected=air;double dt=h;
        if(failure==0)command.recovery_step_verified=false;
        if(failure==1)command.enabled=false;
        if(failure==2)dt=1./120;
        if(failure==3)rejected.contacts[0].contact.status=t::ContactStatus::unavailable;
        if(failure==4)command.manual=std::numeric_limits<double>::quiet_NaN();
        flight.step(dt,rejected.body,rejected.wheels,rejected.contacts,command);
        check(!flight.step(h,air.body,air.wheels,air.contacts,proof).neutral_air_damping_active,
            "clock/input/query/proof loss cannot rearm damping in midair");
    }
    c::Controller reset;qualify(reset);reset.reset();
    check(!reset.step(h,air.body,air.wheels,air.contacts,proof).neutral_air_damping_active,"mount/source reset requires fresh ground proof");
    // Strict geometry alone, while separating slowly enough for the input
    // threshold, does not prove that a new support impulse occurred.
    Fixture unloaded;const t::Vec steep_normal{0,.2,std::sqrt(.96)};
    unloaded.body.linear_velocity=mul(steep_normal,.04);
    for(auto& sample:unloaded.contacts){sample.contact.normal=steep_normal;sample.contact.forward={1,0,0};}
    c::Tuning no_assist;no_assist.balance_stiffness=no_assist.balance_damping=0;no_assist.friction={0,0};
    c::Controller no_load(no_assist);const auto hit=no_load.step(h,unloaded.body,unloaded.wheels,unloaded.contacts,proof);
    check(hit.valid&&hit.grounded&&hit.normal_load==std::array<double,2>{}&&
        !no_load.step(h,air.body,air.wheels,air.contacts,proof).neutral_air_damping_active,
        "unloaded queried plane cannot manufacture a prior supported departure");
}
void neutral_air_contact_and_hop_invariance() {
    c::Tuning disabled;disabled.neutral_air_damping_rate=0;
    c::Controls proof;proof.recovery_step_verified=true;
    for(unsigned scenario=0;scenario<5;++scenario) {
        Fixture f;c::Controller candidate,reference(disabled);
        candidate.step(h,f.body,f.wheels,f.contacts,proof);reference.step(h,f.body,f.wheels,f.contacts,proof);
        f.body.angular_velocity={.1,0,-.1};auto cmd=proof;
        if(scenario==1){cmd.hop_event_id=1;cmd.hop_strength=1;}
        if(scenario==2){cmd.manual=.5;}
        if(scenario==3){f.contacts[0].separation=f.contacts[1].separation=.02;f.body.linear_velocity={0,-2,0};}
        if(scenario==4){f.air();f.contacts[0].contact.status=t::ContactStatus::hit;f.contacts[0].separation=.005;f.body.linear_velocity={0,-4,0};}
        const auto a=candidate.step(h,f.body,f.wheels,f.contacts,cmd),b=reference.step(h,f.body,f.wheels,f.contacts,cmd);
        check(a.valid&&b.valid&&a.force==b.force&&a.torque==b.torque&&a.hop_impulse==b.hop_impulse&&
            a.hop_applied==b.hop_applied&&a.grounded==b.grounded&&a.normal_load==b.normal_load&&
            a.normal_impulse==b.normal_impulse&&a.balance_torque==b.balance_torque&&a.contact_scale==b.contact_scale,
            "actual/speculative contact, manual and hop arithmetic remains exact with neutral damping enabled");
        check(a.air_damping_torque==t::Vec{},"any queried force contact bypasses the damper");
    }
    // Captured182 first front landing: source inputs, no synthesized support.
    Fixture f;f.air();f.body.center_of_mass={299.51837158203125,275.36981201171875,-481.78497314453125};
    f.body.orientation={-.05531269311904907,.8997855186462402,-.12567606568336487,-.4141642153263092};
    f.body.linear_velocity={-2.055708408355713,-4.052579879760742,-1.7676571607589722};
    f.body.angular_velocity={-.5037664175033569,-.0009053579997271299,.5743241906166077};
    f.contacts[0].contact.status=t::ContactStatus::hit;f.contacts[0].separation=.005460713524371386;
    f.contacts[0].contact.point={299.1559753417969,274.3080749511719,-482.0926818847656};
    f.contacts[0].contact.normal={-.001243262435309589,.9999948740005493,.0029513826593756676};
    const auto forward=axes(f.body.orientation)[2];for(auto& sample:f.contacts)sample.contact.forward=forward;
    c::Tuning enabled;enabled.contact_gap=disabled.contact_gap=.03;
    c::Controller candidate(enabled),reference(disabled);Fixture ground;
    candidate.step(h,ground.body,ground.wheels,ground.contacts,proof);reference.step(h,ground.body,ground.wheels,ground.contacts,proof);
    auto clear=f;clear.air();candidate.step(h,clear.body,clear.wheels,clear.contacts,proof);reference.step(h,clear.body,clear.wheels,clear.contacts,proof);
    const auto a=candidate.step(h,f.body,f.wheels,f.contacts,proof),b=reference.step(h,f.body,f.wheels,f.contacts,proof);
    check(a.valid&&b.valid&&a.force==b.force&&a.torque==b.torque&&a.normal_impulse==b.normal_impulse&&
        a.grounded==b.grounded&&!a.neutral_air_damping_active,"captured hard front impact is not hidden or weakened by the air damper");
    check(length(a.torque)>3900,"captured-input hard impact still reaches the original angular impulse budget");
}

void slope_recovery_and_rejections() {
    Fixture f;c::Tuning settings;settings.balance_stiffness=settings.balance_damping=0;settings.friction={0,0};
    c::Controller frictionless(settings);constexpr double angle=.3;
    const t::Vec normal{0,std::cos(angle),-std::sin(angle)},forward{0,std::sin(angle),std::cos(angle)};
    for(unsigned i=0;i<2;++i) {auto& hit=f.contacts[i].contact;hit.normal=normal;hit.forward=forward;hit.point=add(sub(f.body.center_of_mass,mul(normal,.9)),mul(forward,i==0?.575:-.475));}
    const auto slope=frictionless.step(h,f.body,f.wheels,f.contacts);
    check(slope.valid&&std::abs(dot(slope.force,forward))<1e-7,"frictionless slope support does not cancel genuine downhill gravity");
    f=Fixture{};f.contacts[0].separation=f.contacts[1].separation=-.05;c::Controller controller;
    const auto recovery=controller.step(h,f.body,f.wheels,f.contacts);
    check(recovery.valid&&near(recovery.recovery_velocity[0],.4)&&near(recovery.recovery_velocity[1],.4),
        "small penetration has an explicit bounded normal-velocity target");
    check(recovery.force[1]>106*9.8,"constraint recovery can add bounded upward energy and is reported separately");
    const auto original=f.body;const auto wheels=f.wheels;
    for(double bad:{0.,1./120,.1,std::numeric_limits<double>::quiet_NaN()}) {
        c::Controls event;event.hop_event_id=9;event.hop_strength=1;const auto no=controller.step(bad,f.body,f.wheels,f.contacts,event);
        check(!no.valid&&no.issue==c::Issue::unsupported_step&&no.force==t::Vec{}&&no.hop_impulse==t::Vec{},"pause, wrong native rate, stall and NaN cannot submit forces");
    }
    check(controller.consumed_hop_event_id()==0,"invalid samples do not partially consume an event");
    f.contacts[0].contact.status=t::ContactStatus::unavailable;
    check(!controller.step(h,f.body,f.wheels,f.contacts).valid,"unavailable is not silently treated as clear or a floor");
    f=Fixture{};f.contacts[0].contact.normal={};check(!controller.step(h,f.body,f.wheels,f.contacts).valid,"zero contact normal rejects");
    f=Fixture{};f.body.orientation={0,0,0,2};check(!controller.step(h,f.body,f.wheels,f.contacts).valid,"nonunit measured orientation rejects");
    f=Fixture{};c::Controls invalid;invalid.pedal=std::numeric_limits<double>::quiet_NaN();
    check(!controller.step(h,f.body,f.wheels,f.contacts,invalid).valid,"nonfinite input cannot become native force");
    f=Fixture{};f.contacts[1].separation=-.2;check(controller.step(h,f.body,f.wheels,f.contacts).issue==c::Issue::penetration_limit,"deep penetration is rejected instead of teleported out");
    f=Fixture{};settings={};settings.force_limit=1;c::Controller limited(settings);const auto cap=limited.step(h,f.body,f.wheels,f.contacts);
    check(cap.valid&&cap.contact_scale>0&&cap.contact_scale<1&&length(cap.force)<=1&&near(cap.normal_load[0]+cap.normal_load[1],cap.force[1]),
        "contact-only over-budget support is coherently attenuated instead of clipping chassis components");
    c::Controls hop;hop.hop_event_id=11;hop.hop_strength=1;const auto unsafe_hop=limited.step(h,f.body,f.wheels,f.contacts,hop);
    check(!unsafe_hop.valid&&unsafe_hop.issue==c::Issue::output_limit&&limited.consumed_hop_event_id()==0,
        "over-budget separate hop rejects without weakening hop or consuming its event");
    check(original.mass==106&&wheels[0].angular_speed==0,"pure controller leaves caller-owned measured state unchanged");
}
}
int main() {
    neutral_air_damping_passivity();neutral_air_damping_intent_lifetime();neutral_air_contact_and_hop_invariance();
    support_and_gravity();steering_and_platforms();steering_matches_lean_envelope();sustained_turns_remain_ridable();steering_release_settles_without_countersteer_wobble();supported_steering_history_contract();physical_drive_and_brake();hop_events_and_manual_pitch();bounded_impact_contacts();motor_cadence_taper();actual_support_and_speculative_gap();edge_force_normal_does_not_command_body_roll();curved_ramp_balance_and_history();loaded_balance_continuity();recovery_balance_run161();slope_recovery_and_rejections();
    std::cout<<"BMX chassis force controller support, steering, rotor conservation, hop events and bounded native60Hz tests passed.\n";
}
