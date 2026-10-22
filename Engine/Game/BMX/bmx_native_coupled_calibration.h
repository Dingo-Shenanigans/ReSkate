#pragma once
#include "bmx_native_coupled_policy.h"
#include <algorithm>
#include <optional>

namespace dingosdk::bmx::native_coupled::calibration {
using policy::Vec;using policy::Epoch;
enum class Kind {pair_filter,world_contact,pair_positive,world_contact_only};
inline bool world_kind(Kind kind) noexcept {return kind==Kind::world_contact||kind==Kind::world_contact_only;}
enum class Phase {idle,running,complete,failed};
inline constexpr float maximum_fixture_slope=.02f;
inline bool fixture_supported(float pitch,float roll,Vec up) noexcept {
    return std::isfinite(pitch)&&std::isfinite(roll)&&std::abs(pitch)<=maximum_fixture_slope&&std::abs(roll)<=maximum_fixture_slope&&
        policy::finite(up,1)&&std::abs(policy::length(up)-1)<.0001&&up[1]>=std::cos(double(maximum_fixture_slope));
}
struct Sample {Epoch epoch;std::array<Vec,2> position{},linear{},angular{},inertia{};
    std::array<policy::Quat,2> principal_orientation{};std::array<float,2> clearance{},mass{},maximum_angular_speed{},allocated_maximum_angular_speed{};
    std::array<std::uint32_t,8> categories{},collides_with{};std::array<bool,2> integrates_angular_accumulator{};};
struct Report {
    Kind kind{};Phase phase{Phase::idle};const char* issue="not_started";
    std::array<Sample,48> samples{};unsigned count{};
    std::array<bool,2> falling{},contact_candidate{};
    std::array<unsigned,2> settled{};
    bool neutral_response_candidate{},parked{},pair_response{};
    // Rejected evidence is NOT an accepted sample and cannot advance contact,
    // settled, freefall or completion counters. One bounded diagnostic only.
    bool rejected_sample_available{};Sample rejected_sample;
    unsigned rejected_role=0xffffffffU;const char* rejected_cap="none";
    double rejected_value{},rejected_limit{};
};
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline double horizontal(Vec v) noexcept{return std::hypot(double(v[0]),double(v[2]));}
inline double dot(Vec a,Vec b) noexcept {double out{};for(unsigned i=0;i<3;++i)out+=double(a[i])*b[i];return out;}
inline Vec add_scaled(Vec a,Vec b,double t) noexcept {for(unsigned i=0;i<3;++i)a[i]=static_cast<float>(a[i]+b[i]*t);return a;}
inline constexpr float world_fixture_clearance=.08f;
template<class Capsules> inline std::optional<float> proxy_plane_clearance(const Capsules& capsules,unsigned role,Vec actual_com,
    policy::Quat actual_native_orientation,Vec plane_point,Vec plane_normal) noexcept {
    policy::Quat native,principal,relative;
    if(role>=2||capsules.size()!=8||!policy::finite(actual_com)||!policy::finite(plane_point)||!policy::finite(plane_normal,1)||
       std::abs(policy::length(plane_normal)-1)>.0001||!policy::unit(actual_native_orientation,native)||
       !policy::unit(policy::v2_roles[role].principal_to_model,principal)||!policy::unit(policy::rider_from_chassis,relative))return {};
    float lowest=std::numeric_limits<float>::max();
    for(unsigned index=role?5u:0u;index<(role?8u:5u);++index){const auto& shape=capsules[index];
        if(!std::isfinite(shape.radius)||shape.radius<=0||shape.radius>1||!policy::finite(shape.a)||!policy::finite(shape.b))return {};
        for(Vec endpoint:{shape.a,shape.b}){
            endpoint=sub(endpoint,policy::v2_roles[role].com_from_bike_root);
            if(role)endpoint=policy::rotate(policy::conjugate(relative),endpoint);
            const auto offset=policy::rotate(native,policy::native_local_point(endpoint,principal));
            lowest=std::min(lowest,static_cast<float>(dot(sub(add_scaled(actual_com,offset,1),plane_point),plane_normal)-shape.radius));
        }
    }
    if(!std::isfinite(lowest))return {};return lowest;
}
struct WorldPlacement {bool valid{};Vec root{},com{};policy::Quat native_orientation{0,0,0,1};float original_clearance{},translation{};};
// Calibration ONLY: normalize drop energy by moving each genuine owned role
// along its measured support-plane normal. Production role COM/shape frames,
// inertia, pair overlap fixtures and every monitor cap remain untouched.
template<class Capsules> inline WorldPlacement world_placement(const Capsules& capsules,unsigned role,Vec support_root,
    policy::Quat support_orientation,Vec support_normal) noexcept {
    WorldPlacement out;policy::Quat q,principal,relative;
    if(role>=2||!policy::finite(support_root)||!policy::unit(support_orientation,q)||
       !policy::unit(policy::v2_roles[role].principal_to_model,principal)||!policy::unit(policy::rider_from_chassis,relative))return out;
    const auto offset=policy::rotate(q,policy::v2_roles[role].com_from_bike_root);
    const auto initial=add_scaled(support_root,offset,1);out.native_orientation=policy::native_orientation(role?policy::multiply(q,relative):q,principal);
    const auto lowest=proxy_plane_clearance(capsules,role,initial,out.native_orientation,support_root,support_normal);
    if(!lowest)return out;out.original_clearance=*lowest;out.translation=world_fixture_clearance-*lowest;
    if(!std::isfinite(out.translation)||std::abs(out.translation)>1)return out;
    out.root=add_scaled(support_root,support_normal,out.translation);out.com=add_scaled(out.root,offset,1);
    const auto planned=proxy_plane_clearance(capsules,role,out.com,out.native_orientation,support_root,support_normal);
    out.valid=planned&&std::abs(*planned-world_fixture_clearance)<=.0001f;return out;
}
inline double segment_distance(Vec a,Vec b,Vec c,Vec d) noexcept {
    const auto u=sub(b,a),v=sub(d,c),w=sub(a,c);const double A=dot(u,u),B=dot(u,v),C=dot(v,v),D=dot(u,w),E=dot(v,w);
    double s{},t{};
    if(A<1e-15&&C<1e-15)return policy::length(w);
    if(A<1e-15)t=std::clamp(E/C,0.,1.);
    else if(C<1e-15)s=std::clamp(-D/A,0.,1.);
    else {const double denominator=A*C-B*B;s=denominator>1e-15?std::clamp((B*E-C*D)/denominator,0.,1.):0;
        t=(B*s+E)/C;if(t<0){t=0;s=std::clamp(-D/A,0.,1.);}else if(t>1){t=1;s=std::clamp((B-D)/A,0.,1.);}}
    return policy::length(sub(add_scaled(a,u,s),add_scaled(c,v,t)));
}
template<class Capsules> inline Vec overlap_shift(const Capsules& capsules) noexcept {
    const auto& bar=capsules[3];const auto& pelvis=capsules[5];
    return {bar.b[0]-pelvis.a[0]+bar.radius+pelvis.radius-.002f,bar.b[1]-pelvis.a[1],bar.b[2]-pelvis.a[2]};
}
template<class Capsules> inline bool isolated_overlap(const Capsules& capsules) noexcept {
    const auto shift=overlap_shift(capsules);unsigned overlaps{};
    for(unsigned a=0;a<5;++a)for(unsigned b=5;b<8;++b){
        const auto& x=capsules[a];const auto& y=capsules[b];
        const auto gap=segment_distance(x.a,x.b,add_scaled(y.a,shift,1),add_scaled(y.b,shift,1))-x.radius-y.radius;
        if(a==3&&b==5){if(gap>-.0019||gap<-.0021)return false;++overlaps;}
        else if(gap<.05)return false;
    }
    return overlaps==1;
}
inline bool same_pair(const Epoch& a,const Epoch& b) noexcept {
    return a.owner_world==b.owner_world&&a.bodies==b.bodies&&a.token==b.token;
}
// Finite evidence collector only. A completed run is a bounded observation of
// these exact poses, never an automatic native-collision or force-adoption gate.
class Monitor {
    Report report_{};
    bool reject_cap(const Sample& sample,unsigned role,const char* cap,double value,double limit,const char* why) noexcept {
        report_.rejected_sample_available=true;report_.rejected_sample=sample;report_.rejected_role=role;
        report_.rejected_cap=cap;report_.rejected_value=value;report_.rejected_limit=limit;fail(why);return false;
    }
public:
    const Report& report() const noexcept{return report_;}
    bool begin(Kind kind,const Sample& initial) noexcept {
        if(report_.phase!=Phase::idle||!policy::valid_epoch(initial.epoch))return false;
        for(unsigned i=0;i<2;++i)if(!policy::finite(initial.position[i])||!policy::finite(initial.linear[i])||
            !policy::finite(initial.angular[i])||!std::isfinite(initial.clearance[i])||initial.clearance[i]<.05f||
            policy::length(initial.linear[i])>.001||policy::length(initial.angular[i])>.001)return false;
        report_.kind=kind;report_.phase=Phase::running;report_.issue="neutral_native_observation_running";
        report_.samples[0]=initial;report_.count=1;return true;
    }
    void fail(const char* why) noexcept {report_.phase=Phase::failed;report_.issue=why;}
    void parked(bool value) noexcept{report_.parked=value;if(!value)fail("neutral_park_not_proved");}
    bool observe(const Sample& sample) noexcept {
        if(report_.phase!=Phase::running)return false;
        const auto& start=report_.samples[0];const auto& prior=report_.samples[report_.count-1];
        if(!policy::valid_epoch(sample.epoch)||!same_pair(start.epoch,sample.epoch)) {fail("neutral_owner_body_or_epoch_changed");return false;}
        if(sample.epoch.serial==prior.epoch.serial&&sample.epoch.time==prior.epoch.time)return true;
        if(sample.epoch.serial!=prior.epoch.serial+1||std::abs(sample.epoch.time-prior.epoch.time-policy::native_seconds)>1e-5){fail("neutral_requires_each_genuine_native60Hz_epoch");return false;}
        if(report_.count==report_.samples.size()){fail("neutral_sample_capacity");return false;}
        const auto elapsed=sample.epoch.time-start.epoch.time;
        const bool pair=!world_kind(report_.kind),positive=report_.kind==Kind::pair_positive;
        for(unsigned role=0;role<2;++role){
            if(!policy::finite(sample.position[role])||!policy::finite(sample.linear[role])||!policy::finite(sample.angular[role])||
               !std::isfinite(sample.clearance[role])){fail("neutral_nonfinite_actual_body");return false;}
            const auto displacement=sub(sample.position[role],start.position[role]);
            constexpr auto why="neutral_velocity_displacement_or_penetration_cap";
            const std::array<double,6> values{policy::length(sample.linear[role]),policy::length(sample.angular[role]),
                policy::length(displacement),horizontal(displacement),horizontal(sample.linear[role]),sample.clearance[role]};
            const std::array<double,6> limits{pair?2.:6.,positive?2.:pair?.05:8.,pair?.15:1.4,positive?.05:pair?.001:.12,positive?.5:pair?.01:1.,-.04f};
            constexpr std::array<const char*,6> caps{"linear_speed","angular_speed","total_displacement","horizontal_displacement","horizontal_speed","proxy_plane_clearance"};
            for(unsigned cap=0;cap<values.size();++cap)if(cap==5?values[cap]<limits[cap]:values[cap]>limits[cap])
                return reject_cap(sample,role,caps[cap],values[cap],limits[cap],why);
        }
        if(pair){
            const auto relative=sub(sub(sample.position[0],sample.position[1]),sub(start.position[0],start.position[1]));
            if(!positive&&policy::length(relative)>.001)return reject_cap(sample,0xffffffffU,"relative_displacement",policy::length(relative),.001,"neutral_pair_relative_response_detected");
            if(!positive&&policy::length(sub(sample.linear[0],sample.linear[1]))>.01)return reject_cap(sample,0xffffffffU,"relative_linear_speed",policy::length(sub(sample.linear[0],sample.linear[1])),.01,"neutral_pair_relative_response_detected");
        }
        // ALL role and pair caps are now proven. Only an accepted whole pair
        // sample may advance either role's fall/contact/settle evidence.
        for(unsigned role=0;role<2;++role){const auto displacement=sub(sample.position[role],start.position[role]);
            if(positive&&policy::length(sample.angular[role])>.05)report_.pair_response=true;
            if(sample.linear[role][1]<-.2f&&displacement[1]<-.003f)report_.falling[role]=true;
            if(!pair){
                if(report_.falling[role]&&sample.clearance[role]>=-.02f&&sample.clearance[role]<.04f&&
                   sample.linear[role][1]>prior.linear[role][1]+.15f)report_.contact_candidate[role]=true;
                if(report_.contact_candidate[role]&&std::abs(sample.linear[role][1])<.15f&&sample.clearance[role]>=-.02f&&sample.clearance[role]<.04f)
                    ++report_.settled[role];else report_.settled[role]=0;
            }
        }
        if(positive){const auto relative=sub(sub(sample.position[0],sample.position[1]),sub(start.position[0],start.position[1]));
            if(policy::length(relative)>.0005||policy::length(sub(sample.linear[0],sample.linear[1]))>.02)report_.pair_response=true;}
        report_.samples[report_.count++]=sample;
        if(pair&&elapsed>=6*policy::native_seconds-1e-5){
            report_.neutral_response_candidate=report_.falling[0]&&report_.falling[1]&&(!positive||report_.pair_response);
            report_.phase=report_.neutral_response_candidate?Phase::complete:Phase::failed;
            report_.issue=report_.neutral_response_candidate?(positive?"bounded_positive_pair_response_candidate":"bounded_overlap_freefall_no_pair_response_candidate"):
                positive?"positive_pair_response_not_demonstrated":"neutral_pair_did_not_genuinely_fall";
        }
        // Contact-only proof is deliberately distinct from unsupported-body
        // settling. Entire-pair caps were validated before any candidate could
        // advance; completion parks immediately and never proves riding balance.
        if(report_.kind==Kind::world_contact_only&&report_.contact_candidate[0]&&report_.contact_candidate[1]){
            report_.neutral_response_candidate=true;report_.phase=Phase::complete;
            report_.issue="both_actual_roles_contact_candidate_no_settling_claim";
        }
        if(!pair&&report_.phase==Phase::running&&elapsed>=45*policy::native_seconds-1e-5){
            report_.neutral_response_candidate=report_.kind==Kind::world_contact&&report_.contact_candidate[0]&&report_.contact_candidate[1]&&report_.settled[0]>=3&&report_.settled[1]>=3;
            report_.phase=report_.neutral_response_candidate?Phase::complete:Phase::failed;
            report_.issue=report_.neutral_response_candidate?"both_actual_roles_contact_and_settle_candidate":"neutral_world_contact_not_demonstrated";
        }
        return report_.phase!=Phase::failed;
    }
};
}
