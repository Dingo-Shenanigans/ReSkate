#pragma once
#include "bmx_input.h"
#include "bmx_landing_approach.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>

namespace bmx::detail::source {
// This controller judges its own BMX combos. It never writes native Skate score,
// supplies physics yaw, or awards a manual from an unverified input intention.
enum class TrickFailure { none, under_rotated, uncaught, unaligned_landing, canceled, invalid_sample, unstable_landing };
inline constexpr std::string_view trick_failure_name(TrickFailure failure) noexcept {
    switch(failure) {
    case TrickFailure::none:return "none";
    case TrickFailure::under_rotated:return "under-rotated";
    case TrickFailure::uncaught:return "uncaught";
    case TrickFailure::unaligned_landing:return "unaligned landing";
    case TrickFailure::canceled:return "canceled";
    case TrickFailure::invalid_sample:return "invalid native time or heading";
    case TrickFailure::unstable_landing:return "unstable landing";
    }
    return "unknown";
}
struct TrickSettings {
    double bar_seconds{.45}, tail_seconds{.55};
    double catch_window{.12}, catch_blend{.06};
    double landing_tolerance{35.0*std::numbers::pi/180.0};
    double max_sample_seconds{.10}, max_yaw_rate{20.0}, max_air_seconds{15.0};
    double landing_settle_seconds{.06}, landing_gap_seconds{.04};
    double minimum_body_up_alignment{.5}, minimum_support_up_alignment{.15}, maximum_closing_speed{7};
    // Optional per-catalog timing in real simulation seconds. Zero keeps the
    // independent legacy duration. This does not supply native scoring rules.
    std::array<double,80> enter_seconds{},recover_seconds{};
};
struct TrickInput {
    bool enabled{}, grounded{true};
    std::uint64_t owner{}; // canonical rider identity/generation, not a visual actor
    std::uint32_t device{};
    TrickDirection barspin{TrickDirection::none}, tailwhip{TrickDirection::none};
    bool bar_neutral{}, rear_neutral{};
    double body_yaw{}, travel_yaw{}; // actual applied heading and model momentum heading
    // Opt-in native quality policy. Normals come from actual eligible supports;
    // dots are unit-vector alignments; speed=max(0,-relative point normal speed).
    // Keep the policy flag enabled on clear frames too; no normal is consumed
    // until grounded. Defaults retain the existing immediate legacy scoring.
    bool landing_quality_available{};
    bool landing_settle_hold{},landing_settle_hold_invalid{}; // fresh adapter proof; never actual support
    double body_up_alignment{1}, support_up_alignment{1}, closing_normal_speed{};
    catalog::TrickId movement{catalog::TrickId::none};
    bool orientation_available{};
    std::array<double,4> body_quaternion{0,0,0,1}; // measured native XYZW, unit length
};
struct TrickTally {
    std::uint64_t landed_combos{}, landed_barspins{}, landed_tailwhips{}, landed_spin_half_turns{};
    std::uint64_t failed_combos{}, canceled_combos{}, landed_units{}, best_combo_units{};
    std::uint64_t landed_movements{};
    std::array<std::uint64_t,80> landed_by_id{};
    std::uint64_t landed_front_flips{}, landed_back_flips{}, landed_barrel_rolls{};
};
struct TrickState {
    bool airborne{};
    double air_seconds{}, bar_angle{}, tail_angle{}, spin_radians{};
    double bar_release{}, tail_release{}; // 0=contact, 1=free; catch requires neutral
    bool bar_rotating{}, tail_rotating{}, bar_waiting_catch{}, tail_waiting_catch{};
    bool bar_caught{}, tail_caught{};
    unsigned pending_barspins{}, pending_tailwhips{}, pending_spin_half_turns{}, pending_units{};
    TrickFailure failure{TrickFailure::none};
    TrickTally tally;
    bool landing_pending{};
    bool landing_hold_active{};double landing_hold_elapsed_seconds{};
    double landing_support_seconds{}, landing_gap_seconds{};
    catalog::TrickId movement_id{catalog::TrickId::none};
    // Retargeting presentation only: never changes native chassis orientation.
    double movement_phase{}, movement_progress{}, movement_release{};
    double movement_bar_angle{}, movement_tail_angle{}, movement_crank_angle{};
    double movement_bike_pitch{}, movement_bike_roll{};
    double release_left_hand{}, release_right_hand{}, release_left_foot{}, release_right_foot{};
    bool movement_holding{}, movement_recovering{}, movement_caught{};
    unsigned pending_movements{};
    double flip_radians{}, roll_radians{};
    unsigned pending_front_flips{}, pending_back_flips{}, pending_barrel_rolls{};
};
struct TrickEvents {
    bool took_off{}, landed{}, combo_landed{}, failed{}, canceled{}, bar_caught{}, tail_caught{};
    unsigned barspins{}, tailwhips{}, spin_half_turns{}, units{};
    TrickFailure failure{TrickFailure::none};
    bool movement_caught{};
    unsigned movements{};
    unsigned front_flips{}, back_flips{}, barrel_rolls{};
};
class TrickController {
    static constexpr double tau_=2.0*std::numbers::pi;
    struct Channel {
        bool armed{}, rotating{}, waiting{}, caught{}, failed{}, attempted{};
        int direction{};
        double elapsed{}, catch_elapsed{}, blend_remaining{}, origin{}, angle{}, release{};
        unsigned count{};
        std::array<unsigned,2> directional_counts{};
    };
    TrickSettings settings_;
    TrickState state_;
    Channel bar_,tail_;
    struct Movement {
        catalog::TrickId id{catalog::TrickId::none};
        bool armed{}, active{}, holding{}, recovering{}, waiting{}, caught{}, failed{}, attempted{};
        double elapsed{}, recover_elapsed{}, catch_elapsed{}, phase{}, progress{}, release{};
        unsigned count{};
        std::array<unsigned,80> counts{};
    } movement_;
    bool identity_known_{}, ground_seen_{}, spin_attempted_{};
    std::uint64_t owner_{};
    std::uint32_t device_{};
    double previous_yaw_{};
    std::array<double,4> previous_quaternion_{0,0,0,1};
    bool full_orientation_{}, flip_attempted_{}, roll_attempted_{};
    bool landing_pending_{};
    double landing_seconds_{}, landing_gap_seconds_{}, landing_elapsed_{};
    bool landing_hold_started_{},landing_holding_{};double landing_hold_start_{};

