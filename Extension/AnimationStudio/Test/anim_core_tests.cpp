// Animation Studio's keyframes apart from the game: turning numbers, blending keys, what a profile
// and a clip file hold, and that edits never grow a pose.
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/AnimationStudio/skater_mirror.h"
#include <cmath>
#include <iostream>
#include <vector>
#include <stdexcept>

using namespace dingosdk::anim_studio;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b, float within = 1e-3f) { return std::abs(a - b) < within; }
bool same_turn(const Quat& a, const Quat& b) {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return d > 0.9999f;
}

void euler_numbers_read_back() {
    for (const Vec3 angles : {Vec3{0, 0, 0}, Vec3{30, 0, 0}, Vec3{0, -45, 0}, Vec3{10, 20, 30}, Vec3{-120, 60, 170}, Vec3{5, -80, -90}}) {
        const auto back = to_euler(from_euler(angles));
        check(same_turn(from_euler(back), from_euler(angles)), "Degrees turned into a rotation and back give the same rotation");
    }
    const auto x = from_euler({90, 0, 0});
    const auto v = rotate(x, {0, 1, 0});
    check(near(v[0], 0) && near(v[1], 0) && near(v[2], 1), "90 degrees about X turns +Y onto +Z");
}

void slerp_meets_both_ends() {
    const auto a = from_euler({0, 0, 0}), b = from_euler({0, 90, 0});
    check(same_turn(slerp(a, b, 0), a) && same_turn(slerp(a, b, 1), b), "Blending starts and ends on the keys");
    check(same_turn(slerp(a, b, 0.5f), from_euler({0, 45, 0})), "and halfway is half the turn");
}

// A turn through keys keeps its speed at each key (no stop-and-go in playback).
void turn_keeps_its_speed_through_a_key() {
    Animation animation;
    animation.clip.times = {0, 2};
    set_key(animation, 343, 0.0f, {from_euler({0, 0, 0}), {}});
    set_key(animation, 343, 1.0f, {from_euler({90, 0, 0}), {}});
    set_key(animation, 343, 2.0f, {from_euler({180, 0, 0}), {}});
    const auto* track = animation.track(343);
    const auto angle = [&](float a, float b) {
        const auto qa = evaluate(*track, a).rotation, qb = evaluate(*track, b).rotation;
        return 2 * std::acos(std::min(1.0f, std::abs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3])));
    };
    const float speed_before = angle(0.95f, 1.0f) / 0.05f, speed_after = angle(1.0f, 1.05f) / 0.05f;
    check(speed_after > 0.5f * (angle(0.0f, 1.0f) / 1.0f) && speed_before > 0.5f * (angle(0.0f, 1.0f) / 1.0f),
          "A turn keeps its speed through a key");
    check(near(speed_before, speed_after, 0.2f * speed_after), "The speed on either side of a key is the same");
}

void keys_hold_outside_and_blend_between() {
    Animation animation;
    animation.clip.times = {0, 1};
    set_key(animation, 343, 0.2f, {from_euler({0, 0, 0}), {0, 0, 0}});
    set_key(animation, 343, 0.8f, {from_euler({60, 0, 0}), {0, 0.3f, 0}});
    const auto* track = animation.track(343);
    check(track && track->keys.size() == 2, "Two keys on the left foot");
    check(same_turn(evaluate(*track, 0.0f).rotation, from_euler({0, 0, 0})), "Before the first key it holds");
    check(same_turn(evaluate(*track, 1.0f).rotation, from_euler({60, 0, 0})), "After the last key it holds");
    check(near(evaluate(*track, 0.5f).offset[1], 0.15f), "Halfway between the keys the move is halfway");
    set_key(animation, 343, 0.8f + 1.0f / 240, {from_euler({30, 0, 0}), {}});
    check(track->keys.size() == 2, "A key set on top of another replaces it");
    check(remove_key(animation, 343, 0.2f) && track->keys.size() == 1, "and a key can be taken off");
    auto& key = animation.track(343)->keys.front();
    key.ease = Ease::step;
    set_key(animation, 343, 0.9f, {from_euler({0, 0, 0}), {}});
    check(same_turn(evaluate(*animation.track(343), 0.85f).rotation, from_euler({30, 0, 0})), "A stepped key holds until the next one");
}

