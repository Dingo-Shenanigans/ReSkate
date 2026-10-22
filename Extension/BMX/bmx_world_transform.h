#pragma once
#include "bmx_world.h"
#include "bmx_bike_contacts.h"
#include "Engine/Game/BMX/chassis_transform.h"
#include <array>
#include <cmath>
#include <string_view>

namespace dingosdk::bmx::world {
// Models retain assembled coordinates: +Y up, +Z forward, +X rider's left.
// These identifiers are the private model pack's object-blueprint asset names.
inline constexpr std::array<std::string_view, part_count> blueprints{
    "reskate/bmx/frame", "reskate/bmx/fork", "reskate/bmx/rear_wheel",
    "reskate/bmx/front_wheel", "reskate/bmx/cranks", "reskate/bmx/handlebars",
    "reskate/bmx/left_pedal", "reskate/bmx/right_pedal"};
using Matrix = std::array<float, 16>;
inline constexpr auto crank_pivot=geometry::bottom_bracket;
inline constexpr float crank_radius=geometry::crank_radius;
namespace detail {
using Vec = std::array<float, 3>;
inline Vec rotate(Vec point, unsigned axis, float angle, Vec pivot = {}) noexcept {
    for (unsigned i = 0; i < 3; ++i) point[i] -= pivot[i];
    const auto a = (axis + 1) % 3, b = (axis + 2) % 3;
    const auto x = point[a], y = point[b];
    const auto c = std::cos(angle), s = std::sin(angle);
    point[a] = x * c - y * s;
    point[b] = x * s + y * c;
    for (unsigned i = 0; i < 3; ++i) point[i] += pivot[i];
    return point;
}
inline Vec local_point(Vec point, unsigned part, const visual::Frame& frame,
                       const chassis::Quaternion* rotation = nullptr) noexcept {
    if (part == 2) point = rotate(point, 0, frame.rear_wheel_phase, geometry::rear_axle);
    if (part == 3) point = rotate(point, 0, frame.front_wheel_phase, geometry::front_axle);
    if (part == 4) point = rotate(point, 0, frame.crank_phase, crank_pivot);
    if (part == 6 || part == 7) {
        // Platforms translate with their spindle but retain their authored
        // level orientation. Crank arms rotate independently in part 4.
        const auto& platform=geometry::pedal(part==6);
        const auto spindle=geometry::crank_point(platform.spindle,frame.crank_phase);
        for(unsigned i=0;i<3;++i)point[i]+=spindle[i]-platform.spindle[i];
    }
    constexpr Vec head=contacts::steering_pivot;
    if (part == 1 || part == 3 || part == 5)
        point = geometry::rotate_steering(point, frame.steer + (part == 5 ? frame.barspin : 0));
    else
        point = geometry::rotate_steering(point, frame.tailwhip);
    point = rotate(point, 0, frame.movement_bike_pitch, head);
    point = rotate(point, 2, frame.movement_bike_roll, head);
    if (rotation) return chassis::rotate_unit(*rotation, point);
    point = rotate(point, 2, frame.roll);
    point = rotate(point, 0, frame.pitch);
    return rotate(point, 1, frame.yaw);
}
} // namespace detail

// The native setter expects basis columns and translation in slots 12..14.
// Form bases near the local origin before adding large world coordinates, so
// distant-map floating-point precision does not introduce scale into the model.
inline bool transforms(const visual::Frame& frame, std::array<Matrix, part_count>& output) noexcept {
    for (const auto value : frame.position)
        if (!std::isfinite(value) || std::abs(value) > 999990.f) return false;
    chassis::Quaternion rotation;
    if (frame.chassis_quaternion_available) {
        if (!chassis::unit_quaternion(frame.chassis_quaternion, rotation)) return false;
    } else for (const auto value : {frame.yaw, frame.pitch, frame.roll})
        if (!std::isfinite(value)) return false;
    for (const auto value : {frame.steer,
             frame.front_wheel_phase, frame.rear_wheel_phase, frame.crank_phase,
             frame.barspin, frame.tailwhip, frame.movement_bike_pitch, frame.movement_bike_roll})
        if (!std::isfinite(value)) return false;
    for (unsigned part = 0; part < output.size(); ++part) {
        auto& matrix = output[part];
        matrix = {};
        matrix[15] = 1;
        const auto* chassis_rotation = frame.chassis_quaternion_available ? &rotation : nullptr;
        const auto origin = detail::local_point({}, part, frame, chassis_rotation);
        for (unsigned axis = 0; axis < 3; ++axis) {
            detail::Vec basis{};
            basis[axis] = 1;
            basis = detail::local_point(basis, part, frame, chassis_rotation);
            for (unsigned row = 0; row < 3; ++row)
                matrix[axis * 4 + row] = basis[row] - origin[row];
        }
        for (unsigned row = 0; row < 3; ++row)
            matrix[12 + row] = origin[row] + frame.position[row];
    }
    return true;
}
} // namespace dingosdk::bmx::world
