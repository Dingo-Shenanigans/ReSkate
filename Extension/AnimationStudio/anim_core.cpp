#include "skater_effectors.h"
#include "skater_mirror.h"
#include "skater_joint_limits.h"
#include "anim_core.h"
#include "animation_skeleton.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dingosdk::anim_studio {
namespace {
constexpr float pi = 3.14159265358979f;
float dot(const Quat& a, const Quat& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
Vec3 lerp(const Vec3& a, const Vec3& b, float t) noexcept {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
// Cubic Hermite through p1 -> p2 with Catmull-Rom slopes from the neighbours.
Vec3 hermite(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float u, float left_scale, float right_scale) noexcept {
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float m1 = (p2[i] - p0[i]) * left_scale, m2 = (p3[i] - p1[i]) * right_scale;
        out[i] = h00 * p1[i] + h10 * m1 + h01 * p2[i] + h11 * m2;
    }
    return out;
}
void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
float number(const Json& value, const char* what) {
    require(value.is_number(), what);
    const auto result = value.get<float>();
    require(std::isfinite(result), what);
    return result;
}
template <std::size_t N> std::array<float, N> numbers(const Json& value, const char* what) {
    require(value.is_array() && value.size() == N, what);
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = number(value.at(i), what);
    return out;
}
std::string_view ease_name(Ease ease) noexcept {
    return ease == Ease::linear ? "linear" : ease == Ease::step ? "step" : "smooth";
}

constexpr std::array<EditableJoint, 25> joints{{
    {board_joint, "Board", true},
    {7, "Hips", true},
    {42, "Lower back", false},
    {44, "Chest", false},
    {101, "Neck", false},
    {103, "Head", false},
    {275, "Left shoulder", false},
    {276, "Left arm", false},
    {277, "Left elbow", false},
    {278, "Left hand", true},
    {46, "Right shoulder", false},
    {47, "Right arm", false},
    {48, "Right elbow", false},
    {49, "Right hand", true},
    {341, "Left leg", false},
    {342, "Left knee", false},
    {343, "Left foot", true},
    {344, "Left toes", false},
    {8, "Right leg", false},
    {9, "Right knee", false},
    {10, "Right foot", true},
    {11, "Right toes", false},
    {43, "Middle back", false},
    {45, "Upper chest", false},
    {102, "Upper neck", false},
}};
} // namespace

Quat multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat conjugate(const Quat& q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }
Quat normalised(const Quat& q) noexcept {
    const float length = std::sqrt(dot(q, q));
    if (!(length > 1e-8f) || !std::isfinite(length)) return {0, 0, 0, 1};
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}
Quat slerp(Quat a, const Quat& b, float t) noexcept {
    float cosine = dot(a, b);
    if (cosine < 0) {
        a = {-a[0], -a[1], -a[2], -a[3]};
        cosine = -cosine;
    }
    if (cosine > 0.9995f) {
        Quat out{};
        for (std::size_t i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
        return normalised(out);
    }
    const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f)), sine = std::sin(angle);
    const float wa = std::sin((1 - t) * angle) / sine, wb = std::sin(t * angle) / sine;
    return normalised({a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb, a[2] * wa + b[2] * wb, a[3] * wa + b[3] * wb});
}
Vec3 rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 u{q[0], q[1], q[2]};
    const float s = q[3];
    const Vec3 t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    return {v[0] + s * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + s * t[1] + (u[2] * t[0] - u[0] * t[2]),
            v[2] + s * t[2] + (u[0] * t[1] - u[1] * t[0])};
}
Quat axis_angle(const Vec3& axis, float radians) noexcept {
    const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(length > 1e-8f)) return {0, 0, 0, 1};
    const float s = std::sin(radians * 0.5f) / length;
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(radians * 0.5f)};
}
Quat from_euler(const Vec3& degrees) noexcept {
    const float k = pi / 180;
    return normalised(multiply(multiply(axis_angle({1, 0, 0}, degrees[0] * k), axis_angle({0, 1, 0}, degrees[1] * k)),
                               axis_angle({0, 0, 1}, degrees[2] * k)));
}
Vec3 to_euler(const Quat& raw) noexcept {
    const auto q = normalised(raw);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
    const float m12 = 2 * (y * z - x * w), m22 = 1 - 2 * (x * x + y * y);
    const float k = 180 / pi;
    const float b = std::asin(std::clamp(m02, -1.0f, 1.0f));
    if (std::abs(m02) > 0.9999f) {
        // Gimbal lock: all of the turn about X and Z goes on X.
        const float m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z);
        return {std::atan2(m10, m11) * k, b * k, 0};
    }
    return {std::atan2(-m12, m22) * k, b * k, std::atan2(-m01, m00) * k};
}
bool finite(const Joint& joint) noexcept {
    for (const float v : joint.scale) if (!std::isfinite(v) || std::abs(v) > 1000) return false;
    for (const float v : joint.rotation) if (!std::isfinite(v) || std::abs(v) > 1.01f) return false;
    for (const float v : joint.position) if (!std::isfinite(v) || std::abs(v) > 1e6f) return false;
    return true;
}

std::span<const EditableJoint> editable_joints() noexcept { return joints; }
const EditableJoint* editable(std::uint16_t joint) noexcept {
    for (const auto& j : joints) if (j.index == joint) return &j;
    return nullptr;
}

void Clip::sample(float seconds, std::span<Joint> out) const noexcept {
    if (empty() || !std::isfinite(seconds) || frames.size() != times.size() * joints) return;
    const auto count = std::min<std::size_t>(out.size(), joints);
    if (seconds <= times.front() || times.size() == 1) {
        std::copy_n(frames.begin(), count, out.begin());
        return;
    }
    if (seconds >= times.back()) {
        std::copy_n(frames.begin() + (times.size() - 1) * joints, count, out.begin());
        return;
    }
    const auto next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), seconds) - times.begin());
    const auto previous = next - 1;
    const float span = times[next] - times[previous];
    const float t = span > 1e-6f ? (seconds - times[previous]) / span : 0.0f;
    const auto* a = frames.data() + previous * joints;
    const auto* b = frames.data() + next * joints;
    for (std::size_t i = 0; i < count; ++i) {
        out[i].scale = lerp(a[i].scale, b[i].scale, t);
        out[i].rotation = slerp(a[i].rotation, b[i].rotation, t);
        out[i].position = lerp(a[i].position, b[i].position, t);
    }
}

bool Clip::sample_board(float seconds, Joint& out) const noexcept {
    if (empty() || !std::isfinite(seconds) || board.size() != times.size()) return false;
    if (seconds <= times.front() || times.size() == 1) {
        out = board.front();
        return true;
    }
    if (seconds >= times.back()) {
        out = board.back();
        return true;
    }
    const auto next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), seconds) - times.begin());
    const auto previous = next - 1;
    const float span = times[next] - times[previous];
    const float t = span > 1e-6f ? (seconds - times[previous]) / span : 0.0f;
    out.scale = lerp(board[previous].scale, board[next].scale, t);
    out.rotation = slerp(board[previous].rotation, board[next].rotation, t);
    out.position = lerp(board[previous].position, board[next].position, t);
    return true;
}

bool clip_has_vertical_trajectory(const Clip& clip) noexcept {
    if (clip.empty() || clip.joints <= placement_joint || clip.frames.size() != clip.times.size() * clip.joints ||
        clip.takeoff < 0 || clip.landing <= clip.takeoff || clip.landing > clip.duration()) return false;
    // AITrajectory has just the reference above it. Interpolate their world heights at the
    // phase markers without allocating a full pose for every trajectory sample.
    const auto height = [&](std::size_t i) { return world(clip.frame(i), placement_joint).position[1]; };
    const auto at = [&](float t) {
        if (t <= clip.times.front()) return height(0);
        if (t >= clip.times.back()) return height(clip.times.size() - 1);
        const auto next = static_cast<std::size_t>(std::upper_bound(clip.times.begin(), clip.times.end(), t) - clip.times.begin());
        const float u = (t - clip.times[next - 1]) / (clip.times[next] - clip.times[next - 1]);
        return height(next - 1) * (1 - u) + height(next) * u;
    };
    const float start = at(clip.takeoff), end = at(clip.landing);
    if (!std::isfinite(start) || !std::isfinite(end)) return false;
    for (std::size_t i = 0; i < clip.times.size(); ++i) {
        const auto t = clip.times[i];
        if (t <= clip.takeoff || t >= clip.landing) continue;
        const auto u = (t - clip.takeoff) / (clip.landing - clip.takeoff);
        if (height(i) - (start * (1 - u) + end * u) > 0.12f) return true;
    }
    return false;
}

namespace {
Quat quat_log(const Quat& q) noexcept { // a unit rotation as its half turn, a pure quaternion
    const float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (s < 1e-6f) return {q[0], q[1], q[2], 0};
    const float k = std::atan2(s, q[3]) / s;
    return {q[0] * k, q[1] * k, q[2] * k, 0};
}
Quat quat_exp(const Quat& p) noexcept {
    const float s = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (s < 1e-6f) return normalised({p[0], p[1], p[2], 1});
    const float k = std::sin(s) / s;
    return {p[0] * k, p[1] * k, p[2] * k, std::cos(s)};
}
// The same turn as q, on the side of the sphere that is nearest to `ref` (the shorter way round).
Quat nearest(const Quat& q, const Quat& ref) noexcept {
    const float d = q[0] * ref[0] + q[1] * ref[1] + q[2] * ref[2] + q[3] * ref[3];
    return d < 0 ? Quat{-q[0], -q[1], -q[2], -q[3]} : q;
}
// Spherical Bezier control in the key's local tangent space. Timestamp-scaled velocity is
// shared by adjacent segments; uniform SQUAD controls break this at unequally spaced keys.
Quat rotation_control(const Quat& prev, const Quat& q, const Quat& next, float scale) noexcept {
    const Quat inverse = conjugate(q);
    const Quat a = quat_log(multiply(inverse, nearest(prev, q))), b = quat_log(multiply(inverse, nearest(next, q)));
    return normalised(multiply(q, quat_exp({(b[0] - a[0]) * scale, (b[1] - a[1]) * scale, (b[2] - a[2]) * scale, 0})));
}
// A smooth key's slope that never overshoots (a monotone cubic, as Blender's auto-clamped handles): flat where
// the curve turns - the key above or below both its neighbours - and at the ends, else a weighted harmonic mean
// of the slopes either side, so the curve never goes past a key, and never dips the other way before a rise.
float monotone_slope(float d0, float d1, float h0, float h1) noexcept {
    if (!(d0 * d1 > 0) || !(h0 > 0) || !(h1 > 0)) return 0;
    const float w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
}
// One channel between keys a and b (h apart), u of the way, with its neighbours where there are any.
float monotone_curve(std::optional<std::pair<float, float>> before, float pa, float pb, std::optional<std::pair<float, float>> after,
                     float h, float u) noexcept {
    const float d = h > 1e-6f ? (pb - pa) / h : 0.0f;
    const float ma = before ? monotone_slope((pa - before->second) / before->first, d, before->first, h) : 0.0f;
    const float mb = after ? monotone_slope(d, (after->second - pb) / after->first, h, after->first) : 0.0f;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * pa + (u3 - 2 * u2 + u) * h * ma + (-2 * u3 + 3 * u2) * pb + (u3 - u2) * h * mb;
}
} // namespace

