#pragma once
#include "equipment_switch.h"
#include "bmx_mount_pose.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx {
enum class MountOwnerResult { waiting, ready, timed_out, owner_changed };
// A pending request may wait for native dismount/collision initialization, but
// it never transfers to a replacement rider or extends its original deadline.
class PendingMountOwner {
    EquipmentOwner owner_{};
public:
    void reset() noexcept { owner_ = {}; }
    MountOwnerResult update(std::uint64_t now, std::uint64_t deadline,
                           bool available, const EquipmentOwner& owner) noexcept {
        if (now >= deadline) return MountOwnerResult::timed_out;
        if (!available || !owner.client || !owner.entity || !owner.core || !owner.context || !owner.rig)
            return MountOwnerResult::waiting;
        if (owner_.entity && owner_ != owner) return MountOwnerResult::owner_changed;
        owner_ = owner;
        return MountOwnerResult::ready;
    }
};
enum class MountDismountResult { waiting, ready, cancelled };
struct MountDismountSample {
    std::uint64_t now{},deadline{},input_sampled_ms{},serial{};
    double native_time{};
    EquipmentOwner owner;
    bool available{},timing_available{},offboard{},standable{},motion_supported{},physics_available{},quiet{};
    std::uint32_t physics{};
};
// A native-board request can briefly report FollowGround before its native
// dismount has settled. This pending-only policy never acquires or drives it.
// Already-on-foot entry keeps its existing path. The250ms sampled dwell is a
// conservative admission policy, not a measured native animation duration.
class PendingMountDismount {
    EquipmentOwner owner_{};
    bool required_{},clock_seen_{},wall_seen_{},stable_{},cancelled_{};
    std::uint64_t last_wall_{},last_serial_{},stable_wall_{};
    double last_native_{},stable_native_{};
public:
    inline static constexpr std::uint64_t settle_ms=250,maximum_wall_gap_ms=100;
    inline static constexpr double settle_seconds=.25,maximum_native_gap=.1;
    void reset() noexcept {*this={};}
    void interrupt() noexcept {stable_=false;}
    bool required() const noexcept {return required_;}
    MountDismountResult update(const MountDismountSample& s) noexcept {
        if(cancelled_||s.now>=s.deadline){cancelled_=true;return MountDismountResult::cancelled;}
        if(!required_) {
            if(!s.available)return MountDismountResult::waiting;
            if(s.offboard)return MountDismountResult::ready;
            if(!retained_owner_valid(s.owner))return MountDismountResult::waiting;
            required_=true;owner_=s.owner;
        }
        if((s.available&&s.owner!=owner_)||(wall_seen_&&s.now<last_wall_)) {
            cancelled_=true;interrupt();return MountDismountResult::cancelled;
        }
        const auto wall_gap=wall_seen_?s.now-last_wall_:0;
        wall_seen_=true;last_wall_=s.now;
        if(!s.available||!s.timing_available||!s.serial||!std::isfinite(s.native_time)||s.native_time<0) {
            interrupt();return MountDismountResult::waiting;
        }
        if(clock_seen_&&(s.serial<last_serial_||s.native_time<last_native_||
            (s.serial==last_serial_)!=(s.native_time==last_native_))) {
            cancelled_=true;interrupt();return MountDismountResult::cancelled;
        }
        const bool advanced=!clock_seen_||s.serial>last_serial_;
        if(clock_seen_&&(wall_gap>maximum_wall_gap_ms||s.serial-last_serial_>12||
            s.native_time-last_native_>maximum_native_gap))interrupt();
        clock_seen_=true;last_serial_=s.serial;last_native_=s.native_time;
        const bool input_fresh=s.input_sampled_ms&&s.input_sampled_ms<=s.now&&s.now-s.input_sampled_ms<=250;
        if(!s.offboard||!s.standable||!s.motion_supported||!s.physics_available||s.physics!=504||!s.quiet||!input_fresh) {
            interrupt();return MountDismountResult::waiting;
        }
        if(!advanced)return MountDismountResult::waiting;
        if(!stable_) {stable_=true;stable_wall_=s.now;stable_native_=s.native_time;return MountDismountResult::waiting;}
        return s.now-stable_wall_>=settle_ms&&s.native_time-stable_native_>=settle_seconds?
            MountDismountResult::ready:MountDismountResult::waiting;
    }
};

enum class MountGroundResult { query_unavailable, unsupported, invalid_height, ready };
inline MountGroundResult mount_ground_readiness(bool available, bool supported,
                                                double rider_y, double ground_y) noexcept {
    if (!available) return MountGroundResult::query_unavailable;
    if (!supported) return MountGroundResult::unsupported;
    if (!std::isfinite(rider_y) || !std::isfinite(ground_y) || std::abs(rider_y - ground_y) > 1.5)
        return MountGroundResult::invalid_height;
    return MountGroundResult::ready;
}

