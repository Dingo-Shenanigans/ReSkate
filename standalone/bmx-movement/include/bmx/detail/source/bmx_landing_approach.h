#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bmx::detail::source::landing_approach {
using Vec = std::array<double,3>;
struct Motion {
    std::uint64_t token{},serial{};
    double simulation_time{},step_seconds{};
    Vec com{},linear_velocity{},angular_velocity{};
    double body_yaw{};
    bool body_heading_available{};
};
struct Support {
    bool eligible{}; // actual contact eligibility is decided by the caller
    Vec point{},normal{},surface_velocity{};
};
struct Evidence {
    bool valid{},first_contact{},approach_available{},post_available{};
    bool gravity_estimate_available{},approach_travel_available{},post_travel_available{};
    Motion approach;
    std::uint64_t contact_serial{};
    double contact_time{},approach_age_seconds{};
    // The previous point speed uses the NEW contact's observed point/normal,
    // previous COM/v/omega, and no reconstructed/interpolated body pose.
    double observed_previous_closing_speed{},gravity_estimated_closing_speed{};
    double first_contact_post_closing_speed{},post_closing_speed{},scoring_closing_speed{};
    double approach_travel_yaw{},post_travel_yaw{};
    Vec estimate_gravity{};
};
namespace detail {
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec add(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec mul(Vec a,double b) noexcept {for(auto& v:a)v*=b;return a;}
inline Vec cross(Vec a,Vec b) noexcept {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
inline double dot(Vec a,Vec b) noexcept {double out=0;for(unsigned i=0;i<3;++i)out+=a[i]*b[i];return out;}
inline bool finite(Vec a,double limit) noexcept {
    for(double v:a)if(!std::isfinite(v)||std::abs(v)>limit)return false;
    return true;
}
inline bool valid(const Motion& m) noexcept {
    return m.token&&m.serial&&std::isfinite(m.simulation_time)&&m.simulation_time>=0&&
        std::isfinite(m.step_seconds)&&m.step_seconds>0&&m.step_seconds<=.1&&
        finite(m.com,1e6)&&finite(m.linear_velocity,50)&&finite(m.angular_velocity,40)&&
        (!m.body_heading_available||std::isfinite(m.body_yaw));
}
inline double closing(const Motion& m,const Support& s,Vec gravity_delta={}) noexcept {
    const auto point_velocity=add(add(m.linear_velocity,gravity_delta),cross(m.angular_velocity,sub(s.point,m.com)));
    return std::max(0.0,-dot(sub(point_velocity,s.surface_velocity),s.normal));
}
inline bool travel(Vec velocity,double& yaw) noexcept {
    if(std::hypot(velocity[0],velocity[2])<=.15)return false;
    yaw=std::atan2(velocity[0],velocity[2]);return std::isfinite(yaw);
}
}

// Scoring evidence only: never writes or integrates a physical body. A previous
// unsupported observation may precede a native backup collision. Its point-speed
// and gravity-only prediction are conservative classification estimates, NOT a
// measured manifold velocity or an exact reconstructed pre-impact state. Native
// forces/torques and rotation between samples remain unobserved here.
class Tracker {
    Motion previous_{};
    bool observed_{},previous_supported_{};
    Evidence contact_{};
public:
    void reset() noexcept {previous_={};observed_=previous_supported_=false;contact_={};}
    Evidence observe(const Motion& current,const std::array<Support,2>& supports,Vec gravity) noexcept {
        if(!detail::valid(current)||!detail::finite(gravity,100)) {reset();return {};}
        bool supported=false;
        double post=0;
        for(const auto& support:supports)if(support.eligible) {
            if(!detail::finite(support.point,1e6)||!detail::finite(support.normal,2)||
               std::abs(detail::dot(support.normal,support.normal)-1)>.001||
               !detail::finite(support.surface_velocity,50)) {reset();return {};}
            const auto lever=detail::sub(support.point,current.com);
            if(detail::dot(lever,lever)>25) {reset();return {};}
            supported=true;post=std::max(post,detail::closing(current,support));
        }
        const double age=observed_?current.simulation_time-previous_.simulation_time:0;
        const bool adjacent=observed_&&previous_.token==current.token&&
            previous_.serial!=std::numeric_limits<std::uint64_t>::max()&&current.serial==previous_.serial+1&&
            age>0&&std::abs(age-current.step_seconds)<=1e-7;
        if(!adjacent)contact_={};
        if(!supported) {
            contact_={};previous_=current;observed_=true;previous_supported_=false;
            Evidence clear;clear.valid=true;return clear;
        }
        const bool first_contact=!adjacent||!previous_supported_;
        if(first_contact) {
            contact_={};contact_.contact_serial=current.serial;contact_.contact_time=current.simulation_time;
            contact_.first_contact_post_closing_speed=post;
            if(adjacent&&!previous_supported_) {
                contact_.approach_available=true;contact_.approach=previous_;contact_.approach_age_seconds=age;
                contact_.estimate_gravity=gravity;contact_.gravity_estimate_available=true;
                for(const auto& support:supports)if(support.eligible) {
                    contact_.observed_previous_closing_speed=std::max(contact_.observed_previous_closing_speed,
                        detail::closing(previous_,support));
                    contact_.gravity_estimated_closing_speed=std::max(contact_.gravity_estimated_closing_speed,
                        detail::closing(previous_,support,detail::mul(gravity,age)));
                }
                contact_.approach_travel_available=detail::travel(previous_.linear_velocity,contact_.approach_travel_yaw);
            }
        }
        auto evidence=contact_;
        evidence.valid=true;evidence.first_contact=first_contact;evidence.post_available=true;evidence.post_closing_speed=post;
        evidence.post_travel_available=detail::travel(current.linear_velocity,evidence.post_travel_yaw);
        evidence.scoring_closing_speed=std::max({post,evidence.first_contact_post_closing_speed,
            evidence.observed_previous_closing_speed,evidence.gravity_estimated_closing_speed});
        previous_=current;observed_=true;previous_supported_=true;
        return evidence;
    }
};

// This can retain an ALREADY caught pending landing, never create actual
// support or award credit. The original40ms generic gap remains independent.
// The200ms deadline authorizes unsupported HOLD only; an uninterrupted strict
// suffix may finish its original60ms after that deadline. Any new gap may not.
inline constexpr double qualified_settle_hold_seconds=.2;
// Match InputMapper's raw pedal/brake neutral gate. Airborne Actions::pedal
// is forced to zero, so it cannot prove that the player's pedal is released.
inline bool settle_drive_neutral(double pedal,double brake,double threshold) noexcept {
    return std::isfinite(pedal)&&std::isfinite(brake)&&std::isfinite(threshold)&&threshold>=0&&threshold<=1&&
        std::clamp(pedal,0.0,1.0)<=threshold&&std::clamp(brake,0.0,1.0)<=threshold;
}
struct SettleWheel {
    bool verified_plane{},actual_support{};
    Vec point{},normal{},surface_velocity{};
    double separation{},normal_load{};
};
struct SettleSample {
    bool pending{},available{},caught_frozen{},neutral_controls{};
    // Current canonical client/entity/core/context/rig/world/client-context.
    std::array<std::uintptr_t,7> identity{};
    Motion motion;
    std::array<double,4> quaternion{0,0,0,1};
    std::array<SettleWheel,2> wheels;
};
enum class SettleIssue { none,not_pending,unqualified,duplicate,owner,clock,geometry,input,expired };
struct SettleEvidence {bool hold{},cancel{};double elapsed{};SettleIssue issue{SettleIssue::none};};
class SettleHoldTracker {
    SettleSample previous_,seed_;
    bool observed_{},seeded_{},used_{};
    double first_hold_time_{};
    static bool unit(Vec in,Vec& out) noexcept {
        const auto n=std::sqrt(detail::dot(in,in));if(!detail::finite(in,2)||n<1e-9||!std::isfinite(n))return false;
        out=detail::mul(in,1/n);return std::abs(n-1)<=.001;
    }
    static bool orientation(const SettleSample& sample,Vec& up,double& heading) noexcept {
        double n=0;for(double v:sample.quaternion){if(!std::isfinite(v))return false;n+=v*v;}
        if(std::abs(n-1)>.001)return false;
        const double inverse=1/std::sqrt(n);
        auto q=sample.quaternion;for(auto& v:q)v*=inverse;const auto x=q[0],y=q[1],z=q[2],w=q[3];
        up={2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)};
        heading=std::atan2(2*(x*z+y*w),1-2*(x*x+y*y));return std::isfinite(heading);
    }
    static bool geometry_valid(const SettleSample& sample,Vec up,double heading) noexcept {
        double travel{};if(!detail::travel(sample.motion.linear_velocity,travel)||
           std::abs(std::remainder(heading-travel,6.283185307179586))>.6108652381980153)return false;
        for(const auto& wheel:sample.wheels) {
            Vec normal;if(!wheel.verified_plane||!unit(wheel.normal,normal)||
               !detail::finite(wheel.point,1e6)||!detail::finite(wheel.surface_velocity,50)||
               !std::isfinite(wheel.separation)||wheel.separation<-.10||wheel.separation>=0||
               !std::isfinite(wheel.normal_load)||wheel.normal_load<=0||detail::dot(up,normal)<.95||normal[1]<.5)return false;
        }
        return true;
    }
    SettleEvidence refuse(SettleIssue issue,bool fatal=false) noexcept {
        seeded_=false;observed_=false;return {false,used_||fatal,0,issue};
    }
public:
    void reset() noexcept {*this={};}
    SettleEvidence observe(const SettleSample& sample) noexcept {
        if(!sample.pending) {reset();return {false,false,0,SettleIssue::not_pending};}
        if(!sample.available||!sample.caught_frozen||!detail::valid(sample.motion))return refuse(SettleIssue::unqualified);
        for(const auto part:sample.identity)if(!part)return refuse(SettleIssue::owner,true);
        if(observed_) {
            if(sample.identity!=previous_.identity||sample.motion.token!=previous_.motion.token)return refuse(SettleIssue::owner,true);
            if(sample.motion.serial==previous_.motion.serial&&sample.motion.simulation_time==previous_.motion.simulation_time)
                return {false,false,used_?sample.motion.simulation_time-first_hold_time_:0,SettleIssue::duplicate};
            const auto age=sample.motion.simulation_time-previous_.motion.simulation_time;
            if(previous_.motion.serial==std::numeric_limits<std::uint64_t>::max()||sample.motion.serial!=previous_.motion.serial+1||
               std::abs(age-sample.motion.step_seconds)>1e-7||
               std::abs(sample.motion.step_seconds-previous_.motion.step_seconds)>1e-7)return refuse(SettleIssue::clock,true);
        }
        if(!sample.neutral_controls)return refuse(SettleIssue::input);
        Vec up;double heading{};
        if(!orientation(sample,up,heading)||!geometry_valid(sample,up,heading))return refuse(SettleIssue::geometry);
        const bool strict=sample.wheels[0].actual_support||sample.wheels[1].actual_support;
        if(!observed_||!seeded_) {
            if(!strict||!sample.wheels[0].actual_support||!sample.wheels[1].actual_support)return refuse(SettleIssue::unqualified);
            seed_=previous_=sample;observed_=seeded_=true;return {};
        }
        // Pin the original loaded planes for the full candidate, and require
        // strictly recovering negative gaps on unsupported intervals. No
        // default alignment1 or assumed mass*g load is consumed here.
        for(unsigned i=0;i<2;++i) {
            const auto& now=sample.wheels[i];const auto& old=previous_.wheels[i];const auto& initial=seed_.wheels[i];
            Vec normal,old_normal,seed_normal;if(!unit(now.normal,normal)||!unit(old.normal,old_normal)||!unit(initial.normal,seed_normal))return refuse(SettleIssue::geometry);
            const auto delta=detail::sub(detail::sub(now.point,old.point),detail::mul(old.surface_velocity,sample.motion.step_seconds));
            const auto total=detail::sub(detail::sub(now.point,initial.point),detail::mul(initial.surface_velocity,
                sample.motion.simulation_time-seed_.motion.simulation_time));
            if(!old.verified_plane||old.normal_load<=0||old.separation>=0||
               detail::dot(normal,old_normal)<std::cos(.002)||detail::dot(normal,seed_normal)<std::cos(.002)||
               std::abs(detail::dot(delta,old_normal))>.0025||std::abs(detail::dot(delta,normal))>.0025||
               std::abs(detail::dot(total,seed_normal))>.0025||
               detail::dot(delta,delta)>.34*.34||
               detail::dot(detail::sub(now.surface_velocity,old.surface_velocity),detail::sub(now.surface_velocity,old.surface_velocity))>1e-12)
                return refuse(SettleIssue::geometry);
            if(!strict) {
                const auto point_velocity=detail::add(sample.motion.linear_velocity,
                    detail::cross(sample.motion.angular_velocity,detail::sub(now.point,sample.motion.com)));
                const auto opening=detail::dot(detail::sub(point_velocity,now.surface_velocity),normal);
                if(now.separation<=old.separation||!std::isfinite(opening)||opening<=0||opening>.4)return refuse(SettleIssue::geometry);
            }
        }
        const auto com_delta=detail::sub(sample.motion.com,seed_.motion.com);
        if(std::abs(com_delta[1])>.4*qualified_settle_hold_seconds)return refuse(SettleIssue::geometry);
        if(strict) {previous_=sample;return {false,false,used_?sample.motion.simulation_time-first_hold_time_:0,SettleIssue::none};}
        if(!used_) {used_=true;first_hold_time_=previous_.motion.simulation_time;}
        const auto elapsed=sample.motion.simulation_time-first_hold_time_;
        if(elapsed>qualified_settle_hold_seconds+1e-9)return refuse(SettleIssue::expired,true);
        previous_=sample;return {true,false,elapsed,SettleIssue::none};
    }
};
}