Offset evaluate(const Track& track, float seconds) noexcept {
    const auto& keys = track.keys;
    if (keys.empty()) return {};
    if (seconds <= keys.front().time) return {keys.front().rotation, keys.front().offset};
    if (seconds >= keys.back().time) return {keys.back().rotation, keys.back().offset};
    std::size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].time <= seconds) ++i;
    const auto& a = keys[i];
    const auto& b = keys[i + 1];
    const float span = b.time - a.time;
    const float u = span > 1e-6f ? std::clamp((seconds - a.time) / span, 0.0f, 1.0f) : 1.0f;
    switch (a.ease) {
    case Ease::step:
        return {a.rotation, a.offset};
    case Ease::linear:
        return {slerp(a.rotation, b.rotation, u), lerp(a.offset, b.offset, u)};
    case Ease::smooth:
    default: {
        // Smooth keys never overshoot (monotone_curve): a move or a turn eases between its keys without going past
        // either, flat at the ends - the brackets' keys - and where it turns back. (A curve shaped by its
        // neighbours alone went past its keys: below zero before a rise off a bracket, and the board under the floor.)
        const bool has_before = i > 0, has_after = i + 2 < keys.size();
        const float h0 = has_before ? a.time - keys[i - 1].time : 0.0f, h2 = has_after ? keys[i + 2].time - b.time : 0.0f;
        Vec3 offset{};
        for (std::size_t c = 0; c < 3; ++c)
            offset[c] = monotone_curve(has_before ? std::optional<std::pair<float, float>>({h0, keys[i - 1].offset[c]}) : std::nullopt,
                                       a.offset[c], b.offset[c],
                                       has_after ? std::optional<std::pair<float, float>>({h2, keys[i + 2].offset[c]}) : std::nullopt, span, u);
        // The turn the same way, in the frame of key a: each key's turn from a as a rotation vector, each part of it
        // eased as above, then turned back into a rotation.
        // Each neighbour is measured from the key next to it and the steps added up, so a trick that turns on past
        // half a turn keeps turning the same way instead of reading as a turn back.
        const auto step = [](const Quat& from, const Quat& to) { return quat_log(multiply(conjugate(from), nearest(to, from))); };
        const Quat lb = step(a.rotation, b.rotation);
        Quat lp{0, 0, 0, 0}, ln{0, 0, 0, 0};
        if (has_before) {
            const Quat back = step(a.rotation, keys[i - 1].rotation);
            lp = back;
        }
        if (has_after) {
            const Quat on = step(b.rotation, keys[i + 2].rotation);
            ln = {lb[0] + on[0], lb[1] + on[1], lb[2] + on[2], 0};
        }
        Quat turn{0, 0, 0, 0};
        for (std::size_t c = 0; c < 3; ++c)
            turn[c] = monotone_curve(has_before ? std::optional<std::pair<float, float>>({h0, lp[c]}) : std::nullopt, 0.0f, lb[c],
                                     has_after ? std::optional<std::pair<float, float>>({h2, ln[c]}) : std::nullopt, span, u);
        return {normalised(multiply(a.rotation, quat_exp(turn))), offset};
    }
    }
}

