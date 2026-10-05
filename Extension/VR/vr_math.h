#pragma once
// Pose and projection math for ReSkate VR. Header-only and free of Windows and
// OpenXR types so it can be unit tested on any platform (Test/vr_math_tests.cpp).
//
// Conventions shared by the native camera and OpenXR: right-handed, +Y up,
// a view looks down its local -Z. A native camera matrix stores its basis as
// rows: right (0..2), up (4..6), backward (8..10), position (12..14).
#include "Engine/Game/Skater/first_person_spring.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace dingosdk::vr::math {
using first_person::Matrix;
using first_person::Quat;
using first_person::Vec3;

struct Pose {
    Quat orientation{0, 0, 0, 1};
    Vec3 position{};
};

// Where "straight ahead" is in the tracking space, set by recentering.
struct Anchor {
    float heading = 0;
    Vec3 position{};
};

inline constexpr float pi = std::numbers::pi_v<float>;

inline float wrap(float angle) {
    if (!std::isfinite(angle)) return 0;
    angle = std::remainder(angle, 2 * pi);
    return angle <= -pi ? angle + 2 * pi : angle;
}

// Rotation about +Y by `heading` radians; heading 0 looks down -Z.
inline Quat yaw(float heading) { return {0, std::sin(heading / 2), 0, std::cos(heading / 2)}; }

// The heading of a horizontal direction (x, z).
inline float heading_of(float x, float z) { return std::atan2(-x, -z); }

// The heading a camera matrix faces. Looking almost straight up or down, the
// up row stands in for the forward direction it has rolled towards.
inline std::optional<float> heading(const Matrix& m) {
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(m[i]) || !std::isfinite(m[4 + i]) || !std::isfinite(m[8 + i])) return std::nullopt;
    const float fx = -m[8], fz = -m[10];
    if (std::sqrt(fx * fx + fz * fz) >= 0.2f) return heading_of(fx, fz);
    const float sign = m[9] > 0 ? 1.0f : -1.0f; // backward up: looking down, up leans forward
    const float ux = m[4] * sign, uz = m[6] * sign;
    if (std::sqrt(ux * ux + uz * uz) >= 0.2f) return heading_of(ux, uz);
    return std::nullopt;
}

// The heading of an orientation's forward (-Z) axis.
inline float heading(const Quat& q) {
    const auto forward = first_person::rotate(q, {0, 0, -1});
    return heading_of(forward[0], forward[2]);
}

// Heading (third person): follows `target` with time constant `seconds`, jumps when the target itself
// jumps more than `snap` radians in one update (not when the view merely lags a turn),
// and ignores the first `deadband` radians (standing sway).
struct HeadingRig {
    float value = std::numeric_limits<float>::quiet_NaN();
    float target = std::numeric_limits<float>::quiet_NaN();
};
inline float follow_snap(HeadingRig& rig, float target, float elapsed, float seconds, float snap, float deadband) {
    const float last = rig.target;
    rig.target = target;
    if (!std::isfinite(rig.value)) return rig.value = wrap(target);
    const float gap = wrap(target - rig.value);
    if (snap > 0 && std::isfinite(last) && std::abs(wrap(target - last)) > snap) return rig.value = wrap(target);
    if (std::abs(gap) <= deadband) return rig.value;
    const float excess = gap - std::copysign(deadband, gap);
    const float blend = seconds > 0 && std::isfinite(elapsed) ? 1 - std::exp(-std::clamp(elapsed, 0.0f, 1.0f) / seconds) : 1.0f;
    return rig.value = wrap(rig.value + excess * blend);
}

