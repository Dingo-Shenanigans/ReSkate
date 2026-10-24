#include "carry.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::carry {
namespace {
using anim_studio::conjugate;
using anim_studio::multiply;
using anim_studio::normalised;
using anim_studio::rotate;
using arm_reach::chain;
using arm_reach::compose;

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
Quat about_up(float radians) { return {0.0f, std::sin(radians / 2), 0.0f, std::cos(radians / 2)}; }
// The world turn `world` as the local rotation of a joint whose parent is placed at `parent`.
Quat local_of(const Placed& parent, const Quat& world) { return normalised(multiply(conjugate(parent.rotation), world)); }
// The way an aimed thing faces round the upright (level, unit), steady however steeply it points up or
// down: straight down, its forward has no level part left, but its up does (the top of it is the way
// it faces).
Vec3 level_heading(const Quat& aim) {
    const Quat q = normalised(aim);
    const Vec3 forward = rotate(q, {0.0f, 0.0f, -1.0f}), up = rotate(q, {0.0f, 1.0f, 0.0f});
    const float sin_p = std::clamp(forward[1], -1.0f, 1.0f), cos_p = std::sqrt(std::max(0.0f, 1.0f - sin_p * sin_p));
    return unit(Vec3{forward[0] * cos_p - up[0] * sin_p, 0.0f, forward[2] * cos_p - up[2] * sin_p}, {0.0f, 0.0f, 1.0f});
}

// In an arm's chain: chest, shoulder, upper arm, forearm, hand.
constexpr std::size_t chest = 6, shoulder = 7, upper = 8, fore = 9, hand_at = 10;
// Held, not worn: from the wrist joint to the middle of the palm along the hand.
constexpr float palm_from_wrist = 0.075f;
// Where the other hand takes it, in the item's own frame (right, up, back; the grip at the origin), for
// a right-hand grip: the left side of it, underneath. Mirrored for a left grip.
constexpr Vec3 support_from_grip{-0.08f, -0.16f, -0.04f};
// The body as a column the arm keeps out of: about the hips' and chest's line, this far out at most
// (and inside the shoulders).
constexpr float body_radius = 0.24f;
// Its front is held at least this far out in front of the shoulder (along the aim), and at least this
// far below it.
constexpr float min_reach_out = 0.4f, below_shoulder = 0.05f;
// Toward the holding hand's own side it comes up this much (all the way square to that side).
constexpr float side_lift = 0.2f;

// One foot lifted `up` metres and moved by `move` (a step), the knee bending to it the way it already
// bends; the foot keeps its turn.
bool step_foot(std::span<Joint> pose, std::span<const std::uint16_t> path, float up, const Vec3& move) noexcept {
    std::array<Placed, 6> now{};
    if (!compose(pose, path, now)) return false;
    const Vec3 hip = now[3].position, knee = now[4].position;
    const Vec3 ankle = add(now[5].position, add(Vec3{0.0f, up, 0.0f}, move));
    const float a = length(sub(knee, hip)), b = length(sub(now[5].position, knee));
    if (!(a > 0.1f && b > 0.1f)) return true;
    const float d = std::clamp(length(sub(ankle, hip)), std::abs(a - b) + 0.01f, (a + b) * 0.999f);
    const Vec3 along = unit(sub(ankle, hip), {0, -1, 0});
    Vec3 pole = sub(knee, add(hip, scaled(along, dot(sub(knee, hip), along))));
    pole = unit(pole, Vec3{0.0f, 0.0f, 1.0f});
    const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    const Vec3 new_knee = add(hip, add(scaled(along, a * cos_a), scaled(pole, a * sin_a)));
    const Quat foot_world = now[5].rotation;
    const Quat thigh_world = normalised(multiply(arc(sub(knee, hip), sub(new_knee, hip)), now[3].rotation));
    pose[path[3]].rotation = local_of(now[2], thigh_world);
    if (!compose(pose, path, now)) return false;
    const Quat shin_world = normalised(multiply(arc(sub(now[5].position, now[4].position), sub(ankle, now[4].position)), now[4].rotation));
    pose[path[4]].rotation = local_of(now[3], shin_world);
    if (!compose(pose, path, now)) return false;
    pose[path[5]].rotation = local_of(now[4], foot_world);
    return true;
}

struct One {
    Hand hand = Hand::right;
    Vec3 front{};
    Quat aim{0, 0, 0, 1};
    Vec3 front_from_grip{};
    std::optional<Grip> worn;
    float weight = 1.0f;
    // The front reached for where it is (no holding it out in front and below the shoulder): the
    // supporting hand's reach.
    bool free{};
};

// One arm holding the item (its upper arm and forearm, and the hand when it is worn).
Held hold_one(std::span<Joint> pose, const One& goal) noexcept {
    const auto joints = chain(goal.hand);
    std::array<Placed, arm_reach::right_chain.size()> w{};
    if (!compose(pose, joints, w)) return {};
    const Quat aim = normalised(goal.aim);
    const Vec3 s = w[upper].position, e = w[fore].position, h = w[hand_at].position;
    const float a = length(sub(e, s)), b = length(sub(h, e));
    if (!(a > 0.05f && b > 0.05f && a < 1.0f && b < 1.0f)) return {};
    const float scale = std::max(w[hand_at].scale, 0.1f);
    // Where its front is shown held: where it was asked for, but out in front of the shoulder along
    // the aim and no higher than the shoulder, as a person holds a thing they point (a front up by the
    // head or tucked against the body would fold the arm up into the face).
    Vec3 front = goal.front;
    float to_own_side = 0.0f;
    if (!goal.free) {
        const Vec3 ahead = level_heading(aim);
        const float out = dot(sub(front, s), ahead);
        // Toward the holding hand's own side the arm would stretch across to its full length: it comes
        // in closer and up a little there, the elbow bent.
        Vec3 own_side = sub(s, w[chest].position);
        own_side[1] = 0.0f;
        own_side = unit(own_side, ahead);
        const float toward_side = std::clamp(dot(ahead, own_side), 0.0f, 1.0f);
        to_own_side = toward_side;
        // Where it is held follows the aim in one smooth sweep: level, out in front at the chest, under
        // the eyes; aimed down, lower and a little closer, all the way down to the stomach; aimed up,
        // higher and closer to the face, up over the head.
        const float pointing_y = std::clamp(rotate(aim, {0.0f, 0.0f, -1.0f})[1], -1.0f, 1.0f);
        const float up_part = std::max(pointing_y, 0.0f), down_part = std::max(-pointing_y, 0.0f);
        const float height = -below_shoulder - 0.13f * (1.0f - up_part) + 0.55f * up_part - 0.45f * down_part;
        const float reach_out = (min_reach_out + 0.08f * (1.0f - up_part)) * (1.0f - 0.35f * toward_side) * (1.0f - 0.55f * up_part) *
                                (1.0f - 0.15f * down_part);
        const Vec3 across = cross({0.0f, 1.0f, 0.0f}, ahead);
        const float lateral = std::clamp(dot(sub(front, s), across), -0.3f, 0.3f);
        const float out_now = std::max(out, 0.0f);
        // (Further out than that only toward the other side, across the body.)
        const float out_held = toward_side > 0.0f && out_now > reach_out + 0.1f ? out_now - (out_now - reach_out - 0.1f) * toward_side : reach_out;
        front = add(add(s, scaled(ahead, out_held)), scaled(across, lateral));
        front[1] = s[1] + height + side_lift * toward_side;
    }
    const Grip worn = goal.worn.value_or(Grip{});
    const Quat hand_world = normalised(multiply(aim, conjugate(worn.item)));
    const Vec3 grip_offset = scaled(rotate(hand_world, worn.from_wrist), scale);
    Vec3 palm_goal = goal.worn ? sub(sub(front, rotate(aim, goal.front_from_grip)), grip_offset) : sub(front, rotate(aim, goal.front_from_grip));
    if (!goal.free) {
        // The hand itself out in front of the shoulder and not up over it: with the hand tucked in over
        // the shoulder the arm can only fold up, the elbow rising by the head. Aimed up it comes up and
        // in a little, smoothly with the tilt.
        const Vec3 forward = level_heading(aim);
        const float up_part = std::clamp(rotate(aim, {0.0f, 0.0f, -1.0f})[1], 0.0f, 1.0f);
        const float out_min = (0.24f - 0.06f * up_part) * (1.0f - 0.35f * to_own_side);
        const float out_now = dot(sub(palm_goal, s), forward);
        if (out_now < out_min) palm_goal = add(palm_goal, scaled(forward, out_min - out_now));
        const float top = s[1] + 0.04f + 0.22f * up_part + side_lift * to_own_side;
        if (palm_goal[1] > top) palm_goal[1] = top;
    }
    {
        // The body as a column through the hips and chest: the hand's target, and the straight reach to
        // it from the shoulder, kept out of it (pushed out along the way they came closest).
        const Vec3 low = w[2].position, high = w[chest].position;
        const float bottom = low[1] - 0.35f, top = high[1] + 0.15f;
        const auto axis_at = [&](float y) {
            const float t = std::clamp((y - low[1]) / std::max(high[1] - low[1], 0.05f), 0.0f, 1.0f);
            return Vec3{low[0] + (high[0] - low[0]) * t, y, low[2] + (high[2] - low[2]) * t};
        };
        // As wide as the body is: inside the shoulders (the arm hangs from the column's edge).
        Vec3 shoulder_out = sub(s, axis_at(s[1]));
        shoulder_out[1] = 0.0f;
        const float radius = std::min(body_radius, length(shoulder_out) * 0.85f);
        for (int round = 0; round < 3; ++round) {
            float nearest = radius;
            Vec3 out_dir{};
            for (int k = 3; k <= 8; ++k) {
                const Vec3 p = add(s, scaled(sub(palm_goal, s), static_cast<float>(k) / 8.0f));
                if (p[1] < bottom || p[1] > top) continue;
                Vec3 radial = sub(p, axis_at(p[1]));
                radial[1] = 0.0f;
                const float r = length(radial);
                if (r < nearest) {
                    nearest = r;
                    Vec3 side_out = sub(s, w[chest].position);
                    side_out[1] = 0.0f;
                    out_dir = unit(radial, unit(side_out, {goal.hand == Hand::left ? -1.0f : 1.0f, 0, 0}));
                }
            }
            if (nearest >= radius) break;
            palm_goal = add(palm_goal, scaled(out_dir, radius - nearest + 0.01f));
        }
    }
    const float palm = goal.worn ? 0.0f : palm_from_wrist * scale, b_palm = b + palm;
    // Never held at full stretch: the elbow keeps a bend (it comes in a little rather than the arm
    // locking straight).
    const float reach = (a + b_palm) * (goal.free ? 0.995f : 0.93f);
    const float wanted = length(sub(palm_goal, s));
    const float d = std::clamp(wanted, std::abs(a - b_palm) + 0.02f, reach);
    const Vec3 direction = unit(sub(palm_goal, s), {0, -1, 0});
    const Vec3 target = add(s, scaled(direction, d));
    // The elbow out from the body and a little down and back: the shoulder's side of the chest.
    Vec3 side = sub(s, w[chest].position);
    side[1] = 0.0f;
    side = unit(side, {goal.hand == Hand::left ? -1.0f : 1.0f, 0, 0});
    // (Forward for the elbow: the way the arm reaches, level; reaching nearly straight up it has almost
    // none, and would flip about, so it leans on the way the item points then.)
    const Vec3 forward = level_heading(aim);
    Vec3 ahead{direction[0], 0.0f, direction[2]};
    ahead = add(ahead, scaled(forward, std::max(0.0f, 0.6f - length(ahead))));
    ahead = unit(ahead, forward);
    Vec3 pole = add(add(scaled(side, 0.8f), Vec3{0.0f, -0.45f, 0.0f}), scaled(ahead, -0.35f));
    {
        // Raised (up to the eye, up over the head): the elbow comes round to the front and a little out,
        // as an arm does lifting something up in front of the face, gradually with the aim; never back
        // behind the shoulder, never winged straight out.
        const float lifted = std::clamp((rotate(aim, {0.0f, 0.0f, -1.0f})[1] - 0.1f) / 0.8f, 0.0f, 1.0f);
        const float ease_in = lifted * lifted * (3.0f - 2.0f * lifted);
        if (ease_in > 0.0f) {
            const Vec3 lifted_pole = add(add(scaled(side, 0.85f), Vec3{0.0f, -0.4f, 0.0f}), scaled(forward, 0.35f));
            pole = add(scaled(pole, 1.0f - ease_in), scaled(lifted_pole, ease_in));
        }
    }
    pole = sub(pole, scaled(direction, dot(pole, direction)));
    {
        // The elbow hangs: whatever way the arm reaches, it bends with the elbow below the line from
        // shoulder to hand (and out to the side), never folding up over the shoulder. (Reaching up high,
        // straight down from that line would put it back behind the shoulder: there it only comes down
        // as far as it can without going back.)
        Vec3 hang = sub(Vec3{0.0f, -1.0f, 0.0f}, scaled(direction, -direction[1]));
        const float back = dot(hang, forward);
        if (back < 0.0f) hang = sub(hang, scaled(forward, back));
        hang = sub(hang, scaled(direction, dot(hang, direction)));
        const float hang_len = length(hang);
        if (hang_len > 0.05f) pole = add(unit(pole, side), scaled(hang, 0.6f / hang_len));
    }
    pole = unit(pole, side);
    const float cos_a = std::clamp((a * a + d * d - b_palm * b_palm) / (2.0f * a * d), -1.0f, 1.0f);
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
    if (goal.worn) {
        // The hand, from the forearm as it now is, turned so the worn item points along the aim.
        if (!compose(pose, joints, w)) return {};
        auto& hand_bone = pose[joints[hand_at]];
        hand_bone.rotation = blend(hand_bone.rotation, normalised(multiply(conjugate(w[fore].rotation), hand_world)), weight);
    }
    // Where the hand ended up, from the pose as it now is.
    if (!compose(pose, joints, w)) return {};
    Held held;
    if (goal.worn) {
        held.item.rotation = normalised(multiply(w[hand_at].rotation, worn.item));
        held.item.position = add(w[hand_at].position, scaled(rotate(w[hand_at].rotation, worn.from_wrist), scale));
    } else {
        // Held: the grip lies on the forearm's line, past the wrist.
        const Vec3 along = unit(sub(w[hand_at].position, w[fore].position), {0, -1, 0});
        held.item.position = add(w[hand_at].position, scaled(along, palm_from_wrist * scale));
        held.item.rotation = aim;
    }
    held.front = add(held.item.position, rotate(held.item.rotation, goal.front_from_grip));
    held.valid = finite(held.item.position) && finite(held.front);
    held.reached = wanted <= reach + 0.01f;
    return held;
}
} // namespace

