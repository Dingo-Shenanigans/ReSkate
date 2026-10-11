#pragma once
#include <array>
#include <cmath>
#include <optional>

namespace dingosdk {
enum class VelocityBoostDirection { forward, up };

inline std::optional<std::array<float, 3>> velocity_boost_delta(
    const std::array<float, 16>& transform, float speed, VelocityBoostDirection direction,
    const std::array<float, 3>& board_velocity = {}) noexcept {
    const float maximum = direction == VelocityBoostDirection::up ? 25.0f : 300.0f;
    if (!std::isfinite(speed) || speed < 1.0f || speed > maximum) return {};
    if (direction == VelocityBoostDirection::up) return std::array<float, 3>{0, speed, 0};
    std::array<float, 3> delta{transform[8], transform[9], transform[10]};
    float length{};
    for (float component : delta) length += component * component;
    if (!std::isfinite(length) || length <= .01f) return {};
    const auto scale = speed / std::sqrt(length);
    for (auto& component : delta) component *= scale;
    // Fakie travel opposes the skater's facing axis. Use horizontal motion so
    // falling/rising cannot reverse a boost, and retain facing at rest or sideways.
    const float horizontal_length = std::hypot(delta[0], delta[2]);
    if (horizontal_length > .01f) {
        const float travel = (delta[0] * board_velocity[0] + delta[2] * board_velocity[2]) / horizontal_length;
        if (!std::isfinite(travel)) return {};
        // Ignore tiny backwards drift around standstill (0.1 world units/s).
        if (travel < -.1f)
            for (auto& component : delta) component = -component;
    }
    return delta;
}
}
