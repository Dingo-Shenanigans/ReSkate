#pragma once
// The decisions behind a ground-safe arrival (the pause-map waypoint, `trainer ground`) and
// the remembered waypoint. They have no game in them, so the trainer tests run them on their
// own (Test/landing_tests.cpp); trainer.cpp feeds them positions, times and ray results.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace dingosdk::trainer::landing {
using Vec3 = std::array<float, 3>;

// "The topmost surface at x, z" is a downward ray: its closest hit is the highest surface (a
// deck or a roof wins over the street under it). The first ray starts well above the height
// the caller knows of; a custom map can sit higher than that, so a miss is cast once more from
// far above, down to where the first one started.
constexpr float ray_ceiling = 1500.0f, ray_floor = -1500.0f, ray_retry_ceiling = 20000.0f;
// Above the surface: the game's teleport settles the skater from here.
constexpr float stand_height = 0.5f;
struct Ray { float top, bottom; };
inline Ray first_ray(float hint_y) noexcept {
    const float hint = std::isfinite(hint_y) ? std::clamp(hint_y, ray_floor, ray_retry_ceiling - 500.0f) : 0.0f;
    return {std::max(ray_ceiling, hint + 500.0f), ray_floor};
}
inline std::optional<Ray> retry_ray(const Ray &first) noexcept {
    if (first.top >= ray_retry_ceiling) return std::nullopt;
    return Ray{ray_retry_ceiling, first.top};
}

// After a ground-snapped teleport, collision at the spot may still be streaming in. The
// skater is watched for a while and put back on the surface if it sinks through it.
constexpr std::uint64_t watch_ms = 12000, first_look_ms = 1500, look_every_ms = 150, after_fix_ms = 1500;
constexpr std::uint64_t resend_every_ms = 2500, resend_extends_ms = 8000;
constexpr int max_fixes = 6, max_resends = 3;
constexpr float arrived_radius = 30.0f, sunk_below = 1.0f, landed_within = 2.5f, settled_speed = 1.5f;

struct Watch {
    bool active{};
    float x{}, z{}, y{}; // where the skater was sent
    std::uint64_t until{}, next{}, resend_at{};
    int fixes{}, resends{};
};
inline Watch start(float x, float z, float y, std::uint64_t now) noexcept {
    return Watch{.active = true, .x = x, .z = z, .y = y, .until = now + watch_ms, .next = now + first_look_ms,
                 .resend_at = now + resend_every_ms, .fixes = 0, .resends = max_resends};
}

struct Sample {
    std::uint64_t now{};
    Vec3 position{};
    float vertical{};            // vertical speed, up is positive
    std::optional<float> ground; // the topmost surface at the skater's x, z, if known yet
    bool may_resend{true};       // the game will act on a teleport now (not paused on a menu)
};
enum class Step {
    wait,     // nothing to do yet
    resend,   // the teleport never arrived: send it to the watch's x, y, z again
    put_back, // sank under the surface: teleport to the ground at the skater's x, z
    landed,   // standing on the surface; the watch is over
    ended,    // gave up (timed out or corrected too often); the watch is over
};
inline Step step(Watch &w, const Sample &s) noexcept {
    if (!w.active || s.now < w.next) return Step::wait;
    w.next = s.now + look_every_ms;
    if (s.now > w.until || w.fixes >= max_fixes) {
        w.active = false;
        return Step::ended;
    }
    const float dx = s.position[0] - w.x, dz = s.position[2] - w.z;
    if (dx * dx + dz * dz > arrived_radius * arrived_radius) {
        if (w.resends > 0 && s.may_resend && s.now >= w.resend_at) {
            --w.resends;
            w.resend_at = s.now + resend_every_ms;
            w.until = std::max(w.until, s.now + resend_extends_ms);
            return Step::resend;
        }
        return Step::wait;
    }
    if (!s.ground) return Step::wait; // collision still streaming in
    if (s.position[1] < *s.ground - sunk_below) {
        ++w.fixes;
        w.next = s.now + after_fix_ms;
        return Step::put_back;
    }
    if (std::abs(s.position[1] - *s.ground) < landed_within && std::abs(s.vertical) < settled_speed) {
        w.active = false;
        return Step::landed;
    }
    return Step::wait;
}

// One read of the pause map's point-of-interest registry.
struct Reading {
    bool read{};                   // the registry was found and walked
    unsigned typed{};              // map POIs whose kind was read (0: the map has not built them)
    std::optional<Vec3> waypoint;  // the player's waypoint, if one is placed
};
// What to remember after a read. A registry the map has filled but without a waypoint means
// the player removed it; one that is missing, unreadable or empty says nothing, so the last
// waypoint seen is kept.
inline std::optional<Vec3> remembered(const std::optional<Vec3> &before, const Reading &r) noexcept {
    if (r.waypoint) return r.waypoint;
    if (r.read && r.typed > 0) return std::nullopt;
    return before;
}
} // namespace dingosdk::trainer::landing
