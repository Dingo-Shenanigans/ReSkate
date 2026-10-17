#pragma once
// Teleport zones a map ships in teleports.json: a box the local skater rides into, and the spot
// it comes out at. The rules on their own, without the game (map_teleports.cpp runs them; the
// tests are in Test/map_teleports_tests.cpp). World axes, metres, y up; headings in degrees
// about y, 0 facing +z and 90 facing +x, as the skater's own matrix gives them.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::map_teleports {
using Vec3 = std::array<float, 3>;

inline constexpr std::size_t max_zones = 256;
inline constexpr float max_coordinate = 1000000.0f;
// A move longer than this between two looks is a teleport, a respawn or a load, never a ride
// through a zone: the zones wait for the next look instead of testing the whole line.
inline constexpr float max_step = 25.0f;
// After a teleport nothing fires for this long, and not the zone the skater came out in until
// the skater has left it, so two zones that send to each other cannot bounce.
inline constexpr std::uint64_t settle_ms = 750;
// Below this the skater is not travelling anywhere: it comes out facing the spot's heading.
inline constexpr float still_speed = 0.5f;

struct Zone {
    std::string name;
    Vec3 center{}, half{}; // half sizes along the box's own axes
    float yaw{};           // the box turned about y
    Vec3 target{};
    float heading{};
    bool keep_speed{true};
    bool orient{true}; // false: the skater keeps its own heading and line, as THP8's zones without `orient`
};

inline float radians(float degrees) { return degrees * std::numbers::pi_v<float> / 180.0f; }
inline float degrees(float radians) { return radians * 180.0f / std::numbers::pi_v<float>; }
inline float wrap_heading(float degrees) {
    const float wrapped = std::fmod(degrees, 360.0f);
    return wrapped < 0 ? wrapped + 360.0f : wrapped;
}
// The way something moving with this velocity travels across the ground.
inline float travel_heading(const Vec3& velocity) { return wrap_heading(degrees(std::atan2(velocity[0], velocity[2]))); }
inline float ground_speed(const Vec3& velocity) { return std::hypot(velocity[0], velocity[2]); }
inline Vec3 turned(const Vec3& v, float by_degrees) {
    // A turn of the heading by `by_degrees`, the same sense the headings use.
    const float a = radians(by_degrees), c = std::cos(a), s = std::sin(a);
    return {v[0] * c + v[2] * s, v[1], v[2] * c - v[0] * s};
}
inline bool finite(const Vec3& v) {
    for (const float x : v)
        if (!std::isfinite(x) || std::abs(x) > max_coordinate) return false;
    return true;
}
inline bool valid(const Zone& zone) {
    if (!finite(zone.center) || !finite(zone.target) || !finite(zone.half) || !std::isfinite(zone.yaw) ||
        !std::isfinite(zone.heading)) return false;
    for (const float h : zone.half)
        if (!(h > 0.0f) || h > 10000.0f) return false;
    return true;
}
// A point in the box's own axes, measured from its centre.
inline Vec3 local(const Zone& zone, const Vec3& point) {
    const Vec3 offset{point[0] - zone.center[0], point[1] - zone.center[1], point[2] - zone.center[2]};
    return turned(offset, -zone.yaw);
}
inline bool inside(const Zone& zone, const Vec3& point) {
    const auto p = local(zone, point);
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(p[i]) > zone.half[i]) return false;
    return true;
}
// Whether the line the skater moved along since the last look touches the box. THP8's zones
// are flat panels across a pipe or a door, thinner than one frame of riding, so testing only
// where the skater is would miss most of them.
inline bool crossed(const Zone& zone, const Vec3& from, const Vec3& to) {
    const auto a = local(zone, from), b = local(zone, to);
    float first = 0.0f, last = 1.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        const float d = b[i] - a[i];
        if (std::abs(d) < 1e-6f) {
            if (std::abs(a[i]) > zone.half[i]) return false;
            continue;
        }
        float t0 = (-zone.half[i] - a[i]) / d, t1 = (zone.half[i] - a[i]) / d;
        if (t0 > t1) std::swap(t0, t1);
        first = std::max(first, t0);
        last = std::min(last, t1);
        if (first > last) return false;
    }
    return true;
}

// Where and how the skater comes out. Travel and facing turn together, so the skater leaves
// along the spot's heading at the speed it came in with, still fakie if it rode in fakie and
// still at the angle it had to its line; up and down speed are kept as they were.
struct Exit {
    Vec3 position{};
    float heading{};
    Vec3 velocity{};
    bool carry{};
};
inline Exit exit_for(const Zone& zone, float skater_heading, const Vec3& velocity) {
    Exit exit;
    exit.position = zone.target;
    const bool moving = finite(velocity) && ground_speed(velocity) >= still_speed;
    const float turn = !zone.orient ? 0.0f : moving ? zone.heading - travel_heading(velocity) : zone.heading - skater_heading;
    exit.heading = wrap_heading(skater_heading + turn);
    exit.carry = zone.keep_speed && moving;
    if (exit.carry) exit.velocity = turned(velocity, turn);
    return exit;
}

// Follows the skater from look to look and says when it rides into a zone.
class Tracker {
public:
    void reset() { last_.reset(); }
    // A teleport was sent: wait, and hold off the zone the spot is in until the skater leaves it.
    void teleported(const std::vector<Zone>& zones, const Vec3& target, std::uint64_t now) {
        last_.reset();
        quiet_until_ = now + settle_ms;
        held_.clear();
        for (std::size_t i = 0; i < zones.size(); ++i)
            if (inside(zones[i], target)) held_.push_back(i);
    }
    std::optional<std::size_t> step(const std::vector<Zone>& zones, const Vec3& position, std::uint64_t now) {
        if (!finite(position)) { last_.reset(); return std::nullopt; }
        const auto from = last_;
        last_ = position;
        // A held zone stays held for the step that leaves it.
        const auto held = held_;
        std::erase_if(held_, [&](std::size_t i) { return i >= zones.size() || !inside(zones[i], position); });
        if (!from || now < quiet_until_) return std::nullopt;
        const float dx = position[0] - (*from)[0], dy = position[1] - (*from)[1], dz = position[2] - (*from)[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > max_step) return std::nullopt;
        for (std::size_t i = 0; i < zones.size(); ++i) {
            if (std::find(held.begin(), held.end(), i) != held.end()) continue;
            if (crossed(zones[i], *from, position)) return i;
        }
        return std::nullopt;
    }

private:
    std::optional<Vec3> last_;
    std::uint64_t quiet_until_{};
    std::vector<std::size_t> held_;
};
}
