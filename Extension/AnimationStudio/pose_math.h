#pragma once
#include "Extension/AnimationStudio/anim_core.h"
#include <algorithm>
#include <cmath>

// Vector and quaternion steps on the animation clips' Vec3 and Quat, for code that places
// joints by hand (arm reach, carrying, board poses). In their own namespace, so a file takes the
// ones it uses by name and keeps its own helpers beside them.
namespace dingosdk::anim_studio::pose_math {

inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 scaled(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
// `a` at length 1, or `fallback` when it is too short (or not finite) to have a direction.
inline Vec3 unit(const Vec3& a, const Vec3& fallback) {
    const float l = length(a);
    return l > 1e-6f && std::isfinite(l) ? scaled(a, 1.0f / l) : fallback;
}
inline bool finite(const Vec3& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline bool finite(const Quat& q) {
    return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]);
}
// The shortest turn taking direction `from` onto direction `to`.
inline Quat arc(const Vec3& from, const Vec3& to) {
    const Vec3 a = unit(from, {0, 0, 1}), b = unit(to, {0, 0, 1});
    const float d = dot(a, b);
    if (d < -0.9999f) { // half a turn: about any axis square to `from`
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-3f) axis = cross({0, 1, 0}, a);
        axis = unit(axis, {0, 1, 0});
        return {axis[0], axis[1], axis[2], 0.0f};
    }
    const Vec3 c = cross(a, b);
    return normalised({c[0], c[1], c[2], 1.0f + d});
}
// Part way from a to b, the short way round.
inline Quat blend(const Quat& a, Quat b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f)
        for (auto& v : b) v = -v;
    return normalised({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
}

} // namespace dingosdk::anim_studio::pose_math
