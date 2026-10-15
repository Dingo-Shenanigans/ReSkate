#pragma once
#include <algorithm>
#include <cstdint>

namespace dingosdk::multiplayer::map_fetch_policy {
inline constexpr std::uint64_t second_us = 1000000;
inline constexpr std::uint64_t session_us = 30ull * 60 * second_us;
inline constexpr std::uint64_t transfer_us = 27ull * 60 * second_us;
inline constexpr std::uint64_t completion_reserve_us = 3ull * 60 * second_us;
inline constexpr std::uint64_t confirmation_us = 2ull * 60 * second_us;
inline constexpr std::uint64_t installed_load_us = 30ull * second_us;

// All timestamps belong to one process's monotonic clock; none go on the wire.
constexpr bool expired(std::uint64_t deadline, std::uint64_t now) noexcept {
    return deadline && now >= deadline;
}
constexpr std::uint64_t remaining(std::uint64_t deadline, std::uint64_t now) noexcept {
    return now < deadline ? deadline - now : 0;
}
constexpr std::uint64_t transfer_deadline(std::uint64_t overall, std::uint64_t now) noexcept {
    const auto available = remaining(overall, now);
    return now + std::min(transfer_us, available > completion_reserve_us ? available - completion_reserve_us : 0);
}
constexpr bool host_expired(std::uint64_t started, std::uint64_t now) noexcept {
    return started && now >= started && now - started >= session_us;
}
}
