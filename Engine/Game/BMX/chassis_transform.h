#pragma once
#include <array>
#include <cmath>

namespace dingosdk::bmx::chassis {
using Vector = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // Hamilton XYZW; active local-to-world rotation.

inline bool finite(Vector value) noexcept {
    for (const auto component : value) if (!std::isfinite(component)) return false;
    return true;
}

// Accept native float roundoff, not an arbitrary quaternion that would add scale.
// Outputs are unchanged on rejection. Both q and -q represent the same rotation.
inline bool unit_quaternion(Quaternion input, Quaternion& output) noexcept {
    double squared = 0;
    for (const auto component : input) {
        if (!std::isfinite(component)) return false;
        squared += static_cast<double>(component) * component;
    }
    if (std::abs(squared - 1.0) > .001) return false;
    const auto inverse_length = static_cast<float>(1.0 / std::sqrt(squared));
    for (auto& component : input) component *= inverse_length;
    output = input;
    return true;
}

// Internal fast path: rotation must already have passed unit_quaternion().
inline Vector rotate_unit(Quaternion rotation, Vector point) noexcept {
    const Vector twice_cross{
        2 * (rotation[1] * point[2] - rotation[2] * point[1]),
        2 * (rotation[2] * point[0] - rotation[0] * point[2]),
        2 * (rotation[0] * point[1] - rotation[1] * point[0])};
    return {point[0] + rotation[3] * twice_cross[0] + rotation[1] * twice_cross[2] - rotation[2] * twice_cross[1],
        point[1] + rotation[3] * twice_cross[1] + rotation[2] * twice_cross[0] - rotation[0] * twice_cross[2],
        point[2] + rotation[3] * twice_cross[2] + rotation[0] * twice_cross[1] - rotation[1] * twice_cross[0]};
}

inline bool rotate(Quaternion rotation, Vector point, Vector& output) noexcept {
    Quaternion validated;
    if (!finite(point) || !unit_quaternion(rotation, validated)) return false;
    const auto rotated = rotate_unit(validated, point);
    if (!finite(rotated)) return false;
    output = rotated;
    return true;
}

// The local offset is FROM the assembled bike root TO the chassis COM.
// A physics publisher must convert measured COM here before setting Frame.position;
// this helper does not move a native body or assume a particular chassis shape.
inline bool root_from_com(Vector measured_com, Quaternion rotation, Vector local_com_from_root,
                          Vector& output) noexcept {
    Vector offset;
    if (!finite(measured_com) || !rotate(rotation, local_com_from_root, offset)) return false;
    Vector root;
    for (unsigned i = 0; i < 3; ++i) root[i] = measured_com[i] - offset[i];
    if (!finite(root)) return false;
    output = root;
    return true;
}

// Native position Vec4 W is metadata, not a coordinate or a validity requirement.
inline bool root_from_com(std::array<float, 4> measured_com, Quaternion rotation, Vector local_com_from_root,
                          Vector& output) noexcept {
    return root_from_com(Vector{measured_com[0], measured_com[1], measured_com[2]}, rotation,
        local_com_from_root, output);
}
} // namespace dingosdk::bmx::chassis
