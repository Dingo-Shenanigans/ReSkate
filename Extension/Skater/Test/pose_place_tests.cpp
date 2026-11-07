// A joint set to a place instead of moved on top of the animation (Extension/Skater/pose_place.h): in
// its parent's frame, another joint's, the skater's or the world, part of it or all, part of the way;
// its children kept where they were; and the order a pose's joints go in, or why there is none.
#include "Extension/Skater/pose_place.h"
#include "Extension/Skater/pose_replay.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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
using Bone = pose_replay::NativeBone;
std::vector<Bone> native_pose(const std::vector<Joint>& pose) {
    std::vector<Bone> out(pose.size());
    for (std::size_t i = 0; i < pose.size(); ++i) {
        const auto& j = pose[i];
        out[i] = {{j.scale[0], j.scale[1], j.scale[2], 1000.0f + static_cast<float>(i)}, j.rotation,
                  {j.position[0], j.position[1], j.position[2], -1000.0f - static_cast<float>(i)}};
    }
    return out;
}
std::vector<Joint> local_pose(const std::vector<Bone>& bones) {
    std::vector<Joint> out(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const auto& b = bones[i];
        out[i] = {{b.scale[0], b.scale[1], b.scale[2]}, b.rotation, {b.position[0], b.position[1], b.position[2]}};
    }
    return out;
}
bool same_bytes(const std::vector<Bone>& a, const std::vector<Bone>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Bone)) == 0;
}
bool same_joint(const Joint& a, const Joint& b) {
    return a.scale == b.scale && a.rotation == b.rotation && a.position == b.position;
}
std::vector<pose_replay::Entry> ordered(std::vector<pose_replay::Entry> entries) {
    std::vector<std::uint16_t> joints;
    std::vector<int> frames;
    for (const auto& e : entries) {
        joints.push_back(e.joint);
        frames.push_back(e.place && e.place->frame == Frame::joint ? e.place->joint : -1);
    }
    std::vector<std::size_t> indices;
    check(order(joints, frames, indices).empty(), "the replay fixture has a valid dependency order");
    const auto input = entries;
    for (std::size_t i = 0; i < indices.size(); ++i) entries[i] = input[indices[i]];
    return entries;
}
void restore(std::vector<Bone>& current, const std::vector<Bone>& before, const pose_replay::Range& range) {
    // The same byte range the render bridge restores after the renderer has copied it.
    std::copy(before.begin(), before.end(), current.begin() + range.first);
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
    // The native placement moves after animation. Cached local bones move a world goal with it;
    // replaying the request on the current skeleton leaves that goal where it was asked to be.
    {
        auto animated = made_up_pose();
        auto physical = animated;
        physical[1].position = {-4, 2, 6};
        physical[1].rotation = anim_studio::axis_angle({0, 1, 0}, -0.4f);
        physical[42].position[1] += 0.07f;
        physical[42].rotation = anim_studio::axis_angle({1, 0, 0}, 0.23f);
        auto current = native_pose(physical);
        // Opaque fourth lanes are allowed to contain any bits, including non-finite values.
        current[7].scale[3] = std::numeric_limits<float>::quiet_NaN();
        const auto original = current;
        pose_replay::Entry hips; hips.joint = 7; hips.place = Place{Vec3{1, 2, 3}, turned, Frame::world};
        pose_replay::Entry head; head.joint = 103; head.place = Place{std::nullopt, turned, Frame::world};
        const auto entries = ordered({head, hips});
        auto cached = animated;
        const auto cached_range = pose_replay::apply(cached, entries, 1);
        auto old_draw = physical;
        std::copy(cached.begin() + cached_range.first, cached.begin() + cached_range.last, old_draw.begin() + cached_range.first);
        check(!near(world_position(old_draw, 7), {1, 2, 3}), "the fixture reproduces the animation/render root mismatch");
        pose_replay::Written written;
        std::vector<Bone> before;
        pose_replay::Range range;
        check(pose_replay::draw(current, entries, 1, written, before, range), "world goals replay on the current native pose");
        const auto drawn = local_pose(current);
        check(near(world_position(drawn, 7), {1, 2, 3}) && near(anim_studio::world(drawn, 7).rotation, turned),
              "world position and rotation survive the newer placement");
        check(std::memcmp(&current[42], &original[42], sizeof(Bone)) == 0,
              "an unrequested intermediate joint keeps its newer physics values exactly");
        check(std::memcmp(current.data(), original.data(), 7 * sizeof(Bone)) == 0, "root and placement bones are never written");
        for (std::size_t i = 0; i < current.size(); ++i) {
            check(std::memcmp(&current[i].scale[3], &original[i].scale[3], 4) == 0 &&
                  std::memcmp(&current[i].position[3], &original[i].position[3], 4) == 0, "fourth lanes stay exact");
        }
        restore(current, before, range);
        check(same_bytes(current, original), "after drawing every native byte is restored");
    }
    // Parent, skater and other-joint frames follow the current skeleton, including reference joints
    // outside the written interval. A reference that is also posed is evaluated first.
    {
        auto physical = made_up_pose();
        physical[1].position = {7, 3, -5};
        physical[1].rotation = anim_studio::axis_angle({0, 1, 0}, -0.6f);
        physical[278].position[2] += 0.3f;
        const Vec3 point{0, 0.3f, -0.1f};
        pose_replay::Entry link; link.joint = 49; link.place = Place{point, turned, Frame::joint, 278};
        auto current = native_pose(physical);
        pose_replay::Written written;
        std::vector<Bone> before;
        pose_replay::Range range;
        const auto wanted = after(anim_studio::world(physical, 278), point, turned);
        check(pose_replay::draw(current, std::span(&link, 1), 1, written, before, range), "other-joint goal replays");
        check(range.first == 49 && range.last == 50 && near(world_position(local_pose(current), 49), wanted.position),
              "a current reference outside the written interval determines the goal");
        restore(current, before, range);
        pose_replay::Entry anchor; anchor.joint = 278; anchor.place = Place{Vec3{0, 1.5f, 0.2f}, turned, Frame::skater};
        pose_replay::Entry parent_goal; parent_goal.joint = 103; parent_goal.place = Place{point, turned};
        const auto entries = ordered({link, parent_goal, anchor});
        check(pose_replay::draw(current, entries, 1, written, before, range), "mixed frames replay in dependency order");
        const auto drawn = local_pose(current);
        const auto anchor_world = after(anim_studio::world(physical, 1), *anchor.place->position, turned);
        const auto linked_world = after(anchor_world, point, turned);
        check(near(world_position(drawn, 278), anchor_world.position) && near(world_position(drawn, 49), linked_world.position),
              "the skater goal follows the newer placement and the link follows its posed reference");
        check(near(drawn[103].position, point) && near(drawn[103].rotation, turned), "parent goals retain parent-local values");
    }
    // Partial goals and partial weights start from the newer physics, not the animation's baseline.
    {
        auto physical = made_up_pose();
        physical[103].position[1] += 0.08f;
        physical[103].rotation = anim_studio::axis_angle({0, 1, 0}, -0.3f);
        const auto world_before = anim_studio::world(physical, 103);
        pose_replay::Entry goal; goal.joint = 103; goal.place = Place{Vec3{1, 2, 3}, std::nullopt, Frame::world};
        for (const float weight : {0.0f, 0.5f, 1.0f}) {
            auto current = native_pose(physical);
            const auto original = current;
            pose_replay::Written written;
            std::vector<Bone> before;
            pose_replay::Range range;
            const bool changed = pose_replay::draw(current, std::span(&goal, 1), weight, written, before, range);
            if (weight == 0) {
                check(changed && range.first == 103 && range.last == 104 && same_bytes(current, original),
                      "weight zero publishes the current baseline while leaving native bytes exact");
                continue;
            }
            check(changed, "partial placement replays");
            Vec3 expected;
            for (std::size_t a = 0; a < 3; ++a) expected[a] = world_before.position[a] + ((*goal.place->position)[a] - world_before.position[a]) * weight;
            const auto actual = anim_studio::world(local_pose(current), 103);
            check(near(actual.position, expected) && near(actual.rotation, world_before.rotation),
                  "position-only goals blend from current physics and retain current world rotation");
            goal.place = Place{std::nullopt, turned, Frame::world};
            current = original; written.clear();
            check(pose_replay::draw(current, std::span(&goal, 1), weight, written, before, range), "rotation-only placement replays");
            const auto rotated = anim_studio::world(local_pose(current), 103);
            check(near(rotated.position, world_before.position) && near(rotated.rotation, anim_studio::slerp(world_before.rotation, turned, weight)),
                  "rotation-only goals retain current world position");
            goal.place = Place{Vec3{1, 2, 3}, std::nullopt, Frame::world};
        }
    }
    // Kept children use their current world placement. A later explicit child goal still wins, and
    // repeated draws without a restore do not compound scales, offsets or half-weight placement.
    {
        auto physical = made_up_pose();
        physical[8].position[1] += 0.06f;
        physical[42].rotation = anim_studio::axis_angle({1, 0, 0}, 0.4f);
        const auto child_before = anim_studio::world(physical, 42);
        const auto grandchild_before = physical[43];
        const auto grandchild_world = anim_studio::world(physical, 43);
        pose_replay::Entry hips; hips.joint = 7; hips.scale = {1.2f, 1.2f, 1.2f}; hips.offset = {0, 0.05f, 0};
        hips.rotation = turned; hips.place = Place{Vec3{0.1f, 0.8f, -0.2f}, std::nullopt}; hips.keep_children = true;
        auto current = native_pose(physical);
        const auto original = current;
        pose_replay::Written written;
        std::vector<Bone> before;
        pose_replay::Range range;
        check(pose_replay::draw(current, std::span(&hips, 1), 0.5f, written, before, range), "kept-child placement replays");
        const auto drawn = local_pose(current);
        Vec3 wanted_hip;
        for (std::size_t a = 0; a < 3; ++a)
            wanted_hip[a] = (physical[7].position[a] + hips.offset[a] * 0.5f) * 0.5f + (*hips.place->position)[a] * 0.5f;
        check(near(drawn[7].position, wanted_hip) && near(drawn[7].scale, {1.1f, 1.1f, 1.1f}) &&
              near(drawn[7].rotation, anim_studio::normalised(anim_studio::multiply(physical[7].rotation,
                   anim_studio::slerp({0, 0, 0, 1}, turned, 0.5f)))), "rotation, scale and offset precede partial placement");
        check(near(anim_studio::world(drawn, 42).position, child_before.position) && near(anim_studio::world(drawn, 42).rotation, child_before.rotation),
              "the kept child stays at its current physics world transform");
        check(near(anim_studio::world(drawn, 42).scale, child_before.scale) &&
              near(anim_studio::world(drawn, 43).position, grandchild_world.position),
              "kept children retain their world scale and their descendants' world placement");
        check(drawn[43].position == grandchild_before.position && drawn[43].rotation == grandchild_before.rotation,
              "an unrequested descendant keeps its native local transform");
        const auto first_draw = current;
        for (int i = 0; i < 3; ++i) {
            check(pose_replay::draw(current, std::span(&hips, 1), 0.5f, written, before, range), "the same pose may be drawn again");
            check(same_bytes(current, first_draw), "repeated partial poses and kept children never accumulate");
        }
        restore(current, before, range);
        check(same_bytes(current, original), "a repeated draw restores the original, not the previous overlay");
        pose_replay::Entry child; child.joint = 42; child.place = Place{Vec3{0, 0.2f, 0}, turned};
        const auto entries = ordered({child, hips});
        check(pose_replay::draw(current, entries, 1, written, before, range), "a kept child can also be explicitly posed");
        check(near(local_pose(current)[42].position, *child.place->position), "the ordered explicit child goal is applied after keeping it");
    }
    // A new native value wins over the previous write; a broken batch never publishes a prefix.
    {
        auto current = native_pose(made_up_pose());
        pose_replay::Entry entry; entry.joint = 7; entry.offset = {0, 0.03f, 0}; entry.keep_children = true;
        pose_replay::Written written;
        std::vector<Bone> before;
        pose_replay::Range range;
        check(pose_replay::draw(current, std::span(&entry, 1), 1, written, before, range), "initial mixed replay succeeds");
        restore(current, before, range);
        current[7].position[1] += 0.02f;
        const auto newer = current;
        check(pose_replay::draw(current, std::span(&entry, 1), 1, written, before, range), "a newer native pose replays");
        check(std::abs(current[7].position[1] - newer[7].position[1] - 0.03f) < 1e-6f, "a new physics value is the next baseline");
        restore(current, before, range);
        check(same_bytes(current, newer), "the newer native baseline restores exactly");
        // A successful paired restore retires history in on_drawn. Native physics may then quite
        // legitimately produce exactly the value the previous overlay happened to write.
        written.clear();
        current[7].position[1] += entry.offset[1];
        const auto matching_native = current;
        check(pose_replay::draw(current, std::span(&entry, 1), 1, written, before, range), "a completed restore admits a matching new native value");
        check(std::abs(current[7].position[1] - matching_native[7].position[1] - entry.offset[1]) < 1e-6f,
              "genuine physics equal to the old overlay is not stripped after the paired restore");
        restore(current, before, range);
        current = newer; written.clear();
        check(pose_replay::draw(current, std::span(&entry, 1), 1, written, before, range), "a replay can be replaced before restoration");
        pose_replay::Entry replacement; replacement.joint = 103; replacement.offset = {0, 0, 0.02f};
        check(pose_replay::draw(current, std::span(&replacement, 1), 1, written, before, range), "a disjoint new batch replaces the old overlay");
        check(std::memcmp(&current[7], &newer[7], sizeof(Bone)) == 0 && std::memcmp(&current[42], &newer[42], sizeof(Bone)) == 0 &&
              written.size() == 1 && written.front().first == 103, "old parent and kept-child edits are recovered and retired");
        restore(current, before, range);
        check(same_bytes(current, newer), "replacing a batch still restores the complete current native baseline");
        const auto old_written = written;
        const auto old_before = before;
        const auto old_range = range;
        pose_replay::Entry broken; broken.joint = 103;
        broken.place = Place{Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0}, std::nullopt, Frame::world};
        const auto entries = ordered({entry, broken});
        check(!pose_replay::draw(current, entries, 1, written, before, range), "a non-finite result refuses the whole replay");
        check(same_bytes(current, newer) && same_bytes(before, old_before) && range.first == old_range.first && range.last == old_range.last &&
              written.size() == old_written.size(), "a refused batch leaves native bytes, restoration and write history unchanged");
        for (std::size_t i = 0; i < written.size(); ++i)
            check(written[i].first == old_written[i].first && same_joint(written[i].second.before, old_written[i].second.before) &&
                  same_joint(written[i].second.after, old_written[i].second.after), "a refused batch preserves every before/after history value");
    }
    std::cout << "pose_place tests passed\n";
}
