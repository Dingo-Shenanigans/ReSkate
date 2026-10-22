#pragma once
#include "Engine/Game/BMX/bmx_tire_dynamics.h"
#include "Engine/Game/BMX/bmx_contact_patch.h"
#include <array>
#include <cmath>
#include <cstdint>

// Diagnostic geometry ONLY. Prediction is the controller's exact pre-contact
// state, already including its one gravity/base-actuation step. No forces,
// native setters, support or friction admission are supplied by this module.
namespace dingosdk::bmx::approach_surface {
using Vec=std::array<float,3>;
inline constexpr double native_seconds=.01666666567325592;
inline constexpr double force_skin=.03,query_limit=.25,maximum_rotation_remainder=.002;
inline constexpr float patch_spacing=.03f,half_span=.03f,plane_tolerance=.0025f;
inline constexpr double minimum_normal_dot=.9998476951563913; // cos1deg, authored strict same-plane test
struct Surface {
    bool available{},plane{};Vec point{},normal{},forward{};double separation{};
    std::uint64_t entity{};std::array<std::uint64_t,2> body{};
};
struct RayHit {
    bool available{},hit{};Vec point{};float fraction{};
    std::uint64_t entity{};std::array<std::uint64_t,2> body{};
    unsigned native_casts{};std::uint64_t elapsed_microseconds{};bool budget_exhausted{};
};
struct Ray {Vec from{},to{};RayHit hit;};
enum class Issue {disabled,invalid,unqualified_step,no_current_plane,no_body_identity,existing_skin,outside_query,
    hop_reserved,rotation_bound,not_closing,not_crossing,epoch_changed,query_unavailable,clear_footprint,
    malformed_hit,body_changed,plane_disagreement,patch_rejected,normal_disagreement,qualified_geometry};
inline const char* name(Issue issue) noexcept {
    switch(issue){
    case Issue::disabled:return "disabled";case Issue::invalid:return "invalid";
    case Issue::unqualified_step:return "unqualified_step";case Issue::no_current_plane:return "no_current_plane";
    case Issue::no_body_identity:return "no_body_identity";case Issue::existing_skin:return "existing_skin";
    case Issue::outside_query:return "outside_query";case Issue::hop_reserved:return "hop_reserved";
    case Issue::rotation_bound:return "rotation_bound";case Issue::not_closing:return "not_closing";
    case Issue::not_crossing:return "not_crossing";case Issue::epoch_changed:return "epoch_changed";
    case Issue::query_unavailable:return "query_unavailable";case Issue::clear_footprint:return "clear_footprint";
    case Issue::malformed_hit:return "malformed_hit";case Issue::body_changed:return "body_changed";
    case Issue::plane_disagreement:return "plane_disagreement";case Issue::patch_rejected:return "patch_rejected";
    case Issue::normal_disagreement:return "normal_disagreement";case Issue::qualified_geometry:return "qualified_geometry";
    }return "invalid";
}
struct Result {
    bool selected{},considered{},qualified{},epoch_before{},epoch_after{};
    Issue issue=Issue::disabled;unsigned rays{},native_casts{};std::uint64_t query_microseconds{};
    double normal_velocity{},linear_end_gap{},rotation_remainder{},time_to_plane{},target_normal_velocity{};
    Vec projected_center{},fitted_point{},fitted_normal{};
    double maximum_plane_error{},normal_dot{};float fit_residual{};
    std::array<Ray,5> observations{};
};
namespace detail {
inline tire::Vec wide(Vec v) noexcept {return {v[0],v[1],v[2]};}
inline Vec narrow(tire::Vec v) noexcept {return {float(v[0]),float(v[1]),float(v[2])};}
inline tire::Vec sub(tire::Vec a,tire::Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline tire::Vec add(tire::Vec a,tire::Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline tire::Vec mul(tire::Vec a,double b) noexcept {for(auto& x:a)x*=b;return a;}
inline double dot(tire::Vec a,tire::Vec b) noexcept {double r=0;for(unsigned i=0;i<3;++i)r+=a[i]*b[i];return r;}
inline double length(tire::Vec a) noexcept {return std::sqrt(dot(a,a));}
inline bool finite(tire::Vec a,double cap=1e6) noexcept {for(double x:a)if(!std::isfinite(x)||std::abs(x)>cap)return false;return true;}
}
template<class Raycast,class Epoch>
Result observe(bool selected,bool fresh_step,bool hop_reserved,double seconds,const tire::Body& prediction,
               const Surface& current,Raycast&& cast,Epoch&& epoch) noexcept {
    Result out;out.selected=selected;if(!selected)return out;
    try {
        tire::Basis basis;
        if(!std::isfinite(seconds)||std::abs(seconds-native_seconds)>1e-8||!tire::valid(prediction)||
           !detail::finite(prediction.linear_velocity,50)||!detail::finite(prediction.angular_velocity,40)) {out.issue=Issue::invalid;return out;}
        if(!fresh_step){out.issue=Issue::unqualified_step;return out;}
        if(!current.available||!current.plane){out.issue=Issue::no_current_plane;return out;}
        if(!std::isfinite(current.separation)||!detail::finite(detail::wide(current.point))||
           !tire::contact_basis(detail::wide(current.normal),detail::wide(current.forward),basis)) {out.issue=Issue::invalid;return out;}
        if(current.body[0]==0||current.body[1]==0){out.issue=Issue::no_body_identity;return out;}
        if(current.separation<=force_skin){out.issue=Issue::existing_skin;return out;}
        if(current.separation>query_limit){out.issue=Issue::outside_query;return out;}
        if(hop_reserved){out.issue=Issue::hop_reserved;return out;}
        const auto point=detail::wide(current.point),arm=detail::sub(point,prediction.center_of_mass);
        const auto angle=detail::length(prediction.angular_velocity)*seconds;
        out.rotation_remainder=.5*detail::length(arm)*angle*angle;
        if(out.rotation_remainder>maximum_rotation_remainder){out.issue=Issue::rotation_bound;return out;}
        const auto velocity=tire::point_velocity(prediction,point); // existing backend assumes static world surfaces
        out.normal_velocity=detail::dot(velocity,basis.normal);
        out.linear_end_gap=current.separation+seconds*out.normal_velocity;
        out.target_normal_velocity=-current.separation/seconds;
        if(out.normal_velocity>=0){out.issue=Issue::not_closing;return out;}
        if(out.linear_end_gap+out.rotation_remainder>=0){out.issue=Issue::not_crossing;return out;}
        out.considered=true;out.time_to_plane=current.separation/-out.normal_velocity;
        const auto travel=detail::mul(velocity,out.time_to_plane);
        // Projection selects ray endpoints only. Every accepted point below
        // must be returned by the CURRENT native ray, never this projection.
        const auto center=detail::add(point,detail::sub(travel,detail::mul(basis.normal,detail::dot(travel,basis.normal))));
        out.projected_center=detail::narrow(center);
        if(!(out.epoch_before=epoch())){out.issue=Issue::epoch_changed;return out;}
        const std::array<tire::Vec,5> offsets{{{},detail::mul(basis.forward,patch_spacing),detail::mul(basis.forward,-patch_spacing),
            detail::mul(basis.lateral,patch_spacing),detail::mul(basis.lateral,-patch_spacing)}};
        std::array<contact_patch::Sample,5> patch{};
        for(unsigned i=0;i<offsets.size();++i){
            if(!epoch()){out.issue=Issue::epoch_changed;return out;}
            auto& ray=out.observations[i];const auto q=detail::add(center,offsets[i]);
            ray.from=detail::narrow(detail::add(q,detail::mul(basis.normal,half_span)));
            ray.to=detail::narrow(detail::sub(q,detail::mul(basis.normal,half_span)));
            ++out.rays;ray.hit=cast(ray.from,ray.to);const auto& hit=ray.hit;
            out.native_casts+=hit.native_casts;out.query_microseconds+=hit.elapsed_microseconds;
            if(!hit.available){out.issue=Issue::query_unavailable;return out;}
            if(!hit.hit){out.issue=Issue::clear_footprint;return out;}
            if(!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1||!detail::finite(detail::wide(hit.point))) {out.issue=Issue::malformed_hit;return out;}
            const auto on_ray=detail::add(detail::wide(ray.from),detail::mul(detail::sub(detail::wide(ray.to),detail::wide(ray.from)),hit.fraction));
            if(detail::length(detail::sub(detail::wide(hit.point),on_ray))>plane_tolerance){out.issue=Issue::malformed_hit;return out;}
            if(hit.body!=current.body||hit.entity!=current.entity){out.issue=Issue::body_changed;return out;}
            const auto error=std::abs(detail::dot(detail::sub(detail::wide(hit.point),point),basis.normal));
            out.maximum_plane_error=std::max(out.maximum_plane_error,error);
            if(error>plane_tolerance){out.issue=Issue::plane_disagreement;return out;}
            patch[i]={true,hit.point};
        }
        contact_patch::Tuning tuning;tuning.maximum_residual=plane_tolerance;
        const auto fitted=contact_patch::fit(patch,detail::narrow(basis.normal),detail::narrow(basis.forward),detail::narrow(basis.lateral),tuning);
        out.fit_residual=fitted.residual;
        if(!fitted.available||fitted.used!=5){out.issue=Issue::patch_rejected;return out;}
        out.fitted_point=fitted.point;out.fitted_normal=fitted.normal;
        out.normal_dot=detail::dot(detail::wide(fitted.normal),basis.normal);
        if(out.normal_dot<minimum_normal_dot){out.issue=Issue::normal_disagreement;return out;}
        if(!(out.epoch_after=epoch())){out.issue=Issue::epoch_changed;return out;}
        out.qualified=true;out.issue=Issue::qualified_geometry;return out;
    }catch(...){out.qualified=false;out.issue=Issue::query_unavailable;return out;}
}
} // namespace dingosdk::bmx::approach_surface