float channel_value(const Offset& edit, int channel) noexcept {
    if (channel < 3) return to_euler(edit.rotation)[static_cast<std::size_t>(std::clamp(channel, 0, 2))];
    return edit.offset[static_cast<std::size_t>(std::clamp(channel - 3, 0, 2))];
}
Offset with_channel(Offset edit, int channel, float value) noexcept {
    if (!std::isfinite(value)) return edit;
    if (channel < 3) {
        auto euler = to_euler(edit.rotation);
        euler[static_cast<std::size_t>(std::clamp(channel, 0, 2))] = value;
        edit.rotation = from_euler(euler);
    } else edit.offset[static_cast<std::size_t>(std::clamp(channel - 3, 0, 2))] = value;
    return edit;
}
Track* Animation::track(std::uint16_t joint) noexcept {
    for (auto& t : tracks) if (t.joint == joint) return &t;
    return nullptr;
}
const Track* Animation::track(std::uint16_t joint) const noexcept {
    for (const auto& t : tracks) if (t.joint == joint) return &t;
    return nullptr;
}
std::size_t Animation::key_count() const noexcept {
    std::size_t n = 0;
    for (const auto& t : tracks) n += t.keys.size();
    return n;
}
void apply_edits(const Animation& animation, float seconds, std::span<Joint> pose) noexcept {
    if (!std::isfinite(seconds) || seconds < animation.start || (animation.end >= 0 && seconds > animation.end)) return;
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || track.joint >= pose.size()) continue;
        const auto edit = evaluate(track, seconds);
        auto& joint = pose[track.joint];
        joint.rotation = normalised(multiply(joint.rotation, edit.rotation));
        for (std::size_t i = 0; i < 3; ++i) joint.position[i] += edit.offset[i];
    }
}
void retarget_edits(const Animation& animation, float seconds, std::span<const Joint> source,
                    std::span<Joint> target, bool mirror, bool reverse_facing) {
    if (!std::isfinite(seconds) || seconds < animation.start || (animation.end >= 0 && seconds > animation.end) ||
        source.size() < skeleton_joints || target.size() < skeleton_joints) return;
    // Read every frame from the unmodified poses: editing a parent must not change the
    // coordinate conversion for its children later in the track list.
    const std::vector<Joint> native(target.begin(), target.end());
    const auto source_placement = world(source, placement_joint).rotation;
    const auto target_placement = world(native, placement_joint).rotation;
    // Switch changes the body's facing relative to placement independently of
    // which limbs lead. Transfer deltas through that half turn before converting
    // to the live joint basis; a reflection alone reverses local X/Z edits.
    constexpr Quat half_turn{0, 1, 0, 0};
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || !editable(track.joint) || track.joint >= skeleton_joints) continue;
        const auto to = mirror ? mirror_joint[track.joint] : track.joint;
        const auto edit = evaluate(track, seconds);
        if (edit.rotation[0] == 0 && edit.rotation[1] == 0 && edit.rotation[2] == 0 &&
            edit.offset == Vec3{}) continue; // Neutral bracket keys preserve native transforms exactly.
        auto& joint = target[to];
        if (edit.rotation[0] != 0 || edit.rotation[1] != 0 || edit.rotation[2] != 0) {
            const auto src_frame = normalised(multiply(conjugate(source_placement), world(source, track.joint).rotation));
            const auto dst_frame = normalised(multiply(conjugate(target_placement), world(native, to).rotation));
            auto delta = normalised(multiply(multiply(src_frame, edit.rotation), conjugate(src_frame)));
            if (mirror) delta = mirror_rotation(delta);
            if (reverse_facing) delta = normalised(multiply(multiply(half_turn, delta), conjugate(half_turn)));
            const auto local_delta = normalised(multiply(multiply(conjugate(dst_frame), delta), dst_frame));
            joint.rotation = normalised(multiply(native[to].rotation, local_delta));
        }
        const auto sp = parent(track.joint), tp = parent(to);
        if (sp >= 0 && tp >= 0) {
            const auto source_parent = world(source, static_cast<std::uint16_t>(sp));
            const auto target_parent = world(native, static_cast<std::uint16_t>(tp));
            const auto src_parent = normalised(multiply(conjugate(source_placement), source_parent.rotation));
            const auto dst_parent = normalised(multiply(conjugate(target_placement), target_parent.rotation));
            auto offset = rotate(src_parent, edit.offset);
            if (mirror) offset = mirror_vector(offset);
            if (reverse_facing) offset = rotate(half_turn, offset);
            offset = rotate(conjugate(dst_parent), offset);
            const float source_scale = world(source, placement_joint).scale[0];
            const float target_scale = world(native, placement_joint).scale[0];
            if (std::abs(source_scale) > 1e-6f && std::abs(target_parent.scale[0]) > 1e-6f) {
                const float ratio = source_parent.scale[0] / source_scale * target_scale / target_parent.scale[0];
                for (std::size_t k = 0; k < 3; ++k) joint.position[k] = native[to].position[k] + offset[k] * ratio;
            }
        }
    }
}
int limb_group(std::uint16_t joint) noexcept {
    // The arms and legs by their own chains, read off the skeleton: everything hanging under the shoulder
    // or the thigh belongs to that limb, including the hands and the fingers.
    for (int at = joint; at >= 0; at = parent(static_cast<std::uint16_t>(at))) {
        switch (at) {
        case 275: return 1; // left shoulder and below
        case 46: return 2;  // right shoulder and below
        case 341: return 3; // left thigh and below
        case 8: return 4;   // right thigh and below
        default: break;
        }
    }
    return 0;
}
void apply_authored_edit_layer(const Animation& animation, float seconds, std::span<Joint> native, float weight,
                               const LimbWeights* limbs, std::span<const Joint> studio, float match, float match_rest) {
    if (!std::isfinite(weight) || weight <= 0 || !std::isfinite(seconds) ||
        seconds < animation.start || (animation.end >= 0 && seconds > animation.end)) return;
    weight = std::min(weight, 1.0f);
    // The whole body to the studio's pose first; the keys then go on top of it.
    if (match_rest > 0 && studio.size() >= skeleton_joints && native.size() >= skeleton_joints) {
        const float rest = std::min(match_rest, 1.0f) * weight;
        for (std::uint16_t j = 0; j < skeleton_joints; ++j) {
            if (!gameplay_body_joint(j) || !editable(j)) continue;
            const float share = limbs ? rest * std::clamp(limbs->of(j), 0.0f, 1.0f) : rest;
            if (share <= 0) continue;
            native[j].rotation = slerp(native[j].rotation, studio[j].rotation, share);
            // Where each joint sits too - the hips above all, which carry how high and how far forward
            // he is over the board; the rest are bone lengths and match already.
            for (std::size_t k = 0; k < 3; ++k) native[j].position[k] += (studio[j].position[k] - native[j].position[k]) * share;
        }
    }
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || track.joint >= skeleton_joints ||
            track.joint >= native.size() || !editable(track.joint)) continue;
        const float share = limbs ? weight * std::clamp(limbs->of(track.joint), 0.0f, 1.0f) : weight;
        if (share <= 0) continue; // this limb has been given back to the game
        const auto edit = evaluate(track, seconds);
        auto& current = native[track.joint];
        if (edit.rotation[0] != 0 || edit.rotation[1] != 0 || edit.rotation[2] != 0) {
            auto wanted = normalised(multiply(current.rotation, edit.rotation));
            // Matching the studio: the bend laid on the pose it was made on, as far as the joint is really
            // bent - nothing up to 3 degrees (a key that only holds the pose), all of it from 15.
            if (match > 0 && track.joint < studio.size()) {
                const float bend = 2 * std::acos(std::clamp(std::abs(normalised(edit.rotation)[3]), 0.0f, 1.0f));
                const float x = std::clamp((bend - 0.052f) / 0.21f, 0.0f, 1.0f);
                const float how_far = std::min(match, 1.0f) * x * x * (3 - 2 * x);
                if (how_far > 0) {
                    const auto as_made = normalised(multiply(studio[track.joint].rotation, edit.rotation));
                    wanted = slerp(wanted, as_made, how_far);
                }
            }
            current.rotation = share >= 1 ? wanted : slerp(current.rotation, wanted, share);
        }
        for (std::size_t k = 0; k < 3; ++k) current.position[k] += edit.offset[k] * share;
    }
}
std::vector<Joint> studio_pose(const Clip& clip, float seconds, bool mirrored) {
    std::vector<Joint> pose(clip.joints);
    if (clip.empty()) return pose;
    clip.sample(std::clamp(seconds, 0.0f, clip.duration()), pose);
    if (!mirrored || pose.size() < skeleton_joints) return pose;
    std::vector<Joint> out(pose.size());
    for (std::size_t j = 0; j < skeleton_joints; ++j) out[mirror_joint[j]] = mirror_pose(pose[j]);
    for (std::size_t j = skeleton_joints; j < pose.size(); ++j) out[j] = pose[j];
    return out;
}
std::uint32_t crc32_of(const std::string& data) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
// A zip with every file stored as it is (no compression): any unzip program, and Import, read it.
std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    const auto u16 = [](std::string& s, std::uint16_t v) { s.push_back(static_cast<char>(v & 0xFF)); s.push_back(static_cast<char>(v >> 8)); };
    const auto u32 = [&](std::string& s, std::uint32_t v) { u16(s, static_cast<std::uint16_t>(v & 0xFFFF)); u16(s, static_cast<std::uint16_t>(v >> 16)); };
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = crc32_of(data);
        const auto size = static_cast<std::uint32_t>(data.size());
        u32(out, 0x04034b50u); u16(out, 20); u16(out, 0x0800); u16(out, 0); u16(out, 0); u16(out, 0x21);
        u32(out, crc); u32(out, size); u32(out, size); u16(out, static_cast<std::uint16_t>(name.size())); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014b50u); u16(central, 20); u16(central, 20); u16(central, 0x0800); u16(central, 0); u16(central, 0); u16(central, 0x21);
        u32(central, crc); u32(central, size); u32(central, size); u16(central, static_cast<std::uint16_t>(name.size())); u16(central, 0); u16(central, 0);
        u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
        central += name;
    }
    const auto at = static_cast<std::uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50u); u16(out, 0); u16(out, 0); u16(out, static_cast<std::uint16_t>(files.size())); u16(out, static_cast<std::uint16_t>(files.size()));
    u32(out, static_cast<std::uint32_t>(central.size())); u32(out, at); u16(out, 0);
    return out;
}
std::optional<bool> clip_leads_right(const Clip& clip) {
    if (clip.empty() || clip.joints < skeleton_joints) return std::nullopt;
    const float before_pop = clip.takeoff > 0.25f ? clip.takeoff - 0.2f : 0.0f;
    std::vector<Joint> pose(clip.joints);
    clip.sample(before_pop, pose);
    const auto placed = world(pose, placement_joint);
    const auto side = [&](std::uint16_t foot) {
        const auto w = world(pose, foot).position;
        return rotate(conjugate(placed.rotation), Vec3{w[0] - placed.position[0], w[1] - placed.position[1], w[2] - placed.position[2]})[0];
    };
    const float apart = side(10) - side(343); // right foot's x less the left foot's
    if (!std::isfinite(apart) || std::abs(apart) < 0.1f) return std::nullopt;
    return apart > 0;
}
bool place_native_marks(Clip& clip) noexcept {
    const auto n = clip.times.size();
    if (!(clip.takeoff >= 0) || n < 8 || clip.board.size() != n) return false;
    constexpr float tipped = 6.0f;     // degrees the board turns from how it starts: the tail is down
    constexpr float leaves = 0.05f;    // and the board leaves the ground this long after
    constexpr float tolerance = 0.04f; // the library's good pops are within a frame of it
    bool moved = false;
    const auto& rest = clip.board.front().rotation;
    for (std::size_t i = 1; i < n; ++i) {
        const auto& q = clip.board[i].rotation;
        const float d = std::abs(rest[0] * q[0] + rest[1] * q[1] + rest[2] * q[2] + rest[3] * q[3]);
        if (!std::isfinite(d) || 2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.2957795f <= tipped) continue;
        const float pop = clip.times[i] + leaves;
        // Only ever earlier: a late pop is the library's mistake.
        if (pop < clip.takeoff - tolerance && pop < clip.duration() - 0.3f) {
            clip.takeoff = pop;
            moved = true;
        }
        break;
    }
    // The landing: the board's height under the placement, over five frames, at its deepest low.
    std::vector<float> smooth(n, 0.0f);
    for (std::size_t i = 2; i + 2 < n; ++i) {
        float sum = 0;
        for (std::size_t j = i - 2; j <= i + 2; ++j) sum += clip.board[j].position[1];
        smooth[i] = sum / 5;
    }
    std::optional<std::size_t> deepest;
    for (std::size_t i = 3; i + 3 < n; ++i) {
        if (clip.times[i] < clip.takeoff + 0.25f - 1e-3f || clip.times[i] > clip.takeoff + 0.85f + 1e-3f) continue;
        if (!(smooth[i] <= smooth[i - 1] && smooth[i] < smooth[i + 1])) continue;
        if (!deepest || smooth[i] < smooth[*deepest]) deepest = i;
    }
    if (deepest && std::isfinite(smooth[*deepest]) && (clip.landing < 0 || std::abs(clip.times[*deepest] - clip.landing) > 0.05f)) {
        clip.landing = clip.times[*deepest];
        moved = true;
    }
    return moved;
}
namespace {
const JointLimit* limit_of(std::uint16_t joint) noexcept {
    for (const auto& l : joint_limits)
        if (l.joint == joint) return &l;
    return nullptr;
}
float dot3(const Vec3& a, const Vec3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
} // namespace
bool has_joint_limit(std::uint16_t joint) noexcept { return limit_of(joint) != nullptr; }
Quat limit_rotation(std::uint16_t joint, const Quat& local) noexcept {
    const auto* l = limit_of(joint);
    if (!l) return local;
    const Quat ref{l->reference[0], l->reference[1], l->reference[2], l->reference[3]};
    const Vec3 axis{l->axis[0], l->axis[1], l->axis[2]}, across{l->across[0], l->across[1], l->across[2]}, up{l->up[0], l->up[1], l->up[2]};
    // The turn from the average pose, split into twist about the bone and the bone's swing away from it.
    auto d = normalised(multiply(conjugate(ref), local));
    if (d[3] < 0) d = {-d[0], -d[1], -d[2], -d[3]};
    const float along = dot3({d[0], d[1], d[2]}, axis);
    float twist = 2 * std::atan2(along, d[3]);
    if (twist > 3.14159265f) twist -= 6.2831853f;
    if (twist < -3.14159265f) twist += 6.2831853f;
    const auto twist_q = normalised(Quat{axis[0] * along, axis[1] * along, axis[2] * along, d[3]});
    auto swing = normalised(multiply(d, conjugate(twist_q)));
    if (swing[3] < 0) swing = {-swing[0], -swing[1], -swing[2], -swing[3]};
    const Vec3 sv{swing[0], swing[1], swing[2]};
    const float n = std::sqrt(dot3(sv, sv));
    float angle = 2 * std::atan2(n, swing[3]);
    // How far it may swing this way: between the two measured directions either side of it.
    bool changed = false;
    Vec3 dir{1, 0, 0};
    if (n > 1e-6f) {
        dir = {sv[0] / n, sv[1] / n, sv[2] / n};
        const float phi = std::atan2(dot3(dir, up), dot3(dir, across));
        const float at = (phi + 3.14159265f) / 6.2831853f * static_cast<float>(l->swing.size()) - 0.5f;
        const float below = std::floor(at);
        const auto count = static_cast<int>(l->swing.size());
        const auto i0 = ((static_cast<int>(below) % count) + count) % count, i1 = (i0 + 1) % count;
        const float f = at - below;
        const float most = l->swing[static_cast<std::size_t>(i0)] * (1 - f) + l->swing[static_cast<std::size_t>(i1)] * f;
        if (angle > most) {
            angle = most;
            changed = true;
        }
    }
    if (twist < l->twist_min || twist > l->twist_max) {
        twist = std::clamp(twist, l->twist_min, l->twist_max);
        changed = true;
    }
    if (!changed) return local;
    const auto limited = multiply(axis_angle(dir, angle), axis_angle(axis, twist));
    return normalised(multiply(ref, limited));
}
void limit_keyed_joints(const Animation& animation, float seconds, std::span<Joint> pose) noexcept {
    if (!std::isfinite(seconds)) return;
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || track.joint >= pose.size() || !has_joint_limit(track.joint)) continue;
        const auto edit = evaluate(track, seconds);
        if (edit.rotation[0] == 0 && edit.rotation[1] == 0 && edit.rotation[2] == 0) continue; // only holds the pose
        pose[track.joint].rotation = limit_rotation(track.joint, pose[track.joint].rotation);
    }
}
void apply_native_edit_layer(const Animation& animation, float seconds, std::span<const Joint> source,
                             std::span<Joint> native, bool mirror, float weight, bool reverse_facing) {
    if (!std::isfinite(weight) || weight <= 0 || native.size() < skeleton_joints) return;
    weight = std::min(weight, 1.0f);
    std::vector<Joint> edited(native.begin(), native.end());
    retarget_edits(animation, seconds, source, edited, mirror, reverse_facing);
    for (const auto& editable_joint : editable_joints()) {
        const auto i = editable_joint.index;
        if (i >= skeleton_joints) continue; // The physical board has its own fresh publication.
        auto& current = native[i]; const auto& wanted = edited[i];
        if (current.rotation != wanted.rotation) current.rotation = slerp(current.rotation, wanted.rotation, weight);
        if (current.position != wanted.position) current.position = lerp(current.position, wanted.position, weight);
    }
}
std::vector<Offset> pose_edit_deltas(std::span<const Joint> native, std::span<const Joint> edited) {
    std::vector<Offset> deltas(std::min(native.size(), edited.size()));
    for (const auto& joint : editable_joints()) {
        const auto i = joint.index; if (i >= deltas.size()) continue;
        if (native[i].rotation != edited[i].rotation)
            deltas[i].rotation = normalised(multiply(conjugate(native[i].rotation), edited[i].rotation));
        for (std::size_t k = 0; k < 3; ++k) deltas[i].offset[k] = edited[i].position[k] - native[i].position[k];
    }
    return deltas;
}
void apply_pose_deltas(std::span<const Offset> deltas, std::span<Joint> native) {
    for (const auto& joint : editable_joints()) {
        const auto i = joint.index; if (i >= deltas.size() || i >= native.size()) continue;
        if (deltas[i].rotation != Quat{0, 0, 0, 1}) native[i].rotation = normalised(multiply(native[i].rotation, deltas[i].rotation));
        for (std::size_t k = 0; k < 3; ++k) native[i].position[k] += deltas[i].offset[k];
    }
}
Joint board_edited(const Joint& board, const Animation& edits, float seconds, const Quat& ground, bool mirror) {
    if (!std::isfinite(seconds) || seconds < edits.start || (edits.end >= 0 && seconds > edits.end)) return board;
    const auto* track = edits.track(board_joint);
    if (!track || track->hidden || track->keys.empty()) return board;
    auto edit = evaluate(*track, seconds);
    if (edit.rotation[0] == 0 && edit.rotation[1] == 0 && edit.rotation[2] == 0 && edit.offset == Vec3{}) return board;
    if (mirror) { edit.rotation = mirror_board_rotation(edit.rotation); edit.offset = mirror_board_vector(edit.offset); }
    auto out = board;
    const auto moved = rotate(ground, edit.offset);
    for (std::size_t i = 0; i < 3; ++i) out.position[i] += moved[i];
    if (edit.rotation[0] != 0 || edit.rotation[1] != 0 || edit.rotation[2] != 0)
        out.rotation = normalised(multiply(multiply(multiply(ground, edit.rotation), conjugate(ground)), board.rotation));
    return out;
}
float animation_window_weight(const Animation& animation, float seconds) noexcept {
    if (!std::isfinite(seconds) || seconds < animation.start || (animation.end >= 0 && seconds > animation.end)) return 0;
    const float width = animation.end >= 0 ? std::min(0.08f, std::max(0.0f, (animation.end - animation.start) * 0.5f)) : 0.08f;
    if (width <= 0) return 0;
    const auto ease = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); };
    float weight = animation.start > 0 ? ease((seconds - animation.start) / width) : 1.0f;
    if (animation.end >= 0) weight *= ease((animation.end - seconds) / width);
    return weight;
}
void shift_keys(Animation& animation, float delta, float duration) noexcept {
    if (!std::isfinite(delta) || !(duration >= 0)) return;
    for (auto& track : animation.tracks) {
        if (track.locked) continue;
        std::size_t kept = 0;
        for (auto key : track.keys) {
            key.time = std::clamp(key.time + delta, 0.0f, duration);
            if (kept && std::abs(track.keys[kept - 1].time - key.time) < 1e-6f) track.keys[kept - 1] = key;
            else track.keys[kept++] = key;
        }
        track.keys.resize(kept);
    }
    animation.start = std::max(0.0f, animation.start + delta);
}
float on_a_frame(float seconds) noexcept { return std::round(seconds * key_fps) / key_fps; }
bool move_window_edge(Animation& animation, int which, float to, float duration) noexcept {
    if (!std::isfinite(to) || !std::isfinite(duration) || duration < 0 || to < 0 || to > duration) return false;
    const float old_start = animation.start, old_end = animation.end < 0 ? duration : animation.end;
    if (!std::isfinite(old_start) || !std::isfinite(old_end) || old_end < old_start) return false;
    const float from = which == 1 ? old_start : which == 2 ? old_end : -1;
    if (from < 0) return false;
    const float at = on_a_frame(to);
    const float gap = 1.0f / key_fps;
    if (which == 1 && at > old_end - gap) return false;
    if (which == 2 && at < old_start + gap) return false;
    if (which == 1) animation.start = at;
    else animation.end = at;
    if (std::abs(at - from) < 1e-6f) return true;
    constexpr float merge = 0.5f / key_fps;
    for (auto& track : animation.tracks) {
        if (track.locked) continue;
        auto& keys = track.keys;
        const auto on_bracket = std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return std::abs(k.time - from) < merge; });
        if (on_bracket == keys.end()) continue;
        Key moved = *on_bracket;
        moved.time = at;
        keys.erase(on_bracket);
        // Whatever was already standing where it lands gives way to it.
        const auto clash = std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return std::abs(k.time - at) < merge; });
        if (clash != keys.end()) *clash = moved;
        else keys.insert(std::upper_bound(keys.begin(), keys.end(), at, [](float t, const Key& k) { return t < k.time; }), moved);
    }
    return true;
}
Key& set_key(Animation& animation, std::uint16_t joint, float seconds, const Offset& value, float merge) {
    auto* track = animation.track(joint);
    if (!track) {
        animation.tracks.push_back({joint, {}, false, false});
        track = &animation.tracks.back();
    }
    for (auto& key : track->keys)
        if (std::abs(key.time - seconds) <= merge) {
            key.rotation = normalised(value.rotation);
            key.offset = value.offset;
            return key;
        }
    Key key{seconds, normalised(value.rotation), value.offset, Ease::smooth};
    const auto at = std::upper_bound(track->keys.begin(), track->keys.end(), seconds,
                                     [](float t, const Key& k) { return t < k.time; });
    return *track->keys.insert(at, key);
}
bool remove_key(Animation& animation, std::uint16_t joint, float seconds, float merge) {
    auto* track = animation.track(joint);
    if (!track) return false;
    const auto before = track->keys.size();
    std::erase_if(track->keys, [&](const Key& k) { return std::abs(k.time - seconds) <= merge; });
    return track->keys.size() != before;
}
namespace {
// How far a key's turn is from the turn the curve would have had there anyway, in radians.
float turn_apart(const Quat& a, const Quat& b) noexcept {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::clamp(d, 0.0f, 1.0f));
}
// How far a smoothed key is drawn towards the line between its neighbours.
constexpr float smooth_pull = 0.35f;
void pull_towards_neighbours(Key& key, const Key& before, const Key& here, const Key& after) noexcept {
    const float span = after.time - before.time;
    const float u = span > 1e-6f ? (here.time - before.time) / span : 0.5f;
    key.offset = lerp(here.offset, lerp(before.offset, after.offset, u), smooth_pull);
    key.rotation = slerp(here.rotation, slerp(before.rotation, after.rotation, u), smooth_pull);
}
} // namespace

