#pragma once
// Letting other players in a lobby see where your FPV drone is (they need this build too).
// The flight tick publishes the drone's position; the session sends it about 15 times a second as
// a small `drone` packet (Multiplayer/Net/protocol.h); the receiver glides each player's marker from
// one position to the next, and the overlay draws it (UI/Overlay/drone_hud_overlay.cpp).
// Header-only and free of game code, so the networking can use it without pulling in the flight model.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::drone::beacon {
using Clock = std::chrono::steady_clock;
using Position = std::array<float, 3>;
// Which way a drone faces: a unit quaternion (x, y, z, w) of its body, nose and all, without
// the camera's tilt. For drawing a model later; markers only need the position.
using Rotation = std::array<float, 4>;
inline constexpr Rotation no_rotation{0.0f, 0.0f, 0.0f, 1.0f};

inline bool valid_position(const Position& p) {
    for (const float v : p) if (!std::isfinite(v) || std::abs(v) > 1.0e6f) return false;
    return true;
}
inline bool valid_rotation(const Rotation& q) {
    float length = 0.0f;
    for (const float v : q) { if (!std::isfinite(v)) return false; length += v * v; }
    return length > 0.81f && length < 1.21f; // a unit quaternion, give or take rounding
}
// From a body matrix (rows: right, up, back, as the flight model keeps them) and back.
inline Rotation rotation_from_basis(const std::array<float, 16>& m) {
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[4], m11 = m[5], m12 = m[6], m20 = m[8], m21 = m[9], m22 = m[10];
    Rotation q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m12 - m21) / s, (m20 - m02) / s, (m01 - m10) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m20 + m02) / s, (m12 - m21) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m20 - m02) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m20 + m02) / s, (m12 + m21) / s, 0.25f * s, (m01 - m10) / s};
    }
    float length = 0.0f;
    for (const float v : q) length += v * v;
    length = std::sqrt(length);
    if (!(length > 1e-6f) || !std::isfinite(length)) return no_rotation;
    for (auto& v : q) v /= length;
    return q;
}
// The body's right, up and back rows for a rotation.
inline std::array<Position, 3> basis_from_rotation(const Rotation& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
             {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
             {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}}};
}
// Part way from one rotation to another (normalised linear blend, the short way round).
inline Rotation blend_rotation(const Rotation& a, Rotation b, float t) {
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) for (auto& v : b) v = -v;
    Rotation q{};
    float length = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) { q[i] = a[i] + (b[i] - a[i]) * t; length += q[i] * q[i]; }
    length = std::sqrt(length);
    if (!(length > 1e-6f)) return b;
    for (auto& v : q) v /= length;
    return q;
}

// Whether the session's dedicated server lets drones be seen (its `sync_drones` setting). A
// lobby always does. Off: this game neither sends its drone nor shows anyone else's.
inline std::atomic<bool>& server_allows() { static std::atomic<bool> value{true}; return value; }

// ---- Sending: what this player's drone is doing.
struct Local {
    std::mutex mutex;
    bool share = false;           // the `beacon` setting
    Position position{};
    Rotation rotation = no_rotation;
    Clock::time_point updated{};
    Clock::time_point last_sent{};
    bool announced = false;       // the others were last told "flying"
};
inline Local& local() { static Local value; return value; }

// Called every flight tick.
inline void publish_local(const Position& position, const Rotation& rotation, bool share) {
    auto& l = local();
    std::lock_guard lock(l.mutex);
    l.share = share;
    l.position = position;
    l.rotation = valid_rotation(rotation) ? rotation : no_rotation;
    l.updated = Clock::now();
}

struct Outgoing { bool active; Position position; Rotation rotation; };
// How often a flying drone's position goes out.
inline constexpr auto send_interval = std::chrono::milliseconds(66);
// Asked by the network every tick: a position to send now (about 15 times a second while
// flying, and once to say it stopped), or nothing.
inline std::optional<Outgoing> next_outgoing(Clock::time_point now = Clock::now()) {
    auto& l = local();
    std::lock_guard lock(l.mutex);
    const bool flying = l.share && server_allows().load(std::memory_order_relaxed) && l.updated != Clock::time_point{} && now - l.updated < std::chrono::milliseconds(300) &&
                        valid_position(l.position);
    if (flying) {
        if (l.announced && now - l.last_sent < send_interval) return std::nullopt;
        l.announced = true;
        l.last_sent = now;
        return Outgoing{true, l.position, l.rotation};
    }
    if (l.announced) {
        l.announced = false;
        l.last_sent = now;
        return Outgoing{false, l.position, l.rotation};
    }
    return std::nullopt;
}

