#include "Engine/Game/BMX/bmx_tire_dynamics.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>

namespace {
namespace t=dingosdk::bmx::tire;
void check(bool value,const char* reason) {
    if(!value){std::cerr<<reason<<'\n';std::exit(1);}
}
bool near(double a,double b,double tolerance=1e-9){return std::abs(a-b)<=tolerance;}
double dot(t::Vec a,t::Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
t::Vec sub(t::Vec a,t::Vec b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
t::Vec cross(t::Vec a,t::Vec b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
bool same(const t::Body& a,const t::Body& b) {
    return a.mass==b.mass&&a.inertia_body==b.inertia_body&&a.orientation==b.orientation&&
        a.center_of_mass==b.center_of_mass&&a.linear_velocity==b.linear_velocity&&a.angular_velocity==b.angular_velocity;
}
bool same(const t::Wheel& a,const t::Wheel& b){return a.radius==b.radius&&a.inertia==b.inertia&&a.angular_speed==b.angular_speed;}
double energy(const t::Body& body,const t::Wheel& wheel,t::Vec ground={}) {
    const auto v=sub(body.linear_velocity,ground);const auto& q=body.orientation;
    // Independent body-to-world rotation matrix, transposed for angular energy.
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    const t::Vec a{1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)};
    const t::Vec b{2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)};
    const t::Vec c{2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
    const t::Vec angular{dot(body.angular_velocity,a),dot(body.angular_velocity,b),dot(body.angular_velocity,c)};
    double value=.5*body.mass*dot(v,v)+.5*wheel.inertia*wheel.angular_speed*wheel.angular_speed;
    for(unsigned i=0;i<3;++i)value+=.5*body.inertia_body[i]*angular[i]*angular[i];
    return value;
}
t::Vec angular_momentum(const t::Body& body,const t::Wheel& wheel,t::Vec axle) {
    const auto& q=body.orientation;const double x=q[0],y=q[1],z=q[2],w=q[3];
    const t::Vec axes[]{
        {1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)},
        {2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)},
        {2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)}};
    t::Vec result{};
    for(unsigned i=0;i<3;++i) {
        result[i]=wheel.inertia*wheel.angular_speed*axle[i];
        for(unsigned j=0;j<3;++j)result[i]+=body.inertia_body[j]*dot(body.angular_velocity,axes[j])*axes[j][i];
    }
    return result;
}
t::Contact flat() {t::Contact c;c.status=t::ContactStatus::hit;c.point={0,-.4,0};return c;}
void point_and_orientation() {
    t::Body body;body.linear_velocity={1,2,3};body.angular_velocity={0,2,0};
    check(t::point_velocity(body,{0,0,3})==t::Vec{7,2,3},"point velocity includes angular cross lever-arm velocity");
    body={};body.mass=2;body.inertia_body={2,4,8};
    check(t::apply_impulse(body,{0,0,1},{4,0,0}),"finite point impulse must apply");
    check(body.linear_velocity==t::Vec{2,0,0}&&body.angular_velocity==t::Vec{0,1,0},"off-COM impulse conserves linear/angular impulse with actual inertia");
    body={};body.inertia_body={2,4,8};const double half=std::sqrt(.5);body.orientation={0,half,0,half};
    check(t::apply_impulse(body,{0,1,0},{0,0,4}),"rotated inertia must accept a point impulse");
    check(near(body.angular_velocity[0],.5)&&near(body.angular_velocity[1],0)&&near(body.angular_velocity[2],0),
        "world inertia follows body orientation rather than using principal moments on world axes");
    t::Basis basis;
    check(t::contact_basis({0,3,0},{0,2,4},basis),"nonunit normal and nontangent forward direction are orthonormalized");
    check(near(dot(basis.normal,basis.forward),0)&&near(dot(basis.normal,basis.lateral),0)&&near(dot(basis.forward,basis.lateral),0)&&
        near(dot(basis.normal,basis.normal),1)&&near(dot(basis.forward,basis.forward),1)&&near(dot(basis.lateral,basis.lateral),1),
        "contact frame is finite and orthonormal");
    check(!t::contact_basis({0,0,0},{0,0,1},basis)&&!t::contact_basis({0,1,0},{0,7,0},basis),
        "zero normal or undefined rolling tangent cannot invent an axis");
}
void drive_load_and_no_adhesion() {
    t::Body body;body.mass=10;body.linear_velocity={0,-.1,0};t::Wheel wheel;
    auto contact=flat();
    const auto result=t::solve(.01,body,wheel,contact,{2,0},{0,0});
    check(result.valid&&result.touching&&near(result.normal_impulse,1)&&near(result.normal_load,100),"normal load is nonnegative impulse over real time");
    check(near(wheel.angular_speed,.4)&&near(body.linear_velocity[0],0)&&near(body.linear_velocity[2],0)&&
        result.longitudinal_impulse==0&&result.lateral_impulse==0,"frictionless pedal torque spins the wheel without chassis traction");
    body={};body.mass=10;body.linear_velocity={0,1,0};wheel={};
    const auto separating=t::solve(.01,body,wheel,contact,{2,0});
    check(separating.valid&&separating.normal_impulse==0&&body.linear_velocity==t::Vec{0,1,0},"separating contact cannot attract the body or supply grip");
    body={};wheel={};
    const auto no_load=t::solve(.01,body,wheel,contact,{2,0});
    check(no_load.valid&&no_load.normal_load==0&&body.linear_velocity==t::Vec{},"stationary contact does not fabricate mass*g force before an external gravity step");
    body={};wheel={};contact.status=t::ContactStatus::clear;
    const auto clear=t::solve(.01,body,wheel,contact,{2,0});
    check(clear.valid&&!clear.touching&&near(wheel.angular_speed,.4)&&body.linear_velocity==t::Vec{},"known clear contact advances the free wheel without chassis impulses");
}
void braking_and_coasting() {
    for(double speed:{-5.0,0.0,5.0}) {
        t::Body body;body.mass=20;body.inertia_body={4,5,6};body.linear_velocity={0,-.1,speed};
        t::Wheel wheel;wheel.angular_speed=speed/wheel.radius;const double before=energy(body,wheel);
        const auto result=t::solve(.01,body,wheel,flat(),{0,100});
        check(result.valid&&result.brake_energy>=0,"brake removes internal axle energy and retains finite contact");
        check(std::abs(body.linear_velocity[2])<=std::abs(speed)+1e-10&&body.linear_velocity[2]*speed>=0,
            "braking a rolling wheel cannot accelerate chassis speed or reverse its travel");
        check(energy(body,wheel)<=before+1e-9,"static-ground brake/contact response cannot create total chassis-plus-wheel energy");
        if(speed==0)check(wheel.angular_speed==0&&body.linear_velocity[2]==0,"brake at rest cannot spin a wheel or launch the chassis");
    }
    t::Body body;body.linear_velocity={0,-.1,4};t::Wheel wheel;wheel.angular_speed=4/wheel.radius;
    auto contact=flat();contact.point={0,0,0};
    const auto coast=t::solve(.01,body,wheel,contact);
    check(coast.valid&&near(coast.longitudinal_impulse,0)&&near(body.linear_velocity[2],4)&&near(wheel.angular_speed,4/wheel.radius),
        "pure rolling coast does not acquire arbitrary longitudinal drag");
}
void internal_axle_couples() {
    auto contact=flat();contact.status=t::ContactStatus::clear;
    contact.normal={1,1,0};contact.forward={0,0,1};
    t::Basis frame;check(t::contact_basis(contact.normal,contact.forward,frame),"tilted free-wheel axle has a finite frame");
    t::Body body;body.inertia_body={2,3,5};const double yaw=.6;body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
    t::Wheel wheel;const auto initial=angular_momentum(body,wheel,frame.lateral);
    const auto driven=t::solve(.01,body,wheel,contact,{3,0});
    check(driven.valid&&wheel.angular_speed>0&&body.linear_velocity==t::Vec{},"airborne drive spins a rotor without inventing a linear force");
    const auto after=angular_momentum(body,wheel,frame.lateral);
    for(unsigned i=0;i<3;++i)check(near(initial[i],after[i]),"drive axle torque has equal/opposite chassis reaction and conserves total angular momentum");
    contact.normal={0,1,0};contact.forward={0,0,1};
    body={};body.angular_velocity={10,0,0};wheel={};wheel.angular_speed=10;
    const auto before_body=body;const auto before_wheel=wheel;
    const auto rigid=t::solve(.01,body,wheel,contact,{0,100});
    check(rigid.valid&&same(body,before_body)&&same(wheel,before_wheel)&&near(rigid.brake_energy,0),
        "brake cannot damp a rigidly co-rotating chassis/rotor with zero internal slip");
    body={};body.angular_velocity={10,0,0};wheel={};
    const double before=energy(body,wheel);const auto brake=t::solve(.1,body,wheel,contact,{0,100});
    check(brake.valid&&wheel.angular_speed>0&&body.angular_velocity[0]<10&&near(body.angular_velocity[0],wheel.angular_speed),
        "internal braking equalizes relative axle speed without assuming the absolute wheel speed must decrease");
    check(near(angular_momentum(body,wheel,{1,0,0})[0],10)&&near(before-energy(body,wheel),brake.brake_energy)&&brake.brake_energy>0,
        "brake dissipates exact combined energy while retaining chassis-plus-wheel angular momentum");
}
void slope_and_moving_ground() {
    constexpr double seconds=.01,gravity=9.81,angle=.4;
    t::Body body;body.mass=10;body.linear_velocity={0,-gravity*seconds,0};t::Wheel wheel;
    auto contact=flat();contact.normal={0,std::cos(angle),-std::sin(angle)};
    contact.point={0,-.4*contact.normal[1],-.4*contact.normal[2]};contact.forward={0,-std::sin(angle),-std::cos(angle)};
    const auto slope=t::solve(seconds,body,wheel,contact,{}, {0,0});
    const double downhill=gravity*seconds*std::sin(angle);
    check(slope.valid&&near(body.linear_velocity[1],-downhill*std::sin(angle))&&near(body.linear_velocity[2],-downhill*std::cos(angle)),
        "frictionless support removes only normal motion and preserves analytical g*sin(slope) downhill velocity");
    t::Body a;a.mass=10;a.inertia_body={2,3,4};a.linear_velocity={.4,-.1,3};t::Wheel wa;
    auto ca=flat();ca.surface_velocity={0,0,1};
    auto b=a;auto wb=wa;auto cb=ca;const t::Vec boost{2,0,5};
    for(unsigned i=0;i<3;++i){b.linear_velocity[i]+=boost[i];cb.surface_velocity[i]+=boost[i];}
    const double before=energy(a,wa,ca.surface_velocity);
    const auto ra=t::solve(seconds,a,wa,ca),rb=t::solve(seconds,b,wb,cb);
    check(ra.valid&&rb.valid&&near(ra.longitudinal_impulse,rb.longitudinal_impulse)&&near(ra.lateral_impulse,rb.lateral_impulse)&&
        near(wa.angular_speed,wb.angular_speed),"moving-ground impulses are Galilean invariant under a shared world velocity boost");
    for(unsigned i=0;i<3;++i)check(near(b.linear_velocity[i]-a.linear_velocity[i],boost[i])&&near(b.angular_velocity[i],a.angular_velocity[i]),
        "platform-relative velocity, not absolute zero, determines tire contact response");
    check(energy(a,wa,ca.surface_velocity)<=before+1e-9,"moving-ground contact is passive in its translating surface frame");
}
void mirrored_steer_and_combined_grip() {
    t::Body left;left.mass=15;left.inertia_body={3,5,7};left.linear_velocity={0,-.15,4};
    auto right=left;t::Wheel wl,wr;wl.angular_speed=wr.angular_speed=4/wl.radius;
    auto cl=flat(),cr=flat();cl.point=cr.point={0,-.4,.5};cl.forward={std::sin(.25),0,std::cos(.25)};cr.forward={-cl.forward[0],0,cl.forward[2]};
    const auto rl=t::solve(.01,left,wl,cl),rr=t::solve(.01,right,wr,cr);
    check(rl.valid&&rr.valid&&near(left.linear_velocity[0],-right.linear_velocity[0])&&
        near(left.linear_velocity[1],right.linear_velocity[1])&&near(left.linear_velocity[2],right.linear_velocity[2]),
        "mirrored front-wheel steering produces mirrored lateral response without changing forward load");
    check(near(left.angular_velocity[0],right.angular_velocity[0])&&near(left.angular_velocity[1],-right.angular_velocity[1])&&
        near(left.angular_velocity[2],-right.angular_velocity[2])&&std::abs(left.angular_velocity[1])>1e-6,
        "front contact yaw/roll torque mirrors and comes from the lever arm");
    std::mt19937 random(20261017);std::uniform_real_distribution<double> sample(-1,1);
    for(unsigned i=0;i<1200;++i) {
        t::Body body;body.mass=10+5*std::abs(sample(random));body.inertia_body={1+std::abs(sample(random)),2+std::abs(sample(random)),3+std::abs(sample(random))};
        const double yaw=sample(random);body.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
        body.linear_velocity={3*sample(random),-2*std::abs(sample(random)),3*sample(random)};
        body.angular_velocity={sample(random),sample(random),sample(random)};
        t::Wheel wheel;wheel.angular_speed=30*sample(random);
        auto contact=flat();contact.point={sample(random)*.5,-.4,sample(random)*.5};
        contact.normal={sample(random)*.3,1,sample(random)*.3};contact.forward={sample(random)*.4,0,1};
        t::Basis basis;check(t::contact_basis(contact.normal,contact.forward,basis),"adversarial contact frame is usable");
        const auto old_body=body;const auto before_momentum=angular_momentum(body,wheel,basis.lateral);
        const double before=energy(body,wheel);const auto result=t::solve(1.0/120,body,wheel,contact,{0,30*std::abs(sample(random))},{.6,.9});
        check(result.valid&&result.normal_impulse>=0,"anisotropic off-COM contacts remain finite and unilateral");
        check(energy(body,wheel)<=before+1e-8*(1+before),"coupled longitudinal/lateral friction and wheel braking never create energy");
        t::Vec impulse{};
        for(unsigned axis=0;axis<3;++axis)impulse[axis]=result.basis.normal[axis]*result.normal_impulse+
            result.basis.forward[axis]*result.longitudinal_impulse+result.basis.lateral[axis]*result.lateral_impulse;
        const auto external_moment=cross(sub(contact.point,body.center_of_mass),impulse);
        const auto after_momentum=angular_momentum(body,wheel,basis.lateral);
        for(unsigned axis=0;axis<3;++axis) {
            check(near((body.linear_velocity[axis]-old_body.linear_velocity[axis])*body.mass,impulse[axis],1e-8),
                "external contact impulse exactly accounts for chassis linear momentum");
            check(near(after_momentum[axis]-before_momentum[axis],external_moment[axis],1e-8),
                "chassis torque plus rotor spin equals the actual ground-contact moment without double counting");
        }
        if(result.normal_impulse>0) {
            const double x=result.longitudinal_impulse/(.6*result.normal_impulse),y=result.lateral_impulse/(.9*result.normal_impulse);
            check(x*x+y*y<=1+1e-9,"combined tire impulses stay within the load-dependent anisotropic friction ellipse");
        }else check(result.longitudinal_impulse==0&&result.lateral_impulse==0,"zero normal load cannot produce either tire force");
    }
}
void invalid_and_atomic() {
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    for(unsigned fault=0;fault<11;++fault) {
        t::Body body;body.linear_velocity={0,-.2,3};t::Wheel wheel;auto contact=flat();t::Controls controls{3,0};t::Parameters parameters;
        double seconds=.01;
        if(fault==0)contact.status=t::ContactStatus::unavailable;
        if(fault==1)contact.normal={0,0,0};
        if(fault==2)contact.forward={0,1,0};
        if(fault==3)seconds=-.01;
        if(fault==4)seconds=.2;
        if(fault==5)seconds=nan;
        if(fault==6)body.mass=0;
        if(fault==7)body.inertia_body[2]=-1;
        if(fault==8)body.orientation={0,0,0,0};
        if(fault==9)parameters.lateral_friction=std::numeric_limits<double>::infinity();
        if(fault==10)controls.drive_torque=1e6; // finite input, but result exceeds safe wheel-speed bounds
        const auto original_body=body;const auto original_wheel=wheel;
        const auto result=t::solve(seconds,body,wheel,contact,controls,parameters);
        check(!result.valid&&same(body,original_body)&&same(wheel,original_wheel),"unavailable/invalid query, clock, inertia or overflowing drive cannot partially mutate body or wheel");
    }
    t::Body body;body.linear_velocity={0,-.2,3};t::Wheel wheel;const auto original_body=body;const auto original_wheel=wheel;
    check(t::solve(0,body,wheel,flat(),{5,5}).valid&&same(body,original_body)&&same(wheel,original_wheel),"zero simulation time applies no load, brake, drive or contact impulse");
    check(!t::apply_impulse(body,{0,0,1},{nan,0,0})&&same(body,original_body),"invalid standalone point impulse is atomic too");
}
}
int main() {
    point_and_orientation();drive_load_and_no_adhesion();braking_and_coasting();internal_axle_couples();slope_and_moving_ground();
    mirrored_steer_and_combined_grip();invalid_and_atomic();
    std::cout<<"BMX tire impulses, friction/energy bounds, moving contact and validation tests passed\n";
}