enum class NearbyMountDecision { query, waiting, exhausted, cancelled };
struct MountSearchSupport {std::uintptr_t world{},context{};};
// Admission search only. Every candidate still requires the full independent
// prepare/final witness protocol; this policy never retains a support witness.
// Radius order chooses the closest sampled point, not a globally closest floor.
class NearbyMountSearch {
    EquipmentOwner owner_{};
    RetainedBikeIdentity retained_identity_{};
    std::uintptr_t retained_context_{};
    MountSearchSupport support_;
    std::uint64_t token_{},serial_{};
    double native_time_{};
    std::array<float,3> origin_{};
    float yaw_{};
    unsigned index_{};
    bool active_{},clock_ready_{},attempt_open_{};
public:
    inline static constexpr float maximum_radius=.4f;
    inline static constexpr std::array<std::array<float,2>,17> offsets{{
        {0,0},{.2f,0},{-.2f,0},{0,.2f},{0,-.2f},
        {.1414213562f,.1414213562f},{-.1414213562f,.1414213562f},
        {.1414213562f,-.1414213562f},{-.1414213562f,-.1414213562f},
        {.4f,0},{-.4f,0},{0,.4f},{0,-.4f},
        {.2828427125f,.2828427125f},{-.2828427125f,.2828427125f},
        {.2828427125f,-.2828427125f},{-.2828427125f,-.2828427125f}}}; // local left/forward
    void cancel() noexcept {*this={};}
    bool active() const noexcept {return active_;}
    bool exhausted() const noexcept {return active_&&index_>=offsets.size();}
    unsigned index() const noexcept {return index_;}
    std::uintptr_t world() const noexcept {return support_.world;}
    MountSearchSupport support() const noexcept {return support_;}
    float yaw() const noexcept {return yaw_;}
    bool begin(EquipmentOwner owner,std::uint64_t token,std::array<float,3> origin,float yaw) noexcept {
        cancel();if(!retained_owner_valid(owner)||!std::isfinite(yaw))return false;
        for(float value:origin)if(!std::isfinite(value)||std::abs(value)>999990)return false;
        active_=true;owner_=owner;token_=token;origin_=origin;yaw_=yaw;return true;
    }
    // A retained request pins the complete existing pool identity and TLS
    // context. These SDK keys never replace the fresh native owner/body and
    // actual two-pass support proofs at acquisition.
    bool begin_retained(RetainedBikeIdentity identity,std::uintptr_t context,
                        std::array<float,3> origin,float yaw) noexcept {
        cancel();if(!retained_bike_valid(identity)||!context)return false;
        if(!begin(identity.owner,identity.token,origin,yaw)||!bind_support({identity.world,context}))return false;
        retained_identity_=identity;retained_context_=context;return true;
    }
    NearbyMountDecision next_retained(RetainedBikeIdentity identity,std::uintptr_t context,
        EquipmentOwner canonical_owner,std::uint64_t serial,double native_time) noexcept {
        if(!retained_bike_valid(retained_identity_)||identity!=retained_identity_||
           context!=retained_context_||canonical_owner!=retained_identity_.owner) {
            cancel();return NearbyMountDecision::cancelled;
        }
        return next(canonical_owner,identity.token,serial,native_time);
    }
    // First actual support world pins the entire request; zero/unavailable
    // queries never authorize a world or consume another candidate.
    bool bind_support(MountSearchSupport observed) noexcept {
        if(!active_||!observed.world||!observed.context||(support_.world&&
           (support_.world!=observed.world||support_.context!=observed.context))){cancel();return false;}
        support_=observed;return true;
    }
    NearbyMountDecision next(EquipmentOwner owner,std::uint64_t token,
                             std::uint64_t serial,double native_time) noexcept {
        if(!active_)return NearbyMountDecision::cancelled;
        if(owner!=owner_||token!=token_||!serial||!std::isfinite(native_time)||native_time<0) {
            cancel();return NearbyMountDecision::cancelled;
        }
        if(exhausted())return NearbyMountDecision::exhausted;
        if(clock_ready_) {
            if(serial==serial_&&native_time==native_time_)return NearbyMountDecision::waiting;
            if(serial<=serial_||native_time<=native_time_||serial-serial_>12||native_time-native_time_>.1) {
                cancel();return NearbyMountDecision::cancelled;
            }
        }
        serial_=serial;native_time_=native_time;clock_ready_=true;attempt_open_=true;
        return NearbyMountDecision::query;
    }
    std::array<float,3> candidate() const noexcept {
        if(!active_||index_>=offsets.size())return {};
        const auto offset=offsets[index_];auto point=origin_;
        point[0]+=offset[0]*std::cos(yaw_)+offset[1]*std::sin(yaw_);
        point[2]+=-offset[0]*std::sin(yaw_)+offset[1]*std::cos(yaw_);return point;
    }
    // Only a measured geometric refusal consumes a point. Unavailable queries
    // or a raced final clock retry the same point on a later native epoch.
    void finish_attempt(bool measured_rejection) noexcept {
        if(!active_||!attempt_open_)return;
        attempt_open_=false;if(measured_rejection)++index_;
    }
};

