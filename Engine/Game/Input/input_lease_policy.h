#pragma once
#include "input_lease.h"
#include <array>
#include <limits>

namespace dingosdk::input_lease {
// The native service serializes every operation and supplies its monotonically
// ordered outer-report ordinals and observation times under that same gate.
// Reserving a source only gives access to qualified later observations; it never
// suppresses or rewrites input, nor identifies a physical controller replacement.
class Broker {
public:
    explicit Broker(std::uint64_t first_sequence = 1) noexcept : next_sequence_(first_sequence) {}

    bool request(std::uint64_t owner, std::uint32_t user, const Source& source,
                 const Guard& guard, bool source_busy, Handle& out, Reason& reason) noexcept {
        reason = advance(guard);
        if (reason != Reason::none) return false;
        if (!owner) return fail(reason, Reason::invalid_owner);
        if (user > 3 || source.user != user) return fail(reason, Reason::invalid_source);
        if (!source.connected || !source.eligible) return fail(reason, Reason::source_unavailable);
        if (!source.epoch || !source.observation_serial || !source.observed_ms)
            return fail(reason, Reason::invalid_source);
        if (source.context_generation != guard.context_generation)
            return fail(reason, Reason::context_changed);
        if (source.observed_ms > guard.now_ms) return fail(reason, Reason::stale_epoch);
        if (guard.now_ms - source.observed_ms > maximum_age_ms)
            return fail(reason, Reason::source_stale);
        if (active_index_ != retained_capacity || source_busy) return fail(reason, Reason::busy);
        const auto index = vacant();
        if (index == retained_capacity) return fail(reason, Reason::capacity);
        if (!next_sequence_) return fail(reason, Reason::sequence_exhausted);
        if (guard.now_ms > maximum - lifetime_ms) return fail(reason, Reason::deadline);

        auto& record = records_[index];
        record = {};
        record.occupied = true;
        record.owner = owner;
        record.receipt.handle = {next_sequence_};
        record.receipt.user = user;
        record.receipt.requested_ms = guard.now_ms;
        record.receipt.expires_ms = guard.now_ms + lifetime_ms;
        record.receipt.source_epoch = source.epoch;
        record.receipt.context_generation = source.context_generation;
        // Admission records only a continuity boundary, not a readable sample.
        record.last_source = source;
        active_index_ = index;
        out = record.receipt.handle;
        next_sequence_ = next_sequence_ == maximum ? 0 : next_sequence_ + 1;
        reason = Reason::none;
        return true;
    }

    // These are reservation metadata. Only a guard-bearing operation establishes
    // present validity; a cached handle or receipt is not a fresh input sample.
    Handle active() const noexcept {
        return active_index_ == retained_capacity ? Handle{} : records_[active_index_].receipt.handle;
    }
    std::uint32_t active_user() const noexcept {
        return active_index_ == retained_capacity ? 4 : records_[active_index_].receipt.user;
    }

    // Internal native composition queries do not issue a public snapshot or
    // replace the exact snapshot that a plugin may later renew.
    bool accepted(std::uint64_t owner, Handle handle, const Source& source) const noexcept {
        const auto index = owned(owner, handle);
        if (index == retained_capacity) return false;
        const auto& record = records_[index];
        return record.receipt.status == Status::active && record.has_sample &&
            same_source(record.last_source, source);
    }
    bool issued(std::uint64_t owner, Handle handle, const Snapshot& snapshot) const noexcept {
        const auto index = owned(owner, handle);
        return index != retained_capacity && records_[index].has_last_issued &&
            records_[index].last_issued == snapshot;
    }
    void invalidate(Reason reason) noexcept { finish(Status::revoked, reason); }

