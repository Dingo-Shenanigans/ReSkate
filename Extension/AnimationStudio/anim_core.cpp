#include "skater_effectors.h"
#include "skater_mirror.h"
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
        // Time-aware cubic position and rotation curves preserve velocity at uneven key spacing.
        const auto& before = i > 0 ? keys[i - 1].offset : a.offset;
        const auto& after = i + 2 < keys.size() ? keys[i + 2].offset : b.offset;
        const Quat prev = i > 0 ? keys[i - 1].rotation : a.rotation, next = i + 2 < keys.size() ? keys[i + 2].rotation : b.rotation;
        const Quat b_rotation = nearest(b.rotation, a.rotation);
        const float before_time = i > 0 ? keys[i - 1].time : a.time - span;
        const float after_time = i + 2 < keys.size() ? keys[i + 2].time : b.time + span;
        const float left_scale = span / std::max(b.time - before_time, 1e-6f);
        const float right_scale = span / std::max(after_time - a.time, 1e-6f);
        const Quat ca = rotation_control(prev, a.rotation, b_rotation, left_scale / 3);
        const Quat cb = rotation_control(a.rotation, b_rotation, next, -right_scale / 3);
        const Quat qa = slerp(a.rotation, ca, u), qm = slerp(ca, cb, u), qb = slerp(cb, b_rotation, u);
        const Quat moved = slerp(slerp(qa, qm, u), slerp(qm, qb, u), u);
        return {normalised(moved), hermite(before, a.offset, b.offset, after, u, left_scale, right_scale)};
    }
    }
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
void apply_authored_edit_layer(const Animation& animation, float seconds, std::span<Joint> native, float weight) {
    if (!std::isfinite(weight) || weight <= 0 || !std::isfinite(seconds) ||
        seconds < animation.start || (animation.end >= 0 && seconds > animation.end)) return;
    weight = std::min(weight, 1.0f);
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || track.joint >= skeleton_joints ||
            track.joint >= native.size() || !editable(track.joint)) continue;
        const auto edit = evaluate(track, seconds);
        auto& current = native[track.joint];
        if (edit.rotation[0] != 0 || edit.rotation[1] != 0 || edit.rotation[2] != 0) {
            const auto wanted = normalised(multiply(current.rotation, edit.rotation));
            current.rotation = weight >= 1 ? wanted : slerp(current.rotation, wanted, weight);
        }
        for (std::size_t k = 0; k < 3; ++k) current.position[k] += edit.offset[k] * weight;
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
    if (mirror) { edit.rotation = mirror_rotation(edit.rotation); edit.offset = mirror_vector(edit.offset); }
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
            const Key &before = was[i - 1], &after = was[i + 1];
            const float span = after.time - before.time;
            const float u = span > 1e-6f ? (was[i].time - before.time) / span : 0.5f;
            constexpr float pull = 0.35f;
            keys[i].offset = lerp(was[i].offset, lerp(before.offset, after.offset, u), pull);
            keys[i].rotation = slerp(was[i].rotation, slerp(before.rotation, after.rotation, u), pull);
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
    root["speed"] = animation.speed;
    root["loop"] = animation.loop;
    root["pop"] = animation.pop;
    if (!animation.stance.empty()) root["stance"] = animation.stance;
    if (animation.switch_stance) root["switch_stance"] = *animation.switch_stance;
    root["source"] = Json{{"kind", animation.source.kind}, {"asset", animation.source.asset},
                          {"rig", animation.source.rig}, {"board_frame", animation.source.board_frame},
                          {"position_codec", animation.source.position_codec}};
    root["entry"] = animation.entry;
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
    result.speed = root.contains("speed") ? std::clamp(number(root.at("speed"), "bad speed"), 0.1f, 4.0f) : 1.0f;
    result.loop = root.contains("loop") && root.at("loop").is_boolean() ? root.at("loop").get<bool>() : true;
    result.pop = root.value("pop", "");
    result.stance = root.value("stance", "");
    result.entry = root.value("entry", "ground");
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
Quat mirror_rotation(const Quat& q) noexcept { return {q[0], -q[1], -q[2], q[3]}; }
Vec3 mirror_vector(const Vec3& v) noexcept { return {-v[0], v[1], v[2]}; }
Joint mirror_pose(const Joint& joint) noexcept {
    Joint out = joint;
    out.rotation = mirror_rotation(joint.rotation);
    out.position = mirror_vector(joint.position);
    return out;
}
Clip mirror_clip(const Clip& clip) {
    if (clip.joints != skeleton_joints) throw std::runtime_error("only a skater clip can be mirrored");
    Clip out = clip;
    for (std::size_t frame = 0; frame < clip.times.size(); ++frame)
        for (std::uint32_t joint = 0; joint < clip.joints; ++joint)
            out.frames[frame * clip.joints + mirror_joint[joint]] = mirror_pose(clip.frames[frame * clip.joints + joint]);
    for (std::size_t frame = 0; frame < clip.board.size(); ++frame) out.board[frame] = mirror_pose(clip.board[frame]);
    return out;
}
Animation mirror_animation(const Animation& animation) {
    Animation out = animation;
    if (!out.stance.empty()) out.stance = out.stance == "goofy" ? "regular" : "goofy";
    if (animation.clip.joints) out.clip = mirror_clip(animation.clip); // the profile alone has no clip
    for (auto& track : out.tracks) {
        if (track.joint < skeleton_joints) track.joint = mirror_joint[track.joint];
        for (auto& key : track.keys) {
            key.rotation = mirror_rotation(key.rotation);
            key.offset = mirror_vector(key.offset);
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
} // namespace dingosdk::anim_studio
