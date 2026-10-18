// Animation Studio's keyframes apart from the game: turning numbers, blending keys, what a profile
// and a clip file hold, and that edits never grow a pose.
#include "Extension/AnimationStudio/anim_core.h"
#include <cmath>
#include <iostream>
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

int main() {
    try {
        euler_numbers_read_back();
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
