#include "car_grab/lifecycle_journal.h"

#include <limits>

namespace car_grab {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

LifecycleJournal::LifecycleJournal(std::uint64_t first_sequence) noexcept
    : origin_(first_sequence ? first_sequence - 1 : 0), head_(origin_), invalid_(!first_sequence) {}

std::uint64_t LifecycleJournal::append(std::uint64_t id) noexcept {
    if (!id) {
        invalid_.store(true);
        return 0;
    }
    if (invalid_.load()) return 0;
    auto previous = head_.load();
    for (;;) {
        if (previous == std::numeric_limits<std::uint64_t>::max()) {
            invalid_.store(true);
            return 0;
        }
        if (head_.compare_exchange_weak(previous, previous + 1)) break;
        if (invalid_.load()) return 0;
    }
    const auto sequence = previous + 1;
    auto& slot = slots_[(sequence - 1) % capacity];
    if (slot.busy.test_and_set()) {
        // Never wait for a stalled producer while the engine may hold locks.
        invalid_.store(true);
        return 0;
    }
    if (invalid_.load() || slot.sequence.load() >= sequence) {
        // An older reservation can be descheduled before obtaining its slot.
        // If a newer ring lap already published there, it cannot write later.
        invalid_.store(true);
        slot.busy.clear();
        return 0;
    }
    // Sequentially consistent two-phase publication ensures a reader loading a
    // replacement ID cannot also accept the previous publication sequence.
    slot.sequence.store(0);
    slot.id.store(id);
    slot.sequence.store(sequence);
    slot.busy.clear();
    return invalid_.load() ? 0 : sequence;
}

std::uint64_t LifecycleJournal::current_sequence() const noexcept {
    return head_.load();
}

LifecycleReadResult LifecycleJournal::read_since(std::uint64_t after) const {
    const auto through = head_.load();
    const auto failed = [&](LifecycleReadStatus status) {
        return LifecycleReadResult{invalid_.load() ? LifecycleReadStatus::invalid : status, head_.load(), {}};
    };
    if (invalid_.load() || after > through) return failed(LifecycleReadStatus::invalid);
    if (after < origin_) return failed(LifecycleReadStatus::overflow);
    const auto count = through - after;
    if (count > capacity) return failed(LifecycleReadStatus::overflow);

    LifecycleReadResult result{LifecycleReadStatus::complete, through, {}};
    result.ids.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t offset = 1; offset <= count; ++offset) {
        const auto sequence = after + offset;
        const auto& slot = slots_[(sequence - 1) % capacity];
        if (slot.busy.test()) return failed(LifecycleReadStatus::pending);
        const auto before = slot.sequence.load();
        if (before != sequence)
            return failed(before > sequence ? LifecycleReadStatus::overflow : LifecycleReadStatus::pending);
        const auto id = slot.id.load();
        const auto published = slot.sequence.load();
        if (published != sequence)
            return failed(published > sequence ? LifecycleReadStatus::overflow : LifecycleReadStatus::pending);
        if (slot.busy.test()) return failed(LifecycleReadStatus::pending);
        if (!id) return failed(LifecycleReadStatus::invalid);
        result.ids.push_back(id);
    }
    if (invalid_.load()) return failed(LifecycleReadStatus::invalid);
    if (head_.load() - after > capacity) return failed(LifecycleReadStatus::overflow);
    return result;
}
} // namespace car_grab