// Tilt: the head orientation without its twist about +Y (swing-twist, stable through
// flips). Wobble slower than `fast` rad/s is smoothed over `seconds`; faster
// rotation (flips, spins) is followed at once.
struct TiltRig {
    Quat value{0, 0, 0, 1};
    bool ready = false;
};
// The rotation about +Y (heading) of an orientation, from its swing-twist split: unlike
// the heading of the forward axis it stays continuous through flips.
inline float twist_heading(const Quat& q) { return wrap(2 * std::atan2(q[1], q[3])); }
inline Quat untwisted(const Quat& q) {
    const float n = std::sqrt(q[1] * q[1] + q[3] * q[3]);
    if (n < 1e-5f) return q;
    const Quat twist_inverse{0, -q[1] / n, 0, q[3] / n};
    return first_person::normalized(first_person::multiply(twist_inverse, q));
}
// "Through the skater's eyes" (View follows: Head). One Euro filters (Casiez et al. 2012):
// the smoothing cutoff rises with the speed, so slow wobble is smoothed and fast moves pass.
inline float one_euro_alpha(float cutoff, float elapsed) {
    const float tau = 1 / (2 * pi * cutoff);
    return 1 / (1 + tau / elapsed);
}

// The view's heading: the head's turn about +Y, continuous (unwrapped), kept turning at its
// last speed while it is undefined (the head upside down), through a One Euro filter. Push
// twist and stride wobble are smoothed; spins, coffins and carves pass. Tuned on recorded
// sessions: pushing wobble 4.8 -> 2.3 degrees RMS, spins followed within about 10 degrees.
struct EyesHeading {
    bool ready = false;
    float unwrapped = 0, filtered = 0, rate = 0, speed = 0;
};
inline float eyes_heading(EyesHeading& e, const Quat& head, float elapsed, float min_cutoff = 0.15f, float beta = 1.0f) {
    const float twist = twist_heading(head);
    if (!e.ready || elapsed > 0.5f) {
        e = {true, twist, twist, 0, 0};
        return twist;
    }
    if (!(elapsed > 0)) return wrap(e.filtered);
    float step = 0;
    if (std::hypot(head[1], head[3]) > 0.35f) {
        step = wrap(twist - wrap(e.unwrapped));
        e.rate = step / elapsed;
    } else {
        e.rate *= std::exp(-elapsed / 0.5f);
        step = e.rate * elapsed;
    }
    e.unwrapped += step;
    e.speed += one_euro_alpha(1.0f, elapsed) * (e.rate - e.speed);
    e.filtered += one_euro_alpha(min_cutoff + beta * std::abs(e.speed), elapsed) * (e.unwrapped - e.filtered);
    if (std::abs(e.filtered) > 100) { // keep both small (precision) without changing their difference
        const float turns = std::round(e.filtered / (2 * pi)) * 2 * pi;
        e.filtered -= turns;
        e.unwrapped -= turns;
    }
    return wrap(e.filtered);
}

// The view's position: the body origin plus the head's offset from it (world frame, so body
// rotations in flips cannot swing it) through per-axis One Euro filters: the walk and push bob
// is smoothed, ollies and flips pass. A drop below the tracked standing eye height (crouches)
// is scaled by `crouch`. Nothing switches on or off, so the camera never jumps.
struct EyesPosition {
    bool ready = false;
    Vec3 offset{}, last{}, speed{};
    float stand = 0;
};
inline Vec3 eyes_position(EyesPosition& e, const Vec3& head, const Vec3& origin, float elapsed, float crouch,
    float min_cutoff = 0.8f, float beta = 2.0f) {
    const auto offset = first_person::subtract(head, origin);
    if (!e.ready || elapsed > 0.5f || first_person::length(first_person::subtract(offset, e.offset)) > 2.0f) {
        e = {true, offset, offset, {}, offset[1]};
        return head;
    }
    if (elapsed > 0) {
        for (std::size_t i = 0; i < 3; ++i) {
            e.speed[i] += one_euro_alpha(1.0f, elapsed) * ((offset[i] - e.last[i]) / elapsed - e.speed[i]);
            e.offset[i] += one_euro_alpha(min_cutoff + beta * std::abs(e.speed[i]), elapsed) * (offset[i] - e.offset[i]);
        }
        e.last = offset;
        e.stand += (e.offset[1] - e.stand) * (1 - std::exp(-elapsed / (e.offset[1] > e.stand ? 0.3f : 3.0f)));
    }
    const float height = e.offset[1] >= e.stand ? e.offset[1] : e.stand + (e.offset[1] - e.stand) * crouch;
    return first_person::add(origin, {e.offset[0], height, e.offset[2]});
}

