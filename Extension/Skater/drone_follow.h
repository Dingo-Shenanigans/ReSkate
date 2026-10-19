#pragma once
// Follow mode: the FPV drone flies itself behind the skater while the player skates, and the
// view is its camera. It is not a chase camera placed each frame: an autopilot works the same
// sticks a pilot would (angle mode with height hold) through the real flight model
// (drone_flight.h), so it has to lean to speed up, overshoots a turn, swings round to keep the
// skater in shot and climbs over what is in the way, the way someone flying it would.
//
// It sees the skater a reaction time late and moves its sticks with a hand's smoothness and a
// little wander, which keeps the shot from looking machine-perfect. When the skater rides back and
// forth in one place (a halfpipe, a bowl, laps of a spot) it stops chasing and holds a spot off to
// the side to film the session. It looks where it flies and keeps off walls and ceilings rather
// than flying into them. Plain C++, testable off the game.
#include "drone_flight.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::drone::follow {
using namespace dingosdk::drone::detail; // Vec3 and the vector helpers of the flight model

struct Tuning {
    float distance = 4.5f;     // m behind the skater
    float height = 1.4f;       // m above the skater
    float lead = 0.15f;        // s: aims where the skater will be this soon (about its reaction, so it sits `distance` back at any speed)
    float reaction = 0.12f;    // s: how late the pilot sees what the skater does
    float hands = 0.10f;       // s: how smoothly the sticks move
    float wander = 0.035f;     // stick unsteadiness, of full stick
    float max_lean_deg = 40.0f;
    float brake = 2.0f;        // m/s/s the pilot plans to slow down at when closing a gap
    float catch_up = 6.0f;     // m/s faster than the skater it closes a gap at, most
    float gap_gain = 0.8f;     // 1/s: how fast it closes a gap to its spot
    float speed_gain = 1.5f;   // 1/s: how hard it leans to match the speed it wants
    float uptilt_deg = 5.0f;   // camera tilt on the drone without a gimbal (the flight model's own)
    float tilt_ease = 0.4f;    // s: how smoothly the gimbal tilts to keep the skater in the middle
    float session_distance = 7.0f; // m from the middle of a session it films from, at least
    float margin = 1.2f;       // m it keeps from walls and ceilings
    float max_yaw_rate = 4.5f; // rad/s the pilot turns the drone at most
    float clearance = 1.4f;    // m the drone keeps above the ground
};

// The tuning for a follow distance (m) and how aggressively the pilot flies (0 calm, 0.5 as above,
// 1 hard). Aggression is how quickly the pilot sees and answers the skater, how hard it closes a
// gap, leans and turns. Further back it also flies a little higher, and films a session from
// further away.
inline Tuning tuning_for(float distance, float aggression) {
    const auto mix = [a = std::clamp(std::isfinite(aggression) ? aggression : 0.5f, 0.0f, 1.0f)](float calm, float mid, float hard) {
        return a < 0.5f ? calm + (mid - calm) * (a / 0.5f) : mid + (hard - mid) * ((a - 0.5f) / 0.5f);
    };
    Tuning t;
    t.distance = std::clamp(std::isfinite(distance) ? distance : t.distance, 1.0f, 30.0f);
    t.height = 1.0f + 0.09f * t.distance;
    t.session_distance = std::max(t.distance + 2.5f, 5.0f);
    t.reaction = mix(0.20f, 0.12f, 0.06f);
    t.hands = mix(0.16f, 0.10f, 0.05f);
    t.gap_gain = mix(0.5f, 0.8f, 1.2f);
    t.speed_gain = mix(1.0f, 1.5f, 2.0f);
    t.catch_up = mix(4.0f, 6.0f, 10.0f);
    t.brake = mix(1.2f, 2.0f, 3.5f);
    t.max_lean_deg = mix(25.0f, 40.0f, 55.0f);
    t.max_yaw_rate = mix(2.5f, 4.5f, 6.0f);
    t.tilt_ease = mix(0.6f, 0.4f, 0.25f);
    return t;
}

