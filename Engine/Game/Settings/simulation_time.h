#pragma once
#include <cmath>
#include <cstdint>

// skate.'s simulation clock as plain numbers. It steps its simulation at a fixed rate
// (SimulationTime.ForceSimRate, 60 a second) on a clock its time scale slows (SimulationTime.TimeScale),
// each step as long as the rate gives: whole nanoseconds (1e9 / rate), then seconds (Ghidra
// sim_time_clock_apply_rate; measured 2026-10-09: 60 gives 0x3c888888, 200 gives 0.005). A skater
// takes its physics step length from it when its core is built (Ghidra build_pose_object).
namespace dingosdk::simulation_time {
// The length of one simulation step at `rate`, seconds, exactly as the game keeps it. 0 for no rate.
inline float step_seconds(std::uint32_t rate) noexcept {
    return rate ? static_cast<float>(static_cast<double>(1000000000u / rate) * 1e-9) : 0.0f;
}
// The rate a step length was made at; 0 for none.
inline std::uint32_t rate_of(float step_seconds) noexcept {
    return std::isfinite(step_seconds) && step_seconds > 0 ? static_cast<std::uint32_t>(std::lround(1.0 / step_seconds)) : 0;
}
}
