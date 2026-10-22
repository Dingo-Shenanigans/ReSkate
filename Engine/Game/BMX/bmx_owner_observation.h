#pragma once
#include "bmx_pool_admission.h"
#include "bmx_interaction.h"
#include "equipment_switch.h"
#include <atomic>
#include <limits>
#include <cmath>

namespace dingosdk::bmx::owner_observation {
using Owner = pool_admission::Owner;
using WorldKey = pool_admission::WorldKey;
enum class Result { unavailable, matched, changed };
struct Observation {
    bool rider_available{}, world_available{};
    Owner owner;
    WorldKey world;
};
struct RetainedState {
    bool ready{},active{},parked{},detached{},failed{},blocked{},producer_failed{};
};
inline bool may_suspend(const RetainedState& pool) noexcept {
    return pool.ready&&!pool.active&&(pool.parked!=pool.detached)&&
        !pool.failed&&!pool.blocked&&!pool.producer_failed;
}
// SDK-only cancellation shared by first unknown, subsequent waiting ticks and
// the first fresh quiescent tick. Edge machines require a later neutral sample.
inline int reset_interactions(std::atomic<int>& request, BikeInteractionButton& north,
    EquipmentToggleKey& toggle, EquipmentToggleKey& interact, EquipmentToggleKey& carry,
    BikeQuickSwitchButton* quick=nullptr) noexcept {
    const int discarded=request.exchange(0);
    north.reset();if(quick)quick->reset();
    (void)toggle.update(false,false);(void)interact.update(false,false);(void)carry.update(false,false);
    return discarded;
}
// Read-only policy. matched is only an identity/world proof and never a body,
// shape, generation, configuration or actuation authorization.
inline Result evaluate(const Owner& expected_owner, const WorldKey& expected_world,
                       const Observation& current) noexcept {
    if (!expected_owner.valid() || !expected_world.valid() ||
        !current.rider_available || !current.owner.valid()) return Result::unavailable;
    if (current.owner != expected_owner) return Result::changed;
    if (!current.world_available || !current.world.valid()) return Result::unavailable;
    return current.world == expected_world ? Result::matched : Result::changed;
}

// SDK association only. A witness is supplied by the runtime's exact idle,
// retained user bike; a focus-paused or partially acquiring ride is excluded.
struct ReleasedKey {
    Owner owner;
    WorldKey world;
    std::uint32_t body_index=0xffffffffU,body_generation{};
    std::uint64_t token{};
    bool operator==(const ReleasedKey&) const=default;
};
struct ReleaseWitness {
    ReleasedKey key;
    bool available{},parked{},detached{};
};
struct RuntimeRelease {
    bool bike_parked{},bike_crashed{},active{},pending{},chassis_started{},carried{},
        stepping_off{},step_off_intent{},board_switch{},rehearsal{},mounting{};
};
inline bool runtime_released(const RuntimeRelease& r) noexcept {
    return (r.bike_parked!=r.bike_crashed)&&!r.active&&!r.pending&&!r.chassis_started&&
        !r.carried&&!r.stepping_off&&!r.step_off_intent&&!r.board_switch&&!r.rehearsal&&!r.mounting;
}
inline bool same_actor(const Owner& a,const Owner& b) noexcept {
    return a.valid()&&b.valid()&&a.client==b.client&&a.entity==b.entity&&a.core==b.core;
}
inline bool witness_matches(const ReleaseWitness& w,const ReleasedKey& key,
    bool parked,bool detached) noexcept {
    return w.available&&key.owner.valid()&&key.world.valid()&&key.body_index!=0xffffffffU&&
        key.token&&w.key==key&&(parked!=detached)&&w.parked==parked&&w.detached==detached;
}
enum class Recreation { unknown, unchanged, candidate, terminal };
inline Recreation classify_recreation(const Owner& retained,const WorldKey& world,
    const Observation& fresh) noexcept {
    if(!retained.valid()||!world.valid()||!fresh.rider_available||!fresh.owner.valid())return Recreation::unknown;
    if(!same_actor(retained,fresh.owner))return Recreation::terminal;
    if(!fresh.world_available||!fresh.world.valid())return Recreation::unknown;
    if(fresh.world!=world)return Recreation::terminal;
    if(fresh.owner==retained)return Recreation::unchanged;
    // This deliberately covers the measured full context+rig recreation only.
    if(fresh.owner.context==retained.context||fresh.owner.rig==retained.rig)return Recreation::terminal;
    return Recreation::candidate;
}
class RecreationSuffix {
    Owner candidate_;
    std::uint64_t serial_{};
    double time_{};
public:
    void reset() noexcept {*this={};}
    bool observe(const Owner& candidate,std::uint64_t serial,double time) noexcept {
        if(!candidate.valid()||!serial||!std::isfinite(time)||time<0){reset();return false;}
        if(candidate!=candidate_){candidate_=candidate;serial_=serial;time_=time;return false;}
        if(serial<=serial_||time<=time_){reset();return false;}
        // The caller repeats complete current-world custody on each update.
        return true;
    }
};
// Copied motion leases must be rejected after revocation even when a callback
// was descheduled between copying a lease and acquiring SourceState::busy.
class MotionEpoch {
    std::uint64_t value_=1;
    bool failed_{};
public:
    explicit MotionEpoch(std::uint64_t initial=1) noexcept:value_(initial),failed_(!initial){}
    std::uint64_t value() const noexcept {return value_;}
    bool valid(std::uint64_t lease) const noexcept {return !failed_&&lease&&lease==value_;}
    bool revoke() noexcept {
        if(failed_||value_==std::numeric_limits<std::uint64_t>::max()){failed_=true;return false;}
        ++value_;return true;
    }
};
// An owner service can stop inside a nested crashed-bike tick after clearing
// its suspension flag. The outer action loop must still discard local input.
class ReleasedUnwind {
    bool stopped_{};
public:
    void begin_tick() noexcept {stopped_=false;}
    void stop() noexcept {stopped_=true;}
    bool stopped() const noexcept {return stopped_;}
};
inline bool custody_gameplay(bool ready,bool ui_available,bool in_menu) noexcept {
    return ready&&ui_available&&!in_menu;
}
// Custody consumes one token; its promised explicit real mount needs one more.
inline bool custody_token_headroom(std::uint64_t token) noexcept {
    return token&&token<=std::numeric_limits<std::uint64_t>::max()-2;
}
struct CustodyReadiness {
    bool timing{},offboard{},standable{};
    std::uint64_t serial{};
    double time{};
    float step{};
};
inline bool custody_ready(const CustodyReadiness& r) noexcept {
    return r.timing&&r.offboard&&r.standable&&r.serial&&std::isfinite(r.time)&&r.time>=0&&
        std::isfinite(r.step)&&std::abs(static_cast<double>(r.step)-1.0/60.0)<=1e-7;
}
inline bool custody_readiness_matches(const CustodyReadiness& first,const CustodyReadiness& final) noexcept {
    return custody_ready(first)&&custody_ready(final)&&first.serial==final.serial&&
        first.time==final.time&&first.step==final.step;
}
template<class T> struct PreparedAssociation {T owner,receipt;};
template<class T> PreparedAssociation<T> prepare_association(const T& value) {
    return {value,value}; // copies may throw; caller commits only after this returns
}

} // namespace dingosdk::bmx::owner_observation
