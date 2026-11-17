#pragma once
#include <array>
#include <cstdint>

namespace dingosdk {
// Keep a short boost's minimum rise speed through native off-board motion updates.
bool ensure_offboard_up_velocity(std::uintptr_t base, std::uintptr_t core, std::uintptr_t context,
    std::uintptr_t rig, float minimum_up_speed) noexcept;
bool offboard_flight_compatible(std::uintptr_t base) noexcept;
// Only call during a freshly validated local motion/physics update.
// Synchronize the active falling/ground state, preserving its fourth lane.
bool sync_offboard_flight_velocity(std::uintptr_t base, std::uintptr_t core,
    std::uintptr_t context, std::uintptr_t rig, const std::array<float, 3>& velocity) noexcept;
// The same, and the dive and glide states too (FollowRagdoll and its two kin), which keep the
// skater's world velocity at the same place and carry on from it: a skater held and flown by a
// plugin, let go into a dive, went on at the speed it had before it was held (seen in game,
// 2026-11-16: out from under a parachute it fell as fast as it had been falling before it opened).
bool set_offboard_velocity(std::uintptr_t base, std::uintptr_t core,
    std::uintptr_t context, std::uintptr_t rig, const std::array<float, 3>& velocity) noexcept;
// Multiplies the upward part of the active off-board state's velocity, once (a jump that has
// just started). False when the skater is not rising or the active state keeps no velocity
// of its own (animation, trajectory, ragdoll). `before` receives the upward speed found.
bool scale_offboard_up_velocity(std::uintptr_t base, std::uintptr_t core, std::uintptr_t context,
    std::uintptr_t rig, float factor, float* before) noexcept;
}
