#pragma once
#include <array>
#include <cstdint>

namespace dingosdk {
inline constexpr std::array<unsigned, 4> multiplayer_tick_rates{20, 30, 60, 120};
inline constexpr unsigned multiplayer_default_tps = 30;
constexpr bool valid_multiplayer_tps(unsigned tps) noexcept {
    for (const auto rate : multiplayer_tick_rates) if (rate == tps) return true;
    return false;
}
constexpr std::uint32_t multiplayer_pose_interval(unsigned tps) noexcept {
    return 1000000U / (valid_multiplayer_tps(tps) ? tps : multiplayer_default_tps);
}
constexpr bool valid_pose_interval(std::uint32_t interval) noexcept {
    if (interval == 100000 || interval == 200000) return true;
    for (const auto rate : multiplayer_tick_rates)
        if (interval == multiplayer_pose_interval(rate)) return true;
    return false;
}
// The most poses a second one player is sent on a dedicated server, whatever the crowd
// around them (Extension/Multiplayer/Session/room.h, crowd_limits); 0: no limit.
inline constexpr unsigned crowd_pose_budget = 600, min_crowd_budget = 300, max_crowd_budget = 20000;
constexpr bool valid_crowd_budget(unsigned budget) noexcept {
    return !budget || (budget >= min_crowd_budget && budget <= max_crowd_budget);
}
} // namespace dingosdk