// The pilot's memory between ticks.
struct Pilot {
    bool valid = false;
    Vec3 last_target{};                 // the skater's position last tick
    Vec3 target_velocity{};             // smoothed
    Vec3 heading{0, 0, -1};             // which way the skater is going, level
    struct Seen { float age; Vec3 position, velocity; };
    std::array<Seen, 48> seen{};        // what the skater did lately, newest at `head`
    std::size_t head = 0, count = 0;
    float ride_y = 0;                   // the skater's height, without ollies and bumps
    float climb_extra = 0;              // extra height while something blocks the shot
    float blocked_for = 0;              // s the shot has been blocked
    float floor_lift = 0;               // extra height to clear the ground under the spot
    float pitch = 0, roll = 0, yaw = 0, climb = 0; // stick positions now
    float tilt = 0;                     // the camera gimbal's pitch, radians, + = up
    // The skater every 0.1 s over the last six seconds, to tell a session (riding back and forth
    // in one place) from a line.
    struct Sample { Vec3 position, velocity; };
    std::array<Sample, 60> history{};
    std::size_t history_head = 0, history_count = 0;
    float history_clock = 0;
    bool session = false;               // holding a spot to film a session
    float session_for = 0;              // s it has held it
    Vec3 session_spot{}, session_middle{}, session_side{};
    float session_reach = 0;            // m from the middle the riding goes along its axis
    // m to the nearest surface each way around the drone (+x, -x, +z, -z, up); 0 = nothing near.
    std::array<float, 5> nearby{};
    std::size_t near_next = 0;
    std::uint32_t noise = 0x9e3779b9u;
    Vec3 wander_now{}, wander_to{};     // slow unsteadiness, eased toward a new value now and then
    float wander_time = 0;
};

// Where the drone wants to be: behind the skater along its heading, up and a little ahead in time.
inline Vec3 spot(const Vec3& target, const Vec3& velocity, const Vec3& heading, const Tuning& t, float extra) {
    const Vec3 ahead = add(target, scale(velocity, t.lead));
    return add(add(ahead, scale(heading, -t.distance)), Vec3{0, t.height + extra, 0});
}

// A drone pose (camera matrix rows: right, up, back, position) hovering at `at`, facing `look`.
inline std::array<float, 16> pose_facing(const Vec3& at, const Vec3& look) {
    Vec3 forward = detail::sub(look, at);
    forward[1] = 0.0f;
    forward = detail::length(forward) > 1e-3f ? normalize(forward) : Vec3{0, 0, -1};
    const Vec3 right{-forward[2], 0.0f, forward[0]};
    std::array<float, 16> m{};
    set_row(m, 0, right);
    set_row(m, 1, Vec3{0, 1, 0});
    set_row(m, 2, scale(forward, -1.0f));
    m[12] = at[0]; m[13] = at[1]; m[14] = at[2]; m[15] = 1.0f;
    return m;
}