inline Quat steady_tilt(TiltRig& rig, const Quat& head, float elapsed, float seconds, float fast) {
    const auto tilt = untwisted(head);
    if (!rig.ready) {
        rig.value = tilt;
        rig.ready = true;
        return tilt;
    }
    float dot = 0;
    for (std::size_t i = 0; i < 4; ++i) dot += rig.value[i] * tilt[i];
    const float angle = 2 * std::acos(std::clamp(std::abs(dot), 0.0f, 1.0f));
    const bool quick = elapsed > 0 && angle / elapsed > fast;
    const float blend = quick || seconds <= 0 ? 1.0f : 1 - std::exp(-std::max(elapsed, 0.0f) / seconds);
    const float sign = dot < 0 ? -1.0f : 1.0f;
    Quat mixed{};
    for (std::size_t i = 0; i < 4; ++i) mixed[i] = rig.value[i] + (sign * tilt[i] - rig.value[i]) * blend;
    return rig.value = first_person::normalized(mixed);
}

inline Anchor anchor_from(const Pose& head) { return {heading(head.orientation), head.position}; }

// The camera for one eye: `base` is the skater's head camera; `eye` and
// `head` are tracking-space poses, read relative to `anchor`. With
// `level`, only `base_heading` turns the view; otherwise the head camera's
// full orientation does. `forward`, `up` and `side` (metres, right positive) move the seat. Fourth lanes of `base` are kept.
inline Matrix compose(const Matrix& base, float base_heading, bool level, const Pose& eye, const Pose& head,
    const Anchor& anchor, bool positional, float scale, float forward = 0, float up = 0, float side = 0, const Quat* tilt = nullptr) {
    const auto inverse = yaw(-anchor.heading);
    const auto relative = first_person::normalized(first_person::multiply(inverse, eye.orientation));
    const auto offset = first_person::rotate(inverse,
        first_person::subtract(eye.position, positional ? anchor.position : head.position));
    // `tilt` (steady rig): the heading with a filtered pitch and roll.
    const auto turn = tilt ? first_person::normalized(first_person::multiply(yaw(base_heading), *tilt))
        : level ? yaw(base_heading) : first_person::orientation(base);
    // The seat nudge is in the camera frame: forward is -Z. It is not scaled by the world scale.
    const auto moved = first_person::rotate(turn, {offset[0] * scale + side, offset[1] * scale + up, offset[2] * scale - forward});
    Matrix result = base;
    first_person::write(result, first_person::normalized(first_person::multiply(turn, relative)),
        {base[12] + moved[0], base[13] + moved[1], base[14] + moved[2]});
    return result;
}

// Field-of-view tangents: left and down are negative.
struct Tangents {
    float left = -1, right = 1, up = 1, down = -1;
};

inline Tangents tangents_from_angles(float left, float right, float up, float down) {
    return {std::tan(left), std::tan(right), std::tan(up), std::tan(down)};
}

// The half-height tangent of one symmetric frustum, with the image's aspect
// (width / height), that covers both eyes, scaled by `scale`.
inline float render_tangent(const std::array<Tangents, 2>& eyes, float aspect, float scale) {
    float vertical = 0, horizontal = 0;
    for (const auto& eye : eyes) {
        vertical = std::max({vertical, std::abs(eye.up), std::abs(eye.down)});
        horizontal = std::max({horizontal, std::abs(eye.left), std::abs(eye.right)});
    }
    if (!(aspect > 0.05f) || !std::isfinite(aspect)) aspect = 1;
    return std::max(vertical, horizontal / aspect) * scale;
}

// The native camera's FOV in degrees for that frustum.
inline float native_fov_degrees(float tangent, float aspect, bool vertical) {
    return 2 * std::atan(vertical ? tangent : tangent * aspect) * 180 / pi;
}

// Field-of-view angles in radians, as OpenXR takes them: left and down negative.
struct Angles {
    float left = 0, right = 0, up = 0, down = 0;
};

// The frustum actually rendered.
inline Angles layer_angles(float tangent, float aspect) {
    const float h = std::atan(tangent * aspect), v = std::atan(tangent);
    return {-h, h, v, -v};
}
}