std::size_t smooth_track(Track& track, float keep_start, float keep_end) noexcept {
    auto& keys = track.keys;
    if (keys.size() < 2) return 0;
    const auto pinned = [&](const Key& k) {
        return std::abs(k.time - keep_start) < 1.0f / 60 || std::abs(k.time - keep_end) < 1.0f / 60;
    };
    std::size_t changed = 0;
    // Every key eases smoothly: one step or linear key in the middle breaks the curve either side of it.
    for (auto& k : keys)
        if (k.ease != Ease::smooth) {
            k.ease = Ease::smooth;
            ++changed;
        }
    // A key that sits where the line between its neighbours already goes says nothing: it is dropped, so the
    // curve through what is left is not pulled about by keys that were only ever noise.
    for (std::size_t i = 1; i + 1 < keys.size();) {
        const Key &before = keys[i - 1], &here = keys[i], &after = keys[i + 1];
        const float span = after.time - before.time;
        const float u = span > 1e-6f ? (here.time - before.time) / span : 0.5f;
        bool same = !pinned(here);
        if (same) {
            const auto guess = lerp(before.offset, after.offset, u);
            for (std::size_t c = 0; c < 3 && same; ++c) same = std::abs(guess[c] - here.offset[c]) < 0.002f;
            same = same && turn_apart(slerp(before.rotation, after.rotation, u), here.rotation) < 0.5f * 3.14159265f / 180;
        }
        if (same) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
            ++changed;
        } else ++i;
    }
    // What is left is drawn a little towards its neighbours, so a key that was placed by hand a frame off
    // its neighbours no longer shows as a flick.
    if (keys.size() > 2) {
        const auto was = keys;
        for (std::size_t i = 1; i + 1 < keys.size(); ++i) {
            if (pinned(keys[i])) continue;
            pull_towards_neighbours(keys[i], was[i - 1], was[i], was[i + 1]);
            ++changed;
        }
    }
    // On whole frames. Smoothing rebuilds the track, so this is where a key that drifted off a frame -
    // an older animation whose window was once stretched - is put back on one. The two keys on the
    // brackets are left exactly where they are, because the brackets are what they answer to.
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (pinned(keys[i])) continue;
        const float at = on_a_frame(keys[i].time);
        if (std::abs(at - keys[i].time) > 1e-6f) {
            keys[i].time = at;
            ++changed;
        }
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    for (std::size_t i = 1; i < keys.size();) {
        if (std::abs(keys[i].time - keys[i - 1].time) < 0.5f / key_fps) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
            ++changed;
        } else ++i;
    }
    for (auto& k : keys) k.smooth = true;
    return changed;
}
std::size_t smooth_keys_at(Track& track, std::span<const float> times, float keep_start, float keep_end) noexcept {
    auto& keys = track.keys;
    const auto chosen = [&](float t) { return std::any_of(times.begin(), times.end(), [&](float c) { return std::abs(c - t) < 1e-3f; }); };
    const auto pinned = [&](const Key& k) {
        return std::abs(k.time - keep_start) < 1.0f / 60 || std::abs(k.time - keep_end) < 1.0f / 60;
    };
    const auto was = keys; // neighbours as they were, so smoothing two keys side by side does not compound
    std::size_t changed = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!chosen(keys[i].time)) continue;
        if (keys[i].ease != Ease::smooth || !keys[i].smooth) {
            keys[i].ease = Ease::smooth;
            keys[i].smooth = true;
            ++changed;
        }
        if (i == 0 || i + 1 >= keys.size() || pinned(keys[i])) continue;
        pull_towards_neighbours(keys[i], was[i - 1], was[i], was[i + 1]);
        ++changed;
    }
    return changed;
}

std::size_t move_key(Track& track, std::size_t index, float to, float duration) noexcept {
    if (index >= track.keys.size() || !std::isfinite(to) || !std::isfinite(duration)) return index;
    auto key = track.keys[index];
    key.time = std::clamp(to, 0.0f, std::max(duration, 0.0f));
    track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(index));
    std::erase_if(track.keys, [&](const Key& existing) { return std::abs(existing.time - key.time) < 1e-6f; });
    const auto at = std::upper_bound(track.keys.begin(), track.keys.end(), key.time,
                                     [](float t, const Key& k) { return t < k.time; });
    const auto placed = static_cast<std::size_t>(at - track.keys.begin());
    track.keys.insert(at, key);
    return placed;
}

std::int16_t parent(std::uint16_t joint) noexcept {
    return joint < animation_parents.size() ? animation_parents[joint] : -1;
}
Joint world(std::span<const Joint> local, std::uint16_t joint) noexcept {
    std::array<std::uint16_t, 64> chain{};
    std::size_t length = 0;
    for (int at = joint; at >= 0 && at < static_cast<int>(local.size()) && length < chain.size(); at = parent(static_cast<std::uint16_t>(at)))
        chain[length++] = static_cast<std::uint16_t>(at);
    Joint result;
    float scale = 1;
    for (std::size_t i = length; i-- > 0;) {
        const auto& bone = local[chain[i]];
        const auto moved = rotate(result.rotation, {bone.position[0] * scale, bone.position[1] * scale, bone.position[2] * scale});
        for (std::size_t a = 0; a < 3; ++a) result.position[a] += moved[a];
        result.rotation = normalised(multiply(result.rotation, bone.rotation));
        scale *= (bone.scale[0] + bone.scale[1] + bone.scale[2]) / 3.0f;
    }
    result.scale = {scale, scale, scale};
    return result;
}

