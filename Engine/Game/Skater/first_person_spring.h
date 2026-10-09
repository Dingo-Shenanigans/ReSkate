#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace dingosdk::first_person {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
using Matrix = std::array<float, 16>;
inline constexpr float offset_limit = 3, rotation_limit = 180, strength_limit = 100;
struct Settings {
    bool enabled = false;
    Vec3 offset{}, rotation{};
    float up = 50, down = 50, left = 50, right = 50;
    // True first person: the view takes its heading from the head but keeps the
    // horizon level, and filters the animation's wobble. Percentages, 0..100.
    bool stabilize = true, follow_flips = true;
    // Hand the view back to the game's camera while the skater walks (opt-in).
    bool board_only = false;
    float smoothing = 60, head_pitch = 86, head_roll = 0, bob = 25;
};
// Adaptive low-pass ("One Euro") state for one channel: heavy smoothing while the
// value barely moves, little lag while it moves fast (spins, carving).
struct Euro { float value{}, rate{}; };
struct Stable {
    bool ready = false;
    double time{};
    Euro yaw, pitch, roll;
    std::array<Euro, 3> offset{};
};
struct Spring {
    bool ready = false;
    double time{};
    Vec3 position{}, velocity{}, angular_velocity{}, previous_target{}, previous_origin{};
    Quat rotation{0, 0, 0, 1};
    Stable stable{};
};
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
inline Vec3 subtract(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline float length(const Vec3& v) { return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
inline Quat conjugate(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
inline Quat normalized(Quat q) {
    const float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (!std::isfinite(n) || n < 0.00001f) return {0, 0, 0, 1};
    for (auto& v : q) v /= n;
    return q;
}
inline Quat multiply(const Quat& a, const Quat& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 t{2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
            v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],
            v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
inline Quat orientation(const Matrix& m) {
    const float trace = m[0] + m[5] + m[10];
    if (trace > 0) {
        const float s = 2*std::sqrt(trace+1);
        return normalized({(m[6]-m[9])/s, (m[8]-m[2])/s, (m[1]-m[4])/s, s/4});
    }
    if (m[0] > m[5] && m[0] > m[10]) {
        const float s = 2*std::sqrt(1+m[0]-m[5]-m[10]);
        return normalized({s/4, (m[4]+m[1])/s, (m[8]+m[2])/s, (m[6]-m[9])/s});
    }
    if (m[5] > m[10]) {
        const float s = 2*std::sqrt(1+m[5]-m[0]-m[10]);
        return normalized({(m[4]+m[1])/s, s/4, (m[9]+m[6])/s, (m[8]-m[2])/s});
    }
    const float s = 2*std::sqrt(1+m[10]-m[0]-m[5]);
    return normalized({(m[8]+m[2])/s, (m[9]+m[6])/s, s/4, (m[1]-m[4])/s});
}
inline void write(Matrix& m, const Quat& q, const Vec3& position) {
    const std::array axes{rotate(q, {1,0,0}), rotate(q, {0,1,0}), rotate(q, {0,0,1})};
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned i = 0; i < 3; ++i) m[axis*4+i] = axes[axis][i];
        m[12+axis] = position[axis];
    }
}
inline bool valid(const Settings& settings) {
    const auto bounded = [](float v, float limit) { return std::isfinite(v) && std::abs(v) <= limit; };
    for (unsigned i = 0; i < 3; ++i)
        if (!bounded(settings.offset[i], offset_limit) || !bounded(settings.rotation[i], rotation_limit)) return false;
    for (float v : {settings.up, settings.down, settings.left, settings.right,
                    settings.smoothing, settings.head_pitch, settings.head_roll, settings.bob})
        if (!bounded(v, strength_limit) || v < 0) return false;
    return true;
}
inline float dot(const Vec3& a, const Vec3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline float wrap_angle(float radians) { return std::remainder(radians, 2*std::numbers::pi_v<float>); }
inline float euro_alpha(float cutoff, float seconds) {
    const float tau = 1 / (2*std::numbers::pi_v<float>*cutoff);
    return 1 / (1 + tau/seconds);
}
// `change` is the raw step since the last sample (already wrapped for angles).
// The rate is filtered at 0.5 Hz so an animation's back-and-forth averages out
// and only sustained turns (spins, carves) open the filter up.
inline void euro(Euro& state, float change, float seconds, float min_cutoff, float beta) {
    state.rate += (change/seconds - state.rate) * euro_alpha(0.5f, seconds);
    state.value += change * euro_alpha(min_cutoff + beta*std::abs(state.rate), seconds);
}
// Rows (right, up, backward) of a level view looking along yaw / pitch, then
// rolled by `roll` about the view axis. World is Y-up; yaw 0 looks along +Z.
inline void write_level(Matrix& m, float yaw, float pitch, float roll) {
    const Vec3 forward{std::sin(yaw)*std::cos(pitch), std::sin(pitch), std::cos(yaw)*std::cos(pitch)};
    const Vec3 level_right{-std::cos(yaw), 0, std::sin(yaw)};
    const auto level_up = cross(level_right, forward);
    const float c = std::cos(roll), s = std::sin(roll);
    for (unsigned i = 0; i < 3; ++i) {
        m[i] = level_right[i]*c + level_up[i]*s;
        m[4+i] = level_up[i]*c - level_right[i]*s;
        m[8+i] = -forward[i];
    }
}
// Splits the head into heading, nod and tilt, keeps the heading, scales the nod
// and tilt by the settings and filters all three, so the view stops wobbling
// with the animation. Flips and bails (head upside down) hand the view back to
// the real head when follow_flips is on.
inline Matrix stabilize(Stable& state, const Matrix& head, const Settings& settings, double now, const Vec3& origin) {
    const Vec3 right{head[0], head[1], head[2]}, up{head[4], head[5], head[6]};
    const Vec3 forward{-head[8], -head[9], -head[10]};
    const float level = std::sqrt(forward[0]*forward[0] + forward[2]*forward[2]);
    // Looking nearly straight up or down, the top of the head says where the
    // skater is heading better than the face does.
    const float yaw = level > 0.25f ? std::atan2(forward[0], forward[2])
        : forward[1] < 0 ? std::atan2(up[0], up[2]) : std::atan2(-up[0], -up[2]);
    const float pitch = std::asin(std::clamp(forward[1], -1.0f, 1.0f));
    Matrix level_frame = head;
    write_level(level_frame, yaw, pitch, 0);
    const float roll = std::atan2(dot(right, {level_frame[4], level_frame[5], level_frame[6]}),
                                  dot(right, {level_frame[0], level_frame[1], level_frame[2]}));
    const float pitch_target = pitch * settings.head_pitch * 0.01f;
    const float roll_target = roll * settings.head_roll * 0.01f;
    const Vec3 eye{head[12], head[13], head[14]};
    const auto offset = subtract(eye, origin);

    const double elapsed = now - state.time;
    bool snap = !state.ready || !std::isfinite(elapsed) || elapsed < 0 || elapsed > 0.25;
    for (unsigned i = 0; i < 3 && !snap; ++i) snap = std::abs(offset[i] - state.offset[i].value) > 3;
    if (snap) {
        state = {true, now, {yaw, 0}, {pitch_target, 0}, {roll_target, 0},
                 {Euro{offset[0], 0}, Euro{offset[1], 0}, Euro{offset[2], 0}}};
    } else if (elapsed > 0.00001) {
        state.time = now;
        const float seconds = static_cast<float>(elapsed);
        // Smoothing 0% follows the head directly; 100% leaves a 0.15 Hz floor.
        const float s = settings.smoothing * 0.01f;
        const float angle_cutoff = s <= 0 ? 1000.0f : 0.15f * std::pow(40.0f, 1 - s);
        constexpr float angle_beta = 0.6f;
        euro(state.yaw, wrap_angle(yaw - state.yaw.value), seconds, angle_cutoff, angle_beta);
        state.yaw.value = wrap_angle(state.yaw.value);
        euro(state.pitch, pitch_target - state.pitch.value, seconds, angle_cutoff, angle_beta);
        euro(state.roll, wrap_angle(roll_target - state.roll.value), seconds, angle_cutoff, angle_beta);
        for (unsigned i = 0; i < 3; ++i) euro(state.offset[i], offset[i] - state.offset[i].value, seconds, 0.6f, 1.5f);
    }
    auto result = head;
    write_level(result, state.yaw.value, state.pitch.value, state.roll.value);
    // Head bob: the share of the eye's movement around the body that is kept.
    const float bob = settings.bob * 0.01f;
    for (unsigned i = 0; i < 3; ++i) {
        const float smooth = state.offset[i].value;
        result[12+i] = origin[i] + smooth + (offset[i] - smooth) * bob;
    }
    // Upside down (up.y below 0.4, fully past -0.1): blend to the real head.
    const float flip = settings.follow_flips ? std::clamp((0.4f - up[1]) / 0.5f, 0.0f, 1.0f) : 0.0f;
    if (flip > 0) {
        auto stable = orientation(result);
        auto real = orientation(head);
        if (stable[0]*real[0] + stable[1]*real[1] + stable[2]*real[2] + stable[3]*real[3] < 0) for (auto& v : real) v = -v;
        Quat blended{};
        for (unsigned i = 0; i < 4; ++i) blended[i] = stable[i] + (real[i] - stable[i]) * flip;
        write(result, normalized(blended), {result[12], result[13], result[14]});
    }
    return result;
}
inline Matrix target(Matrix head, const Settings& settings) {
    const auto head_rotation = orientation(head);
    const auto position = add({head[12], head[13], head[14]},
        rotate(head_rotation, {settings.offset[0], settings.offset[1], -settings.offset[2]}));
    constexpr float half_degrees = std::numbers::pi_v<float> / 360;
    const float pitch = settings.rotation[0]*half_degrees;
    const float yaw = -settings.rotation[1]*half_degrees;
    const float roll = -settings.rotation[2]*half_degrees;
    const auto offset = multiply(multiply({0,std::sin(yaw),0,std::cos(yaw)},
        {std::sin(pitch),0,0,std::cos(pitch)}), {0,0,std::sin(roll),std::cos(roll)});
    write(head, normalized(multiply(head_rotation, offset)), position);
    return head;
}
inline Vec3 rotation_vector(Quat q) {
    q = normalized(q);
    if (q[3] < 0) for (auto& v : q) v = -v;
    const float n = length({q[0],q[1],q[2]});
    const float factor = n < 0.00001f ? 2 : 2*std::atan2(n, q[3])/n;
    return {q[0]*factor, q[1]*factor, q[2]*factor};
}
inline Quat from_rotation_vector(const Vec3& v) {
    const float angle = length(v);
    const float factor = angle < 0.00001f ? 0.5f : std::sin(angle/2)/angle;
    return normalized({v[0]*factor,v[1]*factor,v[2]*factor,std::cos(angle/2)});
}
inline void damp(float& error, float& velocity, float strength, float seconds) {
    if (strength == 0) { error = 0; velocity = 0; return; }
    // Strength controls lag time, not the force pulling the camera to the head.
    const float frequency = 2 / std::max(strength * 0.01f, 0.001f);
    const float previous = error;
    const float decay = std::exp(-frequency*seconds);
    const float change = velocity + frequency*error;
    error = (error + change*seconds)*decay;
    velocity = (velocity - frequency*change*seconds)*decay;
    if (error*previous <= 0) { error = 0; velocity = 0; }
    else if (std::abs(error) > std::abs(previous)) { error = previous; velocity = 0; }
}
inline Matrix update(Spring& spring, const Matrix& head, const Settings& settings, double now, const Vec3& origin = {}) {
    if (!valid(settings) || !std::isfinite(now)) return head;
    if (!settings.stabilize) spring.stable = {};
    auto result = target(settings.stabilize ? stabilize(spring.stable, head, settings, now, origin) : head, settings);
    const auto position = subtract({result[12],result[13],result[14]}, origin);
    const auto rotation = orientation(result);
    const double elapsed = now - spring.time;
    if (!spring.ready || !settings.enabled || elapsed < 0 || elapsed > 0.25 ||
        length(subtract(position, spring.previous_target)) > 5 ||
        length(subtract(origin, spring.previous_origin)) > 5) {
        spring = {true, now, position, {}, {}, position, origin, rotation, spring.stable};
        return result;
    }
    spring.time = now;
    spring.previous_target = position;
    spring.previous_origin = origin;
    const auto inverse = conjugate(rotation);
    auto error = rotate(inverse, subtract(spring.position, position));
    auto velocity = rotate(inverse, spring.velocity);
    auto angular_error = rotation_vector(multiply(inverse, spring.rotation));
    auto angular_velocity = rotate(inverse, spring.angular_velocity);
    const float average = (settings.up+settings.down+settings.left+settings.right)/4;
    const float seconds = static_cast<float>(elapsed);
    damp(error[0], velocity[0], error[0] > 0 ? settings.left : settings.right, seconds);
    damp(error[1], velocity[1], error[1] > 0 ? settings.down : settings.up, seconds);
    damp(error[2], velocity[2], average, seconds);
    damp(angular_error[0], angular_velocity[0], angular_error[0] > 0 ? settings.down : settings.up, seconds);
    damp(angular_error[1], angular_velocity[1], angular_error[1] > 0 ? settings.right : settings.left, seconds);
    damp(angular_error[2], angular_velocity[2], average, seconds);
    spring.position = add(position, rotate(rotation, error));
    spring.velocity = rotate(rotation, velocity);
    spring.rotation = normalized(multiply(rotation, from_rotation_vector(angular_error)));
    spring.angular_velocity = rotate(rotation, angular_velocity);
    write(result, spring.rotation, add(origin, spring.position));
    return result;
}
struct FrameSpring {
    Spring state{}, frame_start{};
    double frame_time{};

    void reset() { state = {}; frame_start = {}; }
    void begin(double now) { frame_start = state; frame_time = now; }
    Matrix sample(const Matrix& head, const Settings& settings, const Vec3& origin) {
        // A later animation pose refines this frame, without integrating twice.
        auto next = frame_start;
        auto result = update(next, head, settings, frame_time, origin);
        state = next;
        return result;
    }
};
}
