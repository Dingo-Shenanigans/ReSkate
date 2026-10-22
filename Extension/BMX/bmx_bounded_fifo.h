#pragma once
#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace dingosdk::bmx {
// Single caller thread. Put the owning queue on the heap when T is large.
// Slots are filled in place, retained on failed writes and never returned by
// value. A full queue refuses the new item without changing any retained item.
template<class T, std::size_t Capacity> class BoundedFifo {
    static_assert(Capacity > 0);
public:
    template<class Fill> bool try_emplace(Fill&& fill) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<void,Fill,T&>);
        if (size_ == Capacity) return false;
        std::forward<Fill>(fill)(slots_[(head_+size_)%Capacity]);
        ++size_; return true;
    }
    T* front() noexcept { return size_ ? &slots_[head_] : nullptr; }
    const T* front() const noexcept { return size_ ? &slots_[head_] : nullptr; }
    bool pop() noexcept {
        if (!size_) return false;
        head_=(head_+1)%Capacity; --size_; return true;
    }
    std::size_t size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
private:
    std::array<T,Capacity> slots_{};
    std::size_t head_{},size_{};
};
} // namespace dingosdk::bmx
