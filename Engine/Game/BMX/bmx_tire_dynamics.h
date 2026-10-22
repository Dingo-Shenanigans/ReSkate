#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Independent, portable contact arithmetic. This is not copied game physics or
// a Streets tuning match. No native APIs, positional repair, wheel queries,
// bicycle integrator, or rider stabilization are supplied here.
namespace dingosdk::bmx::tire {
using Vec = std::array<double,3>;
using Quat = std::array<double,4>; // Hamilton x,y,z,w; body axes to world
struct Body {
    double mass{1};
    Vec inertia_body{1,1,1}; // chassis moments about COM; independent rotor spin excluded
    Quat orientation{0,0,0,1};
    Vec center_of_mass{},linear_velocity{},angular_velocity{};
};
struct Wheel {
    double radius{.34},inertia{.05},angular_speed{}; // absolute axle spin, not chassis-relative
};
enum class ContactStatus { unavailable, clear, hit };
struct Contact {
    ContactStatus status{ContactStatus::unavailable};
    // Minimal zero-camber disk: axle = normal x projected-forward. A separate
    // tilted wheel axle/contact geometry is deliberately not modeled here.
    // For a clear query normal/forward still describe the wheel's current plane
    // so drive/brake axle reactions remain defined while airborne.
    Vec point{},normal{0,1,0},forward{0,0,1},surface_velocity{};
};
struct Controls { double drive_torque{},brake_torque{}; }; // N*m; brake >=0
struct Parameters {
    double longitudinal_friction{1},lateral_friction{1};
};
struct Basis { Vec normal{},forward{},lateral{}; };
struct Result {
    bool valid{},touching{};
    ContactStatus status{ContactStatus::unavailable};
    Basis basis;
    double normal_impulse{},longitudinal_impulse{},lateral_impulse{}; // N*s
    double normal_load{},brake_energy{}; // N and total dissipated J; no invented static load
};
namespace detail {
inline Vec add(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec a,double b) noexcept {for(auto& v:a)v*=b;return a;}
inline double dot(Vec a,Vec b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Vec cross(Vec a,Vec b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline bool finite(Vec v,double bound=1e9) noexcept {
    for(double x:v)if(!std::isfinite(x)||std::abs(x)>bound)return false;
    return true;
}
inline bool unit(Vec v,Vec& out) noexcept {
    const double square=dot(v,v);
    if(!finite(v)||!std::isfinite(square)||square<1e-18)return false;
    out=mul(v,1/std::sqrt(square));return true;
}
inline Quat rotation(const Body& b) noexcept {
    auto q=b.orientation;double square=0;for(double x:q)square+=x*x;
    for(auto& x:q)x/=std::sqrt(square);
    return q;
}
inline Vec turn(Quat q,Vec v) noexcept {
    const Vec axis{q[0],q[1],q[2]};const auto t=mul(cross(axis,v),2);
    return add(v,add(mul(t,q[3]),cross(axis,t)));
}
inline Vec inverse_inertia(const Body& body,Vec world) noexcept {
    const auto q=rotation(body);const Quat inverse{-q[0],-q[1],-q[2],q[3]};
    auto local=turn(inverse,world);for(unsigned i=0;i<3;++i)local[i]/=body.inertia_body[i];
    return turn(q,local);
}
inline bool positive(double v) noexcept {return std::isfinite(v)&&v>=1e-9&&v<=1e9;}
inline Vec response(const Body& body,Vec apply_arm,Vec at_arm,Vec impulse) noexcept {
    return add(mul(impulse,1/body.mass),cross(inverse_inertia(body,cross(apply_arm,impulse)),at_arm));
}
inline Vec response(const Body& body,Vec arm,Vec impulse) noexcept {
    return response(body,arm,arm,impulse);
}
}
inline bool valid(const Body& body) noexcept {
    if(!detail::positive(body.mass)||!detail::finite(body.center_of_mass,1e6)||
       !detail::finite(body.linear_velocity,1e5)||!detail::finite(body.angular_velocity,1e5))return false;
    for(double moment:body.inertia_body)if(!detail::positive(moment))return false;
    double square=0;
    for(double q:body.orientation){if(!std::isfinite(q))return false;square+=q*q;}
    return std::isfinite(square)&&std::abs(square-1)<=.001;
}
inline bool valid(const Wheel& wheel) noexcept {
    return detail::positive(wheel.inertia)&&std::isfinite(wheel.radius)&&wheel.radius>=1e-6&&wheel.radius<=10&&
        std::isfinite(wheel.angular_speed)&&std::abs(wheel.angular_speed)<=1e5;
}
inline Vec point_velocity(const Body& body,Vec point) noexcept {
    return detail::add(body.linear_velocity,detail::cross(body.angular_velocity,detail::sub(point,body.center_of_mass)));
}
inline bool contact_basis(Vec normal,Vec forward,Basis& out) noexcept {
    Basis candidate;
    if(!detail::unit(normal,candidate.normal)||!detail::finite(forward))return false;
    forward=detail::sub(forward,detail::mul(candidate.normal,detail::dot(forward,candidate.normal)));
    if(!detail::unit(forward,candidate.forward)||
       !detail::unit(detail::cross(candidate.normal,candidate.forward),candidate.lateral))return false;
    candidate.forward=detail::cross(candidate.lateral,candidate.normal);
    out=candidate;return true;
}
// Point impulse uses world COM, orientation and principal body inertia. It
// changes only velocities; invalid inputs/results leave the original intact.
inline bool apply_impulse(Body& body,Vec point,Vec impulse) noexcept {
    if(!valid(body)||!detail::finite(point,1e6)||!detail::finite(impulse))return false;
    auto candidate=body;
    candidate.linear_velocity=detail::add(body.linear_velocity,detail::mul(impulse,1/body.mass));
    candidate.angular_velocity=detail::add(body.angular_velocity,
        detail::inverse_inertia(body,detail::cross(detail::sub(point,body.center_of_mass),impulse)));
    if(!valid(candidate))return false;
    body=candidate;return true;
}
namespace detail {
inline bool axle_couple(Body& body,Wheel& wheel,Vec axle,double impulse) noexcept {
    auto next_body=body;auto next_wheel=wheel;
    next_body.angular_velocity=sub(body.angular_velocity,inverse_inertia(body,mul(axle,impulse)));
    next_wheel.angular_speed+=impulse/wheel.inertia;
    if(!valid(next_body)||!valid(next_wheel))return false;
    body=next_body;wheel=next_wheel;return true;
}
}
// One velocity-constraint solve for one wheel. Apply gravity/external forces
// before calling so supporting contact produces a real normal impulse. A whole
// bike needs a separate contact iteration/warm-start/integration policy: this
// helper neither averages wheel heights nor fabricates mass*g support. Rotor
// axle is fixed for this impulse solve; gyroscopic axis evolution is not supplied.
inline Result solve(double seconds,Body& body,Wheel& wheel,const Contact& contact,
                    Controls controls={},Parameters parameters={}) noexcept {
    Result result;result.status=contact.status;
    if(!valid(body)||!valid(wheel)||!std::isfinite(seconds)||seconds<0||seconds>.10||
       !std::isfinite(controls.drive_torque)||std::abs(controls.drive_torque)>1e6||
       !std::isfinite(controls.brake_torque)||controls.brake_torque<0||controls.brake_torque>1e6||
       !std::isfinite(parameters.longitudinal_friction)||parameters.longitudinal_friction<0||parameters.longitudinal_friction>10||
       !std::isfinite(parameters.lateral_friction)||parameters.lateral_friction<0||parameters.lateral_friction>10)return result;
    if(contact.status==ContactStatus::unavailable)return result;
    if(contact.status!=ContactStatus::clear&&contact.status!=ContactStatus::hit)return result;
    if(!contact_basis(contact.normal,contact.forward,result.basis))return result;
    if(contact.status==ContactStatus::hit&&
       (!detail::finite(contact.point,1e6)||!detail::finite(contact.surface_velocity,1e5)))return result;
    result.valid=true;result.touching=contact.status==ContactStatus::hit;
    if(seconds==0)return result; // paused simulation cannot apply an impulse
    auto next_body=body;auto next_wheel=wheel;
    const auto& basis=result.basis;
    // Motor/brake act between rotor and chassis, never as unbalanced external
    // torques. Braking damps relative axle speed; a rigidly co-rotating wheel
    // and chassis have no internal brake motion to remove.
    if(!detail::axle_couple(next_body,next_wheel,basis.lateral,controls.drive_torque*seconds)) {
        result.valid=false;return result;
    }
    const double relative_spin=next_wheel.angular_speed-detail::dot(next_body.angular_velocity,basis.lateral);
    const double axle_mass=1/wheel.inertia+detail::dot(basis.lateral,detail::inverse_inertia(next_body,basis.lateral));
    const double brake_impulse=-std::copysign(std::min(std::abs(relative_spin)/axle_mass,controls.brake_torque*seconds),relative_spin);
    result.brake_energy=std::max(0.0,-(brake_impulse*relative_spin+.5*brake_impulse*brake_impulse*axle_mass));
    if(!detail::axle_couple(next_body,next_wheel,basis.lateral,brake_impulse)) {
        result.valid=false;return result;
    }
    if(contact.status==ContactStatus::clear){body=next_body;wheel=next_wheel;return result;}
    const auto arm=detail::sub(contact.point,body.center_of_mass);
    const auto relative=detail::sub(point_velocity(next_body,contact.point),contact.surface_velocity);
    const double normal_mass=detail::dot(basis.normal,detail::response(body,arm,basis.normal));
    if(!std::isfinite(normal_mass)||normal_mass<=0){result.valid=false;return result;}
    // No restitution or positional bias: closing contact dissipates normal
    // kinetic energy. Separating contact never attracts the bike to the ground.
    result.normal_impulse=std::max(0.0,-detail::dot(relative,basis.normal)/normal_mass);
    result.normal_load=result.normal_impulse/seconds;
    if(!std::isfinite(result.normal_impulse)||!std::isfinite(result.normal_load)){result.valid=false;return result;}
    if(!apply_impulse(next_body,contact.point,detail::mul(basis.normal,result.normal_impulse))) {
        result.valid=false;return result;
    }
    const double longitudinal_limit=parameters.longitudinal_friction*result.normal_impulse;
    const double lateral_limit=parameters.lateral_friction*result.normal_impulse;
    if(longitudinal_limit>0||lateral_limit>0) {
        const auto velocity=detail::sub(point_velocity(next_body,contact.point),contact.surface_velocity);
        const auto hub=detail::add(contact.point,detail::mul(basis.normal,wheel.radius));
        const auto hub_arm=detail::sub(hub,body.center_of_mass);
        const auto hub_velocity=detail::sub(point_velocity(next_body,hub),contact.surface_velocity);
        const double slip=detail::dot(hub_velocity,basis.forward)-next_wheel.angular_speed*wheel.radius;
        const double lateral=detail::dot(velocity,basis.lateral);
        const double ff=detail::dot(basis.forward,detail::response(body,hub_arm,basis.forward))+wheel.radius*wheel.radius/wheel.inertia;
        const double ll=detail::dot(basis.lateral,detail::response(body,arm,basis.lateral));
        const double fl=detail::dot(basis.forward,detail::response(body,arm,hub_arm,basis.lateral));
        if(!std::isfinite(ff)||!std::isfinite(ll)||!std::isfinite(fl)||ff<=0||ll<=0){result.valid=false;return result;}
        double forward_impulse{},lateral_impulse{};
        if(longitudinal_limit>0&&lateral_limit>0) {
            const double determinant=ff*ll-fl*fl;
            if(!std::isfinite(determinant)||determinant<=1e-12*ff*ll){result.valid=false;return result;}
            forward_impulse=(-slip*ll+lateral*fl)/determinant;
            lateral_impulse=(-lateral*ff+slip*fl)/determinant;
            const double ellipse=std::hypot(forward_impulse/longitudinal_limit,lateral_impulse/lateral_limit);
            if(ellipse>1){forward_impulse/=ellipse;lateral_impulse/=ellipse;}
        }else if(longitudinal_limit>0)forward_impulse=std::clamp(-slip/ff,-longitudinal_limit,longitudinal_limit);
        else lateral_impulse=std::clamp(-lateral/ll,-lateral_limit,lateral_limit);
        if(!std::isfinite(forward_impulse)||!std::isfinite(lateral_impulse)){result.valid=false;return result;}
        // Forward traction is transmitted to the chassis at the hub; its
        // remaining contact moment changes rotor spin. Lateral/normal contact
        // moments go to the chassis. Applying both at the contact and spinning
        // the rotor would double-count longitudinal angular momentum.
        if(!apply_impulse(next_body,hub,detail::mul(basis.forward,forward_impulse))||
           !apply_impulse(next_body,contact.point,detail::mul(basis.lateral,lateral_impulse))){result.valid=false;return result;}
        next_wheel.angular_speed-=forward_impulse*wheel.radius/wheel.inertia;
        if(!valid(next_wheel)){result.valid=false;return result;}
        result.longitudinal_impulse=forward_impulse;result.lateral_impulse=lateral_impulse;
    }
    body=next_body;wheel=next_wheel;
    return result;
}
} // namespace dingosdk::bmx::tire
