#pragma once
// What a session does to the local player's physics, between the session (which knows the
// host and its rules) and the code that changes physics (the trainer). The tuning asset itself
// goes through Extension/Skater/physics_tuning.h; this is the rest.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>
#include "session_competition.h"

namespace dingosdk {
// Physics a player changes outside Gameplay/SkatePhysicsTuning: the trainer's tuning-class
// values and trick multipliers (Extension/Trainer/trainer_session.h reads and writes the
// bytes; they are opaque here and on the wire). Empty: everything the game's own.
inline constexpr std::size_t max_physics_extras = 4096;

namespace session_physics_detail {
inline std::atomic<bool> enforced{};
struct Box {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;
    std::uint64_t revision = 1; // moves whenever `bytes` does; nobody's `seen` starts at it
};
inline Box &local_box() { static Box value; return value; }
inline Box &host_box() { static Box value; return value; }
inline void put(Box &box, std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(box.mutex);
    if (std::ranges::equal(box.bytes, bytes)) return;
    box.bytes.assign(bytes.begin(), bytes.end());
    ++box.revision;
}
inline bool take(Box &box, std::uint64_t &seen, std::vector<std::uint8_t> &bytes) {
    std::lock_guard lock(box.mutex);
    if (seen == box.revision) return false;
    seen = box.revision;
    bytes = box.bytes;
    return true;
}
} // namespace session_physics_detail

// The local player is a guest of a session whose host (or dedicated server) sets everyone's
// physics (the roster's enforce_tuning): their own physics edits stand down, and what the host
// shares is theirs. False outside a session and for a host. Set by the session every tick.
inline bool session_tuning_enforced() noexcept {
    return competition_stock_physics() || session_physics_detail::enforced.load(std::memory_order_acquire);
}
inline void set_session_tuning_enforced(bool enforced) noexcept {
    session_physics_detail::enforced.store(enforced, std::memory_order_release);
}

// The local player's own extras, published by whoever edits them; a host's session sends them
// to its guests. The readers copy `bytes` out only when they are not the ones `seen` names
// (start `seen` at 0), and say whether they did.
inline void set_local_physics_extras(std::span<const std::uint8_t> bytes) {
    session_physics_detail::put(session_physics_detail::local_box(), bytes);
}
inline bool local_physics_extras(std::uint64_t &seen, std::vector<std::uint8_t> &bytes) {
    return session_physics_detail::take(session_physics_detail::local_box(), seen, bytes);
}
// A guest's host's extras, set by the session: empty until the host sends some, and always for
// a dedicated server, whose physics are the game's own. Only meaningful while
// session_tuning_enforced().
inline void set_host_physics_extras(std::span<const std::uint8_t> bytes) {
    session_physics_detail::put(session_physics_detail::host_box(), bytes);
}
inline bool host_physics_extras(std::uint64_t &seen, std::vector<std::uint8_t> &bytes) {
    return session_physics_detail::take(session_physics_detail::host_box(), seen, bytes);
}
} // namespace dingosdk
