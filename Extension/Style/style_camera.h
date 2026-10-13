#pragma once
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <array>
#include <atomic>
#include <cmath>

namespace dingosdk::style_camera {
// Pitch in radians; distance, and height above the skater's root, in metres.
inline constexpr float editor_pitch = 0.05f, editor_distance = 3.6f, editor_height = 0.1f;
[[nodiscard]] inline float editor_yaw(const style::Quat &q) noexcept {
    const float along = std::atan2(1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[2] - q[3] * q[1]));
    return style::wrap_angle(along + addr::style::editor_camera_turn);
}
// Rows: right, up, backward, position. Keeps the matrix's handedness. False when the view has no right axis.
inline bool orbit_camera(std::array<float, 16> &matrix, float yaw, float pitch, float distance, const std::array<float, 3> &target,
                         float height) noexcept {
    const first_person::Vec3 back{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    first_person::Vec3 right{back[2], 0, -back[0]};
    const float length = first_person::length(right);
    if (length < 1e-4f) return false;
    for (auto &axis : right) axis /= length;
    const auto up = first_person::cross(back, right);
    const auto had = first_person::cross({matrix[0], matrix[1], matrix[2]}, {matrix[4], matrix[5], matrix[6]});
    if (first_person::dot(had, {matrix[8], matrix[9], matrix[10]}) < 0)
        for (auto &axis : right) axis = -axis;
    for (std::size_t i = 0; i < 3; ++i) {
        matrix[i] = right[i];
        matrix[4 + i] = up[i];
        matrix[8 + i] = back[i];
        matrix[12 + i] = target[i] + (i == 1 ? height : 0.0f) + back[i] * distance;
    }
    return true;
}
// Shared between threads.
struct OrbitState {
    std::array<std::atomic<float>, 3> target{};
    std::atomic<float> yaw{}, pitch{editor_pitch}, distance{editor_distance}, height{editor_height};
    void aim(const std::array<float, 3> &point) noexcept {
        for (std::size_t i = 0; i < 3; ++i) target[i].store(point[i], std::memory_order_relaxed);
    }
    void frame(float turn) noexcept {
        yaw.store(turn, std::memory_order_relaxed);
        pitch.store(editor_pitch, std::memory_order_relaxed);
        distance.store(editor_distance, std::memory_order_relaxed);
        height.store(editor_height, std::memory_order_relaxed);
    }
    bool pose(std::array<float, 16> &matrix) const noexcept {
        const std::array<float, 3> at{target[0].load(std::memory_order_relaxed), target[1].load(std::memory_order_relaxed),
                                      target[2].load(std::memory_order_relaxed)};
        return orbit_camera(matrix, yaw.load(std::memory_order_relaxed), pitch.load(std::memory_order_relaxed),
                            distance.load(std::memory_order_relaxed), at, height.load(std::memory_order_relaxed));
    }
};
} // namespace dingosdk::style_camera
