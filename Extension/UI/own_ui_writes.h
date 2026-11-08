#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dingosdk::ui {
// While ReSkate's own native menus and HUD write their labels (on this thread), the trick feed
// ignores every UI write: those are not skate.'s trick list.
inline thread_local bool writing_own_ui = false;
struct OwnUiWrites {
    bool previous = std::exchange(writing_own_ui, true);
    ~OwnUiWrites() { writing_own_ui = previous; }
};

// The widget may write our models later, on another thread and outside OwnUiWrites. Keep their
// entry ids as well, so those writes can never become native trick events. Each id includes its
// generation; an empty slot does not claim entry 0. Readers borrow individual atomic slots rather
// than an all-at-once snapshot, so callers publish ids before widgets can write their models.
template <std::size_t Count> class OwnUiEntries {
public:
    void set(const std::vector<std::uint64_t>& entries) noexcept {
        for (std::size_t slot = 0; slot < entries_.size(); ++slot)
            entries_[slot].store(slot < entries.size() ? entries[slot] : 0, std::memory_order_release);
    }
    bool contains(std::uint64_t entry) const noexcept {
        if (!entry) return false;
        for (const auto& slot : entries_)
            if (slot.load(std::memory_order_acquire) == entry) return true;
        return false;
    }

private:
    std::array<std::atomic<std::uint64_t>, Count> entries_{};
};
} // namespace dingosdk::ui
