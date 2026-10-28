#pragma once
#include <atomic>
#include <cstdint>

namespace dingosdk::native_input_source {
// Nonrecursive publication gate. Never hold it across original XInput, an
// extension filter, Lua, UI drawing or another external callback. Existing
// writer state locks, if needed, come after this gate.
class PublicationScope final {
public:
    explicit PublicationScope(bool enabled = true) noexcept;
    ~PublicationScope();
    PublicationScope(const PublicationScope&) = delete;
    PublicationScope& operator=(const PublicationScope&) = delete;
    bool owns() const noexcept { return owns_; }
private:
    bool owns_{};
};
bool publication_held() noexcept;
std::uint64_t publication_revision() noexcept;
// Only internal callers already holding the gate may invalidate publication.
void publication_changed() noexcept;

// Preserve ordinary atomic writer behavior, while making even an A->B->A
// interference change visible to every in-flight raw observation.
template<class T> class PublishedAtomic final {
public:
    constexpr PublishedAtomic() noexcept = default;
    constexpr explicit PublishedAtomic(T value) noexcept : value_(value) {}
    PublishedAtomic(const PublishedAtomic&) = delete;
    PublishedAtomic& operator=(const PublishedAtomic&) = delete;
    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept { return value_.load(order); }
    void store(T value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        PublicationScope scope;
        if (value_.exchange(value, order) != value) publication_changed();
    }
    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        PublicationScope scope;
        const auto before = value_.exchange(value, order);
        if (before != value) publication_changed();
        return before;
    }
    bool compare_exchange_strong(T& expected, T desired,
        std::memory_order order = std::memory_order_seq_cst) noexcept {
        PublicationScope scope;
        const auto before = expected;
        const bool result = value_.compare_exchange_strong(expected, desired, order);
        if (result && before != desired) publication_changed();
        return result;
    }
    operator T() const noexcept { return load(); }
    T operator=(T value) noexcept { store(value); return value; }
private:
    std::atomic<T> value_{};
};
}
