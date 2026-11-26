#pragma once
#include "native_action.h"
#include <array>
#include <cstddef>
#include <limits>

namespace dingosdk::skater_action {
inline constexpr std::uint64_t offer_timeout_ms = 250, observation_timeout_ms = 4000;
inline constexpr std::size_t receipt_slots = 32;
struct State {
    bool known{}, ragdoll_known{}, ragdoll{}, in_air_known{}, in_air{};
    std::uint32_t value{};
    bool operator==(const State&) const = default;
};
constexpr bool target(Kind kind, const State& state) noexcept {
    return state.known && (kind == Kind::board ? state.value == 100 :
        state.value == 504 && state.ragdoll_known && !state.ragdoll && state.in_air_known && !state.in_air);
}
constexpr bool source(Kind kind, const State& state) noexcept {
    return kind == Kind::on_foot ? state.known && (state.value == 100 || state.value == 103) :
        target(Kind::on_foot, state);
}
constexpr bool request_bits_clear(std::uint32_t word, std::uint32_t both_masks) noexcept {
    return (word & both_masks) == 0;
}
struct BitOffer {
    bool inserted{};
    std::uint32_t before{};
    Reason reason{Reason::none};
};
// compare(exchange, expected, observed) executes one guarded atomic compare.
// A changed word is a conflict, never permission to retry with new flags.
template<class Compare> BitOffer offer_bits(std::uint32_t mask, std::uint32_t both_masks, Compare&& compare) noexcept {
    BitOffer result;
    if (!compare(0, 0, result.before)) { result.reason = Reason::request_write_failed; return result; }
    if (!request_bits_clear(result.before, both_masks)) { result.reason = Reason::request_bits_conflict; return result; }
    std::uint32_t observed{};
    if (!compare(result.before | mask, result.before, observed)) { result.reason = Reason::request_write_failed; return result; }
    if (observed != result.before) { result.reason = Reason::request_bits_conflict; return result; }
    result.inserted = true; return result;
}
constexpr Reason lifetime_fence(std::uint64_t expected_lifetime, std::uint64_t current_lifetime,
                                std::uint64_t expected_world, std::uint64_t current_world) noexcept {
    if (expected_lifetime != current_lifetime) return Reason::native_lifetime_changed;
    return expected_world == current_world ? Reason::none : Reason::world_changed;
}
// External synchronization owns the ledger. A claimed slot stays globally reserved
// through the native call, even if its VM is released or its actor is invalidated.
template<class Binding, std::size_t Capacity = receipt_slots> class Ledger {
public:
    struct Entry {
        std::uint64_t owner{}, offered_observation{};
        Receipt receipt{};
        Binding binding{};
        bool occupied{}, claimed{}, abandoned{};
    };
    std::array<Entry, Capacity> entries{};
    Entry* find(std::uint64_t owner, Handle handle) noexcept {
        if (!owner || !handle.sequence) return nullptr;
        for (auto& e : entries) if (e.occupied && !e.abandoned && e.owner == owner &&
            e.receipt.handle.sequence == handle.sequence) return &e;
        return nullptr;
    }
    Entry* active() noexcept {
        for (auto& e : entries) if (e.occupied && (e.claimed || e.receipt.quarantined || !terminal(e.receipt.status))) return &e;
        return nullptr;
    }
    bool enqueue(std::uint64_t owner, Kind kind, const Binding& binding, const State& state,
                 std::uint64_t now, Handle& handle, Reason& reason) noexcept {
        handle = {}; reason = Reason::none;
        if (!owner) { reason = Reason::invalid_owner; return false; }
        if (kind != Kind::on_foot && kind != Kind::board) { reason = Reason::invalid_kind; return false; }
        if (active()) { reason = Reason::busy; return false; }
        if (!target(kind, state) && !source(kind, state)) { reason = Reason::unsupported_state; return false; }
        Entry* slot{};
        for (auto& e : entries) if (!e.occupied) { slot = &e; break; }
        if (!slot || next_ == std::numeric_limits<std::uint64_t>::max()) {
            reason = Reason::receipt_capacity; return false;
        }
        *slot = {}; slot->occupied = true; slot->owner = owner; slot->binding = binding;
        slot->receipt.handle.sequence = ++next_; slot->receipt.kind = kind;
        slot->receipt.requested_ms = now; handle = slot->receipt.handle;
        if (target(kind, state)) {
            slot->receipt.committed_known = state.known; slot->receipt.committed_state = state.value;
            slot->receipt.ragdoll_known = state.ragdoll_known; slot->receipt.ragdoll = state.ragdoll;
            slot->receipt.in_air_known = state.in_air_known; slot->receipt.in_air = state.in_air;
            finish(*slot, Status::already_target, Reason::none, now);
        }
        return true;
    }
    static void finish(Entry& entry, Status status, Reason reason, std::uint64_t now) noexcept {
        if (terminal(entry.receipt.status)) return;
        entry.receipt.status = status; entry.receipt.reason = reason; entry.receipt.observed_ms = now;
    }
    bool cancel(std::uint64_t owner, Handle handle, std::uint64_t now) noexcept {
        auto* e = find(owner, handle);
        if (!e || e->claimed || e->receipt.status != Status::pending) return false;
        finish(*e, Status::cancelled, Reason::cancelled, now); return true;
    }
    bool acknowledge(std::uint64_t owner, Handle handle) noexcept {
        auto* e = find(owner, handle);
        if (!e || e->claimed || e->receipt.quarantined || !terminal(e->receipt.status)) return false;
        *e = {}; return true;
    }
    void release(std::uint64_t owner, std::uint64_t now) noexcept {
        for (auto& e : entries) if (e.occupied && e.owner == owner) {
            if (e.claimed || e.receipt.quarantined) { finish(e, Status::invalidated, Reason::owner_released, now); e.abandoned = true; }
            else e = {};
        }
    }
    static void expire(Entry& e, std::uint64_t now) noexcept {
        if (!e.occupied || terminal(e.receipt.status) || e.claimed) return;
        const auto start = e.receipt.status == Status::pending ? e.receipt.requested_ms : e.receipt.offered_ms;
        const auto timeout = e.receipt.status == Status::pending ? offer_timeout_ms : observation_timeout_ms;
        if (now < start || now - start >= timeout) finish(e, Status::expired, Reason::expired, now);
    }
    static bool claim(Entry& e, std::uint64_t now) noexcept {
        expire(e, now);
        if (!e.occupied || e.claimed || e.receipt.status != Status::pending) return false;
        e.claimed = true; return true;
    }
    static void offered(Entry& e, std::uint64_t now, std::uint64_t observation) noexcept {
        if (!e.claimed || terminal(e.receipt.status)) return;
        e.offered_observation = observation;
        e.receipt.status = Status::offered; e.receipt.offered_ms = now;
    }
    static void selection(Entry& e, std::uint32_t original, std::uint32_t final,
                          bool restored, std::uint64_t now) noexcept {
        e.receipt.original_known = e.receipt.final_known = true;
        e.receipt.original_candidate = original; e.receipt.final_candidate = final;
        e.receipt.flag_restored = restored; e.claimed = false;
        if (!restored) e.receipt.quarantined = true;
        if (e.abandoned) { if (!e.receipt.quarantined) e = {}; return; }
        if (terminal(e.receipt.status)) return;
        if (!restored) finish(e, Status::rejected, Reason::request_restore_failed, now);
        else if (original != final) finish(e, Status::rejected, Reason::selector_overridden, now);
        else if ((e.receipt.kind == Kind::on_foot && original != 504) ||
                 (e.receipt.kind == Kind::board && original != 100 && original != 103 && original != 200))
            finish(e, Status::rejected, Reason::native_refused, now);
        else e.receipt.status = Status::selected;
    }
    static void retire_quarantine(Entry& e) noexcept {
        if (e.claimed || !e.receipt.quarantined) return;
        e.receipt.quarantined = false;
        if (e.abandoned) e = {};
    }
    static void observe(Entry& e, const State& state, std::uint64_t now, std::uint64_t observation) noexcept {
        expire(e, now);
        if (!e.occupied || e.claimed || terminal(e.receipt.status) ||
            e.receipt.status != Status::selected || observation <= e.offered_observation) return;
        e.receipt.committed_known = state.known; e.receipt.committed_state = state.value;
        e.receipt.ragdoll_known = state.ragdoll_known; e.receipt.ragdoll = state.ragdoll;
        e.receipt.in_air_known = state.in_air_known; e.receipt.in_air = state.in_air;
        e.receipt.observed_ms = now;
        if (target(e.receipt.kind, state)) finish(e, Status::completed, Reason::none, now);
    }
private:
    std::uint64_t next_{};
};
} // namespace dingosdk::skater_action
