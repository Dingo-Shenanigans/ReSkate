#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx {
// The supported native selector accepts its ordinary offboard request from
// neutral skating (100) and grounded ollie crouch (103). Air/pop/mount states
// keep their own native lifecycle and are never coerced into a dismount.
inline constexpr bool natural_dismount_state(std::uint32_t state) noexcept {
    return state == 100 || state == 103;
}
inline constexpr bool natural_dismount_selection(std::uint32_t current,
                                                std::uint32_t committed) noexcept {
    return current == committed && natural_dismount_state(current);
}
// A held key after startup, focus loss or a menu never switches equipment.
class EquipmentToggleKey {
    bool armed_{};
public:
    bool update(bool down, bool allowed) noexcept {
        if (!allowed) { armed_ = false; return false; }
        if (!down) { armed_ = true; return false; }
        const bool result = armed_;
        armed_ = false;
        return result;
    }
};
struct EquipmentOwner {
    std::uintptr_t client{}, entity{}, core{}, context{}, rig{};
    bool operator==(const EquipmentOwner&) const = default;
};
struct RetainedBikeIdentity {
    EquipmentOwner owner;
    std::uintptr_t world{}, native_state{};
    std::uint32_t body_index = 0xffffffffU, body_generation{};
    std::uint64_t token{};
    bool operator==(const RetainedBikeIdentity&) const = default;
};
enum class CrashedSwitchDecision { allowed, preserve_wait, unavailable, invalidated };
struct CrashedSwitchInput {
    RetainedBikeIdentity expected, current;
    // The caller supplies a freshly resolved canonical rider and a coherent,
    // owned native body observation. These flags never authorize native reads.
    bool rider_available{}, body_available{}, coherent_snapshot{};
    bool native_offboard{}, native_standable{};
};
inline bool retained_owner_valid(const EquipmentOwner& owner) noexcept {
    return owner.client && owner.entity && owner.core && owner.context && owner.rig;
}
inline bool retained_bike_valid(const RetainedBikeIdentity& identity) noexcept {
    return retained_owner_valid(identity.owner) && identity.world && identity.native_state &&
        identity.body_index < 1048576U && identity.token;
}
// Preflight an explicit equipment request BEFORE parking or hiding a crashed
// bike. A known owner/body change invalidates the retained lifetime; a missing
// observation preserves it only under the caller's existing bounded expiry.
// Readiness refusal retains no intent, starts no timer and consumes no controls.
inline CrashedSwitchDecision crashed_switch_preflight(const CrashedSwitchInput& input) noexcept {
    if (!retained_bike_valid(input.expected)) return CrashedSwitchDecision::invalidated;
    if (!input.rider_available || !retained_owner_valid(input.current.owner))
        return CrashedSwitchDecision::unavailable;
    if (input.expected.owner != input.current.owner) return CrashedSwitchDecision::invalidated;
    if (!input.body_available || !input.coherent_snapshot) return CrashedSwitchDecision::unavailable;
    if (!retained_bike_valid(input.current) || input.expected != input.current)
        return CrashedSwitchDecision::invalidated;
    if (!input.native_offboard || !input.native_standable) return CrashedSwitchDecision::preserve_wait;
    return CrashedSwitchDecision::allowed;
}
enum class StepOffBoardResult { idle, waiting, ready, cancelled, timed_out };
struct StepOffBoardSample {
    bool allowed{}, available{}, timing_available{}, parked_proven{}, stepping_off{};
    bool offboard{}, standable{}, motion_released{};
    EquipmentOwner owner;
    std::uintptr_t world{};
    std::uint64_t token{}, serial{};
    double native_time{};
    std::array<float,3> root{};
};
// A normal mounted F6 first uses the existing step-off. No board intent is
// emitted until a later native observation proves the rider reached its end.
// Current parked ownership proof is supplied by the native adapter each tick.
class StepOffBoardIntent {
    bool active_{};
    std::uint64_t started_{}, deadline_{}, completed_serial_{};
    double completed_time_{};
    EquipmentOwner owner_{};
    std::uintptr_t world_{};
    std::uint64_t token_{};
    std::array<float,3> endpoint_{};
public:
    inline static constexpr float endpoint_allowance=.15f;
    bool begin(std::uint64_t now, EquipmentOwner owner, std::uintptr_t world,
               std::uint64_t token, const std::array<float,3>& endpoint) noexcept {
        if(active_||!retained_owner_valid(owner)||!world||!token||now>UINT64_MAX-15000)return false;
        for(float value:endpoint)if(!std::isfinite(value))return false;
        active_=true;started_=now;deadline_=now+15000;owner_=owner;
        world_=world;token_=token;endpoint_=endpoint;return true;
    }
    void cancel() noexcept {*this={};}
    bool active() const noexcept {return active_;}
    EquipmentOwner owner() const noexcept {return owner_;}
    std::uint64_t pending_deadline_ms() const noexcept {return active_?deadline_:0;}
    StepOffBoardResult update(std::uint64_t now,const StepOffBoardSample& sample) noexcept {
        if(!active_)return StepOffBoardResult::idle;
        if(now<started_||now>=deadline_) {cancel();return StepOffBoardResult::timed_out;}
        if(!sample.allowed) {cancel();return StepOffBoardResult::cancelled;}
        if(!sample.available)return StepOffBoardResult::waiting;
        if(sample.owner!=owner_||sample.world!=world_||sample.token!=token_||!sample.parked_proven) {
            cancel();return StepOffBoardResult::cancelled;
        }
        if(sample.stepping_off||!sample.timing_available||!sample.serial||!std::isfinite(sample.native_time))
            return StepOffBoardResult::waiting;
        if(!completed_serial_) {
            completed_serial_=sample.serial;completed_time_=sample.native_time;
            return StepOffBoardResult::waiting;
        }
        if(sample.serial<completed_serial_||sample.native_time<completed_time_) {
            cancel();return StepOffBoardResult::cancelled;
        }
        if(sample.serial==completed_serial_||sample.native_time==completed_time_||
           !sample.offboard||!sample.standable||!sample.motion_released)return StepOffBoardResult::waiting;
        float distance2{};
        for(unsigned i=0;i<3;++i) {
            if(!std::isfinite(sample.root[i]))return StepOffBoardResult::waiting;
            const float delta=sample.root[i]-endpoint_[i];distance2+=delta*delta;
        }
        if(!std::isfinite(distance2)||distance2>endpoint_allowance*endpoint_allowance)return StepOffBoardResult::waiting;
        cancel();return StepOffBoardResult::ready;
    }
};
enum class BoardSwitchResult { idle, blocked, waiting, request_mount, complete, timed_out, owner_changed };
// The game owns the animation and physics transition. Only an observed grounded
// board state completes this bounded request; intermediate mounting states wait.
class SkateboardSwitch {
    bool active_{};
    std::uint64_t deadline_{}, next_request_{};
    EquipmentOwner owner_{};
public:
    void request(std::uint64_t now, EquipmentOwner expected_owner = {}) noexcept {
        active_ = true; deadline_ = now + 15000; next_request_ = 0; owner_ = expected_owner;
    }
    void cancel() noexcept { active_ = false; owner_ = {}; }
    bool active() const noexcept { return active_; }
    // Passive diagnostic identity/deadline observation; never updates or requests.
    bool pending_for(std::uint64_t now,const EquipmentOwner& owner) const noexcept {
        return active_&&retained_owner_valid(owner_)&&owner_==owner&&deadline_>=15000&&
            now>=deadline_-15000&&now<deadline_;
    }
    std::uint64_t pending_deadline_ms() const noexcept {return active_?deadline_:0;}
    BoardSwitchResult update(std::uint64_t now, bool allowed, bool available,
        const EquipmentOwner& owner, bool offboard, bool standable, bool grounded_board) noexcept {
        if (!active_) return BoardSwitchResult::idle;
        if (now >= deadline_) { cancel(); return BoardSwitchResult::timed_out; }
        if (!allowed) return BoardSwitchResult::blocked;
        if (!available || !owner.entity) return BoardSwitchResult::waiting;
        if (owner_.entity && owner_ != owner) { cancel(); return BoardSwitchResult::owner_changed; }
        owner_ = owner;
        if (grounded_board) { cancel(); return BoardSwitchResult::complete; }
        if (!offboard || !standable || now < next_request_) return BoardSwitchResult::waiting;
        next_request_ = now + 350; // native one-shot lease is 250ms
        return BoardSwitchResult::request_mount;
    }
};
} // namespace dingosdk::bmx