// Eligibility for SDK-only cancellation of a not-yet-acquired retained mount.
// This is deliberately not a native owner/body proof or permission to publish
// cached presentation, traverse handles, reseed, activate, park or spend tokens.
struct RetainedMountCancellation {
    RetainedBikeIdentity identity;
    EquipmentOwner parked_owner;
    std::uintptr_t world{},context{};
    std::uint64_t token{};
    bool native_chassis{},parked{},from_parked{},active{},pending{};
    bool carried{},crashed{},chassis_started{},rehearsal{},private_variant{};
    bool stepping_off{},step_off_board{},board_switch{},pool_parked{};
};
inline bool retained_mount_cancellation_allowed(const RetainedMountCancellation& s) noexcept {
    return s.native_chassis&&s.parked&&s.from_parked&&(s.active||s.pending)&&s.pool_parked&&
        !s.carried&&!s.crashed&&!s.chassis_started&&!s.rehearsal&&!s.private_variant&&
        !s.stepping_off&&!s.step_off_board&&!s.board_switch&&retained_bike_valid(s.identity)&&
        s.identity.owner==s.parked_owner&&s.identity.world==s.world&&s.context&&s.identity.token==s.token;
}

enum class CarryPathObservation { clear,measured_obstacle,unknown };
inline constexpr bool setdown_search_permitted(CarryPathObservation path,bool explicit_request) noexcept {
    return path!=CarryPathObservation::unknown&&(path==CarryPathObservation::measured_obstacle||explicit_request);
}
struct SetDownWitness {
    RetainedBikeIdentity identity;
    std::uintptr_t context{};
    std::uint64_t serial{};
    double native_time{};
    bool coherent_parked_body{},standing_offboard{},approach_available{},approach_clear{};
};
// Carries an explicit set-down intent, never a support witness. It reuses the
// existing17-point radius/epoch policy; only full fresh final witnesses select.
class SetDownSearch {
    NearbyMountSearch search_;
    RetainedBikeIdentity identity_{};
    std::uintptr_t context_{};
    std::uint64_t serial_{};
    double time_{};
    bool query_open_{};
public:
    void cancel() noexcept {*this={};}
    bool active() const noexcept {return search_.active();}
    bool exhausted() const noexcept {return search_.exhausted();}
    unsigned index() const noexcept {return search_.index();}
    auto candidate() const noexcept {return search_.candidate();}
    float yaw() const noexcept {return search_.yaw();}
    MountSearchSupport support() const noexcept {return {identity_.world,context_};}
    bool begin(RetainedBikeIdentity identity,std::uintptr_t context,std::array<float,3> root,float yaw) noexcept {
        cancel();if(!retained_bike_valid(identity)||!context)return false;
        if(!search_.begin(identity.owner,identity.token,root,yaw)||!search_.bind_support({identity.world,context}))return false;
        identity_=identity;context_=context;return true;
    }
    NearbyMountDecision next(RetainedBikeIdentity current,std::uintptr_t context,std::uint64_t serial,double time) noexcept {
        query_open_=false;
        if(current!=identity_||context!=context_){cancel();return NearbyMountDecision::cancelled;}
        const auto decision=search_.next(current.owner,current.token,serial,time);
        if(decision==NearbyMountDecision::query){query_open_=true;serial_=serial;time_=time;}
        return decision;
    }
    void finish_attempt(bool measured_rejection) noexcept {query_open_=false;search_.finish_attempt(measured_rejection);}
    bool ready(const mount_pose::Seed& seed,const SetDownWitness& witness) const noexcept {
        return active()&&query_open_&&witness.identity==identity_&&witness.context==context_&&
            witness.serial==serial_&&witness.native_time==time_&&witness.coherent_parked_body&&
            witness.standing_offboard&&witness.approach_available&&witness.approach_clear&&
            mount_pose::matches(seed,identity_.world,context_,serial_,time_);
    }
};
} // namespace dingosdk::bmx