bool crouch(std::span<Joint> pose, float depth, float twist) noexcept {
    if (!((depth > 0.001f || std::abs(twist) > 0.001f) && std::isfinite(depth) && std::isfinite(twist))) return true;
    depth = std::clamp(depth, 0.0f, 0.6f);
    struct Leg {
        std::span<const std::uint16_t> path;
        std::array<Placed, 6> before{};
    };
    std::array<Leg, 2> legs{{{right_leg}, {left_leg}}};
    for (auto& leg : legs)
        if (!compose(pose, leg.path, leg.before)) return false;
    // The hips down, in their parent's frame (the skater's placement).
    const auto& parent = legs[0].before[1];
    auto& hips = pose[hips_joint];
    const Vec3 drop = rotate(conjugate(parent.rotation), Vec3{0.0f, -depth, 0.0f});
    hips.position = add(hips.position, scaled(drop, 1.0f / std::max(parent.scale, 0.1f)));
    // Turned round the upright, the feet staying where they are.
    if (std::abs(twist) > 0.001f) {
        std::array<Placed, 6> now{};
        if (!compose(pose, legs[0].path, now)) return false;
        hips.rotation = local_of(parent, normalised(multiply(about_up(twist), now[2].rotation)));
    }
    for (auto& leg : legs) {
        std::array<Placed, 6> now{};
        if (!compose(pose, leg.path, now)) return false;
        const Vec3 hip = now[3].position, knee = leg.before[4].position, ankle = leg.before[5].position;
        const float a = length(sub(leg.before[4].position, leg.before[3].position)), b = length(sub(ankle, knee));
        if (!(a > 0.1f && b > 0.1f)) continue;
        const float d = std::clamp(length(sub(ankle, hip)), std::abs(a - b) + 0.01f, (a + b) * 0.999f);
        const Vec3 along = unit(sub(ankle, hip), {0, -1, 0});
        // The knee bends the way it already does (forward of the hip-to-ankle line).
        const Vec3 old_line = unit(sub(ankle, leg.before[3].position), along);
        const Vec3 old_mid = add(leg.before[3].position, scaled(old_line, dot(sub(knee, leg.before[3].position), old_line)));
        Vec3 pole = sub(knee, old_mid);
        pole = sub(pole, scaled(along, dot(pole, along)));
        pole = unit(pole, Vec3{0.0f, 0.0f, 1.0f});
        const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
        const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
        const Vec3 new_knee = add(hip, add(scaled(along, a * cos_a), scaled(pole, a * sin_a)));
        // Thigh, then shin, turned onto the new knee and the ankle; the foot keeps its turn in the world.
        const Quat thigh_world = normalised(multiply(arc(sub(now[4].position, hip), sub(new_knee, hip)), now[3].rotation));
        pose[leg.path[3]].rotation = local_of(now[2], thigh_world);
        if (!compose(pose, leg.path, now)) return false;
        const Quat shin_world = normalised(multiply(arc(sub(now[5].position, now[4].position), sub(ankle, now[4].position)), now[4].rotation));
        pose[leg.path[4]].rotation = local_of(now[3], shin_world);
        if (!compose(pose, leg.path, now)) return false;
        pose[leg.path[5]].rotation = local_of(now[4], leg.before[5].rotation);
    }
    return true;
}