std::string to_json(const Animation& animation) {
    Json root = Json::object();
    root["format"] = "fourth-animation-studio";
    root["version"] = 1;
    root["name"] = animation.name;
    root["trick"] = animation.trick;
    if (!animation.author.empty()) root["author"] = animation.author;
    if (!animation.label.empty()) root["label"] = animation.label;
    root["speed"] = animation.speed;
    root["loop"] = animation.loop;
    root["pop"] = animation.pop;
    if (!animation.stance.empty()) root["stance"] = animation.stance;
    if (animation.switch_stance) root["switch_stance"] = *animation.switch_stance;
    root["source"] = Json{{"kind", animation.source.kind}, {"asset", animation.source.asset},
                          {"rig", animation.source.rig}, {"board_frame", animation.source.board_frame},
                          {"position_codec", animation.source.position_codec}};
    root["entry"] = animation.entry;
    if (animation.fakie) root["fakie"] = *animation.fakie;
    if (animation.board_backwards) root["board_backwards"] = *animation.board_backwards;
    if (!animation.upgraded.empty()) root["upgraded"] = animation.upgraded;
    if (animation.board_nose_forward) root["board_keys"] = "nose-forward";
    root["start"] = animation.start;
    root["end"] = animation.end;
    root["duration"] = animation.clip.duration();
    root["takeoff"] = animation.clip.takeoff;
    root["landing"] = animation.clip.landing;
    Json tracks = Json::array();
    for (const auto& track : animation.tracks) {
        if (track.keys.empty()) continue;
        Json t = Json::object();
        t["joint"] = track.joint;
        if (const auto* e = editable(track.joint)) t["label"] = std::string(e->label);
        t["hidden"] = track.hidden;
        t["locked"] = track.locked;
        Json keys = Json::array();
        for (const auto& key : track.keys) {
            Json k = Json::object();
            k["t"] = key.time;
            k["r"] = Json::array({key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]});
            k["p"] = Json::array({key.offset[0], key.offset[1], key.offset[2]});
            k["ease"] = std::string(ease_name(key.ease));
            if (key.smooth) k["smooth"] = true;
            keys.push_back(std::move(k));
        }
        t["keys"] = std::move(keys);
        tracks.push_back(std::move(t));
    }
    root["tracks"] = std::move(tracks);
    return root.dump(2);
}
Animation from_json(std::string_view text) {
    const auto root = Json::parse(text);
    require(root.is_object() && root.value("format", "") == "fourth-animation-studio", "not an Animation Studio profile");
    require(root.contains("version") && root.at("version").is_number() && root.at("version").get<int>() == 1,
            "the profile is from a newer Animation Studio");
    Animation result;
    result.name = root.value("name", "");
    result.trick = root.value("trick", "");
    result.author = root.value("author", "");
    result.label = root.value("label", "").substr(0, 48);
    result.speed = root.contains("speed") ? std::clamp(number(root.at("speed"), "bad speed"), 0.1f, 4.0f) : 1.0f;
    result.loop = root.contains("loop") && root.at("loop").is_boolean() ? root.at("loop").get<bool>() : true;
    result.pop = root.value("pop", "");
    result.stance = root.value("stance", "");
    result.entry = root.value("entry", "ground");
    if (root.contains("fakie") && root.at("fakie").is_boolean()) result.fakie = root.at("fakie").get<bool>();
    if (root.contains("board_backwards") && root.at("board_backwards").is_boolean()) result.board_backwards = root.at("board_backwards").get<bool>();
    result.upgraded = root.value("upgraded", "");
    require(result.upgraded.size() < 64, "bad upgrade mark");
    result.board_nose_forward = root.value("board_keys", "") == "nose-forward";
    require(result.entry == "ground" || result.entry == "rail" || result.entry == "unknown", "bad trick entry");
    if (root.contains("source")) {
        const auto& source = root.at("source");
        require(source.is_object(), "bad animation source");
        result.source = {source.value("kind", "unknown"), source.value("asset", ""), source.value("rig", ""), source.value("board_frame", "")};
        result.source.position_codec = source.value("position_codec", "");
        require(result.source.position_codec.size() < 128, "source position codec is too long");
        require(result.source.kind == "unknown" || result.source.kind == "native_asset" || result.source.kind == "render_capture", "bad source kind");
        require(result.source.asset.size() < 1024 && result.source.rig.size() < 1024 && result.source.board_frame.size() < 128, "source metadata too long");
    }
    if (root.contains("switch_stance")) {
        require(root.at("switch_stance").is_boolean(), "bad switch stance");
        result.switch_stance = root.at("switch_stance").get<bool>();
    }
    require(result.stance.empty() || result.stance == "regular" || result.stance == "goofy", "bad stance");
    result.start = root.contains("start") ? number(root.at("start"), "bad start bracket") : 0.0f;
    result.end = root.contains("end") ? number(root.at("end"), "bad end bracket") : -1.0f;
    if (result.pop != "low" && result.pop != "mid" && result.pop != "high") result.pop.clear();
    const float duration = root.contains("duration") ? number(root.at("duration"), "bad duration") : 0.0f;
    require(duration >= 0 && duration < 120, "bad duration");
    require(result.start >= 0 && result.start <= duration && (result.end == -1 ||
            (result.end >= result.start && result.end <= duration + 0.01f)), "bad animation window");
    std::size_t total = 0;
    if (root.contains("tracks")) {
        const auto& tracks = root.at("tracks");
        require(tracks.is_array() && tracks.size() <= joints.size(), "bad tracks");
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks.at(i);
            require(t.is_object() && t.contains("joint") && t.at("joint").is_number_integer(), "a track has no joint");
            const auto joint = t.at("joint").get<int>();
            require(joint >= 0 && joint < 65536 && editable(static_cast<std::uint16_t>(joint)), "a track names a joint the editor does not edit");
            require(!result.track(static_cast<std::uint16_t>(joint)), "two tracks for one joint");
            Track track{static_cast<std::uint16_t>(joint), {},
                        t.contains("hidden") && t.at("hidden").is_boolean() && t.at("hidden").get<bool>(),
                        t.contains("locked") && t.at("locked").is_boolean() && t.at("locked").get<bool>()};
            require(t.contains("keys") && t.at("keys").is_array(), "a track has no keys");
            const auto& keys = t.at("keys");
            total += keys.size();
            require(total <= 20000, "too many keys");
            for (std::size_t k = 0; k < keys.size(); ++k) {
                const auto& key = keys.at(k);
                require(key.is_object() && key.contains("t") && key.contains("r") && key.contains("p"), "a key is incomplete");
                Key value;
                value.time = number(key.at("t"), "bad key time");
                require(value.time >= 0 && value.time <= duration + 0.01f, "a key lies past the clip");
                value.rotation = normalised(numbers<4>(key.at("r"), "bad key rotation"));
                value.offset = numbers<3>(key.at("p"), "bad key offset");
                for (const float v : value.offset) require(std::abs(v) < 10, "a key moves a joint more than 10 m");
                const auto ease = key.value("ease", "smooth");
                value.ease = ease == "linear" ? Ease::linear : ease == "step" ? Ease::step : Ease::smooth;
                value.smooth = key.value("smooth", false);
                track.keys.push_back(value);
            }
            std::stable_sort(track.keys.begin(), track.keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
            result.tracks.push_back(std::move(track));
        }
    }
    // A piece of the game's own animation (a grind's loop, a grab's hold) is known by the clip it came from: one
    // saved before pieces were given that name gets it here, so it plays in game like one saved since.
    if (result.trick.empty() && result.source.kind == "native_asset" && !result.source.asset.empty()) result.trick = "native:" + result.source.asset;
    return result;
}

// "FAS3": the joints that never move in the clip (most of the face and the fingers) are stored once,
// the others every frame, and the board every frame when there is one. "FAS1" (no board) and "FAS2"
// (every joint every frame, then the board) still read.
std::vector<std::uint8_t> clip_bytes(const Clip& clip) {
    require(clip.joints > 0 && clip.joints <= 512 && !clip.times.empty() && clip.times.size() <= 20000 &&
            clip.frames.size() == clip.times.size() * clip.joints, "the clip has incomplete poses");
    require(clip.board.empty() || clip.board.size() == clip.times.size(), "the clip has incomplete board poses");
    for (std::size_t i = 0; i < clip.times.size(); ++i)
        require(std::isfinite(clip.times[i]) && clip.times[i] >= 0 && (i == 0 || clip.times[i] >= clip.times[i - 1]), "bad clip timestamps");
    for (const auto& joint : clip.frames) require(finite(joint), "the clip holds a broken pose");
    for (const auto& joint : clip.board) require(finite(joint), "the clip holds a broken board");
    std::vector<std::uint8_t> out;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    };
    const auto put_joint = [&](const Joint& j) {
        put(j.scale.data(), 12);
        put(j.rotation.data(), 16);
        put(j.position.data(), 12);
    };
    const bool board = clip.board.size() == clip.times.size() && !clip.board.empty();
    const std::uint32_t frames = static_cast<std::uint32_t>(clip.times.size());
    put("FAS3", 4);
    put(&clip.joints, 4);
    put(&frames, 4);
    put(&clip.takeoff, 4);
    put(&clip.landing, 4);
    const std::uint8_t has_board = board ? 1 : 0;
    put(&has_board, 1);
    put(clip.times.data(), clip.times.size() * 4);
    std::vector<std::uint8_t> moves(clip.joints, 0);
    const auto same = [](const Joint& a, const Joint& b) {
        for (std::size_t i = 0; i < 3; ++i)
            if (a.scale[i] != b.scale[i] || a.position[i] != b.position[i]) return false;
        for (std::size_t i = 0; i < 4; ++i)
            if (a.rotation[i] != b.rotation[i]) return false;
        return true;
    };
    for (std::uint32_t j = 0; j < clip.joints && frames; ++j)
        for (std::uint32_t f = 1; f < frames && !moves[j]; ++f) moves[j] = !same(clip.frames[f * clip.joints + j], clip.frames[j]);
    put(moves.data(), moves.size());
    for (std::uint32_t j = 0; j < clip.joints && frames; ++j)
        if (!moves[j]) put_joint(clip.frames[j]);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t j = 0; j < clip.joints; ++j)
            if (moves[j]) put_joint(clip.frames[f * clip.joints + j]);
        if (board) put_joint(clip.board[f]);
    }
    return out;
}
Clip clip_from_bytes(std::span<const std::uint8_t> bytes) {
    std::size_t at = 0;
    const auto get = [&](void* data, std::size_t size) {
        require(bytes.size() - at >= size, "the clip file is cut short");
        std::memcpy(data, bytes.data() + at, size);
        at += size;
    };
    const auto get_joint = [&](Joint& j, const char* what) {
        get(j.scale.data(), 12);
        get(j.rotation.data(), 16);
        get(j.position.data(), 12);
        require(finite(j), what);
    };
    char magic[4]{};
    require(bytes.size() >= 20, "the clip file is cut short");
    get(magic, 4);
    const bool v3 = std::memcmp(magic, "FAS3", 4) == 0;
    bool board = std::memcmp(magic, "FAS2", 4) == 0;
    require(v3 || board || std::memcmp(magic, "FAS1", 4) == 0, "not an Animation Studio clip");
    Clip clip;
    std::uint32_t frames{};
    get(&clip.joints, 4);
    get(&frames, 4);
    get(&clip.takeoff, 4);
    get(&clip.landing, 4);
    require(clip.joints > 0 && clip.joints <= 512 && frames > 0 && frames <= 20000, "the clip is empty or too large");
    if (v3) {
        std::uint8_t has_board{};
        get(&has_board, 1);
        board = has_board != 0;
    }
    clip.times.resize(frames);
    get(clip.times.data(), frames * 4ull);
    for (std::size_t i = 0; i < clip.times.size(); ++i)
        require(std::isfinite(clip.times[i]) && clip.times[i] >= 0 && (i == 0 || clip.times[i] >= clip.times[i - 1]), "the clip's times are out of order");
    if (v3) {
        std::vector<std::uint8_t> moves(clip.joints);
        get(moves.data(), moves.size());
        std::size_t moving = 0;
        for (const auto m : moves) moving += m ? 1 : 0;
        require(bytes.size() - at == (clip.joints - moving) * 40ull + frames * (moving + (board ? 1 : 0)) * 40ull, "the clip file has the wrong size");
        clip.frames.resize(static_cast<std::size_t>(frames) * clip.joints);
        if (board) clip.board.resize(frames);
        Joint still;
        for (std::uint32_t j = 0; j < clip.joints; ++j)
            if (!moves[j]) {
                get_joint(still, "the clip holds a broken pose");
                for (std::uint32_t f = 0; f < frames; ++f) clip.frames[f * clip.joints + j] = still;
            }
        for (std::uint32_t f = 0; f < frames; ++f) {
            for (std::uint32_t j = 0; j < clip.joints; ++j)
                if (moves[j]) get_joint(clip.frames[f * clip.joints + j], "the clip holds a broken pose");
            if (board) get_joint(clip.board[f], "the clip holds a broken board");
        }
    } else {
        require(bytes.size() - at == static_cast<std::size_t>(frames) * (clip.joints + (board ? 1 : 0)) * 40ull, "the clip file has the wrong size");
        clip.frames.resize(static_cast<std::size_t>(frames) * clip.joints);
        if (board) clip.board.resize(frames);
        for (auto& j : clip.frames) get_joint(j, "the clip holds a broken pose");
        for (auto& j : clip.board) get_joint(j, "the clip holds a broken board");
    }
    require(std::isfinite(clip.takeoff) && std::isfinite(clip.landing), "bad takeoff or landing");
    require((clip.takeoff == -1 || (clip.takeoff >= 0 && clip.takeoff <= clip.duration())) &&
            (clip.landing == -1 || (clip.landing >= 0 && clip.landing <= clip.duration())) &&
            (clip.takeoff < 0 || clip.landing < 0 || clip.landing >= clip.takeoff), "takeoff or landing outside the clip");
    return clip;
}
std::string file_safe(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_') out += c;
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Animation") : out;
}
} // namespace dingosdk::anim_studio

