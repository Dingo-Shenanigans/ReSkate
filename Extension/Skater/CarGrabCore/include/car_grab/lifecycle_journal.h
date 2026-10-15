#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace car_grab {
enum class LifecycleReadStatus { complete, overflow, pending, invalid };
struct LifecycleReadResult {
    LifecycleReadStatus status = LifecycleReadStatus::invalid;
    std::uint64_t through{};
    std::vector<std::uint64_t> ids{};
};
// A value-only bounded removal journal. append never allocates or waits for a
// slot owner. Native callers separately guard removals still in progress.
// current_sequence includes reservations whose publication may be pending.
// A reader owns a complete range (after, through]; other statuses expose no IDs
// and require the caller to invalidate/rebaseline its dependent snapshot.
// Zero IDs, serial exhaustion, and colliding/late slot writers fail closed for
// the lifetime of this journal. Reader overflow/future cursors do not poison it.
class LifecycleJournal {
public:
    static constexpr std::size_t capacity = 512;
    // A nondefault seed continues an earlier sequence namespace, but this
    // instance owns no events before first_sequence.
    explicit LifecycleJournal(std::uint64_t first_sequence = 1) noexcept;
    std::uint64_t append(std::uint64_t id) noexcept;
    std::uint64_t current_sequence() const noexcept;
    LifecycleReadResult read_since(std::uint64_t after) const;
private:
    struct Slot {
        std::atomic_flag busy{};
        std::atomic<std::uint64_t> sequence{};
        std::atomic<std::uint64_t> id{};
    };
    std::array<Slot, capacity> slots_{};
    const std::uint64_t origin_{};
    std::atomic<std::uint64_t> head_{};
    std::atomic<bool> invalid_{};
};
} // namespace car_grab
