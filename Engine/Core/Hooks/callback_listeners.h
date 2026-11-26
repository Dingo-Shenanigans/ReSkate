#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace dingosdk {
namespace callback_listeners_detail {
template<class Callback, std::size_t Slots, class... Args>
void dispatch(const std::array<Callback, Slots>& snapshot, Args... args) noexcept {
    for (std::size_t i = 0; i < Slots; ++i) {
        if (!snapshot[i]) continue;
        bool repeated{};
        for (std::size_t j = 0; j < i; ++j) repeated = repeated || snapshot[j] == snapshot[i];
        if (!repeated) snapshot[i](args...);
    }
}
}
// Registration is a client operation. Dispatch only snapshots fixed atomic slots;
// removing a callback cannot withdraw a snapshot that is already running.
// Concurrent registration can mix slot versions; a moved callback still runs at
// most once. This does not promise a globally coherent membership snapshot.
template<class Callback, std::size_t Slots> class CallbackListeners {
    static_assert(Slots > 0);
    static_assert(std::atomic<Callback>::is_always_lock_free);
    std::mutex registration_;
    std::array<std::atomic<Callback>, Slots> slots_{};
public:
    bool add(Callback callback) noexcept {
        if (!callback) return false;
        std::lock_guard lock(registration_);
        for (const auto& slot : slots_)
            if (slot.load(std::memory_order_acquire) == callback) return true;
        for (auto& slot : slots_)
            if (!slot.load(std::memory_order_relaxed)) {
                slot.store(callback, std::memory_order_release);
                return true;
            }
        return false;
    }
    void remove(Callback callback) noexcept {
        if (!callback) return;
        std::lock_guard lock(registration_);
        for (auto& slot : slots_)
            if (slot.load(std::memory_order_relaxed) == callback)
                slot.store(nullptr, std::memory_order_release);
    }
    template<class... Args> void dispatch(Args... args) const noexcept {
        std::array<Callback, Slots> snapshot{};
        for (std::size_t i = 0; i < Slots; ++i)
            snapshot[i] = slots_[i].load(std::memory_order_acquire);
        callback_listeners_detail::dispatch(snapshot, args...);
    }
};

// A reader accepts only the same idle epoch before and after its work. Entry is
// visible before any callback; exit advances the epoch before becoming idle.
// This invalidates observations across an invocation, including registration
// during one. It is an observation gate, not a native memory/lifetime lock.
class CallbackEpoch {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> sequence_{1}, active_{};
    std::atomic<bool> valid_{true};
    void advance() noexcept {
        if (sequence_.fetch_add(1, std::memory_order_acq_rel) == std::numeric_limits<std::uint64_t>::max())
            valid_.store(false, std::memory_order_release);
    }
public:
    void enter() noexcept {
        if (active_.fetch_add(1, std::memory_order_acq_rel) == std::numeric_limits<std::uint64_t>::max())
            valid_.store(false, std::memory_order_release);
        advance();
    }
    void leave() noexcept {
        advance();
        if (!active_.fetch_sub(1, std::memory_order_acq_rel))
            valid_.store(false, std::memory_order_release);
    }
    bool idle(std::uint64_t& out) const noexcept {
        out = 0;
        const auto before = sequence_.load(std::memory_order_acquire);
        if (!valid_.load(std::memory_order_acquire) || active_.load(std::memory_order_acquire)) return false;
        const auto after = sequence_.load(std::memory_order_acquire);
        if (before != after || !valid_.load(std::memory_order_acquire)) return false;
        out = after;
        return true;
    }
};
} // namespace dingosdk