void keys_can_be_dragged_in_time() {
    Track track{7, {{0.1f}, {0.5f}, {0.9f}}};
    const auto index = move_key(track, 0, 0.7f, 1.0f);
    check(index == 1 && near(track.keys[1].time, 0.7f) && near(track.keys[0].time, 0.5f), "A key dragged past another moves past it");
    move_key(track, 2, 5.0f, 1.0f);
    check(near(track.keys.back().time, 1.0f), "A key stays on the clip");
}

void edits_are_on_top_of_the_pose_and_never_grow_it() {
    Animation animation;
    animation.clip.times = {0, 2};
    for (int i = 0; i < 1000; ++i)
        set_key(animation, editable_joints()[static_cast<std::size_t>(i) % editable_joints().size()].index, static_cast<float>(i / 24) / 50.0f,
                {from_euler({1, 0, 0}), {}});
    check(animation.key_count() == 1000, "A thousand keys are kept");
    std::vector<Joint> pose(skeleton_joints);
    pose[343].rotation = from_euler({0, 20, 0});
    apply_edits(animation, 0.5f, pose);
    check(pose.size() == skeleton_joints, "The pose keeps its 395 joints whatever is keyed");
    check(same_turn(pose[343].rotation, multiply(from_euler({0, 20, 0}), from_euler({1, 0, 0}))), "The edit turns the joint on from its own pose");
    check(same_turn(pose[200].rotation, Quat{0, 0, 0, 1}), "A joint without keys is untouched");
}

void joints_hang_from_their_parents() {
    std::vector<Joint> pose(skeleton_joints);
    pose[7].position = {0, 1, 0};        // the hips 1 m up
    pose[341].position = {0.1f, 0, 0};   // the left thigh beside them
    pose[342].position = {0, -0.5f, 0};  // the knee below the thigh
    pose[341].rotation = from_euler({0, 0, 90});
    const auto knee = world(pose, 342);
    // The thigh's quarter turn about Z swings the knee's -Y offset onto +X.
    check(near(knee.position[0], 0.6f) && near(knee.position[1], 1.0f), "Turning the thigh carries the knee");
    const auto hips = world(pose, 7);
    check(near(hips.position[1], 1.0f), "and the hips stay where they were");
}

void profiles_read_back_and_bad_ones_are_refused() {
    Animation animation;
    animation.name = "Kickflip 2";
    animation.trick = "Kickflip";
    animation.clip.times = {0, 1.5f};
    animation.speed = 0.5f;
    set_key(animation, 7, 0.4f, {from_euler({0, 10, 0}), {0, 0.1f, 0}});
    set_key(animation, 278, 1.0f, {from_euler({20, 0, 5}), {}}).ease = Ease::linear;
    const auto back = from_json(to_json(animation));
    check(back.name == "Kickflip 2" && back.trick == "Kickflip" && near(back.speed, 0.5f), "Name, trick and speed read back");
    check(back.key_count() == 2 && back.track(278) && back.track(278)->keys[0].ease == Ease::linear, "Keys read back");
    check(same_turn(back.track(7)->keys[0].rotation, from_euler({0, 10, 0})) && near(back.track(7)->keys[0].offset[1], 0.1f),
          "with their values");
    bool refused = false;
    try { (void)from_json(R"({"format":"fourth-animation-studio","version":1,"duration":1,"tracks":[{"joint":200,"keys":[]}]})"); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "A track on a joint the editor does not edit is refused");
    refused = false;
    try { (void)from_json(R"({"format":"fourth-animation-studio","version":1,"duration":1,"tracks":[{"joint":7,"keys":[{"t":9,"r":[0,0,0,1],"p":[0,0,0]}]}]})"); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "A key past the clip is refused");
    check(file_safe("  ../Kick:flip*  ") == "Kickflip", "File names keep only safe letters");
}

void clips_read_back_and_broken_ones_are_refused() {
    Clip clip;
    clip.joints = 3;
    clip.times = {0, 0.5f};
    clip.frames.resize(6);
    clip.frames[4].position = {1, 2, 3};
    clip.takeoff = 0.1f;
    clip.landing = 0.4f;
    const auto bytes = clip_bytes(clip);
    const auto back = clip_from_bytes(bytes);
    check(back.joints == 3 && back.times.size() == 2 && near(back.frames[4].position[2], 3) && near(back.landing, 0.4f), "A clip reads back");
    std::vector<Joint> pose(3);
    back.sample(0.25f, pose);
    check(near(pose[1].position[1], 1.0f), "and samples halfway between frames");
    auto cut = bytes;
    cut.pop_back();
    bool refused = false;
    try { (void)clip_from_bytes(cut); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "A cut-short clip is refused");
}
} // namespace