    static double bounded(double v,double fallback,double low,double high) noexcept {
        return std::isfinite(v)?std::clamp(v,low,high):fallback;
    }
    static TrickSettings sanitize(TrickSettings s) noexcept {
        const TrickSettings d;
        s.bar_seconds=bounded(s.bar_seconds,d.bar_seconds,.15,1.0);
        s.tail_seconds=bounded(s.tail_seconds,d.tail_seconds,.15,1.0);
        s.catch_window=bounded(s.catch_window,d.catch_window,.02,.30);
        s.catch_blend=bounded(s.catch_blend,d.catch_blend,.01,.20);
        s.landing_tolerance=bounded(s.landing_tolerance,d.landing_tolerance,.05,std::numbers::pi/3);
        s.max_sample_seconds=bounded(s.max_sample_seconds,d.max_sample_seconds,.01,.10);
        s.max_yaw_rate=bounded(s.max_yaw_rate,d.max_yaw_rate,1,30);
        s.max_air_seconds=bounded(s.max_air_seconds,d.max_air_seconds,1,30);
        s.landing_settle_seconds=bounded(s.landing_settle_seconds,d.landing_settle_seconds,.01,.50);
        s.landing_gap_seconds=bounded(s.landing_gap_seconds,d.landing_gap_seconds,0,.10);
        s.minimum_body_up_alignment=bounded(s.minimum_body_up_alignment,d.minimum_body_up_alignment,.1,1);
        s.minimum_support_up_alignment=bounded(s.minimum_support_up_alignment,d.minimum_support_up_alignment,.05,1);
        s.maximum_closing_speed=bounded(s.maximum_closing_speed,d.maximum_closing_speed,.1,50);
        for(auto*times:{&s.enter_seconds,&s.recover_seconds})for(auto&value:*times)
            value=std::isfinite(value)&&value>0?std::clamp(value,.02,5.):0;
        return s;
    }
    double enter_seconds(const catalog::Entry& entry) const noexcept {
        const auto i=static_cast<unsigned>(entry.id);
        return i<settings_.enter_seconds.size()&&settings_.enter_seconds[i]>0?settings_.enter_seconds[i]:entry.enter_seconds;
    }
    double recover_seconds(const catalog::Entry& entry) const noexcept {
        const auto i=static_cast<unsigned>(entry.id);
        return i<settings_.recover_seconds.size()&&settings_.recover_seconds[i]>0?settings_.recover_seconds[i]:entry.recover_seconds;
    }
    static void add(std::uint64_t& value,std::uint64_t amount) noexcept {
        const auto remaining=std::numeric_limits<std::uint64_t>::max()-value;
        value+=std::min(remaining,amount);
    }
    bool attempted() const noexcept {return bar_.attempted||tail_.attempted||spin_attempted_||movement_.attempted||flip_attempted_||roll_attempted_;}
    void clear_live(TrickFailure failure=TrickFailure::none) noexcept {
        const auto tally=state_.tally;
        state_={};state_.tally=tally;state_.failure=failure;
        bar_={};tail_={};movement_={};ground_seen_=false;spin_attempted_=false;previous_yaw_=0;
        full_orientation_=flip_attempted_=roll_attempted_=false;previous_quaternion_={0,0,0,1};
        landing_pending_=false;landing_seconds_=landing_gap_seconds_=landing_elapsed_=0;
        landing_hold_started_=landing_holding_=false;landing_hold_start_=0;
    }
    bool cancel_live(TrickFailure failure) noexcept {
        const bool pending=state_.airborne&&attempted();
        if(pending)add(state_.tally.canceled_combos,1);
        clear_live(failure);
        return pending;
    }
    void sync() noexcept {
        state_.bar_angle=bar_.angle;state_.tail_angle=tail_.angle;
        state_.bar_release=bar_.release;state_.tail_release=tail_.release;
        state_.bar_rotating=bar_.rotating;state_.tail_rotating=tail_.rotating;
        state_.bar_waiting_catch=bar_.waiting;state_.tail_waiting_catch=tail_.waiting;
        state_.bar_caught=bar_.caught;state_.tail_caught=tail_.caught;
        state_.pending_barspins=bar_.count;state_.pending_tailwhips=tail_.count;
        state_.pending_spin_half_turns=spin_attempted_?
            static_cast<unsigned>(std::floor(std::abs(state_.spin_radians)/std::numbers::pi+.5)):0;
        state_.pending_movements=movement_.count;
        const auto full_turns=[](double radians) {return static_cast<unsigned>(std::floor(std::abs(radians)/tau_+.5));};
        const unsigned flips=flip_attempted_?full_turns(state_.flip_radians):0;
        state_.pending_front_flips=state_.flip_radians>0?flips:0;
        state_.pending_back_flips=state_.flip_radians<0?flips:0;
        state_.pending_barrel_rolls=roll_attempted_?full_turns(state_.roll_radians):0;
        state_.pending_units=bar_.count+tail_.count+state_.pending_spin_half_turns+movement_.count+
            state_.pending_front_flips+state_.pending_back_flips+state_.pending_barrel_rolls;
        state_.landing_pending=landing_pending_;state_.landing_support_seconds=landing_seconds_;
        state_.landing_gap_seconds=landing_gap_seconds_;
        state_.landing_hold_active=landing_holding_;
        state_.landing_hold_elapsed_seconds=landing_hold_started_?landing_elapsed_-landing_hold_start_:0;
        state_.movement_id=movement_.id;state_.movement_phase=movement_.phase;
        state_.movement_progress=movement_.progress;state_.movement_release=movement_.release;
        state_.movement_holding=movement_.holding;state_.movement_recovering=movement_.recovering;
        state_.movement_caught=movement_.caught;
        const auto* entry=catalog::find(movement_.id);
        const auto mask=entry?entry->release_mask:0;
        state_.release_left_hand=(mask&catalog::left_hand)?movement_.release:0;
        state_.release_right_hand=(mask&catalog::right_hand)?movement_.release:0;
        state_.release_left_foot=(mask&catalog::left_foot)?movement_.release:0;
        state_.release_right_foot=(mask&catalog::right_foot)?movement_.release:0;
        const double t=movement_.progress;
        const double turn=t*t*(3-2*t)*tau_;
        state_.movement_bar_angle=entry?entry->bar_turns*turn+entry->bar_pose_radians*movement_.phase:0;
        state_.movement_tail_angle=entry?entry->tail_turns*turn:0;
        state_.movement_crank_angle=entry?entry->crank_turns*turn:0;
        state_.movement_bike_pitch=state_.movement_bike_roll=0;
        if(entry) {
            const double pose=entry->motion==catalog::Motion::pose?movement_.phase:smooth(t);
            state_.movement_bike_pitch=entry->bike_pitch*pose;
            state_.movement_bike_roll=entry->bike_roll*pose;
            // Minor compound components return to neutral while the full-cycle
            // component stays unwrapped. Both ends are the same physical pose.
            if(entry->family==catalog::Family::grand_slam)
                state_.movement_bike_roll=entry->bike_roll*std::sin(tau_*smooth(t));
            if(entry->family==catalog::Family::bri_flip)
                state_.movement_bike_pitch=entry->bike_pitch*std::sin(tau_*smooth(t));
        }
    }
    static double smooth(double t) noexcept {return t*t*(3-2*t);}
    bool movement_channel(catalog::TrickId request,bool neutral,double seconds) noexcept {
        auto& m=movement_;
        if(neutral&&request==catalog::TrickId::none)m.armed=true;
        if(request!=catalog::TrickId::none&&!catalog::legacy_rotation(request)) {
            const bool start=m.armed&&!m.active&&!m.failed&&
                !bar_.rotating&&!bar_.waiting&&!tail_.rotating&&!tail_.waiting&&
                bar_.release==0&&tail_.release==0;
            m.armed=false; // consume busy requests, never defer them to a later frame
            if(start) {
                m.id=request;m.active=true;m.attempted=true;m.caught=false;
                m.holding=m.recovering=m.waiting=false;
                m.elapsed=m.recover_elapsed=m.catch_elapsed=m.phase=m.progress=m.release=0;
                return false;
            }
        }
        if(!m.active||m.failed)return false;
        const auto* entry=catalog::find(m.id);
        if(!entry){m.failed=true;return false;}
        if(m.recovering) {
            const auto duration=recover_seconds(*entry);
            m.recover_elapsed=std::min(duration,m.recover_elapsed+seconds);
            const double left=1-smooth(m.recover_elapsed/duration);
            if(entry->motion==catalog::Motion::pose)m.phase=left;
            m.release=left;
            if(m.recover_elapsed+1e-9<duration)return false;
            m.active=m.recovering=false;m.caught=true;m.phase=m.release=0;
            ++m.count;++m.counts[static_cast<std::size_t>(m.id)];
            return true;
        }
        if(!m.holding&&!m.waiting) {
            const auto duration=enter_seconds(*entry);
            m.elapsed+=seconds;m.progress=std::min(1.0,m.elapsed/duration);
            m.phase=entry->motion==catalog::Motion::pose?smooth(m.progress):std::sin(std::numbers::pi*m.progress);
            m.release=std::min(1.0,m.progress/.15);
            if(m.progress<1)return false;
            if(entry->motion==catalog::Motion::pose)m.holding=true;
            else {m.waiting=true;m.catch_elapsed=std::max(0.0,m.elapsed-duration);}
        }else if(m.waiting)m.catch_elapsed+=seconds;
        if(m.waiting&&m.catch_elapsed>settings_.catch_window+1e-9) {
            m.failed=true;m.release=1;return false;
        }
        if(neutral) {
            // Release in the air starts a measured recovery. First contact never
            // advances this channel, so a landing frame cannot manufacture a catch.
            m.holding=m.waiting=false;m.recovering=true;m.recover_elapsed=0;
        }
        return false;
    }
    bool channel(Channel& c,TrickDirection request,bool neutral,double seconds,double duration,bool allowed=true) noexcept {
        if(c.caught&&c.blend_remaining>0) {
            c.blend_remaining=std::max(0.0,c.blend_remaining-seconds);
            c.release=c.blend_remaining/settings_.catch_blend;
        }
        if(neutral&&request==TrickDirection::none)c.armed=true;
        if(request!=TrickDirection::none) {
            const bool start=allowed&&c.armed&&!c.rotating&&!c.waiting&&!c.failed;
            c.armed=false; // busy/held requests are consumed, never queued
            if(start) {
                c.attempted=true;c.rotating=true;c.caught=false;c.elapsed=0;
                c.catch_elapsed=0;c.blend_remaining=0;c.origin=c.angle;
                c.direction=request==TrickDirection::left?-1:1;
                return false; // the request arrived at the end of this sample
            }
        }
        if(c.failed){c.release=1;return false;}
        if(c.rotating) {
            c.elapsed+=seconds;
            const double progress=std::min(1.0,c.elapsed/duration);
            const double smooth=progress*progress*(3-2*progress);
            c.angle=c.origin+c.direction*tau_*smooth;
            c.release=std::min(1.0,progress/.10);
            if(progress<1)return false;
            c.rotating=false;c.waiting=true;c.catch_elapsed=std::max(0.0,c.elapsed-duration);
        }else if(c.waiting)c.catch_elapsed+=seconds;
        if(c.waiting) {
            c.release=1;
            if(c.catch_elapsed>settings_.catch_window+1e-9) {
                c.waiting=false;c.failed=true;
            }else if(neutral) {
                c.waiting=false;c.caught=true;++c.count;
                ++c.directional_counts[c.direction<0?0U:1U];
                c.blend_remaining=settings_.catch_blend;
                return true;
            }
        }
        return false;
    }
    TrickFailure landing_failure(const TrickInput& input) const noexcept {
        TrickFailure failure=state_.failure;
        if(failure==TrickFailure::none&&(bar_.rotating||tail_.rotating))failure=TrickFailure::under_rotated;
        if(failure==TrickFailure::none&&(bar_.waiting||tail_.waiting||bar_.failed||tail_.failed))failure=TrickFailure::uncaught;
        if(failure==TrickFailure::none&&movement_.active)
            failure=movement_.progress<1?TrickFailure::under_rotated:TrickFailure::uncaught;
        if(failure==TrickFailure::none&&movement_.failed)failure=TrickFailure::uncaught;
        const double rotation=std::abs(state_.spin_radians);
        const auto half_turns=static_cast<unsigned>(std::floor(rotation/std::numbers::pi+.5));
        if(failure==TrickFailure::none&&spin_attempted_&&
           (!half_turns||std::abs(rotation-half_turns*std::numbers::pi)>settings_.landing_tolerance))
            failure=TrickFailure::under_rotated;
        const auto complete_turn=[&](double radians) {
            const auto count=static_cast<unsigned>(std::floor(std::abs(radians)/tau_+.5));
            return count&&std::abs(std::abs(radians)-count*tau_)<=settings_.landing_tolerance;
        };
        if(failure==TrickFailure::none&&((flip_attempted_&&!complete_turn(state_.flip_radians))||
            (roll_attempted_&&!complete_turn(state_.roll_radians))))failure=TrickFailure::under_rotated;
        // Forward and fakie are both legitimate; a sideways landing is not.
        if(failure==TrickFailure::none&&attempted()&&
           std::abs(std::cos(input.body_yaw-input.travel_yaw))<std::cos(settings_.landing_tolerance))
            failure=TrickFailure::unaligned_landing;
        return failure;
    }
    static bool valid_quality(const TrickInput& input) noexcept {
        return std::isfinite(input.body_up_alignment)&&std::abs(input.body_up_alignment)<=1.000001&&
            std::isfinite(input.support_up_alignment)&&std::abs(input.support_up_alignment)<=1.000001&&
            std::isfinite(input.closing_normal_speed)&&input.closing_normal_speed>=0&&input.closing_normal_speed<=1e6;
    }
    bool clean_quality(const TrickInput& input) const noexcept {
        return input.body_up_alignment>=settings_.minimum_body_up_alignment&&
            input.support_up_alignment>=settings_.minimum_support_up_alignment&&
            input.closing_normal_speed<=settings_.maximum_closing_speed;
    }
    static bool valid_orientation(const std::array<double,4>& q) noexcept {
        double length=0;
        for(double v:q){if(!std::isfinite(v))return false;length+=v*v;}
        return std::abs(length-1)<=.001;
    }
    bool orientation_change(double seconds,const TrickInput& input,std::array<double,3>& delta) noexcept {
        if(input.orientation_available!=full_orientation_)return false;
        if(!full_orientation_) {
            delta={0,std::remainder(input.body_yaw-previous_yaw_,tau_),0};
            previous_yaw_=input.body_yaw;
            return std::abs(delta[1])<=settings_.max_yaw_rate*seconds+.05;
        }
        const auto& a=previous_quaternion_;const auto& b=input.body_quaternion;
        // inverse(previous)*current: measured incremental BODY-local rotation.
        // This avoids Euler heading jumps when a real front/backflip passes vertical.
        std::array<double,4> relative{
            a[3]*b[0]-a[0]*b[3]-a[1]*b[2]+a[2]*b[1],
            a[3]*b[1]+a[0]*b[2]-a[1]*b[3]-a[2]*b[0],
            a[3]*b[2]-a[0]*b[1]+a[1]*b[0]-a[2]*b[3],
            a[3]*b[3]+a[0]*b[0]+a[1]*b[1]+a[2]*b[2]};
        if(relative[3]<0)for(auto& v:relative)v=-v; // q and -q describe the same pose
        const double sine=std::hypot(relative[0],relative[1],relative[2]);
        const double angle=2*std::atan2(sine,std::max(0.0,relative[3]));
        if(angle>settings_.max_yaw_rate*seconds+.05)return false;
        const double scale=sine>1e-12?angle/sine:2;
        delta={relative[0]*scale,relative[1]*scale,relative[2]*scale};
        previous_quaternion_=b;previous_yaw_=input.body_yaw;
        return true;
    }
    TrickEvents land(const TrickInput& input,TrickFailure forced=TrickFailure::none) noexcept {
        TrickEvents event;event.landed=true;
        const auto failure=forced==TrickFailure::none?landing_failure(input):forced;
        const double rotation=std::abs(state_.spin_radians);
        const auto half_turns=static_cast<unsigned>(std::floor(rotation/std::numbers::pi+.5));
        if(attempted()&&failure!=TrickFailure::none) {
            event.failed=true;event.failure=failure;add(state_.tally.failed_combos,1);
        }else if(attempted()) {
            event.barspins=bar_.count;event.tailwhips=tail_.count;
            event.spin_half_turns=spin_attempted_?half_turns:0;
            event.movements=movement_.count;
            const unsigned flips=flip_attempted_?static_cast<unsigned>(std::floor(std::abs(state_.flip_radians)/tau_+.5)):0;
            event.front_flips=state_.flip_radians>0?flips:0;
            event.back_flips=state_.flip_radians<0?flips:0;
            event.barrel_rolls=roll_attempted_?static_cast<unsigned>(std::floor(std::abs(state_.roll_radians)/tau_+.5)):0;
            event.units=event.barspins+event.tailwhips+event.spin_half_turns+event.movements+
                event.front_flips+event.back_flips+event.barrel_rolls;
            event.combo_landed=event.units>0;
            if(event.combo_landed) {
                add(state_.tally.landed_combos,1);add(state_.tally.landed_barspins,event.barspins);
                add(state_.tally.landed_tailwhips,event.tailwhips);add(state_.tally.landed_spin_half_turns,event.spin_half_turns);
                add(state_.tally.landed_units,event.units);
                add(state_.tally.landed_movements,event.movements);
                add(state_.tally.landed_front_flips,event.front_flips);add(state_.tally.landed_back_flips,event.back_flips);
                add(state_.tally.landed_barrel_rolls,event.barrel_rolls);
                add(state_.tally.landed_by_id[34],bar_.directional_counts[0]);
                add(state_.tally.landed_by_id[44],bar_.directional_counts[1]);
                add(state_.tally.landed_by_id[56],tail_.directional_counts[0]);
                add(state_.tally.landed_by_id[52],tail_.directional_counts[1]);
                for(std::size_t i=0;i<movement_.counts.size();++i)
                    add(state_.tally.landed_by_id[i],movement_.counts[i]);
                state_.tally.best_combo_units=std::max(state_.tally.best_combo_units,std::uint64_t{event.units});
            }
        }
        clear_live(event.failed?failure:TrickFailure::none);
        ground_seen_=true;previous_yaw_=input.body_yaw;
        full_orientation_=input.orientation_available;previous_quaternion_=input.body_quaternion;
        return event;
    }
public:
    explicit TrickController(TrickSettings settings={}) noexcept:settings_(sanitize(settings)){}
    const TrickState& state() const noexcept {return state_;}
    // Lifecycle interruptions cancel the live attempt and preserve landed tally.
    // Resuming in the air cannot resurrect an attempt; ground must be observed.
    void cancel() noexcept {cancel_live(TrickFailure::canceled);}
    void reset() noexcept {state_={};bar_={};tail_={};movement_={};identity_known_=false;ground_seen_=false;spin_attempted_=false;flip_attempted_=roll_attempted_=full_orientation_=false;previous_quaternion_={0,0,0,1};owner_=0;device_=0;previous_yaw_=0;landing_pending_=false;landing_seconds_=landing_gap_seconds_=landing_elapsed_=0;landing_hold_started_=landing_holding_=false;landing_hold_start_=0;}
    TrickEvents update(double seconds,const TrickInput& input) noexcept {
        TrickEvents event;
        if(!input.enabled||!input.owner) {
            event.canceled=cancel_live(TrickFailure::canceled);event.failure=event.canceled?TrickFailure::canceled:TrickFailure::none;
            return event;
        }
        if(identity_known_&&(owner_!=input.owner||device_!=input.device)) {
            event.canceled=cancel_live(TrickFailure::canceled);event.failure=event.canceled?TrickFailure::canceled:TrickFailure::none;
            owner_=input.owner;device_=input.device;return event;
        }
        identity_known_=true;owner_=input.owner;device_=input.device;
        if(!std::isfinite(seconds)||seconds<0||seconds>settings_.max_sample_seconds+1e-9||
           !std::isfinite(input.body_yaw)||!std::isfinite(input.travel_yaw)) {
            event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
        }
        if(!catalog::valid(input.movement)||(input.orientation_available&&!valid_orientation(input.body_quaternion))) {
            event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
        }
        if(seconds==0)return event; // no native step means no rotation, catch or late landing credit
        if(landing_pending_) {
            // First contact froze channels and accumulated air spin. Neither
            // short bounces nor time on the ground may finish/catch a trick.
            std::array<double,3> change{};
            landing_elapsed_+=seconds;
            if(!orientation_change(seconds,input,change)||
               state_.air_seconds+landing_elapsed_>settings_.max_air_seconds) {
                event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
            }
            previous_yaw_=input.body_yaw;landing_holding_=false;
            if(input.landing_settle_hold_invalid) {
                event.canceled=cancel_live(TrickFailure::unstable_landing);event.failure=TrickFailure::unstable_landing;return event;
            }
            if(!input.grounded) {
                landing_seconds_=0;landing_gap_seconds_+=seconds;
                if(input.landing_settle_hold&&!landing_hold_started_) {
                    landing_hold_started_=true;landing_hold_start_=landing_elapsed_-seconds;
                }
                const bool bounded_hold=input.landing_settle_hold&&landing_hold_started_&&
                    landing_elapsed_-landing_hold_start_<=landing_approach::qualified_settle_hold_seconds+1e-9;
                landing_holding_=bounded_hold;
                if((landing_hold_started_&&!bounded_hold)||
                   (!bounded_hold&&landing_gap_seconds_>settings_.landing_gap_seconds+1e-9)) {
                    event.canceled=cancel_live(TrickFailure::unstable_landing);
                    event.failure=TrickFailure::unstable_landing;
                }else sync();
                return event;
            }
            if(!input.landing_quality_available||!valid_quality(input)) {
                event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
            }
            if(!clean_quality(input))return land(input,TrickFailure::unstable_landing);
            if(landing_failure(input)!=TrickFailure::none)return land(input);
            // The interval ending at first recontact may still be airborne.
            // Begin strict settling at zero after ANY unsupported pending sample.
            const bool recontact=landing_gap_seconds_>0;landing_gap_seconds_=0;
            if(!recontact)landing_seconds_+=seconds;
            if(landing_seconds_+1e-9>=settings_.landing_settle_seconds)return land(input);
            sync();return event;
        }
        if(!state_.airborne) {
            if(input.grounded){ground_seen_=true;previous_yaw_=input.body_yaw;full_orientation_=input.orientation_available;previous_quaternion_=input.body_quaternion;return event;}
            if(!ground_seen_)return event;
            state_.airborne=true;state_.air_seconds=0;state_.failure=TrickFailure::none;
            bar_={};tail_={};movement_={};spin_attempted_=false;previous_yaw_=input.body_yaw;
            flip_attempted_=roll_attempted_=false;full_orientation_=input.orientation_available;previous_quaternion_=input.body_quaternion;
            event.took_off=true;
            // Ignore requests held through takeoff. Each channel must first be
            // neutral in the air before a distinct air gesture can start it.
            bar_.armed=input.bar_neutral&&input.barspin==TrickDirection::none;
            tail_.armed=input.rear_neutral&&input.tailwhip==TrickDirection::none;
            movement_.armed=input.rear_neutral&&input.movement==catalog::TrickId::none;
            sync();return event;
        }
        std::array<double,3> change{};
        if(!orientation_change(seconds,input,change)) {
            event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
        }
        previous_yaw_=input.body_yaw;
        state_.spin_radians+=change[1];state_.flip_radians+=change[0];state_.roll_radians+=change[2];
        spin_attempted_=spin_attempted_||std::abs(state_.spin_radians)>=std::numbers::pi/2;
        flip_attempted_=flip_attempted_||std::abs(state_.flip_radians)>=std::numbers::pi/2;
        roll_attempted_=roll_attempted_||std::abs(state_.roll_radians)>=std::numbers::pi/2;
        // Landing is evaluated before advancing animation/catch. Time observed
        // after contact cannot finish an under-rotation or catch a late frame.
        if(input.grounded) {
            if(landing_failure(input)!=TrickFailure::none||!input.landing_quality_available)return land(input);
            if(!valid_quality(input)) {
                event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
            }
            if(!clean_quality(input))return land(input,TrickFailure::unstable_landing);
            // This sample's elapsed interval may precede contact. Begin at zero
            // and count only following consecutive observed support intervals.
            landing_pending_=true;landing_seconds_=landing_gap_seconds_=landing_elapsed_=0;
            landing_hold_started_=landing_holding_=false;landing_hold_start_=0;sync();return event;
        }
        state_.air_seconds+=seconds;
        if(state_.air_seconds>settings_.max_air_seconds) {
            event.canceled=cancel_live(TrickFailure::invalid_sample);event.failure=TrickFailure::invalid_sample;return event;
        }
        // Existing bar/whip channels can still combine. Catalog movements own their
        // limbs while active; competing pulses are consumed rather than queued.
        const bool movement_busy=movement_.active;
        event.bar_caught=channel(bar_,input.barspin,input.bar_neutral,seconds,settings_.bar_seconds,!movement_busy);
        event.tail_caught=channel(tail_,input.tailwhip,input.rear_neutral,seconds,settings_.tail_seconds,!movement_busy);
        event.movement_caught=movement_channel(input.movement,input.rear_neutral,seconds);
        if(movement_.failed&&state_.failure==TrickFailure::none)state_.failure=TrickFailure::uncaught;
        if((bar_.failed||tail_.failed)&&state_.failure==TrickFailure::none)state_.failure=TrickFailure::uncaught;
        sync();return event;
    }
};
} // namespace bmx::detail::source
