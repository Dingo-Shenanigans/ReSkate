#pragma once
#include <atomic>
#include <cstddef>

namespace dingosdk {
// Noclip (and teleporting), No Bail and the forward / up boosts as the session's host lets
// the local player use them. All are allowed outside a session, and always for the host
// itself and a dedicated server's admins.
namespace session_tools_detail {
inline std::atomic<bool> noclip{true}, no_bail{true}, boosts{true};
inline std::atomic<bool> session{};
inline std::atomic<std::size_t> slots{};
}
inline bool session_noclip_allowed() noexcept { return session_tools_detail::noclip.load(std::memory_order_acquire); }
inline bool session_no_bail_allowed() noexcept { return session_tools_detail::no_bail.load(std::memory_order_acquire); }
inline bool session_boosts_allowed() noexcept { return session_tools_detail::boosts.load(std::memory_order_acquire); }
// Hosting or joined to a multiplayer session (set by the session every tick). Any thread.
inline bool multiplayer_session_active() noexcept { return session_tools_detail::session.load(std::memory_order_acquire); }
inline void set_multiplayer_session_active(bool active) noexcept {
    session_tools_detail::session.store(active, std::memory_order_release);
}
// The remote-player slots the session used at the start of its tick: every other player sits in a
// lower slot (set by the session every tick). 0 in solo play. Any thread.
inline std::size_t multiplayer_slots_in_use() noexcept { return session_tools_detail::slots.load(std::memory_order_acquire); }
inline void set_multiplayer_slots_in_use(std::size_t slots) noexcept {
    session_tools_detail::slots.store(slots, std::memory_order_release);
}
inline void set_session_tools_allowed(bool noclip, bool no_bail, bool boosts) noexcept {
    session_tools_detail::noclip.store(noclip, std::memory_order_release);
    session_tools_detail::no_bail.store(no_bail, std::memory_order_release);
    session_tools_detail::boosts.store(boosts, std::memory_order_release);
}
} // namespace dingosdk
