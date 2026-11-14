#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace dingosdk::editor {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
inline constexpr float pi = 3.14159265358979323846f;
inline Vec3 add(Vec3 a, Vec3 b) {
    for (unsigned i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
inline Vec3 sub(Vec3 a, Vec3 b) {
    for (unsigned i = 0; i < 3; ++i)
        a[i] -= b[i];
    return a;
}
inline Vec3 mul(Vec3 a, float b) {
    for (auto &v : a)
        v *= b;
    return a;
}
inline float dot(Vec3 a, Vec3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 normalized(Vec3 a) {
    const auto n = std::sqrt(dot(a, a));
    return n > 1e-6f ? mul(a, 1 / n) : Vec3{};
}
inline float snapped(float x, float step) {
    return step > 0 && std::isfinite(step) ? std::round(x / step) * step : x;
}
inline Quat rotation(Vec3 degrees) {
    for (auto &v : degrees)
        v *= pi / 360;
    const float sx = std::sin(degrees[0]), cx = std::cos(degrees[0]), sy = std::sin(degrees[1]),
                cy = std::cos(degrees[1]), sz = std::sin(degrees[2]), cz = std::cos(degrees[2]);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz};
}
inline Vec3 angles(Quat q) {
    const auto [x, y, z, w] = q;
    return {std::atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * 180 / pi,
            std::asin(std::clamp(2 * (w * y - z * x), -1.0f, 1.0f)) * 180 / pi,
            std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * 180 / pi};
}
inline Vec3 rotated(Vec3 v, Quat q) {
    Vec3 u{q[0], q[1], q[2]};
    const auto t = mul(cross(u, v), 2);
    return add(v, add(mul(t, q[3]), cross(u, t)));
}
inline Quat multiplied(Quat a, Quat b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
struct Camera {
    std::array<float, 16> world{};
    float vertical_fov{}, width{}, height{};
    bool valid() const {
        if (!std::isfinite(vertical_fov) || vertical_fov < 1 || vertical_fov > 175 || width < 1 || height < 1)
            return false;
        // The rows' fourth lanes are SIMD padding the camera never reads, and the game leaves
        // anything in them: after skating or turning the camera they held values that are not
        // numbers, which made this refuse a good camera, and the editor would not fly (until a
        // pause rewrote them).
        for (std::size_t i = 0; i < world.size(); ++i)
            if (i % 4 != 3 && !std::isfinite(world[i]))
                return false;
        return true;
    }
    Vec3 origin() const {
        return {world[12], world[13], world[14]};
    }
    Vec3 basis(unsigned axis) const {
        return {world[axis * 4], world[axis * 4 + 1], world[axis * 4 + 2]};
    }
    std::optional<std::array<float, 2>> project(Vec3 point) const {
        if (!valid())
            return {};
        const auto delta = sub(point, origin());
        const auto depth = -dot(delta, basis(2));
        if (depth < .05f)
            return {};
        const auto scale = height / (2 * std::tan(vertical_fov * pi / 360) * depth);
        return std::array<float, 2>{width * .5f + dot(delta, basis(0)) * scale,
                                    height * .5f - dot(delta, basis(1)) * scale};
    }
    Vec3 ray(float x, float y) const {
        if (!valid())
            return {};
        const auto t = std::tan(vertical_fov * pi / 360);
        return normalized(add(mul(basis(2), -1), add(mul(basis(0), (2 * x - width) / height * t),
                                                     mul(basis(1), (1 - 2 * y / height) * t))));
    }
};
inline std::optional<Vec3> plane_hit(Vec3 origin, Vec3 ray, Vec3 point, Vec3 normal) {
    const auto denom = dot(ray, normal);
    if (std::abs(denom) < 1e-5f)
        return {};
    const auto distance = dot(sub(point, origin), normal) / denom;
    if (!std::isfinite(distance) || distance < .05f || distance > 5000)
        return {};
    return add(origin, mul(ray, distance));
}
// Closest point on an axis to the cursor ray, in world units from its origin.
inline std::optional<float> axis_parameter(Vec3 origin, Vec3 ray, Vec3 point, Vec3 axis) {
    const auto b = dot(ray, axis), denom = 1 - b * b;
    if (denom < .001f)
        return {};
    const auto delta = sub(origin, point);
    return (dot(delta, axis) - b * dot(delta, ray)) / denom;
}
} // namespace dingosdk::editor
