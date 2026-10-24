#include "arm_reach.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::arm_reach {
namespace {
using anim_studio::conjugate;
using anim_studio::multiply;
using anim_studio::normalised;
using anim_studio::rotate;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scaled(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3& a, const Vec3& fallback) {
    const float l = length(a);
    return l > 1e-6f && std::isfinite(l) ? scaled(a, 1.0f / l) : fallback;
}
bool finite(const Vec3& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
bool finite(const Quat& q) { return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]); }
// The shortest turn taking direction `from` onto direction `to`.
Quat arc(const Vec3& from, const Vec3& to) {
    const Vec3 a = unit(from, {0, 0, 1}), b = unit(to, {0, 0, 1});
    const float d = dot(a, b);
    if (d < -0.9999f) { // half a turn: about any axis square to `from`
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-3f) axis = cross({0, 1, 0}, a);
        axis = unit(axis, {0, 1, 0});
        return {axis[0], axis[1], axis[2], 0.0f};
    }
    const Vec3 c = cross(a, b);
    return normalised({c[0], c[1], c[2], 1.0f + d});
}
// Part way from a to b, the short way round.
Quat blend(const Quat& a, Quat b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f)
        for (auto& v : b) v = -v;
    return normalised({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
}
// In a chain: chest, shoulder, upper arm, forearm, hand.
constexpr std::size_t chest = 6, shoulder = 7, upper = 8, fore = 9, hand_at = 10;
static_assert(right_chain.size() == hand_at + 1 && left_chain.size() == hand_at + 1);
} // namespace

bool compose(std::span<const Joint> pose, std::span<const std::uint16_t> joints, std::span<Placed> out) noexcept {
    if (out.size() < joints.size()) return false;
    Placed at;
    for (std::size_t i = 0; i < joints.size(); ++i) {
        if (joints[i] >= pose.size()) return false;
        const auto& bone = pose[joints[i]];
        if (!finite(bone.position) || !finite(bone.rotation) || !finite(bone.scale)) return false;
        Placed next;
        next.position = add(at.position, rotate(at.rotation, scaled(bone.position, at.scale)));
        next.rotation = normalised(multiply(at.rotation, bone.rotation));
        next.scale = at.scale * (bone.scale[0] + bone.scale[1] + bone.scale[2]) / 3.0f;
        if (!finite(next.position) || !(std::abs(next.position[0]) < 1e6f && std::abs(next.position[1]) < 1e6f && std::abs(next.position[2]) < 1e6f))
            return false;
        out[i] = next;
        at = next;
    }
    return true;
}

Reached reach(std::span<Joint> pose, const Goal& goal) noexcept {
    const auto joints = chain(goal.hand);
    std::array<Placed, right_chain.size()> w{};
    if (!compose(pose, joints, w) || !finite(goal.at) || (goal.rotation && !finite(*goal.rotation))) return {};
    const Vec3 s = w[upper].position, e = w[fore].position, h = w[hand_at].position;
    const float a = length(sub(e, s)), b = length(sub(h, e));
    if (!(a > 0.05f && b > 0.05f && a < 1.0f && b < 1.0f)) return {};
    const float reach_length = (a + b) * 0.995f;
    const float wanted = length(sub(goal.at, s));
    const float d = std::clamp(wanted, std::abs(a - b) + 0.02f, reach_length);
    const Vec3 direction = unit(sub(goal.at, s), {0, -1, 0});
    const Vec3 target = add(s, scaled(direction, d));
    // The elbow out from the body and a little down and back: the shoulder's side of the chest.
    Vec3 side = sub(s, w[chest].position);
    side[1] = 0.0f;
    side = unit(side, {goal.hand == Hand::left ? -1.0f : 1.0f, 0, 0});
    Vec3 ahead{direction[0], 0.0f, direction[2]};
    ahead = unit(ahead, cross(side, {0, 1, 0}));
    Vec3 pole = add(add(scaled(side, 0.8f), Vec3{0.0f, -0.45f, 0.0f}), scaled(ahead, -0.35f));
    pole = sub(pole, scaled(direction, dot(pole, direction)));
    pole = unit(pole, side);
    const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    const Vec3 elbow = add(s, add(scaled(direction, a * cos_a), scaled(pole, a * sin_a)));
    const Vec3 wrist = add(elbow, scaled(unit(sub(target, elbow), direction), b));
    // The upper arm turned onto the new elbow, then the forearm (as the upper arm left it) onto the wrist.
    const Quat turn_upper = arc(sub(e, s), sub(elbow, s));
    const Quat upper_world = normalised(multiply(turn_upper, w[upper].rotation));
    const Quat turn_fore = arc(rotate(turn_upper, sub(h, e)), sub(wrist, elbow));
    const Quat fore_world = normalised(multiply(turn_fore, multiply(turn_upper, w[fore].rotation)));
    // Back to each joint's own rotation, part way by the weight.
    const float weight = std::clamp(goal.weight, 0.0f, 1.0f);
    auto& upper_bone = pose[joints[upper]];
    auto& fore_bone = pose[joints[fore]];
    upper_bone.rotation = blend(upper_bone.rotation, normalised(multiply(conjugate(w[shoulder].rotation), upper_world)), weight);
    fore_bone.rotation = blend(fore_bone.rotation, normalised(multiply(conjugate(upper_world), fore_world)), weight);
    if (goal.rotation) {
        // The hand, from the forearm as it now is, turned to the rotation asked for.
        if (!compose(pose, joints, w)) return {};
        auto& hand_bone = pose[joints[hand_at]];
        hand_bone.rotation = blend(hand_bone.rotation, normalised(multiply(conjugate(w[fore].rotation), normalised(*goal.rotation))), weight);
    }
    if (!compose(pose, joints, w)) return {};
    Reached out;
    out.valid = finite(w[hand_at].position);
    out.hand = w[hand_at];
    out.reached = wanted <= reach_length + 0.01f;
    return out;
}

Reached hand_of(std::span<const Joint> pose, Hand hand) noexcept {
    std::array<Placed, right_chain.size()> w{};
    if (!compose(pose, chain(hand), w)) return {};
    return {finite(w[hand_at].position), w[hand_at], true};
}
}
