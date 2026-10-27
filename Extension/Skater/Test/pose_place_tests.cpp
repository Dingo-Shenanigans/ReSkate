// A joint set to a place instead of moved on top of the animation (Extension/Skater/pose_place.h): in
// its parent's frame, another joint's, the skater's or the world, part of it or all, part of the way;
// its children kept where they were; and the order a pose's joints go in, or why there is none.
#include "Extension/Skater/pose_place.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace dingosdk;
using namespace dingosdk::pose_place;

void check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
bool near(const Vec3& a, const Vec3& b) {
    return std::abs(a[0] - b[0]) < 1e-4f && std::abs(a[1] - b[1]) < 1e-4f && std::abs(a[2] - b[2]) < 1e-4f;
}
bool near(const Quat& a, const Quat& b) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return std::abs(std::abs(dot) - 1) < 1e-4f;
}
// A made-up pose on the real skeleton: every joint a little way from its parent, turned a little, the
// skater placed and turned in the world.
std::vector<Joint> made_up_pose() {
    std::vector<Joint> pose(anim_studio::skeleton_joints);
    for (std::size_t i = 0; i < pose.size(); ++i) {
        pose[i].position = {0.01f * static_cast<float>(i % 7), 0.1f, 0.02f};
        pose[i].rotation = anim_studio::axis_angle({0, 0, 1}, 0.05f * static_cast<float>(i % 5));
    }
    pose[1].position = {3, 0, -2};
    pose[1].rotation = anim_studio::axis_angle({0, 1, 0}, 1.2f);
    return pose;
}
Vec3 world_position(const std::vector<Joint>& pose, std::uint16_t joint) { return anim_studio::world(pose, joint).position; }
Joint after(const Joint& frame, const Vec3& position, const Quat& rotation) {
    // Where something at `position`, turned `rotation`, in `frame` is in the world.
    const auto moved = anim_studio::rotate(frame.rotation, {position[0] * frame.scale[0], position[1] * frame.scale[0], position[2] * frame.scale[0]});
    Joint out;
    out.position = {frame.position[0] + moved[0], frame.position[1] + moved[1], frame.position[2] + moved[2]};
    out.rotation = anim_studio::normalised(anim_studio::multiply(frame.rotation, rotation));
    return out;
}
} // namespace

int main() {
    const Quat turned = anim_studio::axis_angle({1, 0, 0}, 0.7f);
    // In its parent's frame: its own values, as asked.
    {
        auto pose = made_up_pose();
        apply(pose, 42, {Vec3{0, 0.2f, 0}, turned}, 1);
        check(near(pose[42].position, {0, 0.2f, 0}) && near(pose[42].rotation, turned), "placed in its parent's frame: its own values");
    }
    // Part of the way: halfway between where it was and where it is asked to be.
    {
        auto pose = made_up_pose();
        const auto before = pose[42].position;
        apply(pose, 42, {Vec3{0, 0.5f, 0}, std::nullopt}, 0.5f);
        check(near(pose[42].position, {before[0] * 0.5f, (before[1] + 0.5f) * 0.5f, before[2] * 0.5f}), "half the weight: halfway");
    }
    // In the world: where it is asked to be, whatever its parents do; the part left out as it was.
    {
        auto pose = made_up_pose();
        const auto turn_before = anim_studio::world(pose, 103).rotation;
        apply(pose, 103, {Vec3{1, 2, 3}, std::nullopt, Frame::world}, 1);
        check(near(world_position(pose, 103), {1, 2, 3}), "placed in the world: there");
        check(near(anim_studio::world(pose, 103).rotation, turn_before), "placed in the world: its turn as it was");
    }
    // In the skater's frame: moves and turns with the skater.
    {
        auto pose = made_up_pose();
        apply(pose, 103, {Vec3{0, 1.5f, 0.2f}, turned, Frame::skater}, 1);
        const auto wanted = after(anim_studio::world(pose, 1), {0, 1.5f, 0.2f}, turned);
        check(near(world_position(pose, 103), wanted.position) && near(anim_studio::world(pose, 103).rotation, wanted.rotation),
              "placed in the skater's frame");
    }
    // Relative to another joint it does not hang from (a link of a chain): follows that joint.
    {
        auto pose = made_up_pose();
        const Place link{Vec3{0, 0, -0.1f}, turned, Frame::joint, 278};
        apply(pose, 49, link, 1);
        auto wanted = after(anim_studio::world(pose, 278), {0, 0, -0.1f}, turned);
        check(near(world_position(pose, 49), wanted.position) && near(anim_studio::world(pose, 49).rotation, wanted.rotation),
              "placed relative to another joint");
        pose[278].rotation = anim_studio::axis_angle({0, 1, 0}, 0.9f); // the other joint moves; placed again
        apply(pose, 49, link, 1);
        wanted = after(anim_studio::world(pose, 278), {0, 0, -0.1f}, turned);
        check(near(world_position(pose, 49), wanted.position), "placed relative to another joint: follows it");
    }
    // Its children kept: they stay where they were in the world, and what hangs from them too; not
    // kept, they move with it.
    {
        auto pose = made_up_pose();
        const auto thigh = world_position(pose, 8), spine = world_position(pose, 42), shin = world_position(pose, 9);
        std::vector<Kept> kept;
        keep(pose, 7, kept);
        check(kept.size() == 3, "the hips have three children");
        apply(pose, 7, {Vec3{0, 0.3f, 0}, turned}, 1);
        put_back(pose, 7, kept);
        check(near(world_position(pose, 8), thigh) && near(world_position(pose, 42), spine) && near(world_position(pose, 9), shin),
              "children kept: where they were");
        auto carried = made_up_pose();
        apply(carried, 7, {Vec3{0, 0.3f, 0}, turned}, 1);
        check(!near(world_position(carried, 42), spine), "children not kept: carried along");
    }
    // The order: a joint after what it hangs from, and after the joint it is placed relative to.
    {
        std::vector<std::size_t> out;
        const std::uint16_t joints[] = {49, 278, 9, 7};
        const int relative_to[] = {278, -1, -1, -1};
        check(order(joints, relative_to, out).empty(), "an order is found");
        const auto at = [&](std::size_t entry) { return std::find(out.begin(), out.end(), entry) - out.begin(); };
        check(out.size() == 4 && at(1) < at(0), "after the joint it is placed relative to");
        check(at(3) < at(2), "after the joint it hangs from");
    }
    // No order: relative to itself, to a joint that hangs from it, or joints relative to each other.
    {
        std::vector<std::size_t> out;
        const std::uint16_t one[] = {42};
        const int itself[] = {42};
        check(!order(one, itself, out).empty(), "relative to itself: no order");
        const std::uint16_t hips[] = {7};
        const int below[] = {42};
        check(!order(hips, below, out).empty(), "relative to a joint that hangs from it: no order");
        const std::uint16_t two[] = {49, 278};
        const int circle[] = {278, 49};
        const auto why = order(two, circle, out);
        check(why.find("circle") != std::string::npos && out.empty(), "relative to each other: no order, and why");
    }
    std::cout << "pose_place tests passed\n";
}
