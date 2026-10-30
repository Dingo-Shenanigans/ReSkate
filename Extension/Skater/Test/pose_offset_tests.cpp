// A joint moved, turned and scaled on top of the pose as it is drawn (Extension/Skater/pose_offset.h):
// the same as skater.pose's offset on the animation's pose, kept on what the body physics did to the
// joint after the animation, part of the way, several on one joint, and never on top of itself.
#include "Extension/Skater/pose_offset.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace dingosdk;
using namespace dingosdk::pose_offset;

void check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
bool near(const Vec3& a, const Vec3& b) {
    return std::abs(a[0] - b[0]) < 1e-5f && std::abs(a[1] - b[1]) < 1e-5f && std::abs(a[2] - b[2]) < 1e-5f;
}
bool near(const Quat& a, const Quat& b) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return std::abs(std::abs(dot) - 1) < 1e-5f;
}
}

int main() {
    // The hips as the animation left them, and as the body physics moved them before the pose was drawn.
    Joint animated;
    animated.position = {0.01f, 0.95f, -0.02f};
    animated.rotation = anim_studio::axis_angle({0, 1, 0}, 0.3f);
    Joint physics = animated;
    physics.position[1] += 0.04f;
    physics.rotation = anim_studio::axis_angle({1, 0, 0}, 0.1f);

    // An offset alone: added to the joint as it is drawn, so what the body physics did stays.
    {
        std::vector<Delta> deltas;
        add(deltas, 7, Quat{0, 0, 0, 1}, 1, {0, 0.021f, 0}, 1);
        check(deltas.size() == 1 && deltas[0].joint == 7, "one joint");
        const auto drawn = apply(physics, deltas[0]);
        check(near(drawn.position, {0.01f, 0.95f + 0.04f + 0.021f, -0.02f}), "the body physics' move kept, the offset on it");
        check(near(drawn.rotation, physics.rotation), "the body physics' turn kept");
    }
    // A zero offset (a pose of the hips by 0) changes nothing at all: before, it put back the
    // animation's hips and the feet went into the deck.
    {
        std::vector<Delta> deltas;
        add(deltas, 7, Quat{0, 0, 0, 1}, 1, {0, 0, 0}, 1);
        const auto drawn = apply(physics, deltas[0]);
        check(near(drawn.position, physics.position) && near(drawn.rotation, physics.rotation) && near(drawn.scale, physics.scale),
              "a zero offset leaves the joint as drawn");
    }
    // Turn, scale and move: the same as pose_joints does to the animation's pose (turned in its own
    // axes, every axis scaled, moved in its parent's frame), part of the way.
    {
        const Quat turn = anim_studio::axis_angle({0, 0, 1}, 0.8f);
        std::vector<Delta> deltas;
        add(deltas, 42, turn, 1.5f, {0.1f, 0, 0}, 0.5f);
        const auto drawn = apply(animated, deltas[0]);
        const Quat half = anim_studio::slerp(Quat{0, 0, 0, 1}, turn, 0.5f);
        check(near(drawn.rotation, anim_studio::normalised(anim_studio::multiply(animated.rotation, half))), "turned half way in its own axes");
        check(near(drawn.scale, {1.25f, 1.25f, 1.25f}), "scaled half way");
        check(near(drawn.position, {0.06f, 0.95f, -0.02f}), "moved half way");
    }
    // Several on one joint go on in order, as they would one after another.
    {
        const Quat a = anim_studio::axis_angle({1, 0, 0}, 0.4f), b = anim_studio::axis_angle({0, 1, 0}, 0.6f);
        std::vector<Delta> deltas;
        add(deltas, 7, a, 2, {0, 0.01f, 0}, 1);
        add(deltas, 7, b, 0.5f, {0, 0.02f, 0}, 1);
        check(deltas.size() == 1, "one delta a joint");
        const auto drawn = apply(animated, deltas[0]);
        check(near(drawn.rotation, anim_studio::normalised(anim_studio::multiply(anim_studio::multiply(animated.rotation, a), b))), "turns in order");
        check(near(drawn.scale, {1, 1, 1}) && near(drawn.position, {0.01f, 0.98f, -0.02f}), "scales multiply, moves add");
    }
    // Scale never reaches 0 (a joint at scale 0 has no inverse).
    {
        std::vector<Delta> deltas;
        add(deltas, 7, Quat{0, 0, 0, 1}, 0, {0, 0, 0}, 1);
        check(apply(animated, deltas[0]).scale[0] >= smallest_scale, "never quite 0");
    }
    // A pose that comes to be drawn again still holding what was written gets the delta on the joint
    // it was written from, not on top of itself; one the game set anew gets it on the new value.
    {
        std::vector<Delta> deltas;
        add(deltas, 7, Quat{0, 0, 0, 1}, 1, {0, 0.021f, 0}, 1);
        const auto first = apply(before_delta(physics, std::nullopt), deltas[0]);
        const Written written{physics, first};
        const auto again = apply(before_delta(first, written), deltas[0]);
        check(near(again.position, first.position), "drawn again: not moved twice");
        Joint anew = physics;
        anew.position[1] += 0.005f;
        check(near(apply(before_delta(anew, written), deltas[0]).position, {0.01f, 0.95f + 0.045f + 0.021f, -0.02f}), "set anew: moved from there");
    }
    std::cout << "pose offset tests passed\n";
    return 0;
}