    void observe(Handle begin_cookie, const Source& source, const Guard& guard) noexcept {
        if (advance(guard) != Reason::none || active_index_ == retained_capacity) return;
        auto& record = records_[active_index_];
        if (source.user != record.receipt.user) return;
        if (!source.connected || !source.eligible) {
            finish(Status::revoked, Reason::source_unavailable);
            return;
        }
        if (!source.epoch || source.epoch != record.receipt.source_epoch) {
            finish(Status::revoked, Reason::source_changed);
            return;
        }
        if (source.context_generation != record.receipt.context_generation) {
            finish(Status::revoked, Reason::context_changed);
            return;
        }
        if (!source.observation_serial || !source.observed_ms || source.observed_ms > guard.now_ms ||
            source.observation_serial < record.last_source.observation_serial ||
            source.observed_ms < record.last_source.observed_ms) {
            finish(Status::revoked, Reason::stale_epoch);
            return;
        }
        if (guard.now_ms - source.observed_ms > maximum_age_ms) {
            finish(Status::expired, Reason::source_stale);
            return;
        }
        if (source.observation_serial == record.last_source.observation_serial) {
            // One ordinal cannot acquire new contents after it was accepted.
            if (!same_source(source, record.last_source)) finish(Status::revoked, Reason::stale_epoch);
            return;
        }
        // An outer poll started before this reservation cannot seed its cache.
        if (begin_cookie != record.receipt.handle) return;
        record.last_source = source;
        record.sample = {record.receipt.handle, source.epoch, source.observation_serial,
                         source.observed_ms, source.context_generation, source.user, source.packet, source.pad};
        record.has_sample = true;
        record.receipt.status = Status::active;
        record.receipt.observed_ms = source.observed_ms;
        record.receipt.last_observation = source.observation_serial;
    }

    void tick(const Guard& guard) noexcept { (void)advance(guard); }

    bool read(std::uint64_t owner, Handle handle, const Guard& guard,
              Snapshot& out, Reason& reason) noexcept {
        reason = advance(guard);
        if (reason != Reason::none) return false;
        auto* record = find(owner, handle, reason);
        if (!record) return false;
        if (record->receipt.status != Status::active || !record->has_sample)
            return fail(reason, record->receipt.reason == Reason::none ? Reason::unavailable : record->receipt.reason);
        out = record->sample;
        record->last_issued = out;
        record->has_last_issued = true;
        reason = Reason::none;
        return true;
    }

    bool renew(std::uint64_t owner, Handle handle, const Snapshot& snapshot,
               const Guard& guard, Reason& reason) noexcept {
        reason = advance(guard);
        if (reason != Reason::none) return false;
        auto* record = find(owner, handle, reason);
        if (!record) return false;
        if (record->receipt.status != Status::active || !record->has_sample)
            return fail(reason, record->receipt.reason == Reason::none ? Reason::unavailable : record->receipt.reason);
        // Caller-supplied epoch numbers are not authority. Only an exact issued
        // snapshot can renew, once for each strictly advancing observation. A
        // newer accepted observation need not invalidate a still-fresh read.
        if (!record->has_last_issued || snapshot != record->last_issued ||
            snapshot.observation_serial <= record->renewed_observation ||
            snapshot.handle != handle || snapshot.source_epoch != record->receipt.source_epoch ||
            snapshot.context_generation != record->receipt.context_generation ||
            snapshot.user != record->receipt.user || snapshot.observed_ms > guard.now_ms)
            return fail(reason, Reason::stale_epoch);
        if (guard.now_ms - snapshot.observed_ms > maximum_age_ms)
            return fail(reason, Reason::source_stale);
        if (guard.now_ms > maximum - lifetime_ms || record->receipt.renewals == maximum)
            return fail(reason, Reason::deadline);
        record->renewed_observation = snapshot.observation_serial;
        record->receipt.expires_ms = guard.now_ms + lifetime_ms;
        ++record->receipt.renewals;
        reason = Reason::none;
        return true;
    }

    bool release(std::uint64_t owner, Handle handle) noexcept {
        const auto index = owned(owner, handle);
        if (index == retained_capacity || terminal(records_[index].receipt.status)) return false;
        // There is no native report debt in the observation-only broker.
        finish(Status::released, Reason::cancelled);
        return true;
    }

    bool state(std::uint64_t owner, Handle handle, Receipt& out) const noexcept {
        const auto index = owned(owner, handle);
        if (index == retained_capacity) return false;
        out = records_[index].receipt;
        return true;
    }

    bool acknowledge(std::uint64_t owner, Handle handle) noexcept {
        const auto index = owned(owner, handle);
        if (index == retained_capacity || !terminal(records_[index].receipt.status)) return false;
        records_[index] = {};
        return true;
    }