// Reads the skater's last few seconds: riding back and forth in one place starts a session (the
// drone holds a spot off to the side), riding away or stopping for a while ends it.
inline void read_session(Pilot& p, const Vec3& drone_at, const Tuning& t, const World* world) {
    const std::size_t n = std::min<std::size_t>(p.history_count, 50); // the last five seconds
    if (n < 30) { p.session = false; return; }
    const auto at = [&](std::size_t back) -> const Pilot::Sample& {
        return p.history[(p.history_head + p.history.size() - back) % p.history.size()];
    };
    // The way it rides most (across a halfpipe), from how its velocity spreads.
    float xx = 0, xz = 0, zz = 0, low = 1e9f, high = -1e9f, path = 0;
    Vec3 middle{};
    for (std::size_t back = 0; back < n; ++back) {
        const auto& sample = at(back);
        xx += sample.velocity[0] * sample.velocity[0];
        xz += sample.velocity[0] * sample.velocity[2];
        zz += sample.velocity[2] * sample.velocity[2];
        middle = add(middle, sample.position);
        low = std::min(low, sample.position[1]);
        high = std::max(high, sample.position[1]);
        if (back) path += std::hypot(sample.position[0] - at(back - 1).position[0], sample.position[2] - at(back - 1).position[2]);
    }
    middle = scale(middle, 1.0f / static_cast<float>(n));
    const float angle = 0.5f * std::atan2(2.0f * xz, xx - zz);
    const Vec3 axis{std::cos(angle), 0.0f, std::sin(angle)};
    const float half = 0.5f * (xx + zz), spread = std::sqrt(std::max(half * half - (xx * zz - xz * xz), 0.0f));
    const bool line = half + spread > 3.0f * std::max(half - spread, 1e-3f); // one way and back, not round
    int reversals = 0;
    std::size_t since_reversal = n;
    float sign = 0, lo = 1e9f, hi = -1e9f;
    for (std::size_t back = n; back-- > 0;) {
        const auto& sample = at(back);
        const float along = detail::dot(sample.velocity, axis);
        const float x = detail::dot(detail::sub(sample.position, middle), axis);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        if (std::abs(along) < 1.5f) continue;
        const float now = along > 0.0f ? 1.0f : -1.0f;
        if (sign != 0.0f && now != sign) { ++reversals; since_reversal = back; }
        sign = now;
    }
    const Vec3 first = at(n - 1).position, last = at(0).position;
    const float net = std::hypot(last[0] - first[0], last[2] - first[2]);
    const bool back_and_forth = reversals >= 2 && path > 8.0f && net < 0.4f * path;
    if (!p.session) {
        if (!back_and_forth) return;
        p.session = true;
        p.session_for = 0.0f;
        // Watched from off to the side of the way it rides (down the length of a halfpipe), on the
        // side the drone is already on; round riding (a bowl) from where the drone already is.
        const Vec3 across{-axis[2], 0.0f, axis[0]};
        Vec3 from{drone_at[0] - middle[0], 0.0f, drone_at[2] - middle[2]};
        if (line) p.session_side = detail::dot(from, across) >= 0.0f ? across : scale(across, -1.0f);
        else p.session_side = detail::length(from) > 0.5f ? normalize(from) : across;
    } else {
        // Ends: no turn back for a while, or the skater has left the middle.
        const float out = std::hypot(last[0] - p.session_middle[0], last[2] - p.session_middle[2]);
        if (p.session_for > 3.0f && (since_reversal > 35 || out > p.session_reach + 6.0f)) { p.session = false; return; }
        if (line) {
            const Vec3 across{-axis[2], 0.0f, axis[0]};
            p.session_side = detail::dot(across, p.session_side) >= 0.0f ? across : scale(across, -1.0f);
        }
    }
    p.session_middle = middle;
    p.session_reach = std::max(-lo, hi);
    // Far enough to see both ends of the riding, high enough to see the airs.
    const float distance = std::clamp(0.6f * (hi - lo), t.session_distance, 14.0f);
    const float height = low + std::clamp(0.5f * (high - low) + 1.5f, 1.8f, 6.0f);
    const Vec3 look_from{middle[0], height, middle[2]};
    const auto place = [&](const Vec3& side, float& room) {
        Vec3 spot = add(look_from, scale(side, distance));
        room = distance;
        Vec3 hit{};
        if (world && world->cast && world->cast(look_from, add(spot, scale(side, t.margin)), hit)) {
            room = std::max(detail::length(detail::sub(hit, look_from)) - t.margin, 0.5f);
            spot = add(look_from, scale(side, room));
        }
        return spot;
    };
    float room = 0;
    p.session_spot = place(p.session_side, room);
    if (room < 3.0f) { // boxed in on this side: the other side, if it is any better
        float other_room = 0;
        const Vec3 other = place(scale(p.session_side, -1.0f), other_room);
        if (other_room > room + 1.0f) { p.session_side = scale(p.session_side, -1.0f); p.session_spot = other; }
    }
}

