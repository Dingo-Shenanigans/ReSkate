#pragma once
#include <atomic>

namespace dingosdk {
namespace session_competition_detail { inline std::atomic<bool> stock{}; }
// Local participation, including hosts/admins, overrides session permissions.
// Their settings are retained for after Leave.
inline bool competition_stock_physics() noexcept {
    return session_competition_detail::stock.load(std::memory_order_acquire);
}
inline void set_competition_stock_physics(bool locked) noexcept {
    session_competition_detail::stock.store(locked,std::memory_order_release);
}
}