bool hunch(std::span<Joint> pose, const Vec3& toward, float angle, float look, float twist) noexcept {
    if (!(std::abs(angle) + std::abs(look) + std::abs(twist) > 1e-4f) || !finite(toward)) return true;
    Vec3 level{toward[0], 0.0f, toward[2]};
    if (length(level) < 1e-4f) return true;
    level = unit(level, {0, 0, 1});
    // Turning about this bends the top of the body over toward `level`.
    const Vec3 axis = unit(cross(Vec3{0.0f, 1.0f, 0.0f}, level), {1, 0, 0});
    // Spine, Spine1, Spine2, Spine3 share the back's angle; Neck, Neck1 and Head the look.
    constexpr std::array<float, 7> share{0.15f, 0.25f, 0.3f, 0.3f, 0.35f, 0.25f, 0.4f};
    constexpr std::array<float, 7> twist_share{0.1f, 0.1f, 0.15f, 0.2f, 0.15f, 0.1f, 0.2f};
    for (std::size_t i = 0; i < back_joints.size(); ++i) {
        std::array<Placed, head_chain.size()> w{};
        if (!compose(pose, head_chain, w)) return false;
        const std::size_t at = 3 + i; // its place in head_chain
        const float turn = (i < 4 ? angle : look) * share[i];
        const Quat bend{axis[0] * std::sin(turn / 2), axis[1] * std::sin(turn / 2), axis[2] * std::sin(turn / 2), std::cos(turn / 2)};
        const Quat world = normalised(multiply(bend, multiply(about_up(twist * twist_share[i]), w[at].rotation)));
        pose[back_joints[i]].rotation = local_of(w[at - 1], world);
    }
    return true;
}

