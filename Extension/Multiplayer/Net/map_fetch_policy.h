#pragma once
#include <algorithm>
#include <cstdint>

namespace dingosdk::multiplayer::map_fetch_policy {
inline constexpr std::uint64_t second_us = 1000000;
// How long a session waits on a map being fetched: checking, confirmation, download, install,
// apply and join. The download itself is not cut short by it: one the session gave up on goes
// on to the end and installs, and the player joins again (map_download.cpp).
inline constexpr std::uint64_t session_us = 30ull * 60 * second_us;
inline constexpr std::uint64_t confirmation_us = 2ull * 60 * second_us;
inline constexpr std::uint64_t installed_load_us = 30ull * second_us;

// All timestamps belong to one process's monotonic clock; none go on the wire.
constexpr bool expired(std::uint64_t deadline, std::uint64_t now) noexcept {
    return deadline && now >= deadline;
}
constexpr std::uint64_t remaining(std::uint64_t deadline, std::uint64_t now) noexcept {
    return now < deadline ? deadline - now : 0;
}
constexpr bool host_expired(std::uint64_t started, std::uint64_t now) noexcept {
    return started && now >= started && now - started >= session_us;
}
}