// Keeps off walls and ceilings: looks along the way it flies and the way it wants to, and slows
// toward anything there in time to stop short of it (sliding along it); and pushes away from
// anything already close around it. Changes `want_velocity` (level) and returns whether the way up
// is blocked.
inline bool avoid(Pilot& p, const State& drone, const Vec3& position, Vec3& want_velocity, const Tuning& t, const World& world) {
    using detail::length;
    for (const Vec3& going : {drone.velocity, want_velocity}) {
        Vec3 dir{going[0], 0.0f, going[2]};
        const float speed = length(dir);
        if (speed < 0.3f) continue;
        dir = scale(dir, 1.0f / speed);
        const float reach = t.margin + 0.5f + speed * 0.7f;
        Vec3 hit{};
        if (!world.cast(position, add(position, scale(dir, reach)), hit)) continue;
        Vec3 n = detail::surface_normal(world, position, dir, reach, hit, 0.3f);
        n[1] = 0.0f;
        if (length(n) < 0.2f) continue; // the ground, not a wall
        n = normalize(n);
        const float room = length(detail::sub(hit, position)) - t.margin;
        const float allowed = room > 0.0f ? std::sqrt(2.0f * 4.0f * room) : room * 2.0f; // m/s toward it; < 0 backs off
        const float toward = -detail::dot(want_velocity, n);
        if (toward > allowed) want_velocity = add(want_velocity, scale(n, toward - allowed));
    }
    // Two of the five ways around it each tick.
    static constexpr std::array<Vec3, 5> ways{{{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}}};
    const float reach = t.margin + 0.3f;
    for (int i = 0; i < 2; ++i) {
        const std::size_t w = p.near_next;
        p.near_next = (p.near_next + 1) % ways.size();
        Vec3 hit{};
        p.nearby[w] = world.cast(position, add(position, scale(ways[w], reach)), hit) ? std::max(length(detail::sub(hit, position)), 0.01f) : 0.0f;
    }
    for (std::size_t w = 0; w < 4; ++w)
        if (p.nearby[w] > 0.0f) want_velocity = add(want_velocity, scale(ways[w], -(reach - p.nearby[w]) * 3.0f));
    return p.nearby[4] > 0.0f && p.nearby[4] < t.margin;
}

// The flight settings it flies with: the player's own drone, made easy to fly well (self-levelling
// and height hold), and bumps never cut the motors (nobody is there to re-arm it).
inline Settings flight_settings(Settings cfg, const Tuning& t) {
    cfg.angle_mode = true;
    cfg.alt_hold = true;
    cfg.max_angle_deg = std::clamp(t.max_lean_deg + 10.0f, 20.0f, 80.0f);
    cfg.camera_uptilt_deg = t.uptilt_deg;
    cfg.crash_speed = 0.0f;
    cfg.range = 0.0f;
    cfg.armed = true;
    return cfg;
}

