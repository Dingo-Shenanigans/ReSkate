#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include "Engine/Game/BMX/equipment_switch.h"
#include "bmx_impact_receipt.h"

namespace dingosdk::bmx::crash_recovery {
using Vec=std::array<float,3>;
struct Identity {
    std::uintptr_t client{},entity{},core{},context{},rig{},world{};
    std::uint64_t body_token{};
    std::uint32_t body_index=std::numeric_limits<std::uint32_t>::max(),body_generation{};
    bool operator==(const Identity&) const noexcept = default;
};
struct Input {
    Identity expected{},current{}; // expected is pinned when control is released
    bool owner_valid{},body_valid{},native_standable{},native_offboard{};
    double native_started{},native_now{};
    std::uint64_t crash_serial{},current_serial{};
    Vec bike_root{},rider_ground{},linear_velocity{};
};
enum class Issue {
    ready,owner_unavailable,body_unavailable,invalid_identity,owner_changed,
    invalid_native_clock,settling,not_standable,not_offboard,invalid_geometry,
    moving,too_far_horizontal,too_far_vertical
};
struct Decision {
    bool eligible{};
    Issue issue{Issue::owner_unavailable};
    double elapsed{},speed{},horizontal_distance{},vertical_distance{};
};
inline bool valid(const Identity& value) noexcept {
    return value.client&&value.entity&&value.core&&value.context&&value.rig&&value.world&&
        value.body_token&&value.body_index!=std::numeric_limits<std::uint32_t>::max();
}
inline bool finite(Vec value) noexcept {
    for(float component:value)if(!std::isfinite(component))return false;
    return true;
}
// Eligibility only: no automatic recovery, input generation or native access.
// Call only for an explicit recovery request after fresh native ownership/body
// validation. rider_ground must already remove the native rider-root height;
// comparing the elevated character root would mismeasure vertical proximity.
// This decision neither proves a clear set-down site nor authorizes a teleport.
inline Decision evaluate(const Input& input) noexcept {
    Decision out;
    if(!input.owner_valid)return out;
    if(!input.body_valid){out.issue=Issue::body_unavailable;return out;}
    if(!valid(input.expected)||!valid(input.current)){out.issue=Issue::invalid_identity;return out;}
    if(input.expected!=input.current){out.issue=Issue::owner_changed;return out;}
    if(!std::isfinite(input.native_started)||!std::isfinite(input.native_now)||
       input.native_started<0||input.native_now<input.native_started||
       !input.crash_serial||input.current_serial<=input.crash_serial) {
        out.issue=Issue::invalid_native_clock;return out;
    }
    out.elapsed=input.native_now-input.native_started;
    if(!std::isfinite(out.elapsed)){out.issue=Issue::invalid_native_clock;return out;}
    if(out.elapsed<.6){out.issue=Issue::settling;return out;}
    if(!input.native_standable){out.issue=Issue::not_standable;return out;}
    if(!input.native_offboard){out.issue=Issue::not_offboard;return out;}
    if(!finite(input.bike_root)||!finite(input.rider_ground)||!finite(input.linear_velocity)) {
        out.issue=Issue::invalid_geometry;return out;
    }
    const double dx=double(input.rider_ground[0])-input.bike_root[0];
    const double dy=double(input.rider_ground[1])-input.bike_root[1];
    const double dz=double(input.rider_ground[2])-input.bike_root[2];
    out.speed=std::hypot(double(input.linear_velocity[0]),double(input.linear_velocity[1]),double(input.linear_velocity[2]));
    out.horizontal_distance=std::hypot(dx,dz);out.vertical_distance=std::abs(dy);
    if(out.speed>1){out.issue=Issue::moving;return out;}
    if(out.horizontal_distance>1.8){out.issue=Issue::too_far_horizontal;return out;}
    if(out.vertical_distance>1.5){out.issue=Issue::too_far_vertical;return out;}
    out.eligible=true;out.issue=Issue::ready;return out;
}

// SDK-only interaction admission after a released rider's actual native fall.
// A transient pre-ragdoll FollowGround frame is not recovered standing. The
// receipt identifies this crash; the two standing epochs are current evidence.
struct InteractionSample {
    RetainedBikeIdentity identity;
    bool coherent{},offboard{},standable{}; // standable is guarded native FollowGround
    std::uint64_t serial{};
    double native_time{};
    float seconds{};
};
class InteractionAdmission {
    RetainedBikeIdentity identity_{};
    impact_receipt::Receipt receipt_{};
    bool custody_recovered_{},seeded_{},ready_{};
    std::uint64_t released_serial_{},last_serial_{};
    double released_time_{},last_time_{};
    float last_seconds_{};
    static bool clock_valid(const InteractionSample& sample) noexcept {
        return sample.serial&&std::isfinite(sample.native_time)&&sample.native_time>=0&&
            std::isfinite(sample.seconds)&&sample.seconds>0&&sample.seconds<=.1f;
    }
public:
    void reset() noexcept {*this={};}
    void withdraw() noexcept {seeded_=ready_=false;last_serial_=0;last_time_=0;last_seconds_=0;}
    void begin(const RetainedBikeIdentity& identity,const impact_receipt::Receipt& receipt,
               std::uint64_t serial,double native_time) noexcept {
        reset();identity_=identity;receipt_=receipt;released_serial_=serial;released_time_=native_time;
    }
    // Only the successful released-owner custody receipt may establish this
    // alternative provenance. Historical fall/request identities stay unchanged.
    void adopt_recovered(const RetainedBikeIdentity& identity,std::uint64_t serial,double native_time) noexcept {
        begin(identity,{},serial,native_time);custody_recovered_=true;
    }
    static bool completed_fall(const impact_receipt::Receipt& receipt,const bail::Status& fall) noexcept {
        if(!impact_receipt::matches(receipt,fall)||!fall.consumed||!fall.ragdoll_observed||
           fall.pending||fall.expired||fall.protected_owner)return false;
        // consumed/ragdoll are reset for each matched request. native_calls is
        // a lifetime total retained across requests, not a one-call receipt.
        if(!fall.activation_enabled)return !receipt.intent_armed;
        return receipt.intent_armed&&fall.intent_prior_false&&fall.intent_written&&fall.intent_true_returned&&
            fall.intent_true_readback&&fall.intent_reset&&!fall.intent_reset_pending&&fall.intent_reset_readback&&
            fall.intent_true_calls==1&&fall.intent_true_serial&&fall.intent_reset_serial>fall.intent_true_serial&&
            ((fall.intent_reset_calls==1&&fall.intent_native_calls==2)||
             (fall.intent_native_cleared&&fall.intent_reset_calls==0&&fall.intent_native_calls==1));
    }
    bool observe(const InteractionSample& sample,const bail::Status& fall) noexcept {
        if(!retained_bike_valid(identity_)||sample.identity!=identity_||!sample.coherent||
           !clock_valid(sample)||!released_serial_||!std::isfinite(released_time_)||released_time_<0||
           sample.serial<=released_serial_||sample.native_time<=released_time_||
           !(custody_recovered_||completed_fall(receipt_,fall))||!sample.offboard||!sample.standable) {
            withdraw();return false;
        }
        if(seeded_&&sample.serial==last_serial_&&sample.native_time==last_time_&&sample.seconds==last_seconds_)
            return ready_; // fresh revalidation of an already-qualified epoch gives no additional credit
        const bool consecutive=seeded_&&sample.serial>last_serial_&&sample.serial-last_serial_==1&&
            sample.native_time>last_time_&&std::abs(sample.native_time-last_time_-double(sample.seconds))<=1e-5&&
            std::abs(sample.seconds-last_seconds_)<=1e-6f;
        // A missing, reversed, paused or discontinuous step cannot contribute
        // old standing credit. This current good frame can seed a new suffix.
        ready_=consecutive;seeded_=true;last_serial_=sample.serial;
        last_time_=sample.native_time;last_seconds_=sample.seconds;return ready_;
    }
};
} // namespace dingosdk::bmx::crash_recovery
