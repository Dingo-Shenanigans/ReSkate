#pragma once
#include "controller.h"
#include <array>

namespace car_grab::reach {
using Quaternion = std::array<float, 4>;
struct Joint {
    Vec3 scale{1, 1, 1};
    Quaternion rotation{0, 0, 0, 1};
    Vec3 translation{};
};
struct Arm {
    Joint parent_world, shoulder, upper, lower, hand;
};
struct Solution {
    bool available{}, clamped{};
    Quaternion upper{0, 0, 0, 1}, lower{0, 0, 0, 1};
    Vec3 hand_position{};
};
bool valid(const Joint&) noexcept;
Joint compose(const Joint& parent, const Joint& child) noexcept;
Solution solve(const Arm&, Vec3 target, float weight) noexcept;
float blend_weight(float previous, bool active, double seconds) noexcept;
} // namespace car_grab::reach
