#pragma once
#include <array>
#include <cmath>
#include <cstddef>

// Frostbite's LinearTransform as the game keeps it in memory: the right, up and forward rows,
// then the translation, four floats each (the fourth lanes are not the matrix's and are never
// read). Points are row vectors: a point given in a transform lands at point x transform.
namespace dingosdk::game {
using Vec3 = std::array<float, 3>;
using LinearTransform = std::array<std::array<float, 4>, 4>;

inline float length(const Vec3& v) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
// Where a point given in a transform lands.
constexpr Vec3 place(const Vec3& point, const LinearTransform& transform) noexcept {
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i)
        result[i] = point[0] * transform[0][i] + point[1] * transform[1][i] + point[2] * transform[2][i] + transform[3][i];
    return result;
}
// Where a direction given in a transform points: turned, not moved.
constexpr Vec3 turn(const Vec3& direction, const LinearTransform& transform) noexcept {
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i)
        result[i] = direction[0] * transform[0][i] + direction[1] * transform[1][i] + direction[2] * transform[2][i];
    return result;
}
// `inner` given in `outer`: the transform a point of `inner` lands in through both.
constexpr LinearTransform compose(const LinearTransform& inner, const LinearTransform& outer) noexcept {
    LinearTransform result{};
    for (std::size_t row = 0; row < 3; ++row) {
        const auto axis = turn({inner[row][0], inner[row][1], inner[row][2]}, outer);
        result[row] = {axis[0], axis[1], axis[2], 0.0f};
    }
    const auto position = place({inner[3][0], inner[3][1], inner[3][2]}, outer);
    result[3] = {position[0], position[1], position[2], 1.0f};
    return result;
}
}