namespace dingosdk::anim_studio {
// The game's mirror, measured 2026-10-22: a heelflip recorded riding normal (mirrored) matches the library's
// heelflip reflected across the skater's Z = 0 plane exactly (median distance 0.0018), and nothing like it
// reflected across X = 0 (0.275), which is what this used to do. Z = 0 holds the board's long axis (the
// AITrajectory's X), so the feet keep their ends of the board, swap left for right, and he faces the other
// side of it - regular from goofy, as the game draws it. Applied to every joint's own rotation it comes to
// the same as reflecting the whole skeleton, as the left/right table keeps the hierarchy.
Quat mirror_rotation(const Quat& q) noexcept { return {-q[0], -q[1], q[2], q[3]}; }
Vec3 mirror_vector(const Vec3& v) noexcept { return {v[0], v[1], -v[2]}; }
Joint mirror_pose(const Joint& joint) noexcept {
    Joint out = joint;
    out.rotation = mirror_rotation(joint.rotation);
    out.position = mirror_vector(joint.position);
    return out;
}
// The board under the same mirror. The plane holds its long axis, so the board is not turned end for end:
// reflected as a shape it is the same board, only its offset across is reflected. As a rotation that is
// the reflected frame turned half a turn about its own up (the board is symmetrical across its middle).
Joint mirror_board_pose(const Joint& board) noexcept {
    Joint out = board;
    out.rotation = normalised(multiply(mirror_rotation(board.rotation), Quat{0, 1, 0, 0}));
    out.position = mirror_vector(board.position);
    return out;
}
// A board key, which works in the board's own frame (x across, y up, z along it): the game's mirror plane is
// the one through the board's middle along its length, so across is what reflects.
Quat mirror_board_rotation(const Quat& q) noexcept { return {q[0], -q[1], -q[2], q[3]}; }
Vec3 mirror_board_vector(const Vec3& v) noexcept { return {-v[0], v[1], v[2]}; }
Clip mirror_clip(const Clip& clip) {
    if (clip.joints != skeleton_joints) throw std::runtime_error("only a skater clip can be mirrored");
    Clip out = clip;
    for (std::size_t frame = 0; frame < clip.times.size(); ++frame)
        for (std::uint32_t joint = 0; joint < clip.joints; ++joint)
            out.frames[frame * clip.joints + mirror_joint[joint]] = mirror_pose(clip.frames[frame * clip.joints + joint]);
    for (std::size_t frame = 0; frame < clip.board.size(); ++frame) out.board[frame] = mirror_board_pose(clip.board[frame]);
    return out;
}
// Mirroring a pose, done where a mirror is actually a mirror. mirror_rotation and mirror_vector reflect
// across X = 0, which is only the right thing to do in the frame that plane is defined in: the skater's
// own, from the root down. Applied straight to a parent-local pose, as they were, every bone is reflected
// in its own tilted frame and the error compounds along the chain - which is what tore the skater up.
//
// So: take the pose out to that frame, reflect it there, swap each joint for its opposite number, and put
// it back as parent-local against the mirrored parents. Bones keep their lengths and the body stays whole
// whatever the rest pose does, because nothing assumes the rest pose is symmetrical.
namespace {
Joint local_against(const Joint& parent, const Joint& world_joint) noexcept {
    Joint out;
    out.scale = world_joint.scale;
    const float scale = parent.scale[0] > 1e-6f ? parent.scale[0] : 1.0f;
    out.rotation = normalised(multiply(conjugate(parent.rotation), world_joint.rotation));
    const Vec3 apart{world_joint.position[0] - parent.position[0], world_joint.position[1] - parent.position[1],
                     world_joint.position[2] - parent.position[2]};
    const auto turned = rotate(conjugate(parent.rotation), apart);
    for (std::size_t i = 0; i < 3; ++i) out.position[i] = turned[i] / scale;
    return out;
}
Joint placed_in(const Joint& frame, const Joint& local) noexcept {
    Joint out = local;
    const float scale = frame.scale[0] > 1e-6f ? frame.scale[0] : 1.0f;
    const auto moved = rotate(frame.rotation, {local.position[0] * scale, local.position[1] * scale, local.position[2] * scale});
    for (std::size_t i = 0; i < 3; ++i) out.position[i] = frame.position[i] + moved[i];
    out.rotation = normalised(multiply(frame.rotation, local.rotation));
    return out;
}
// One frame of a pose, mirrored. `body_only` leaves everything that is not the skinned body as it was.
// The whole skeleton is reflected in the root's frame. The body alone is reflected in the placement's:
// that is where he stands on his board (and where the board's own mirror is done), whereas the root can
// be anywhere in the world - reflecting across the root's middle line threw him off the board and into
// the air whenever the root was not right under him, which in the game it never is.
void mirror_frame(std::span<const Joint> pose, std::span<Joint> out, bool body_only) {
    const auto count = std::min(pose.size(), out.size());
    std::vector<Joint> in_world(count), mirrored(count);
    for (std::size_t j = 0; j < count; ++j) in_world[j] = world(pose, static_cast<std::uint16_t>(j));
    mirrored = in_world;
    const bool about_placement = body_only && placement_joint < count;
    const Joint about = about_placement ? in_world[placement_joint] : Joint{};
    for (std::size_t j = 0; j < count; ++j) {
        const auto other = mirror_joint[j];
        if (other >= count) continue;
        if (body_only && (!gameplay_body_joint(static_cast<std::uint16_t>(j)) || !gameplay_body_joint(other))) continue;
        Joint turned = about_placement ? local_against(about, in_world[j]) : in_world[j];
        turned.rotation = mirror_rotation(turned.rotation);
        turned.position = mirror_vector(turned.position);
        mirrored[other] = about_placement ? placed_in(about, turned) : turned;
    }
    for (std::size_t j = 0; j < count; ++j) {
        const auto up = parent(static_cast<std::uint16_t>(j));
        out[j] = up >= 0 && static_cast<std::size_t>(up) < count ? local_against(mirrored[static_cast<std::size_t>(up)], mirrored[j])
                                                                 : mirrored[j];
    }
}
} // namespace
void mirror_pose_frame(std::span<const Joint> pose, std::span<Joint> out, bool body_only) { mirror_frame(pose, out, body_only); }
Clip mirror_body_clip(const Clip& clip) {
    if (clip.joints != skeleton_joints) throw std::runtime_error("only a skater clip can be mirrored");
    Clip out = clip;
    for (std::size_t frame = 0; frame < clip.times.size(); ++frame)
        mirror_frame({clip.frames.data() + frame * clip.joints, clip.joints},
                     {out.frames.data() + frame * clip.joints, clip.joints}, true);
    // The board is one transform under the placement, so it reflects where it stands.
    for (std::size_t frame = 0; frame < clip.board.size(); ++frame) out.board[frame] = mirror_board_pose(clip.board[frame]);
    return out;
}
// ---- which of the game's own clips is playing -----------------------------------------------------
namespace {
constexpr std::uint16_t piece_body_joints = 375; // the body: everything below Ground
constexpr float piece_window = 0.25f;            // seconds of the live pose compared at once
constexpr float piece_enter = 0.03f, piece_leave = 0.045f, piece_margin = 0.004f, piece_moving = 0.008f;
float pose_distance(const Quat& a, const Quat& b) noexcept {
    return 1 - std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
}
} // namespace
void PieceTracker::set_pieces(const std::vector<const Clip*>& clips) {
    models_.clear();
    reset();
    for (std::size_t piece = 0; piece < clips.size(); ++piece) {
        const auto* clip = clips[piece];
        if (!clip || clip->empty() || clip->joints < piece_body_joints || clip->times.size() < 4) continue;
        // The joints that say most about this piece: the twenty that move most over it.
        const auto& mid = clip->frame(clip->times.size() / 2);
        std::vector<std::pair<float, std::uint16_t>> moved;
        for (std::uint16_t j = 2; j < piece_body_joints; ++j) {
            float sum = 0;
            for (std::size_t f = 0; f < clip->times.size(); ++f) sum += pose_distance(clip->frame(f)[j].rotation, mid[j].rotation);
            moved.push_back({sum / static_cast<float>(clip->times.size()), j});
        }
        std::sort(moved.begin(), moved.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::uint16_t> picked;
        for (const auto& [amount, j] : moved)
            if (picked.size() < 20 && amount > 0.0005f) picked.push_back(j);
        if (picked.size() < 6) continue;
        for (const bool mirrored : {false, true}) {
            Model model;
            model.piece = piece;
            model.mirrored = mirrored;
            model.times = clip->times;
            for (const auto j : picked) model.joints.push_back(mirrored ? mirror_joint[j] : j);
            if (std::any_of(model.joints.begin(), model.joints.end(), [](std::uint16_t j) { return j >= piece_body_joints; })) continue;
            model.frames.reserve(clip->times.size() * picked.size());
            for (std::size_t f = 0; f < clip->times.size(); ++f)
                for (const auto j : picked) {
                    const auto& q = clip->frame(f)[j].rotation;
                    model.frames.push_back(mirrored ? mirror_rotation(q) : q);
                }
            models_.push_back(std::move(model));
        }
    }
}
void PieceTracker::reset() noexcept {
    history_.clear();
    last_at_ = -1;
    locked_ = candidate_ = -1;
    streak_ = 0;
}
PieceTracker::Fit PieceTracker::fit(const Model& m, const std::array<const Sample*, 8>& window, const std::array<float, 8>& ages) const {
    Fit out;
    const auto frames = m.times.size(), picked = m.joints.size();
    // How far each sample of the window is from each frame of the piece.
    std::vector<float> d(window.size() * frames);
    for (std::size_t h = 0; h < window.size(); ++h)
        for (std::size_t f = 0; f < frames; ++f) {
            float sum = 0;
            for (std::size_t j = 0; j < picked; ++j) sum += pose_distance(window[h]->rotation[m.joints[j]], m.frames[f * picked + j]);
            d[h * frames + f] = sum / static_cast<float>(picked);
        }
    // Holding one frame still: what standing in a similar pose scores.
    for (std::size_t f = 0; f < frames; ++f) {
        float sum = 0;
        for (std::size_t h = 0; h < window.size(); ++h) sum += d[h * frames + f];
        out.still = std::min(out.still, sum / static_cast<float>(window.size()));
    }
    // Moving through it in step, at the game's speed or a little either side of it.
    const auto at_time = [&](float t) {
        const auto it = std::lower_bound(m.times.begin(), m.times.end(), t);
        if (it == m.times.end()) return frames - 1;
        const auto i = static_cast<std::size_t>(it - m.times.begin());
        return i > 0 && t - m.times[i - 1] < *it - t ? i - 1 : i;
    };
    for (const float speed : {0.85f, 1.0f, 1.15f})
        for (std::size_t k = 0; k < frames; ++k) {
            const float end = m.times[k];
            if (end - speed * ages.back() < m.times.front()) continue;
            float sum = 0;
            for (std::size_t h = 0; h < window.size(); ++h) sum += d[h * frames + at_time(end - speed * ages[h])];
            const float score = sum / static_cast<float>(window.size());
            if (score < out.score) {
                out.score = score;
                out.time = end;
            }
        }
    if (out.time >= 0) {
        // The stretch of the piece the window covered has to be one where it moves.
        const auto first = at_time(out.time - ages.back()), last = at_time(out.time);
        float sum = 0;
        for (std::size_t j = 0; j < picked; ++j) sum += pose_distance(m.frames[first * picked + j], m.frames[last * picked + j]);
        out.moving = sum / static_cast<float>(picked) > piece_moving;
    }
    return out;
}
std::optional<PieceMatch> PieceTracker::update(double at, std::span<const Joint> pose) {
    if (models_.empty() || pose.size() < piece_body_joints) return std::nullopt;
    if (last_at_ >= 0 && (at - last_at_ > 0.2 || at < last_at_)) reset();
    const double step = last_at_ >= 0 ? at - last_at_ : 0;
    last_at_ = at;
    Sample sample;
    sample.at = at;
    sample.rotation.resize(piece_body_joints);
    for (std::uint16_t j = 0; j < piece_body_joints; ++j) sample.rotation[j] = pose[j].rotation;
    history_.push_back(std::move(sample));
    while (history_.size() > 2 && at - history_.front().at > piece_window + 0.15) history_.pop_front();
    if (at - history_.front().at < piece_window) return std::nullopt;
    // Eight samples spread over the window, oldest first.
    std::array<const Sample*, 8> window{};
    std::array<float, 8> ages{};
    for (std::size_t i = 0; i < window.size(); ++i) {
        const double want = at - piece_window * (1.0 - static_cast<double>(i) / 7.0);
        const Sample* best = &history_.back();
        for (const auto& s : history_)
            if (std::abs(s.at - want) < std::abs(best->at - want)) best = &s;
        window[i] = best;
    }
    // `fit` takes them newest first, each with how long ago it was.
    std::array<const Sample*, 8> newest_first{};
    for (std::size_t i = 0; i < window.size(); ++i) {
        newest_first[i] = window[window.size() - 1 - i];
        ages[i] = static_cast<float>(at - newest_first[i]->at);
    }
    const auto good = [](const Fit& f) { return f.time >= 0 && f.score < piece_enter && f.score < f.still - piece_margin && f.moving; };
    if (locked_ >= 0) {
        const auto& m = models_[static_cast<std::size_t>(locked_)];
        const auto f = fit(m, newest_first, ages);
        if (f.time > last_fit_time_ + 0.005f) {
            advanced_at_ = at;
            last_fit_time_ = f.time;
        }
        // Let go when it stops fitting, or stops moving on through the piece.
        if (f.time < 0 || f.score > piece_leave || at - advanced_at_ > 0.15) {
            locked_ = -1;
            candidate_ = -1;
            streak_ = 0;
            return std::nullopt;
        }
        const float predicted = play_time_ + static_cast<float>(step);
        play_time_ = std::abs(f.time - predicted) < 0.12f ? predicted + 0.3f * (f.time - predicted) : f.time;
        play_time_ = std::clamp(play_time_, m.times.front(), m.times.back());
        return PieceMatch{m.piece, m.mirrored, play_time_, f.score};
    }
    int best = -1;
    Fit best_fit;
    for (std::size_t i = 0; i < models_.size(); ++i) {
        const auto f = fit(models_[i], newest_first, ages);
        if (good(f) && f.score < best_fit.score) {
            best = static_cast<int>(i);
            best_fit = f;
        }
    }
    // Taken up after three frames in a row on the same piece, moving forward through it.
    if (best >= 0 && best == candidate_ && best_fit.time >= candidate_last_ && best_fit.time - candidate_last_ < 0.12f) {
        ++streak_;
        candidate_last_ = best_fit.time;
    } else {
        candidate_ = best;
        streak_ = best >= 0 ? 1 : 0;
        candidate_from_ = candidate_last_ = best_fit.time;
    }
    if (candidate_ >= 0 && streak_ >= 3 && candidate_last_ - candidate_from_ >= 0.03f) {
        locked_ = candidate_;
        play_time_ = last_fit_time_ = candidate_last_;
        advanced_at_ = at;
        const auto& m = models_[static_cast<std::size_t>(locked_)];
        return PieceMatch{m.piece, m.mirrored, play_time_, best_fit.score};
    }
    return std::nullopt;
}
Animation mirror_animation(const Animation& animation) {
    Animation out = animation;
    if (!out.stance.empty()) out.stance = out.stance == "goofy" ? "regular" : "goofy";
    if (animation.clip.joints) out.clip = mirror_clip(animation.clip); // the profile alone has no clip
    for (auto& track : out.tracks) {
        if (track.joint < skeleton_joints) track.joint = mirror_joint[track.joint];
        const bool board = track.joint == board_joint;
        for (auto& key : track.keys) {
            key.rotation = board ? mirror_board_rotation(key.rotation) : mirror_rotation(key.rotation);
            key.offset = board ? mirror_board_vector(key.offset) : mirror_vector(key.offset);
        }
    }
    return out;
}
} // namespace dingosdk::anim_studio

namespace dingosdk::anim_studio {
namespace {
Vec3 sub3(const Vec3& a, const Vec3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add3(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale3(const Vec3& a, float k) noexcept { return {a[0] * k, a[1] * k, a[2] * k}; }
float length3(const Vec3& a) noexcept { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
Vec3 unit3(const Vec3& a) noexcept {
    const float n = length3(a);
    return n > 1e-9f ? scale3(a, 1.0f / n) : Vec3{0, 1, 0};
}
// The turn that takes direction `from` onto direction `to` (both unit).
Quat turn_between(const Vec3& from, const Vec3& to) noexcept {
    const float d = std::clamp(from[0] * to[0] + from[1] * to[1] + from[2] * to[2], -1.0f, 1.0f);
    Vec3 axis{from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2], from[0] * to[1] - from[1] * to[0]};
    if (length3(axis) < 1e-6f) {
        if (d > 0) return {0, 0, 0, 1};
        axis = std::abs(from[0]) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        axis = unit3(sub3(axis, scale3(from, from[0] * axis[0] + from[1] * axis[1] + from[2] * axis[2])));
        return {axis[0], axis[1], axis[2], 0};
    }
    return normalised(axis_angle(unit3(axis), std::acos(d)));
}
}

Chain chain_above(std::uint16_t end, std::size_t length) noexcept {
    Chain chain;
    std::array<std::uint16_t, 8> from_end{};
    std::size_t count = 0;
    for (int at = end; at >= 0 && count < std::min<std::size_t>(length, from_end.size()); at = parent(static_cast<std::uint16_t>(at)))
        from_end[count++] = static_cast<std::uint16_t>(at);
    for (std::size_t i = 0; i < count; ++i) chain.joints[i] = from_end[count - 1 - i];
    chain.count = count;
    return chain;
}

void solve_chain(std::span<const Joint> pose, const Chain& chain, const Vec3& target, std::span<Quat> rotations) noexcept {
    const std::size_t n = chain.count;
    if (n < 2 || rotations.size() < n) return;
    // Current world joints and turns, then the lengths between them.
    std::array<Vec3, 8> cur{}, pos{};
    std::array<Quat, 8> turn{};
    for (std::size_t i = 0; i < n; ++i) {
        const auto w = world(pose, chain.joints[i]);
        cur[i] = pos[i] = w.position;
        turn[i] = w.rotation;
    }
    std::array<float, 8> len{};
    for (std::size_t i = 0; i + 1 < n; ++i) len[i] = length3(sub3(cur[i + 1], cur[i]));
    // FABRIK: from the end to the target, then from the top back to its place, until the end is there.
    const Vec3 root = cur[0];
    for (int iteration = 0; iteration < 40; ++iteration) {
        pos[n - 1] = target;
        for (std::size_t i = n - 1; i-- > 0;) pos[i] = add3(pos[i + 1], scale3(unit3(sub3(pos[i], pos[i + 1])), len[i]));
        pos[0] = root;
        for (std::size_t i = 0; i + 1 < n; ++i) pos[i + 1] = add3(pos[i], scale3(unit3(sub3(pos[i + 1], pos[i])), len[i]));
        if (length3(sub3(pos[n - 1], target)) < 1e-4f) break;
    }
    // The bend: each joint turns so its bone points where the solved chain has it, from the top down.
    Quat parent_turn = world(pose, parent(chain.joints[0])).rotation;
    if (parent(chain.joints[0]) < 0) parent_turn = {0, 0, 0, 1};
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Vec3 was = unit3(sub3(cur[i + 1], cur[i])), now = unit3(sub3(pos[i + 1], pos[i]));
        const Quat world_turn = normalised(multiply(turn_between(was, now), turn[i]));
        rotations[i] = normalised(multiply(conjugate(parent_turn), world_turn));
        parent_turn = world_turn;
    }
}

void drag_joint(Animation& animation, std::span<const Joint> native, float seconds, std::uint16_t end, const Vec3& target, std::size_t length) {
    if (end >= native.size()) throw std::runtime_error("the joint is not on the skater");
    const Chain chain = chain_above(end, length);
    if (chain.count < 2) throw std::runtime_error("the joint has no limb above it to bend");
    std::vector<Joint> pose(native.begin(), native.end());
    apply_edits(animation, seconds, pose);
    std::array<Quat, 8> bent{};
    solve_chain(pose, chain, target, bent);
    // The top and the middle bend; the end is carried by them and keeps its own turn.
    for (std::size_t i = 0; i + 1 < chain.count; ++i) {
        const auto joint = chain.joints[i];
        const Quat delta = normalised(multiply(conjugate(native[joint].rotation), bent[i]));
        Vec3 offset{};
        if (const auto* track = animation.track(joint)) offset = evaluate(*track, seconds).offset;
        set_key(animation, joint, seconds, {delta, offset});
    }
}
} // namespace dingosdk::anim_studio
namespace dingosdk::anim_studio {
bool gameplay_body_joint(std::uint16_t joint) noexcept {
    if (joint >= skeleton_joints) return false;
    for (int at = joint; at >= 0; at = parent(static_cast<std::uint16_t>(at))) {
        if (std::find(effector_joints.begin(), effector_joints.end(), at) != effector_joints.end()) return false;
        if (at == hips_joint) return true;
    }
    return false;
}
// ---- the game's own clip, and how far into it it is ------------------------------------------------
namespace {
constexpr std::uint16_t native_body_joints = 375; // the body: everything below Ground
float degrees_between(const Quat& a, const Quat& b) noexcept {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.2957795f;
}
} // namespace
float NativeModels::edit_time(const Model& model, float seconds) const noexcept {
    if (model.own || !std::isfinite(seconds)) return seconds;
    if (!(model.up >= 0) || !(model.down > model.up) || !(edit_up >= 0) || !(edit_down > edit_up)) return seconds;
    if (seconds <= model.up) return std::max(0.0f, edit_up - (model.up - seconds));
    if (seconds >= model.down) return edit_down + (seconds - model.down);
    return edit_up + (seconds - model.up) * (edit_down - edit_up) / (model.down - model.up);
}
float NativeModels::model_time(const Model& model, float seconds) const noexcept {
    if (model.own || !std::isfinite(seconds)) return seconds;
    if (!(model.up >= 0) || !(model.down > model.up) || !(edit_up >= 0) || !(edit_down > edit_up)) return seconds;
    if (seconds <= edit_up) return std::max(0.0f, model.up - (edit_up - seconds));
    if (seconds >= edit_down) return model.down + (seconds - edit_down);
    return model.up + (seconds - edit_up) * (model.down - model.up) / (edit_down - edit_up);
}
std::shared_ptr<const NativeModels> build_native_models(const std::vector<std::shared_ptr<const Clip>>& clips, std::size_t own) {
    auto out = std::make_shared<NativeModels>();
    if (own < clips.size() && clips[own]) {
        out->edit_up = clips[own]->takeoff;
        out->edit_down = clips[own]->landing;
    }
    // The body joints a trick is edited by: the ones that say most about the pose, the same for every clip.
    std::vector<std::uint16_t> body;
    for (const auto& e : editable_joints())
        if (e.index < native_body_joints && e.index < skeleton_joints && gameplay_body_joint(e.index) &&
            mirror_joint[e.index] < native_body_joints)
            body.push_back(e.index);
    for (std::size_t c = 0; c < clips.size(); ++c) {
        const auto& clip = clips[c];
        if (!clip || clip->empty() || clip->joints < native_body_joints || clip->times.size() < 4 || body.size() < 6) continue;
        for (const bool mirrored : {false, true}) {
            NativeModels::Model model;
            model.clip = c;
            model.own = c == own;
            model.mirrored = mirrored;
            model.up = clip->takeoff;
            model.down = clip->landing;
            model.duration = clip->duration();
            model.times = clip->times;
            // Mirrored, the live joint on the other side is compared with this one reflected (as PieceTracker).
            for (const auto j : body) model.joints.push_back(mirrored ? mirror_joint[j] : j);
            model.frames.reserve(clip->times.size() * body.size());
            for (std::size_t f = 0; f < clip->times.size(); ++f)
                for (const auto j : body) {
                    const auto& q = clip->frame(f)[j].rotation;
                    model.frames.push_back(mirrored ? mirror_rotation(q) : q);
                }
            out->models.push_back(std::move(model));
        }
    }
    return out;
}
void NativeClock::start(std::shared_ptr<const NativeModels> models) noexcept {
    reset();
    models_ = std::move(models);
}
void NativeClock::reset() noexcept {
    models_.reset();
    locked_ = -1;
    time_ = error_ = baseline_ = -1;
    confidence_ = 0;
    bad_ = updates_ = 0;
    candidate_ = -1;
    streak_ = 0;
    lost_ = false;
}
float NativeClock::error_at(const NativeModels::Model& model, std::size_t frame, std::span<const Joint> pose) const noexcept {
    const auto n = model.joints.size();
    if (!n || frame >= model.times.size()) return 180;
    float sum = 0;
    for (std::size_t k = 0; k < n; ++k) sum += degrees_between(pose[model.joints[k]].rotation, model.frames[frame * n + k]);
    return sum / static_cast<float>(n);
}
NativeClock::Reading NativeClock::update(std::span<const Joint> pose, float step) noexcept {
    Reading out;
    if (!models_ || models_->models.empty() || pose.size() < native_body_joints) return out;
    if (!std::isfinite(step) || step < 0) step = 0;
    step = std::min(step, 0.1f);
    const auto& models = models_->models;
    // A frame's error and the time between it and its neighbours, refined to between frames.
    const auto refine = [&](const NativeModels::Model& m, std::size_t f, float e) {
        if (f == 0 || f + 1 >= m.times.size()) return m.times[f];
        const float before = error_at(m, f - 1, pose), after = error_at(m, f + 1, pose);
        const float curve = before - 2 * e + after;
        if (!(curve > 1e-4f)) return m.times[f];
        const float shift = std::clamp(0.5f * (before - after) / curve, -0.5f, 0.5f);
        return shift < 0 ? m.times[f] + shift * (m.times[f] - m.times[f - 1]) : m.times[f] + shift * (m.times[f + 1] - m.times[f]);
    };
    if (locked_ < 0) {
        // Looking for it: every clip, both ways round, from the crouch before its pop to just after its landing.
        // (Not its roll away: riding looks like the end of every trick.)
        // The edit's own clip wins a near tie: it can be the very clip the game plays (a library pop).
        int best_model = -1;
        std::size_t best_frame = 0;
        float best = 180, best_ranked = 180;
        for (std::size_t i = 0; i < models.size(); ++i) {
            const auto& m = models[i];
            const float from = m.up >= 0 ? m.up - 0.3f : 0.0f;
            const float to = m.down > m.up ? m.down + 0.05f : m.duration * 0.6f;
            for (std::size_t f = 0; f < m.times.size(); ++f) {
                if (m.times[f] < from || m.times[f] > to) continue;
                const float e = error_at(m, f, pose), ranked = m.own ? e - 0.5f : e;
                if (ranked < best_ranked) { best_ranked = ranked; best = e; best_model = static_cast<int>(i); best_frame = f; }
            }
        }
        out.error = best;
        if (best_model < 0 || best >= lock_degrees) return out;
        locked_ = best_model;
        time_ = refine(models[static_cast<std::size_t>(locked_)], best_frame, best);
        error_ = baseline_ = best;
        confidence_ = 0;
        updates_ = 0;
    } else if (!lost_) {
        // Following it: where the game should be now, and where its pose says it is, near that.
        const auto& m = models[static_cast<std::size_t>(locked_)];
        const float predicted = time_ + step;
        std::size_t best_frame = 0;
        float best = 180;
        bool any = false;
        for (std::size_t f = 0; f < m.times.size(); ++f) {
            if (m.times[f] < predicted - 0.1f || m.times[f] > predicted + 0.1f) continue;
            const float e = error_at(m, f, pose);
            if (e < best) { best = e; best_frame = f; any = true; }
        }
        if (any) {
            const float matched = refine(m, best_frame, best);
            // Pulled towards what the pose says, never back: a pose that is held looks like any time it is held at.
            time_ = std::max(time_, predicted + 0.35f * (matched - predicted));
            error_ = best;
        } else {
            time_ = predicted;
            error_ = 180;
        }
        // How near this clip comes at best, from its first updates: a recording of the trick (the game's pose
        // after its own corrections) never comes as near as the game's own clip does.
        if (++updates_ <= 8) baseline_ = std::min(baseline_, error_);
        // Another of the clips fitting the game better (the game's own clip rather than a recording of it, as a
        // recording drifts off it towards the landing): moved onto, at the same moment of the trick.
        out.switched = false;
        {
            const float edit_now = models_->edit_time(m, time_);
            int alternative = -1;
            float alternative_error = error_ - 1.0f, alternative_time = -1;
            for (std::size_t i = 0; i < models.size(); ++i) {
                const auto& b = models[i];
                if (static_cast<int>(i) == locked_ || b.mirrored != m.mirrored) continue;
                const float there = models_->model_time(b, edit_now);
                std::size_t frame = 0;
                float e_best = 180;
                bool found = false;
                for (std::size_t f = 0; f < b.times.size(); ++f) {
                    if (b.times[f] < there - 0.08f || b.times[f] > there + 0.08f) continue;
                    const float e = error_at(b, f, pose);
                    if (e < e_best) { e_best = e; frame = f; found = true; }
                }
                if (found && e_best < alternative_error) {
                    alternative_error = e_best;
                    alternative = static_cast<int>(i);
                    alternative_time = refine(b, frame, e_best);
                }
            }
            if (alternative >= 0) {
                streak_ = alternative == candidate_ ? streak_ + 1 : 1;
                candidate_ = alternative;
                if (streak_ >= 3) {
                    locked_ = alternative;
                    time_ = alternative_time;
                    error_ = alternative_error;
                    baseline_ = std::min(baseline_, alternative_error);
                    candidate_ = -1;
                    streak_ = 0;
                    bad_ = 0;
                    out.switched = true;
                }
            } else {
                candidate_ = -1;
                streak_ = 0;
            }
        }
        const auto& now_on = models[static_cast<std::size_t>(locked_)];
        bad_ = error_ > std::max(lose_degrees, baseline_ + 4.0f) ? bad_ + 1 : 0;
        if (bad_ >= 3 || time_ >= now_on.duration - 0.5f / 60) lost_ = true;
    } else {
        // Lost: on at the clip's own pace, and still measured against it (the caller tells the game's own
        // landing from its next animation by how far the game's pose goes from it).
        time_ += step;
        const auto& m = models[static_cast<std::size_t>(locked_)];
        if (!m.times.empty()) {
            const auto it = std::lower_bound(m.times.begin(), m.times.end(), time_);
            const auto f = it == m.times.end() ? m.times.size() - 1 : static_cast<std::size_t>(it - m.times.begin());
            error_ = error_at(m, f, pose);
        }
    }
    const auto& m = models[static_cast<std::size_t>(locked_)];
    // On the clip in full; less as the game blends away from it, smoothed so a single odd pose does not flicker.
    const float on = lost_ ? 0.0f : std::clamp((lose_degrees - error_) / (lose_degrees - good_degrees), 0.0f, 1.0f);
    confidence_ += (on - confidence_) * (confidence_ <= 0 ? 1.0f : 0.35f);
    out.locked = true;
    out.lost = lost_;
    // Past its clip's end only once lost (a clip the trick's own is read across onto can be shorter).
    out.time = std::clamp(models_->edit_time(m, lost_ ? time_ : std::min(time_, m.duration)), 0.0f, std::numeric_limits<float>::max());
    out.error = error_;
    out.baseline = baseline_;
    out.clip = static_cast<int>(m.clip);
    out.mirrored = m.mirrored;
    out.confidence = confidence_;
    return out;
}
} // namespace dingosdk::anim_studio