// One tick: what the pilot does with the sticks. `drone` is the flight state, `position` the
// drone's, `target` the skater's position and `skater_forward` which way the skater faces.
inline Cmd fly(Pilot& p, const State& drone, const Vec3& position, const Vec3& target, const Vec3& skater_forward,
               float seconds, const Settings& cfg, const Tuning& t, const World* world = nullptr) {
    using detail::length;
    using detail::sub;
    const float dt = std::clamp(std::isfinite(seconds) ? seconds : 0.0f, 0.0f, 0.05f);
    Cmd cmd;
    cmd.hold = true;
    cmd.throttle = detail::hover_of(cfg);
    cmd.gimbal_tilt_rad = p.tilt;
    if (!(dt > 0.0f)) return cmd;

    // What the skater is doing: velocity from its moves (a jump of more than a few metres in one
    // tick is a respawn, not a move), and the way it is going.
    if (!p.valid || length(sub(target, p.last_target)) > 8.0f) {
        p.valid = true;
        p.target_velocity = {0, 0, 0};
        Vec3 f{skater_forward[0], 0.0f, skater_forward[2]};
        p.heading = length(f) > 0.1f ? normalize(f) : Vec3{0, 0, -1};
        p.count = 0;
        p.ride_y = target[1];
        p.tilt = std::clamp(std::atan2(target[1] + 0.9f - position[1], std::max(std::hypot(target[0] - position[0], target[2] - position[2]), 0.5f)), -1.2f, 0.35f);
        p.climb_extra = p.blocked_for = p.floor_lift = 0;
        p.history_count = 0;
        p.history_clock = 0;
        p.session = false;
        p.nearby = {};
    } else {
        const Vec3 now = scale(sub(target, p.last_target), 1.0f / dt);
        const float k = 1.0f - std::exp(-dt / 0.15f);
        p.target_velocity = add(p.target_velocity, scale(sub(now, p.target_velocity), k));
        // A pilot follows the lie of the ground, not every ollie: height is eased toward slowly,
        // slower still going up (most rises are jumps that come straight back down).
        const float tau = target[1] > p.ride_y ? 1.4f : 0.6f;
        p.ride_y += (target[1] - p.ride_y) * (1.0f - std::exp(-dt / tau));
    }
    p.last_target = target;
    // Remembered, so the pilot can act on what it saw a reaction time ago.
    for (std::size_t i = 0; i < p.count; ++i) p.seen[i].age += dt;
    p.head = (p.head + 1) % p.seen.size();
    p.seen[p.head] = {0.0f, Vec3{target[0], p.ride_y, target[2]}, Vec3{p.target_velocity[0], 0.0f, p.target_velocity[2]}};
    p.count = std::min(p.count + 1, p.seen.size());
    Pilot::Seen seen = p.seen[p.head];
    for (std::size_t back = 0; back < p.count; ++back) {
        const auto& s = p.seen[(p.head + p.seen.size() - back) % p.seen.size()];
        seen = s;
        if (s.age >= t.reaction) break;
    }
    // Every 0.1 s: is it riding a session?
    p.history_clock += dt;
    if (p.history_clock >= 0.1f) {
        p.history_clock = std::min(p.history_clock - 0.1f, 0.1f);
        p.history_head = (p.history_head + 1) % p.history.size();
        p.history[p.history_head] = {target, Vec3{p.target_velocity[0], 0.0f, p.target_velocity[2]}};
        p.history_count = std::min(p.history_count + 1, p.history.size());
        read_session(p, position, t, world);
    }
    if (p.session) p.session_for += dt;
    // Heading: the way the skater moves once it moves at all, else the way it faces; turned toward
    // gently, as a pilot swings round behind.
    {
        Vec3 want{seen.velocity[0], 0.0f, seen.velocity[2]};
        if (length(want) < 1.5f) want = Vec3{skater_forward[0], 0.0f, skater_forward[2]};
        if (length(want) > 0.1f) {
            const float k = 1.0f - std::exp(-dt * (length(seen.velocity) > 4.0f ? 2.2f : 0.8f));
            const Vec3 blended = add(p.heading, scale(sub(normalize(want), p.heading), k));
            if (length(blended) > 0.1f) p.heading = normalize(blended);
        }
    }

    // Where to be: behind the skater, or in a session the spot it films from. A wall between the
    // skater and the spot behind brings the spot in to this side of it; one that leaves it too close,
    // or stands between the drone and its spot, is climbed over (a little, and only if it lasts).
    // The ground under the spot keeps it up.
    Vec3 goal = p.session ? p.session_spot : spot(seen.position, seen.velocity, p.heading, t, p.climb_extra);
    const Vec3 chest = add(seen.position, Vec3{0, 1.0f, 0});
    if (world && world->cast) {
        Vec3 hit{};
        bool blocked = false;
        if (!p.session) {
            const Vec3 out = sub(goal, chest);
            if (const float out_length = length(out); out_length > 1.0f) {
                const Vec3 dir = scale(out, 1.0f / out_length);
                if (world->cast(add(chest, scale(dir, 0.6f)), add(goal, scale(dir, t.margin)), hit)) {
                    const float room = length(sub(hit, chest)) - t.margin;
                    goal = add(chest, scale(dir, std::max(room, 0.0f)));
                    blocked = room < t.distance * 0.5f;
                }
            }
        }
        blocked = blocked || world->cast(position, goal, hit);
        p.blocked_for = blocked ? p.blocked_for + dt : 0.0f;
        p.climb_extra = p.blocked_for > 0.3f ? std::min(p.climb_extra + 1.5f * dt, 3.0f) : std::max(p.climb_extra - 0.8f * dt, 0.0f);
        // Ground under the spot (a bank, a ledge) lifts it, quickly up and slowly back down.
        Vec3 ground{};
        float lift = 0.0f;
        if (world->cast(add(goal, Vec3{0, 2.0f, 0}), add(goal, Vec3{0, -(t.height + 6.0f), 0}), ground))
            lift = std::max(ground[1] + t.clearance - goal[1], 0.0f);
        p.floor_lift += (lift - p.floor_lift) * (1.0f - std::exp(-dt / (lift > p.floor_lift ? 0.2f : 1.0f)));
        goal[1] += p.floor_lift;
    }

    // Speed: match the skater, closing the gap; then lean to get it, no further than a pilot would.
    const Vec3 error = sub(goal, position);
    // Closing a gap: no faster than it can still slow down from gently by the time it gets there,
    // so it does not overshoot and have to stand on the brakes (nose up, the skater out of shot).
    Vec3 want_velocity = p.session ? Vec3{0, 0, 0} : seen.velocity;
    {
        const Vec3 level{error[0], 0.0f, error[2]};
        if (const float d = length(level); d > 0.01f) {
            const float close = std::min({t.gap_gain * d, std::sqrt(2.0f * t.brake * d), t.catch_up});
            want_velocity = add(want_velocity, scale(level, close / d));
        }
    }
    // Too close (the skater stopped short, or came back at it): back off as a pilot pulls away,
    // rather than flying past and losing the shot.
    {
        // Measured from where the skater must be by now, not where it was seen a reaction ago.
        const Vec3 now_at = add(seen.position, scale(seen.velocity, t.reaction));
        Vec3 away{position[0] - now_at[0], 0.0f, position[2] - now_at[2]};
        const float gap = length(away), keep = t.distance * 0.7f;
        if (gap < keep) {
            away = gap > 0.05f ? scale(away, 1.0f / gap) : scale(p.heading, -1.0f);
            want_velocity = add(want_velocity, scale(away, (keep - gap) * 2.0f));
        }
    }
    const bool ceiling = world && world->cast && avoid(p, drone, position, want_velocity, t, *world);
    Vec3 accel = scale(sub(want_velocity, drone.velocity), t.speed_gain);
    const float g = std::max(cfg.gravity, 1.0f);
    const float most = std::tan(std::clamp(t.max_lean_deg, 5.0f, 70.0f) * kDegToRad) * g;
    {
        Vec3 level{accel[0], 0.0f, accel[2]};
        if (const float a = length(level); a > most) { accel[0] *= most / a; accel[2] *= most / a; }
    }
    const Vec3 forward = [&] {
        Vec3 f = scale(row(drone.body, 2), -1.0f);
        f[1] = 0.0f;
        return length(f) > 0.05f ? normalize(f) : Vec3{0, 0, -1};
    }();
    const Vec3 right{-forward[2], 0.0f, forward[0]};
    const float lean = std::clamp(cfg.max_angle_deg, 1.0f, 89.0f) * kDegToRad;
    // Angle mode: pitch stick back (+) leans the nose up, so speeding up forward is stick forward.
    const float want_pitch = clamp1(-std::atan(dot(accel, forward) / g) / lean);
    const float want_roll = clamp1(std::atan(dot(accel, right) / g) / lean);
    // Height: eased toward, a few metres a second at most. The ground under the drone and just
    // ahead of where it is going comes first: it climbs off it (harder the closer it is, and as
    // fast as it must to clear a rise ahead in time) and only sinks slowly near it, whatever its
    // spot asks.
    const float climb_rate = std::max(cfg.climb_rate, 1.0f);
    float want_up = std::clamp(1.5f * error[1], -2.5f, 3.0f); // m/s
    bool ground_close = false;
    if (world && world->cast) {
        const float reach = t.clearance + 4.0f;
        float room = 1e9f, rise = -1e9f;
        Vec3 hit{};
        if (world->cast(add(position, Vec3{0, 0.5f, 0}), add(position, Vec3{0, -reach, 0}), hit)) room = position[1] - hit[1];
        const Vec3 going{drone.velocity[0], 0.0f, drone.velocity[2]};
        if (const float speed = length(going); speed > 1.0f) {
            constexpr float soon = 0.8f; // s ahead it looks
            const Vec3 at = add(position, scale(going, std::min(soon, 10.0f / speed)));
            if (world->cast(add(at, Vec3{0, 3.0f, 0}), add(at, Vec3{0, -reach, 0}), hit))
                rise = (hit[1] + t.clearance - position[1]) / soon;
        }
        if (room < t.clearance) want_up = std::max(want_up, (t.clearance - room) * 4.0f + 0.5f);
        if (rise > 0.0f) want_up = std::max(want_up, rise);
        if (want_up < 0.0f && room < t.clearance + 1.5f) want_up *= std::clamp((room - t.clearance) / 1.5f, 0.0f, 1.0f);
        ground_close = room < t.clearance;
    }
    float want_climb = clamp1(std::min(want_up, 8.0f) / climb_rate);
    if (ceiling) want_climb = std::min(want_climb, ground_close ? 0.0f : -1.0f / climb_rate); // something just above
    // Yaw: keep the skater in the middle of the shot, at a pilot's turn rate.
    float want_yaw{};
    {
        Vec3 to{chest[0] - position[0], 0.0f, chest[2] - position[2]};
        if (length(to) > 0.5f) {
            to = normalize(to);
            const float across = dot(cross(forward, to), Vec3{0, 1, 0}); // + = the skater is to the left
            const float angle = std::atan2(across, dot(forward, to));
            // Far off the middle of the shot the pilot snaps round faster.
            const float gain = 3.0f + 3.0f * std::clamp(std::abs(angle) / 0.6f, 0.0f, 1.0f);
            want_yaw = std::clamp(angle * gain, -t.max_yaw_rate, t.max_yaw_rate);
        }
    }

    // Hands: the sticks follow what the pilot wants smoothly, with a little wander.
    p.wander_time -= dt;
    if (p.wander_time <= 0.0f) {
        p.wander_time = 0.4f + 0.5f * (detail::noise(p.noise) + 1.0f);
        p.wander_to = {detail::noise(p.noise), detail::noise(p.noise), detail::noise(p.noise)};
    }
    const float w = 1.0f - std::exp(-dt / 0.35f);
    p.wander_now = add(p.wander_now, scale(sub(p.wander_to, p.wander_now), w));
    const float h = 1.0f - std::exp(-dt / std::max(t.hands, 0.01f));
    p.pitch += (want_pitch + t.wander * p.wander_now[0] - p.pitch) * h;
    p.roll += (want_roll + t.wander * p.wander_now[1] - p.roll) * h;
    p.yaw += (want_yaw * (1.0f + 0.15f * p.wander_now[2]) - p.yaw) * h;
    p.climb += (want_climb - p.climb) * h;
    if (ground_close) p.climb = std::max(p.climb, want_climb); // (no easing into the ground)

    cmd.pitch = clamp1(p.pitch);
    cmd.roll = clamp1(p.roll);
    cmd.yaw_rad = p.yaw * dt; // + = turn left
    cmd.climb = clamp1(p.climb);
    // The camera sits on a tilt gimbal, eased toward the skater: leaning to speed up or slow down
    // no longer swings the shot to the ground or the sky. It follows half of a jump; in a session
    // all of it (the airs are the shot).
    {
        const float aim = p.ride_y + (p.session ? 1.0f : 0.5f) * (target[1] - p.ride_y);
        const Vec3 to = sub(Vec3{target[0], aim + 0.9f, target[2]}, position);
        const float level = std::hypot(to[0], to[2]);
        const float want_tilt = std::clamp(std::atan2(to[1], std::max(level, 0.5f)), -1.2f, 0.6f);
        p.tilt += (want_tilt - p.tilt) * (1.0f - std::exp(-dt / std::max(t.tilt_ease, 0.05f)));
        cmd.gimbal_tilt_rad = p.tilt;
    }
    return cmd;
}
} // namespace dingosdk::drone::follow