    void release_all(std::uint64_t owner) noexcept {
        if (!owner) return;
        if (active_index_ != retained_capacity && records_[active_index_].owner == owner) {
            records_[active_index_].receipt.orphaned = true;
            finish(Status::released, Reason::cancelled);
        }
        // A stopped owner cannot acknowledge receipts, and no report remains to
        // neutralize, so keeping its terminal entries would only exhaust capacity.
        for (auto& record : records_) if (record.occupied && record.owner == owner) record = {};
    }

private:
    struct Record {
        bool occupied{}, has_sample{}, has_last_issued{};
        std::uint64_t owner{}, renewed_observation{};
        Receipt receipt{};
        Source last_source{};
        Snapshot sample{}, last_issued{};
    };
    static constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

    static bool fail(Reason& out, Reason reason) noexcept { out = reason; return false; }
    static bool same_source(const Source& a, const Source& b) noexcept {
        return a.user == b.user && a.packet == b.packet && a.epoch == b.epoch &&
            a.observation_serial == b.observation_serial && a.observed_ms == b.observed_ms &&
            a.context_generation == b.context_generation && a.pad == b.pad &&
            a.connected == b.connected && a.eligible == b.eligible;
    }
    std::size_t vacant() const noexcept {
        for (std::size_t i = 0; i < records_.size(); ++i) if (!records_[i].occupied) return i;
        return retained_capacity;
    }
    std::size_t owned(std::uint64_t owner, Handle handle) const noexcept {
        if (!owner || !handle.sequence) return retained_capacity;
        for (std::size_t i = 0; i < records_.size(); ++i)
            if (records_[i].occupied && records_[i].owner == owner && records_[i].receipt.handle == handle) return i;
        return retained_capacity;
    }
    Record* find(std::uint64_t owner, Handle handle, Reason& reason) noexcept {
        if (!owner) { reason = Reason::invalid_owner; return nullptr; }
        const auto index = owned(owner, handle);
        if (index != retained_capacity) return &records_[index];
        // Do not disclose another owner's retained receipt.
        reason = Reason::unavailable;
        return nullptr;
    }
    void finish(Status status, Reason reason) noexcept {
        if (active_index_ == retained_capacity) return;
        auto& record = records_[active_index_];
        record.receipt.status = status;
        record.receipt.reason = reason;
        record.receipt.source_released = true;
        record.has_sample = record.has_last_issued = false;
        active_index_ = retained_capacity;
    }
    Reason advance(const Guard& guard) noexcept {
        // Wall-clock regression can never extend an existing reservation or
        // reset the high-water mark for the next owner.
        Reason reason = Reason::none;
        if (!guard.now_ms || guard.now_ms < last_now_) reason = Reason::stale_epoch;
        else {
            last_now_ = guard.now_ms;
            if (!guard.ready) reason = Reason::client_not_ready;
            else if (!guard.focused) reason = Reason::focus_lost;
            else if (!guard.gameplay) reason = Reason::gameplay_inactive;
            else if (!guard.menu_closed) reason = Reason::menu_open;
            else if (!guard.interference_clear) reason = Reason::interference;
            else if (!guard.context_generation) reason = Reason::context_changed;
            else if (active_index_ != retained_capacity &&
                records_[active_index_].receipt.context_generation != guard.context_generation)
                reason = Reason::context_changed;
        }
        if (reason != Reason::none) {
            finish(Status::revoked, reason);
            return reason;
        }
        if (active_index_ != retained_capacity) {
            const auto& receipt = records_[active_index_].receipt;
            if (guard.now_ms >= receipt.expires_ms) {
                finish(Status::expired, Reason::deadline);
                return Reason::deadline;
            }
            const auto observed = receipt.status == Status::warming ? receipt.requested_ms : receipt.observed_ms;
            if (guard.now_ms - observed > maximum_age_ms) {
                finish(Status::expired, Reason::source_stale);
                return Reason::source_stale;
            }
        }
        return Reason::none;
    }

    std::array<Record, retained_capacity> records_{};
    std::size_t active_index_{retained_capacity};
    std::uint64_t next_sequence_{}, last_now_{};
};
}