// Goofy: the mirror pairs the joints on the two sides, is its own inverse, and moves a right key to the left.
void mirror_pairs_and_comes_back() {
    for (std::uint16_t joint = 0; joint < skeleton_joints; ++joint) {
        check(mirror_joint[mirror_joint[joint]] == joint, "Mirroring twice gives the same joint");
    }
    check(mirror_joint[7] == 7 && mirror_joint[42] == 42, "The hips and the spine sit on the middle line");
    check(mirror_joint[8] != 8 && mirror_joint[mirror_joint[8]] == 8, "The right thigh pairs with the left");
    Joint right;
    right.rotation = from_euler({20, 30, 40});
    right.position = {0.3f, 0.1f, -0.2f};
    const auto back = mirror_pose(mirror_pose(right));
    check(same_turn(back.rotation, right.rotation) && near(back.position[0], right.position[0]), "A pose mirrored twice is the pose");
    Animation animation;
    animation.clip.joints = skeleton_joints;
    animation.clip.times = {0.0f};
    animation.clip.frames.assign(skeleton_joints, Joint{});
    animation.clip.frames[8].position = {0.4f, 0, 0};
    const auto goofy = mirror_animation(animation);
    check(goofy.clip.frames[mirror_joint[8]].position[0] == -0.4f, "A right thigh's place goes to the left, on the other side");
    set_key(animation, 8, 0.5f, {from_euler({10, 0, 0}), {0.2f, 0, 0}});
    const auto goofy_key = mirror_animation(animation);
    const auto* track = goofy_key.track(mirror_joint[8]);
    check(track && near(track->keys[0].offset[0], -0.2f), "A key on the right moves to the left, mirrored");
}


Vec3 sub3_test(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float length3_test(const Vec3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// Dragging a foot: the leg bends so the foot reaches the drag, the bones keep their lengths, and a drag to
// where the foot already is changes nothing. Works on the skater's own joints (Hips 7, RightUpLeg 8,
// RightLeg 9, RightFoot 10).
void dragged_foot_reaches_the_drag() {
    std::vector<Joint> native(skeleton_joints);
    native[8].position = {0.1f, -0.05f, 0};
    native[9].position = {0, -0.4f, 0};
    native[10].position = {0, -0.4f, 0};
    const auto before_thigh = world(native, 8).position, before_shin = world(native, 9).position, before_foot = world(native, 10).position;
    const float shin = length3_test(sub3_test(world(native, 10).position, world(native, 9).position));
    const Vec3 target = {before_foot[0] + 0.1f, before_foot[1] + 0.25f, before_foot[2] + 0.12f};
    Animation animation;
    animation.clip.joints = skeleton_joints;
    drag_joint(animation, native, 0.5f, 10, target);
    std::vector<Joint> posed(native);
    apply_edits(animation, 0.5f, posed);
    const auto foot = world(posed, 10).position;
    check(length3_test(sub3_test(foot, target)) < 2e-3f, "The dragged foot reaches the drag");
    const auto thigh = world(posed, 8).position, knee = world(posed, 9).position;
    check(near(length3_test(sub3_test(knee, thigh)), length3_test(sub3_test(before_shin, before_thigh)), 2e-3f), "The thigh keeps its length");
    check(near(length3_test(sub3_test(foot, knee)), shin, 2e-3f), "The shin keeps its length");
    check(near(thigh[0], before_thigh[0], 1e-4f) && near(thigh[1], before_thigh[1], 1e-4f), "The hip end of the chain stays where it is");
    Animation still;
    still.clip.joints = skeleton_joints;
    drag_joint(still, native, 0.5f, 10, before_foot);
    std::vector<Joint> rest(native);
    apply_edits(still, 0.5f, rest);
    check(length3_test(sub3_test(world(rest, 10).position, before_foot)) < 2e-3f, "A drag to where the foot is holds it there");
}

int main() {
    try {
        euler_numbers_read_back();
        mirror_pairs_and_comes_back();
        dragged_foot_reaches_the_drag();
        turn_keeps_its_speed_through_a_key();
        slerp_meets_both_ends();
        keys_hold_outside_and_blend_between();
        keys_can_be_dragged_in_time();
        edits_are_on_top_of_the_pose_and_never_grow_it();
        joints_hang_from_their_parents();
        profiles_read_back_and_bad_ones_are_refused();
        clips_read_back_and_broken_ones_are_refused();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "anim_core: all passed\n";
    return 0;
}