Held carry(std::span<Joint> pose, const Goal& goal) noexcept {
    if (pose.size() <= 343 || !finite(goal.front) || !finite(goal.aim) || !finite(goal.front_from_grip)) return {};
    const float body_weight = std::clamp(goal.weight, 0.0f, 1.0f);
    if (std::abs(goal.turn) > 1e-4f && std::isfinite(goal.turn)) {
        // The whole body turned round the upright through the hips: the hips (everything hangs from
        // them) turned in place.
        std::array<Placed, 3> w3{};
        if (!compose(pose, std::span<const std::uint16_t>(right_leg).first(3), w3)) return {};
        pose[hips_joint].rotation = local_of(w3[1], normalised(multiply(about_up(goal.turn * body_weight), w3[2].rotation)));
    }
    // Turning on the spot: the feet step round with it, one then the other, each lifted a little and
    // set down a little in under the hips.
    if (goal.step > 0.01f && std::isfinite(goal.step_phase)) {
        const float phase = goal.step_phase - std::floor(goal.step_phase);
        const bool right_foot = phase < 0.5f;
        const float half = right_foot ? phase * 2.0f : (phase - 0.5f) * 2.0f;
        const float lift = std::sin(half * 3.14159265f) * 0.09f * std::clamp(goal.step, 0.0f, 1.0f) * body_weight;
        std::array<Placed, 6> leg{};
        const auto path = right_foot ? std::span<const std::uint16_t>(right_leg) : std::span<const std::uint16_t>(left_leg);
        if (lift > 0.002f && compose(pose, path, leg)) {
            Vec3 in = sub(leg[2].position, leg[5].position);
            in[1] = 0.0f;
            const float inward = std::min(length(in), 0.06f) * std::sin(half * 3.14159265f);
            if (!step_foot(pose, path, lift, scaled(unit(in, {0, 0, 0}), inward * 0.5f))) return {};
        }
    }
    // Which way it points against the way the body faces (round the upright), for the body turning to
    // it: the hips a quarter, the back and shoulders and head the rest.
    float full = 0.0f, pitch = 0.0f;
    Vec3 facing{0, 0, 1};
    if (goal.hunch > 0.0f || goal.crouch > 0.0f) {
        facing = body_facing(pose);
        const Vec3 level = level_heading(goal.aim);
        full = std::clamp(std::atan2(dot(cross(facing, level), {0.0f, 1.0f, 0.0f}), dot(facing, level)), -1.6f, 1.6f);
        pitch = std::asin(std::clamp(rotate(normalised(goal.aim), {0.0f, 0.0f, -1.0f})[1], -1.0f, 1.0f)); // down is negative
    }
    if (goal.still)
        for (std::size_t i = 0; i < back_joints.size(); ++i)
            pose[back_joints[i]].rotation = blend(pose[back_joints[i]].rotation, standing_back[i], body_weight);
    constexpr float hips_share = 0.25f;
    const bool turning = goal.hunch > 0.0f;
    if ((goal.crouch > 0.0f || turning) && !crouch(pose, goal.crouch * body_weight, turning ? full * hips_share * body_weight : 0.0f)) return {};
    if (turning) {
        // Over it, the way the body faces: toward the aim, but no more than 60 degrees off the body's own
        // facing (never folded backwards).
        const Vec3 toward = rotate(about_up(std::clamp(full, -1.05f, 1.05f)), facing);
        // Pointed well up, it is held up to the eye instead: no curl over it, the head tilted back to
        // it (eased in with the tilt).
        const float up_t = std::clamp((pitch - 0.3f) / 0.6f, 0.0f, 1.0f);
        // The back over it as it goes down with the aim, a little at level, more aimed down; the head
        // down to it the same way, a touch at level, more as it goes down; aimed up, the head back.
        const float amount = goal.hunch * std::clamp(0.45f - pitch * (pitch < 0.0f ? 0.55f : 0.7f), 0.2f, 1.2f) * body_weight * (1.0f - up_t);
        const float look = std::clamp(0.2f - pitch * (pitch < 0.0f ? 0.18f : 0.45f), 0.1f, 0.65f) * body_weight * (1.0f - up_t) -
                           up_t * std::min(pitch, 1.2f) * 0.45f * body_weight;
        // Up to the eye: the shoulders turn a little, the holding arm's shoulder coming forward, so the
        // arm reaches out in front of the face.
        float shoulders = 0.0f;
        if (up_t > 0.0f) {
            std::array<Placed, arm_reach::right_chain.size()> arm{};
            if (compose(pose, chain(goal.hand), arm)) {
                const Vec3 r = sub(arm[upper].position, arm[chest].position);
                const float way = dot(cross({0.0f, 1.0f, 0.0f}, r), facing) >= 0.0f ? 1.0f : -1.0f;
                shoulders = way * 0.35f * up_t * body_weight;
            }
        }
        // The shoulders and head turn the rest of the way to it off to the side.
        if (!hunch(pose, toward, amount, look, full * (1.0f - hips_share) * body_weight + shoulders)) return {};
    }
    One first;
    first.hand = goal.hand;
    first.front = goal.front;
    first.aim = goal.aim;
    first.front_from_grip = goal.front_from_grip;
    first.worn = goal.worn;
    first.weight = goal.weight;
    const auto held = hold_one(pose, first);
    if (!held.valid || !goal.both) return held;
    // The other hand to its side, underneath, where the first one put it.
    Vec3 support = support_from_grip;
    if (goal.hand == Hand::left) support[0] = -support[0];
    const Vec3 point = add(held.item.position, rotate(held.item.rotation, support));
    One other;
    other.hand = goal.hand == Hand::left ? Hand::right : Hand::left;
    other.aim = normalised(goal.aim);
    other.weight = goal.weight;
    other.free = true;
    other.front = point; // (front_from_grip 0: its grip goes to `point`)
    (void)hold_one(pose, other);
    return held;
}

float pushing(std::span<const Joint> pose) noexcept {
    std::array<Placed, 6> r{}, l{};
    if (!compose(pose, right_leg, r) || !compose(pose, left_leg, l)) return 0.0f;
    const float apart = std::abs(r[5].position[1] - l[5].position[1]);
    return std::clamp((apart - 0.04f) / 0.06f, 0.0f, 1.0f);
}

Vec3 body_facing(std::span<const Joint> pose) noexcept {
    std::array<Placed, 6> right_thigh{}, left_thigh{};
    if (!compose(pose, right_leg, right_thigh) || !compose(pose, left_leg, left_thigh)) return {0, 0, 1};
    Vec3 across = sub(left_thigh[3].position, right_thigh[3].position);
    across[1] = 0.0f;
    return unit(cross(across, {0.0f, 1.0f, 0.0f}), {0, 0, 1});
}
}
