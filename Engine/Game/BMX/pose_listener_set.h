#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace dingosdk::bmx {
// Bounded process-lifetime callbacks. Removal stops future snapshots; an already
// snapshotted callback may still run, so consumers must gate their own live owner.
template<class Callback, std::size_t Slots> class PoseListenerSet {
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
            if (slot.load(std::memory_order_relaxed) == callback) slot.store(nullptr, std::memory_order_release);
    }
    template<class... Args> void dispatch(Callback legacy, Args... args) const noexcept {
        std::array<Callback, Slots> snapshot{};
        for (std::size_t i = 0; i < Slots; ++i) snapshot[i] = slots_[i].load(std::memory_order_acquire);
        for (const auto callback : snapshot) if (callback && callback != legacy) callback(args...);
        if (legacy) legacy(args...);
    }
};
}
