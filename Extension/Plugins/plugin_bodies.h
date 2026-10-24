#pragma once
#include "plugin_game.h"
#include <algorithm>
#include <cmath>
#include <optional>

// Touching and the raycast at players (reskate.session.touch, reskate.session.raycast), on the
// skaters' bodies as capsules (Game::bodies). Header only.
namespace dingosdk::plugins::bodies {
namespace detail {
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// The closest points of segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9).
inline std::pair<Vec3, Vec3> closest(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    const auto d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    constexpr float tiny = 1e-8f;
    float s{}, t{};
    if (a <= tiny && e <= tiny) return {p1, p2};
    if (a <= tiny) t = std::clamp(f / e, 0.f, 1.f);
    else {
        const float c = dot(d1, r);
        if (e <= tiny) s = std::clamp(-c / a, 0.f, 1.f);
        else {
            const float b = dot(d1, d2), denom = a * e - b * b;
            s = denom > tiny ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.f, 1.f);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.f, 1.f);
            }
        }
    }
    return {add(p1, scale(d1, s)), add(p2, scale(d2, t))};
}
// Where along from + t * d (t 0..1) the ray first enters a sphere, or empty.
inline std::optional<float> enter_sphere(const Vec3& from, const Vec3& d, const Vec3& centre, float radius) {
    const auto m = sub(from, centre);
    const float a = dot(d, d), b = dot(m, d), c = dot(m, m) - radius * radius;
    if (c <= 0) return 0.f; // starts inside
    if (a <= 1e-12f || b > 0) return std::nullopt;
    const float disc = b * b - a * c;
    if (disc < 0) return std::nullopt;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t > 1) return std::nullopt;
    return std::max(t, 0.f);
}
// The same for a capsule: its two end spheres and the cylinder between them.
inline std::optional<float> enter_capsule(const Vec3& from, const Vec3& d, const BodyCapsule& c) {
    std::optional<float> best;
    const auto keep = [&](std::optional<float> t) {
        if (t && (!best || *t < *best)) best = t;
    };
    keep(enter_sphere(from, d, c.a, c.radius));
    keep(enter_sphere(from, d, c.b, c.radius));
    const auto axis = sub(c.b, c.a);
    const float length2 = dot(axis, axis);
    if (length2 > 1e-8f) {
        // The ray and the axis with the axis taken out: a circle in the plane across it.
        const auto m = sub(from, c.a);
        const auto across = [&](const Vec3& v) { return sub(v, scale(axis, dot(v, axis) / length2)); };
        const auto dp = across(d), mp = across(m);
        const float a = dot(dp, dp), b = dot(mp, dp), k = dot(mp, mp) - c.radius * c.radius;
        if (a > 1e-12f) {
            const float disc = b * b - a * k;
            if (disc >= 0) {
                const float t = std::max((-b - std::sqrt(disc)) / a, 0.f);
                if (t <= 1) {
                    const float along = dot(add(m, scale(d, t)), axis) / length2;
                    if (along >= 0 && along <= 1 && (k > 0 || t == 0)) keep(t);
                }
            }
        }
    }
    return best;
}
}

// The nearest two capsules of two bodies: the gap between their surfaces (negative where they
// overlap) and the point halfway across it.
struct Nearest {
    std::uint8_t mine{}, theirs{};
    float gap{};
    Vec3 at{};
};
inline std::optional<Nearest> nearest(const Body& mine, const Body& theirs) {
    std::optional<Nearest> best;
    for (const auto& a : mine.capsules)
        for (const auto& b : theirs.capsules) {
            const auto [pa, pb] = detail::closest(a.a, a.b, b.a, b.b);
            const auto apart = detail::sub(pb, pa);
            const float gap = std::sqrt(detail::dot(apart, apart)) - a.radius - b.radius;
            if (!best || gap < best->gap) best = Nearest{a.part, b.part, gap, detail::scale(detail::add(pa, pb), 0.5f)};
        }
    return best;
}

// The first body part of another player (not player 0) a ray from `from` to `to` enters.
struct RayHit {
    std::uint64_t player{};
    std::uint8_t part{};
    Vec3 position{};
    float distance{};
};
inline std::optional<RayHit> ray_hit(const std::vector<Body>& list, const Vec3& from, const Vec3& to) {
    const auto d = detail::sub(to, from);
    const float length = std::sqrt(detail::dot(d, d));
    std::optional<RayHit> best;
    for (const auto& body : list) {
        if (!body.player) continue;
        for (const auto& c : body.capsules)
            if (const auto t = detail::enter_capsule(from, d, c); t && (!best || *t * length < best->distance))
                best = RayHit{body.player, c.part, detail::add(from, detail::scale(d, *t)), *t * length};
    }
    return best;
}
}
