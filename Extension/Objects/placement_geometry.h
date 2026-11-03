#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace dingosdk::placement_geometry {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
using Pose = std::array<float, 12>;
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
    for (auto& v : a)
        v *= b;
    return a;
}
inline float dot(Vec3 a, Vec3 b) {
    float v{};
    for (unsigned i = 0; i < 3; ++i)
        v += a[i] * b[i];
    return v;
}
inline float distance2(Vec3 a, Vec3 b) {
    const auto d = sub(a, b);
    return dot(d, d);
}
inline Quat conjugate(Quat q) { return {-q[0], -q[1], -q[2], q[3]}; }
inline Quat product(Quat a, Quat b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Vec3 rotate(Quat q, Vec3 v) {
    const auto r = product(product(q, {v[0], v[1], v[2], 0}), conjugate(q));
    return {r[0], r[1], r[2]};
}
inline bool finite(const Pose& p) {
    for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(p[i]) || std::abs(p[i]) > 100000)
            return false;
    float n{};
    for (unsigned i = 4; i < 8; ++i)
        n += p[i] * p[i];
    return n > .95f && n < 1.05f && p[0] > .001f;
}
inline bool same_pose(const Pose& a, const Pose& b) {
    if (distance2({a[8], a[9], a[10]}, {b[8], b[9], b[10]}) > .000001f)
        return false;
    for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 7u})
        if (std::abs(a[i] - b[i]) > .0001f)
            return false;
    return true;
}
// Authored native edge ABI; preserve metadata while changing only coordinates.
struct Edge {
    std::array<float, 4> from{}, to{}, normal_a{}, normal_b{};
    std::array<std::uint32_t, 4> details{};
    std::array<std::uint64_t, 2> identity{};
    std::array<std::uint32_t, 4> flags{};
};
static_assert(sizeof(Edge) == 0x70);
inline Vec3 xyz(const std::array<float, 4>& v) { return {v[0], v[1], v[2]}; }
inline bool finite(const Edge& e) {
    for (const auto* v : {&e.from, &e.to, &e.normal_a, &e.normal_b})
        for (unsigned n = 0; n < 3; ++n)
            if (!std::isfinite((*v)[n]))
                return false;
    return true;
}
inline Edge transform(Edge e, Quat q, Vec3 p) {
    for (auto* point : {&e.from, &e.to}) {
        auto v = add(rotate(q, xyz(*point)), p);
        std::copy(v.begin(), v.end(), point->begin());
    }
    for (auto* normal : {&e.normal_a, &e.normal_b}) {
        auto v = rotate(q, xyz(*normal));
        std::copy(v.begin(), v.end(), normal->begin());
    }
    return e;
}
inline bool overlaps(const Edge& e, Vec3 c, Vec3 h) {
    for (unsigned i = 0; i < 3; ++i)
        if (std::max(e.from[i], e.to[i]) < c[i] - h[i] || std::min(e.from[i], e.to[i]) > c[i] + h[i])
            return false;
    return true;
}
inline bool same_edge(const Edge& a, const Edge& b) { return a.identity == b.identity && a.details == b.details; }
} // namespace dingosdk::placement_geometry