// ---- Receiving: other players' drones.
// A marker glides from where it was shown when a position arrived to that new position, over the
// time the last update took to come. So it moves smoothly, about one update behind the drone,
// instead of jumping a few times a second.
struct Remote {
    std::uint64_t id{};
    std::string name;
    Position position{};          // in a snapshot: where to draw the marker now
    Clock::time_point seen{};     // when the latest position arrived
    Position from{}, to{};        // the glide: from the shown position to the latest one
    Clock::duration glide{};      // how long the glide takes
    Rotation rotation = no_rotation;                     // in a snapshot: which way it faces now
    Rotation from_rotation = no_rotation, to_rotation = no_rotation;
    // For its motor sound (Multiplayer/Remote/drone_sound.h): how fast it goes and the throttle it
    // seems to be on, worked out from how it moves (estimate_throttle), as packets carry no stick.
    Position velocity{};
    float speed = 0.0f;
    float throttle = 0.0f;
};
// The throttle a drone is on, from how it accelerates and which way its props point: thrust is the
// acceleration it has besides gravity, along its up axis. Thrust goes with output^1.5 and hovering
// takes `hover` (drone_flight.h), so output = hover * (thrust / g)^(2/3). Drag is left out: a fast
// drone sounds a little less busy than it is.
inline constexpr float assumed_hover = 0.35f, assumed_gravity = 9.81f;
inline float estimate_throttle(const Position& acceleration, const Rotation& facing) {
    const auto up = basis_from_rotation(facing)[1];
    const float thrust = acceleration[0] * up[0] + (acceleration[1] + assumed_gravity) * up[1] + acceleration[2] * up[2];
    if (!std::isfinite(thrust) || thrust <= 0.0f) return 0.0f;
    return std::clamp(assumed_hover * std::pow(thrust / assumed_gravity, 2.0f / 3.0f), 0.0f, 1.0f);
}
inline float glide_fraction(const Remote& r, Clock::time_point now) {
    if (r.glide <= Clock::duration::zero()) return 1.0f;
    return std::clamp(std::chrono::duration<float>(now - r.seen) / std::chrono::duration<float>(r.glide), 0.0f, 1.0f);
}
inline Position glide_position(const Remote& r, Clock::time_point now) {
    const float t = glide_fraction(r, now);
    return {r.from[0] + (r.to[0] - r.from[0]) * t, r.from[1] + (r.to[1] - r.from[1]) * t, r.from[2] + (r.to[2] - r.from[2]) * t};
}
// Far jumps (a reset, a teleport) are shown at once rather than slid across the map.
inline constexpr float snap_distance = 50.0f;
struct Remotes {
    std::mutex mutex;
    std::vector<Remote> list;
};
inline Remotes& remotes() { static Remotes value; return value; }
constexpr std::size_t max_remote = 32;
inline constexpr auto stale_after = std::chrono::milliseconds(1500);

inline void receive(std::uint64_t id, std::string name, const Position& position, const Rotation& rotation, bool active,
                    Clock::time_point now = Clock::now()) {
    if (!id || !valid_position(position)) return;
    const Rotation facing = valid_rotation(rotation) ? rotation : no_rotation;
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    for (auto it = r.list.begin(); it != r.list.end(); ++it)
        if (it->id == id) {
            if (!active) { r.list.erase(it); return; }
            if (!name.empty()) it->name = std::move(name);
            const auto shown = glide_position(*it, now);
            float jump = 0.0f;
            for (std::size_t i = 0; i < 3; ++i) jump += (position[i] - shown[i]) * (position[i] - shown[i]);
            // The glide lasts as long as this update took to arrive, within sensible bounds.
            const auto gap = std::clamp<Clock::duration>(now - it->seen, std::chrono::milliseconds(20),
                                                         std::chrono::milliseconds(250));
            const bool snap = jump > snap_distance * snap_distance;
            // Velocity and acceleration between the last two positions as they arrived; smoothed,
            // as arrival times jitter. A long gap or a jump starts over.
            const float dt = std::chrono::duration<float>(now - it->seen).count();
            if (!snap && dt >= 0.02f && dt <= 0.5f) {
                Position velocity{}, acceleration{};
                float speed = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    velocity[i] = (position[i] - it->to[i]) / dt;
                    acceleration[i] = (velocity[i] - it->velocity[i]) / dt;
                    speed += velocity[i] * velocity[i];
                }
                const float throttle = estimate_throttle(acceleration, facing);
                it->throttle += (throttle - it->throttle) * 0.35f;
                it->speed += (std::sqrt(speed) - it->speed) * 0.5f;
                it->velocity = velocity;
            } else {
                it->velocity = {};
                it->speed = 0.0f;
                it->throttle = assumed_hover;
            }
            it->from_rotation = snap ? facing : blend_rotation(it->from_rotation, it->to_rotation, glide_fraction(*it, now));
            it->to_rotation = facing;
            it->from = snap ? position : shown;
            it->to = position;
            it->position = position;
            it->glide = gap;
            it->seen = now;
            return;
        }
    if (!active || r.list.size() >= max_remote) return;
    Remote added{id, std::move(name), position, now, position, position, {}, facing, facing, facing};
    added.throttle = assumed_hover;
    r.list.push_back(std::move(added));
}
// Someone left the session.
inline void forget(std::uint64_t id) {
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [id](const Remote& x) { return x.id == id; });
}
// Whether the player `id` is flying a drone now, by their beacon.
inline bool flying(std::uint64_t id, Clock::time_point now = Clock::now()) {
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    for (const auto& x : r.list)
        if (x.id == id) return now - x.seen <= stale_after;
    return false;
}
// The drones to show now (stale ones are dropped).
inline std::vector<Remote> snapshot(Clock::time_point now = Clock::now()) {
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [now](const Remote& x) { return now - x.seen > stale_after; });
    auto out = r.list;
    for (auto& x : out) {
        x.position = glide_position(x, now);
        x.rotation = blend_rotation(x.from_rotation, x.to_rotation, glide_fraction(x, now));
    }
    return out;
}
} // namespace dingosdk::drone::beacon
