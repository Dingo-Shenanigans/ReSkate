#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace first_person_vendor::first_person {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
using Matrix = std::array<float, 16>;
inline constexpr float offset_limit = 3, rotation_limit = 180, strength_limit = 100;
struct LookAngles { float pitch{}, yaw{}; };
struct Settings {
    bool enabled = false;
    Vec3 offset{}, rotation{};
    float up = 50, down = 50, left = 50, right = 50;
};
struct Spring {
    bool ready = false;
    double time{};
    Vec3 position{}, velocity{}, angular_velocity{}, previous_target{}, previous_origin{};
    Quat rotation{0, 0, 0, 1};
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
    for (float v : {settings.up, settings.down, settings.left, settings.right})
        if (!bounded(v, strength_limit) || v < 0) return false;
    return true;
}
inline Matrix target(Matrix head, const Settings& settings, LookAngles look = {}) {
    const auto head_rotation = orientation(head);
    const auto position = add({head[12], head[13], head[14]},
        rotate(head_rotation, {settings.offset[0], settings.offset[1], -settings.offset[2]}));
    constexpr float half_degrees = std::numbers::pi_v<float> / 360;
    const float pitch = settings.rotation[0]*half_degrees;
    const float yaw = -settings.rotation[1]*half_degrees;
    const float roll = -settings.rotation[2]*half_degrees;
    const auto offset = multiply(multiply({0,std::sin(yaw),0,std::cos(yaw)},
        {std::sin(pitch),0,0,std::cos(pitch)}), {0,0,std::sin(roll),std::cos(roll)});
    const float look_pitch = look.pitch * half_degrees, look_yaw = -look.yaw * half_degrees;
    const auto glance = multiply({0,std::sin(look_yaw),0,std::cos(look_yaw)},
        {std::sin(look_pitch),0,0,std::cos(look_pitch)});
    write(head, normalized(multiply(multiply(head_rotation, offset), glance)), position);
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
inline Matrix update(Spring& spring, const Matrix& head, const Settings& settings, double now, const Vec3& origin = {}, LookAngles look = {}) {
    if (!valid(settings) || !std::isfinite(now)) return head;
    auto result = target(head, settings, look);
    const auto position = subtract({result[12],result[13],result[14]}, origin);
    const auto rotation = orientation(result);
    const double elapsed = now - spring.time;
    if (!spring.ready || !settings.enabled || elapsed < 0 || elapsed > 0.25 ||
        length(subtract(position, spring.previous_target)) > 5 ||
        length(subtract(origin, spring.previous_origin)) > 5) {
        spring = {true, now, position, {}, {}, position, origin, rotation};
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
    Matrix sample(const Matrix& head, const Settings& settings, const Vec3& origin, LookAngles look = {}) {
        // A later animation pose refines this frame, without integrating twice.
        auto next = frame_start;
        auto result = update(next, head, settings, frame_time, origin, look);
        state = next;
        return result;
    }
};
}
