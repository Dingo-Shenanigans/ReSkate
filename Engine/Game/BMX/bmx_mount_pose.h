#pragma once
#include "Engine/Game/BMX/bmx_geometry.h"
#include "Engine/Game/BMX/chassis_transform.h"
#include <algorithm>
#include <array>
#include <cstdint>

// No native calls or body writes. Caller must obtain every Sample
// from a fresh actual plane fit plus native witness in the specified world.
namespace dingosdk::bmx::mount_pose {
using Vec=chassis::Vector;
using Quat=chassis::Quaternion;
inline constexpr float maximum_slope=.18f,maximum_plane_residual=.0025f;
struct Request {
    Vec position{}; // requested root X/Z; Y is the freshly sampled floor hint
    float yaw{};
    std::uintptr_t world{},context{};
    std::uint64_t serial{};
    double simulation_time{};
};
struct Sample {
    bool available{},hit{},verified_plane{};
    Vec point{},normal{};
    std::uintptr_t world{},context{};
    std::uint64_t serial{};
    double simulation_time{};
};
enum class Issue { none,invalid_request,unavailable,unsupported,stale_world_or_clock,
    invalid_normal,too_steep,discontinuous,needs_final_witness,final_support_mismatch };
struct Seed {
    bool prepared{},ready{};
    Issue issue{Issue::invalid_request};
    Request request;
    Vec root{},com{},up{};
    Quat orientation{0,0,0,1};
    float pitch{},roll{};
    std::array<double,2> cross_plane_residual{};
    double cross_plane_allowance{};
};
namespace detail {
inline Vec add(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec a,double b) {for(auto& v:a)v=static_cast<float>(v*b);return a;}
inline double dot(Vec a,Vec b) {double v=0;for(unsigned i=0;i<3;++i)v+=double(a[i])*b[i];return v;}
inline double length(Vec a) {return std::sqrt(dot(a,a));}
inline Vec cross(Vec a,Vec b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline bool bounded(Vec a) {if(!chassis::finite(a))return false;for(float v:a)if(std::abs(v)>999990)return false;return true;}
inline bool unit(Vec a,Vec& out) {
    const auto n=length(a);if(!std::isfinite(n)||n<1e-8)return false;out=mul(a,1/n);return true;
}
inline Issue samples(const Request& r,const std::array<Sample,2>& s) {
    for(const auto& v:s) {
        if(!v.available)return Issue::unavailable;
        if(!v.hit||!v.verified_plane)return Issue::unsupported;
        if(v.world!=r.world||v.context!=r.context||v.serial!=r.serial||v.simulation_time!=r.simulation_time)
            return Issue::stale_world_or_clock;
        if(!bounded(v.point)||!bounded(v.normal)||std::abs(dot(v.normal,v.normal)-1)>.001)
            return Issue::invalid_normal;
        if(v.normal[1]<std::cos(maximum_slope)-1e-6)return Issue::too_steep;
    }
    return Issue::none;
}
inline Quat rotation(Vec x,Vec y,Vec z) {
    Quat q{};const float trace=x[0]+y[1]+z[2];
    if(trace>0) {const float s=2*std::sqrt(1+trace);q={(y[2]-z[1])/s,(z[0]-x[2])/s,(x[1]-y[0])/s,s*.25f};}
    else if(x[0]>y[1]&&x[0]>z[2]) {const float s=2*std::sqrt(1+x[0]-y[1]-z[2]);q={s*.25f,(x[1]+y[0])/s,(x[2]+z[0])/s,(y[2]-z[1])/s};}
    else if(y[1]>z[2]) {const float s=2*std::sqrt(1+y[1]-x[0]-z[2]);q={(x[1]+y[0])/s,s*.25f,(y[2]+z[1])/s,(z[0]-x[2])/s};}
    else {const float s=2*std::sqrt(1+z[2]-x[0]-y[1]);q={(x[2]+z[0])/s,(y[2]+z[1])/s,s*.25f,(x[1]-y[0])/s};}
    return q;
}
inline Quat heading_tilt(float yaw,double pitch,double roll) {
    // qY(yaw)*qX(pitch)*qZ(roll): +Z keeps the requested horizontal heading.
    const double sy=std::sin(yaw),cy=std::cos(yaw),sp=std::sin(pitch),cp=std::cos(pitch),sr=std::sin(roll),cr=std::cos(roll);
    const Vec left{static_cast<float>(cy*cr+sy*sp*sr),static_cast<float>(cp*sr),static_cast<float>(-sy*cr+cy*sp*sr)};
    const Vec up{static_cast<float>(-cy*sr+sy*sp*cr),static_cast<float>(cp*cr),static_cast<float>(sy*sr+cy*sp*cr)};
    const Vec forward{static_cast<float>(sy*cp),static_cast<float>(-sp),static_cast<float>(cy*cp)};
    return rotation(left,up,forward);
}
}
inline Seed prepare(const Request& request,std::array<Sample,2> support) noexcept {
    using namespace detail;Seed out;out.request=request;
    if(!bounded(request.position)||!std::isfinite(request.yaw)||!request.world||!request.context||!request.serial||
       !std::isfinite(request.simulation_time)||request.simulation_time<0)return out;
    out.issue=samples(request,support);if(out.issue!=Issue::none)return out;
    for(auto& sample:support)unit(sample.normal,sample.normal);
    out.issue=Issue::invalid_request;
    if(dot(support[0].normal,support[1].normal)<std::cos(.02)) {out.issue=Issue::discontinuous;return out;}
    const auto delta=sub(support[1].point,support[0].point);
    out.cross_plane_residual={dot(delta,support[0].normal),dot(delta,support[1].normal)};
    // The two local patches need not be one infinite plane. Bound their global
    // disagreement by measured normal variation over the actual support span.
    // Parallel steps retain the strict local 2.5mm bound. This is a rejection
    // bound, not proof that unqueried terrain between the wheels is smooth.
    out.cross_plane_allowance=maximum_plane_residual+length(delta)*length(sub(support[0].normal,support[1].normal));
    if(std::abs(out.cross_plane_residual[0])>out.cross_plane_allowance||
       std::abs(out.cross_plane_residual[1])>out.cross_plane_allowance) {out.issue=Issue::discontinuous;return out;}
    Vec averaged_up;
    if(!unit(add(support[0].normal,support[1].normal),averaged_up)) {out.issue=Issue::invalid_normal;return out;}
    // Cross-slope roll comes from fresh normals. Solve pitch/rootY against each
    // wheel's own observed plane instead of requiring a shared plane or adding
    // penetration tolerance. Never reuse a cached parked/render quaternion.
    const double local_x=std::cos(request.yaw)*averaged_up[0]-std::sin(request.yaw)*averaged_up[2];
    const double roll=-std::asin(std::clamp(local_x,-1.0,1.0));
    const double cosine_roll=std::cos(roll),cosine_limit=std::cos(double(maximum_slope));
    if(cosine_roll<cosine_limit-1e-7) {out.issue=Issue::too_steep;return out;}
    const double limit=std::acos(std::clamp(cosine_limit/cosine_roll,-1.0,1.0));
    for(unsigned i=0;i<2;++i) {
        const auto& sample=support[i];
        const Vec nominal{request.position[0]+(i==0?geometry::front_axle[2]:geometry::rear_axle[2])*std::sin(request.yaw),
            request.position[1],request.position[2]+(i==0?geometry::front_axle[2]:geometry::rear_axle[2])*std::cos(request.yaw)};
        if(length(sub(sample.point,nominal))>.35) {out.issue=Issue::discontinuous;return out;}
    }
    const auto heights=[&](double pitch,std::array<double,2>& values,Quat& q) {
        if(!chassis::unit_quaternion(heading_tilt(request.yaw,pitch,roll),q))return false;
        for(unsigned i=0;i<2;++i) {
            const auto hub=chassis::rotate_unit(q,i==0?geometry::front_axle:geometry::rear_axle);
            const auto& sample=support[i];
            // Subtract the local origin first to avoid large-world cancellation.
            values[i]=request.position[1]+(geometry::wheel_radius+
                dot(sample.normal,sub(sample.point,request.position))-dot(sample.normal,hub))/sample.normal[1];
            if(!std::isfinite(values[i]))return false;
        }
        return true;
    };
    double low=-limit,high=limit;std::array<double,2> low_y{},high_y{},solved_y{};Quat q;
    if(!heights(low,low_y,q)||!heights(high,high_y,q))return out;
    const double low_gap=low_y[0]-low_y[1],high_gap=high_y[0]-high_y[1];
    if(low_gap>1e-6||high_gap< -1e-6) {out.issue=Issue::too_steep;return out;}
    double pitch{};
    for(unsigned iteration=0;iteration<32;++iteration) {
        pitch=(low+high)*.5;
        if(!heights(pitch,solved_y,q))return out;
        if(solved_y[0]>solved_y[1])high=pitch;else low=pitch;
    }
    if(!heights(pitch,solved_y,out.orientation)||std::abs(solved_y[0]-solved_y[1])>.00001)return out;
    out.pitch=static_cast<float>(pitch);out.roll=static_cast<float>(roll);
    out.up=chassis::rotate_unit(out.orientation,{0,1,0});
    out.root=request.position;const double height=std::max(solved_y[0],solved_y[1]);
    if(!std::isfinite(height)||std::abs(height-request.position[1])>.35)return out;
    out.root[1]=static_cast<float>(height);
    out.com=add(out.root,chassis::rotate_unit(out.orientation,geometry::com_from_root));
    if(!bounded(out.root)||!bounded(out.com))return out;
    out.prepared=true;out.issue=Issue::needs_final_witness;return out;
}
// A candidate plane can end before the newly rotated wheel. Require entirely
// fresh actual witnesses at the candidate final hubs before it becomes a seed.
inline Seed verify_final(Seed candidate,std::array<Sample,2> support) noexcept {
    using namespace detail;candidate.ready=false;
    if(!candidate.prepared)return candidate;
    Quat checked;
    if(!bounded(candidate.root)||!bounded(candidate.com)||!chassis::unit_quaternion(candidate.orientation,checked)||
       !bounded(candidate.up)||std::abs(dot(candidate.up,candidate.up)-1)>.001||
       dot(candidate.up,chassis::rotate_unit(checked,{0,1,0}))<.9999||
       length(sub(candidate.com,add(candidate.root,chassis::rotate_unit(checked,geometry::com_from_root))))>.0005) {
        candidate.issue=Issue::invalid_request;return candidate;
    }
    candidate.issue=samples(candidate.request,support);if(candidate.issue!=Issue::none)return candidate;
    for(auto& sample:support)unit(sample.normal,sample.normal);
    for(unsigned i=0;i<2;++i) {
        const auto hub=add(candidate.root,chassis::rotate_unit(candidate.orientation,
            i==0?geometry::front_axle:geometry::rear_axle));
        const auto difference=sub(hub,support[i].point);const double distance=dot(difference,support[i].normal);
        const auto tangent=sub(difference,mul(support[i].normal,distance));
        if(dot(candidate.up,support[i].normal)<std::cos(.02)||distance-geometry::wheel_radius<-.0005||
           distance-geometry::wheel_radius>.005||length(tangent)>.025) {
            candidate.issue=Issue::final_support_mismatch;return candidate;
        }
    }
    candidate.ready=true;candidate.issue=Issue::none;return candidate;
}
inline bool matches(const Seed& seed,std::uintptr_t world,std::uintptr_t context,
                    std::uint64_t serial,double simulation_time) noexcept {
    using namespace detail;Quat checked;
    return seed.ready&&seed.prepared&&seed.issue==Issue::none&&world&&context&&serial&&
        seed.request.world==world&&seed.request.context==context&&seed.request.serial==serial&&
        seed.request.simulation_time==simulation_time&&std::isfinite(simulation_time)&&
        bounded(seed.root)&&bounded(seed.com)&&chassis::unit_quaternion(seed.orientation,checked)&&
        chassis::rotate_unit(checked,{0,1,0})[1]>=std::cos(maximum_slope)-1e-6&&
        length(sub(seed.com,add(seed.root,chassis::rotate_unit(checked,geometry::com_from_root))))<=.0005;
}
} // namespace dingosdk::bmx::mount_pose
