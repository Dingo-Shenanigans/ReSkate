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
// Multiplies the upward part of the active off-board state's velocity, once (a jump that has
// just started). False when the skater is not rising or the active state keeps no velocity
// of its own (animation, trajectory, ragdoll). `before` receives the upward speed found.
bool scale_offboard_up_velocity(std::uintptr_t base, std::uintptr_t core, std::uintptr_t context,
    std::uintptr_t rig, float factor, float* before) noexcept;
}
