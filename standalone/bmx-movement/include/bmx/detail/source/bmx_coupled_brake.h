#pragma once
#include "bmx_tire_dynamics.h"
// Fixed-orientation wheel brake/contact block; no pose integration.
namespace bmx::detail::source::chassis_control::coupled_brake {
using Vec=tire::Vec;
using Matrix=std::array<std::array<double,3>,3>;
namespace detail {
using namespace tire::detail;
inline bool disk(const std::array<std::array<double,2>,2>& a,
                 const std::array<double,2>& b,std::array<double,2>& x) noexcept {
    const auto at=[&](double multiplier,std::array<double,2>& value) {
        const double aa=a[0][0]+multiplier,dd=a[1][1]+multiplier;
        const double determinant=aa*dd-a[0][1]*a[0][1];
        if(!std::isfinite(determinant)||aa<=0||dd<=0||determinant<=0)return false;
        value={(-dd*b[0]+a[0][1]*b[1])/determinant,
               (a[0][1]*b[0]-aa*b[1])/determinant};
        return std::isfinite(value[0])&&std::isfinite(value[1]);
    };
    if(!at(0,x))return false;
    if(std::hypot(x[0],x[1])<=1)return true;
    // SPD A: ||(A+alpha I)^-1 b|| <= ||b||/alpha. Thus this upper
    // endpoint is feasible; no unbounded convergence loop is needed.
    double low=0,high=std::max(1.0,std::hypot(b[0],b[1]));
    std::array<double,2> feasible{};
    if(!at(high,feasible)||std::hypot(feasible[0],feasible[1])>1+1e-12)return false;
    for(unsigned iteration=0;iteration<64;++iteration) {
        const double middle=(low+high)*.5;std::array<double,2> trial{};
        if(!at(middle,trial))return false;
        if(std::hypot(trial[0],trial[1])>1)low=middle;
        else {high=middle;feasible=trial;}
    }
    x=feasible;return true;
}

// Minimize .5*x'K*x+g'x over the friction ellipse and the brake interval.
// Scale friction impulses to a unit disk and brake to [-1,1]. Enumerating
// brake free/lower/upper leaves one exact 2D disk projection per candidate.
inline bool constrained(const Matrix& k,const std::array<double,3>& g,
                        const std::array<double,3>& limits,
                        std::array<double,3>& impulse) noexcept {
    if(!std::isfinite(limits[2])||limits[2]<0)return false;
    Matrix a{};std::array<double,3> b{};
    for(unsigned i=0;i<3;++i) {
        if(!std::isfinite(limits[i])||limits[i]<0||!std::isfinite(g[i]))return false;
        b[i]=g[i]*limits[i];
        for(unsigned j=0;j<3;++j) {
            if(!std::isfinite(k[i][j]))return false;
            a[i][j]=k[i][j]*limits[i]*limits[j];
        }
    }
    if(limits[2]==0) {a[2][2]=1;b[2]=0;}
    if(!(a[2][2]>0)||!std::isfinite(a[2][2]))return false;
    // Zero is feasible and has zero work. Seed it explicitly so roundoff in
    // a nearly singular candidate can never select an energy-adding response.
    double best=0;std::array<double,3> winner{};
    for(int brake_mode=-1;brake_mode<=1;++brake_mode) {
        const bool free=brake_mode==0;
        std::array<std::array<double,2>,2> reduced{};std::array<double,2> rhs{};
        for(unsigned i=0;i<2;++i) {
            rhs[i]=b[i]+(free?-a[i][2]*b[2]/a[2][2]:a[i][2]*brake_mode);
            for(unsigned j=0;j<2;++j)
                reduced[i][j]=a[i][j]-(free?a[i][2]*a[2][j]/a[2][2]:0);
        }
        // A zero friction coefficient fixes that coordinate to zero. Give the
        // unused coordinate an independent positive diagonal for disk().
        for(unsigned i=0;i<2;++i)if(limits[i]==0) {
            rhs[i]=0;reduced[i][i]=1;reduced[i][1-i]=reduced[1-i][i]=0;
        }
        std::array<double,2> friction{};
        if(!disk(reduced,rhs,friction))return false;
        std::array<double,3> candidate{friction[0],friction[1],static_cast<double>(brake_mode)};
        if(free)candidate[2]=-(b[2]+a[2][0]*candidate[0]+a[2][1]*candidate[1])/a[2][2];
        if(!std::isfinite(candidate[2])||std::abs(candidate[2])>1+1e-12)continue;
        candidate[2]=std::clamp(candidate[2],-1.0,1.0);
        double objective=0;
        for(unsigned i=0;i<3;++i) {
            objective+=b[i]*candidate[i];
            for(unsigned j=0;j<3;++j)objective+=.5*candidate[i]*a[i][j]*candidate[j];
        }
        if(!std::isfinite(objective))return false;
        if(objective<best) {best=objective;winner=candidate;}
    }
    for(unsigned i=0;i<3;++i)impulse[i]=winner[i]*limits[i];
    return true;
}
} // namespace detail

struct Result {tire::Result contact;double brake_impulse{};};
inline Result solve_tangent(double h,tire::Body& body,tire::Wheel& wheel,const tire::Contact& contact,
                            double normal_impulse,double brake_torque,tire::Parameters parameters) noexcept {
    Result out;auto& result=out.contact;result.status=contact.status;
    if(!tire::valid(body)||!tire::valid(wheel)||!std::isfinite(h)||h<=0||h>.1||
       !std::isfinite(brake_torque)||brake_torque<0||brake_torque>1e6||
       !std::isfinite(normal_impulse)||normal_impulse<0||
       !std::isfinite(parameters.longitudinal_friction)||parameters.longitudinal_friction<0||parameters.longitudinal_friction>10||
       !std::isfinite(parameters.lateral_friction)||parameters.lateral_friction<0||parameters.lateral_friction>10||
       (contact.status!=tire::ContactStatus::clear&&contact.status!=tire::ContactStatus::hit)||
       !tire::contact_basis(contact.normal,contact.forward,result.basis))return out;
    if(contact.status==tire::ContactStatus::hit&&
       (!detail::finite(contact.point,1e6)||!detail::finite(contact.surface_velocity,1e5)))return out;
    if(contact.status==tire::ContactStatus::clear) {
        // Clear queries carry only the wheel plane. Keep the established tire
        // API: ignored point/platform fields need not contain usable geometry.
        const double before=wheel.angular_speed;
        result=tire::solve(h,body,wheel,contact,{0,brake_torque},parameters);
        if(result.valid)out.brake_impulse=(wheel.angular_speed-before)*wheel.inertia;
        return out;
    }
    auto next=body;auto next_wheel=wheel;const auto& basis=result.basis;
    const auto arm=detail::sub(contact.point,body.center_of_mass);
    result.normal_impulse=normal_impulse;result.normal_load=normal_impulse/h;
    if(!std::isfinite(result.normal_load))return out;
    const auto hub=detail::add(contact.point,detail::mul(basis.normal,wheel.radius));
    const auto hub_arm=detail::sub(hub,body.center_of_mass);
    const std::array<Vec,3> linear{basis.forward,basis.lateral,Vec{}};
    const std::array<Vec,3> angular{detail::cross(hub_arm,basis.forward),
        detail::cross(arm,basis.lateral),detail::mul(basis.lateral,-1)};
    const std::array<double,3> rotor{-wheel.radius,0,1};
    const auto hub_relative=detail::sub(tire::point_velocity(next,hub),contact.surface_velocity);
    const auto contact_relative=detail::sub(tire::point_velocity(next,contact.point),contact.surface_velocity);
    const std::array<double,3> speed{detail::dot(hub_relative,basis.forward)-next_wheel.angular_speed*wheel.radius,
        detail::dot(contact_relative,basis.lateral),next_wheel.angular_speed-detail::dot(next.angular_velocity,basis.lateral)};
    Matrix inverse_mass{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
        inverse_mass[i][j]=detail::dot(linear[i],linear[j])/body.mass+
            detail::dot(angular[i],detail::inverse_inertia(body,angular[j]))+rotor[i]*rotor[j]/wheel.inertia;
    const std::array<double,3> limits{parameters.longitudinal_friction*result.normal_impulse,
        parameters.lateral_friction*result.normal_impulse,brake_torque*h};
    std::array<double,3> impulse{};
    if(!detail::constrained(inverse_mass,speed,limits,impulse))return out;
    Vec linear_impulse{},angular_impulse{};
    double rotor_impulse=0;
    for(unsigned i=0;i<3;++i) {
        linear_impulse=detail::add(linear_impulse,detail::mul(linear[i],impulse[i]));
        angular_impulse=detail::add(angular_impulse,detail::mul(angular[i],impulse[i]));
        rotor_impulse+=rotor[i]*impulse[i];
    }
    next.linear_velocity=detail::add(next.linear_velocity,detail::mul(linear_impulse,1/body.mass));
    next.angular_velocity=detail::add(next.angular_velocity,detail::inverse_inertia(body,angular_impulse));
    next_wheel.angular_speed+=rotor_impulse/wheel.inertia;
    if(!tire::valid(next)||!tire::valid(next_wheel))return out;
    result.longitudinal_impulse=impulse[0];result.lateral_impulse=impulse[1];out.brake_impulse=impulse[2];
    // The joint block's total work is well-defined; its friction/brake energy
    // split depends on the interpolation path. Do not reuse the isolated-axle
    // tire solver's brake_energy formula as an attribution for this block.
    result.touching=contact.status==tire::ContactStatus::hit;result.valid=true;
    body=next;wheel=next_wheel;return out;
}

// Standalone convenience entry, preserving tire::solve's transactional/clear
// contract. The bike controller re-solves accumulated normal and tangent rows
// separately instead, so each wheel's own brake reaction can alter its load.
inline Result solve(double h,tire::Body& body,tire::Wheel& wheel,const tire::Contact& contact,
                    double brake_torque,tire::Parameters parameters) noexcept {
    if(contact.status==tire::ContactStatus::clear)
        return solve_tangent(h,body,wheel,contact,0,brake_torque,parameters);
    Result rejected;
    if(contact.status!=tire::ContactStatus::hit||!tire::valid(body)||!tire::valid(wheel)||
       !std::isfinite(h)||h<=0||h>.1||!detail::finite(contact.point,1e6)||
       !detail::finite(contact.surface_velocity,1e5))return rejected;
    tire::Basis basis;
    if(!tire::contact_basis(contact.normal,contact.forward,basis))return rejected;
    const auto arm=detail::sub(contact.point,body.center_of_mass);
    const auto relative=detail::sub(tire::point_velocity(body,contact.point),contact.surface_velocity);
    const double inverse_mass=detail::dot(basis.normal,detail::response(body,arm,basis.normal));
    if(!std::isfinite(inverse_mass)||inverse_mass<=0)return rejected;
    const double normal=std::max(0.0,-detail::dot(relative,basis.normal)/inverse_mass);
    auto candidate=body;auto candidate_wheel=wheel;
    if(!std::isfinite(normal)||!tire::apply_impulse(candidate,contact.point,detail::mul(basis.normal,normal)))return rejected;
    const auto solved=solve_tangent(h,candidate,candidate_wheel,contact,normal,brake_torque,parameters);
    if(solved.contact.valid){body=candidate;wheel=candidate_wheel;}
    return solved;
}
} // namespace bmx::detail::source::chassis_control::coupled_brake
