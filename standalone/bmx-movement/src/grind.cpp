#include <bmx/grind.hpp>
#include <algorithm>
#include <cmath>

namespace bmx {
GrindResult solve_grind(const Body& body,const GrindContact& contact,Vec3 gravity,double seconds) noexcept {
    namespace math=detail::source::tire::detail;
    GrindResult out;
    if(!std::isfinite(seconds)||seconds<minimum_fixed_step||seconds>maximum_fixed_step||
       !detail::source::tire::valid(body)||!contact.available||!math::finite(gravity,100)||
       !math::finite(contact.point,1e6)||!math::finite(contact.surface_velocity,1e5)||
       !std::isfinite(contact.separation)||!std::isfinite(contact.friction)||contact.friction<0||contact.friction>10)return out;
    Vec3 normal,tangent;
    if(!math::unit(contact.normal,normal)||!math::unit(contact.tangent,tangent)||
       std::abs(math::dot(normal,tangent))>1e-3)return out;
    out.valid=true;
    const auto relative=math::sub(detail::source::tire::point_velocity(body,contact.point),contact.surface_velocity);
    out.speed=math::dot(relative,tangent);
    if(!contact.touching||contact.separation>.005||contact.separation<-.03)return out;
    const auto arm=math::sub(contact.point,body.center_of_mass);
    auto predicted=body;
    predicted.linear_velocity=math::add(predicted.linear_velocity,math::mul(gravity,seconds));
    const auto at_point=math::sub(detail::source::tire::point_velocity(predicted,contact.point),contact.surface_velocity);
    const double normal_mass=math::dot(normal,math::response(predicted,arm,normal));
    if(!std::isfinite(normal_mass)||normal_mass<=1e-9){out.valid=false;return out;}
    out.normal_impulse=std::max(0.0,-math::dot(at_point,normal)/normal_mass);
    const auto normal_impulse=math::mul(normal,out.normal_impulse);
    if(!detail::source::tire::apply_impulse(predicted,contact.point,normal_impulse)){out.valid=false;return out;}
    const double tangential_mass=math::dot(tangent,math::response(predicted,arm,tangent));
    if(!std::isfinite(tangential_mass)||tangential_mass<=1e-9){out.valid=false;return out;}
    const auto slip=math::sub(detail::source::tire::point_velocity(predicted,contact.point),contact.surface_velocity);
    const double friction_limit=contact.friction*out.normal_impulse;
    out.friction_impulse=std::clamp(-math::dot(slip,tangent)/tangential_mass,-friction_limit,friction_limit);
    const auto impulse=math::add(normal_impulse,math::mul(tangent,out.friction_impulse));
    out.force=math::mul(impulse,1/seconds);out.torque=math::mul(math::cross(arm,impulse),1/seconds);
    if(!math::finite(out.force)||!math::finite(out.torque)||math::dot(out.force,out.force)>40000.0*40000||
       math::dot(out.torque,out.torque)>4000.0*4000) {out={};return out;}
    if(!detail::source::tire::apply_impulse(predicted,contact.point,math::mul(tangent,out.friction_impulse))) {out={};return out;}
    out.speed=math::dot(math::sub(detail::source::tire::point_velocity(predicted,contact.point),contact.surface_velocity),tangent);
    out.active=out.normal_impulse>0;
    return out;
}
}
