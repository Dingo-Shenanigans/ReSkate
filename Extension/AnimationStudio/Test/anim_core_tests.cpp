// Animation Studio's keyframes apart from the game: turning numbers, blending keys, what a profile
// and a clip file hold, and that edits never grow a pose.
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/AnimationStudio/skater_mirror.h"
#include "Extension/AnimationStudio/anim_presets.h"
#include "Extension/AnimationStudio/anim_research.h"
#include "Extension/AnimationStudio/anim_runtime_rules.h"
#include "Extension/AnimationStudio/anim_diagnostics.h"
#include "Extension/AnimationStudio/anim_library.h"
#include "Extension/AnimationStudio/anim_playback.h"
#include "Extension/AnimationStudio/native_asset_names.h"
#include "Extension/AnimationStudio/recording_rules.h"
#include <limits>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <stdexcept>

using namespace dingosdk::anim_studio;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct NamedNativeFixture { std::string asset, label, category, context, search, state; };
bool near(float a, float b, float within = 1e-3f) { return std::abs(a - b) < within; }
bool same_turn(const Quat& a, const Quat& b) {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return d > 0.9999f;
}

void native_names_and_search() {
    const auto flip = native_asset_name("animation/dingo/c_proto_onb_trick_n_360flip_high_dark", "ClipControllerAsset");
    check(flip.label == "Nollie 360 Flip High Dark", "Readable names preserve pop family, height and native variant");
    check(native_search_matches(flip.search, native_search_terms("HIGH tre-flip nollie")), "Readable aliases match all query words regardless of order/case/punctuation");
    check(native_search_matches(flip.search, native_search_terms("360flip haut")), "Compact engine spelling and height alias remain searchable");
    check(!native_search_matches(flip.search, native_search_terms("kickflip high")), "Unrelated tricks do not match");
    check(native_asset_name("animation/dingo/c_proto_onb_trick_n_nollie_mid").label == "Nollie Medium", "Nollie is not duplicated");
    check(native_asset_name("animation/dingo/c_proto_onb_trick_o_kickflip_low").label == "Kickflip Low", "Confirmed ollie-family marker is hidden without inventing stance");
    check(native_asset_name("animation/dingo/c_proto_onb_manual_t_fakie_high").label == "Manual T Fakie High", "Unknown codes remain visible rather than guessed");
    check(native_asset_name("animation/dingo/c_proto_ofbnb_walk_stop_90l_lfoot_fem").label == "Walk Stop 90 Left Left Foot Female", "Movement directions and physical feet retain their meaning");
    check(native_asset_name("animation/dingo/c_proto_onb_gesture_mysteryXYZ_07").label == "Gesture Mystery XYZ 07", "Unknown animation and variant identifiers are retained");
    const auto folder = [](const char* asset) { return native_asset_folder(asset, "ClipControllerAsset"); };
    check(folder("c_proto_onb_grind_fs_feeble_bs_smith_rail_into") == "Grinds/Feeble + Smith/Entries", "Shared grind clips are grouped by tricks instead of FS/BS developer prefixes");
    check(folder("c_proto_onb_grind_5050_rail_loop") == "Grinds/50-50/Loops", "Grind loops have a specific family/phase folder");
    check(folder("c_proto_onb_grind_fs_bs_5_0_rail_into") == "Grinds/5-0/Entries" && native_asset_name("c_proto_onb_grind_5_0_loop").label == "Grind 5-0 Loop", "Five-oh source tokens form one readable trick name and folder");
    check(folder("c_proto_onb_manual_t_fakie_high") == "Manuals/Other movements", "Unspecified phases are not invented");
    check(folder("c_proto_ofbnb_walk_loop_short") == "Off board/Walk/Loops", "Locomotion is separate from on-board tricks");
    check(folder("c_proto_onb_trick_n_360flip_high") == "Tricks/360 Flip/Variants", "Trick variants share a readable family folder");
    check(native_asset_folder("anything", "TransitionAsset") == "Game logic/Transitions", "Actual root types separate graph transitions from playable animations");
    check(native_search_terms("  --- \t ").empty(), "Punctuation-only input does not create misleading search terms");
    std::vector<NamedNativeFixture> variants{{"source-b", "Wave", "Gestures", "Off board"}, {"source-a", "Wave", "Gestures", "Off board"}, {"source-c", "Wave", "Gestures", "On board"}};
    native_disambiguate_names(variants);
    check(variants[0].label == "Wave / Off board / Variant 2" && variants[1].label == "Wave / Off board / Variant 1" && variants[2].label == "Wave / On board",
          "Equal names retain distinct source context and stable variants independent of input order");
    check(variants[0].asset == "source-b", "Human names never change the identity used to open an animation");
}

void manual_and_scripted_recording_keep_distinct_rules() {
    const auto manual = evaluate_recorded_take(false, "kickflip", "heelflip", "low", 3, true, 0.7f);
    check(manual.rejection == TakeRejection::none && manual.pop == "high", "Manual takes accept the recognized trick and actual grade, ignoring stale batch targets");
    const auto fallback = evaluate_recorded_take(false, "kickflip", "", "", 0, true, 0.5f);
    check(fallback.rejection == TakeRejection::none && fallback.pop == "mid", "Manual recording still works without native grading hooks");
    check(evaluate_recorded_take(true, "heelflip", "varialheelflip", "high", 3, true, 0.7f).rejection == TakeRejection::wrong_trick,
          "Batch recording cannot save the wrong recognized trick");
    check(evaluate_recorded_take(true, "kickflip", "kickflip", "low", 3, true, 0.7f).rejection == TakeRejection::wrong_pop,
          "Batch recording keeps the game's actual requested pop grade");
    check(evaluate_recorded_take(true, "kickflip", "kickflip", "high", 0, true, 0.7f).rejection == TakeRejection::missing_grade,
          "Batch cannot substitute an air-time guess for missing native grading");
    check(evaluate_recorded_take(false, "kickflip", "", "", 4, true, 0.7f).pop == "high", "Unknown native enum values use the manual fallback");
    check(evaluate_recorded_take(false, "", "", "", 3, true, 0.7f).rejection == TakeRejection::missing_trick &&
          evaluate_recorded_take(false, "kickflip", "", "", 3, false, 0.7f).rejection == TakeRejection::missing_board,
          "Manual recording still rejects missing identity and board capture");
    check(pop_grade_since(100, 0.5) == 0 && pop_grade_since(5000, 0.5) == 4200,
          "Grade lookup uses a bounded recent time window without unsigned wraparound");
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

void animation_targets_keep_their_motion_and_hierarchy() {
    Clip clip;
    clip.joints = skeleton_joints;
    clip.times = {0, 0.1f, 0.2f};
    clip.frames.assign(skeleton_joints * 3, Joint{});
    for (std::size_t f = 0; f < 3; ++f) {
        clip.frames[f * skeleton_joints + 50].position = {0.1f * f, 1.0f * f, 0};
        clip.frames[f * skeleton_joints + 50].rotation = from_euler({10.0f * static_cast<float>(f), 0, 0});
    }
    for (std::size_t f = 0; f < 3; ++f) {
        clip.frames[f * skeleton_joints + 376].position = {0, 0.3f * f, 0};
        clip.frames[f * skeleton_joints + 390].position = {0, 0.4f * f, 0};
    }
    const auto decoded = clip_from_bytes(clip_bytes(clip));
    check(decoded.frames == clip.frames, "Serialization preserves authored attachment, ground and deck target motion exactly");
    check(parent(376) == 375 && parent(379) == 375 && parent(380) == 1 && parent(390) == 380 && parent(394) == 380,
          "Animation targets use the 395-joint Ant skeleton, not the 386-bone render skeleton");
    std::vector<Joint> pose(skeleton_joints);
    pose[1].position = {1, 2, 3}; pose[375].position = {0, 4, 0};
    pose[380].position = {0, 5, 0}; pose[380].rotation = from_euler({0, 90, 0});
    pose[376].position = {0, 0.3f, 0}; pose[390].position = {1, 0.4f, 0};
    const auto ground = world(pose, 376), deck = world(pose, 390);
    check(near(ground.position[1], 6.3f) && near(deck.position[1], 7.4f) && near(deck.position[2], 2),
          "Ground and deck effectors follow their actual parents and deck targets rotate with the board");
    clip.board.clear();
    for (std::size_t f = 0; f < clip.times.size(); ++f) {
        auto& mount = clip.frames[f * skeleton_joints + skateboard_root_joint];
        mount.position = {0.4f * f, 0.1f * f, 0};
        mount.rotation = from_euler({80.0f * f, 50.0f * f, 0});
        clip.board.push_back(mount);
    }
    std::vector<Joint> sampled(skeleton_joints);
    for (const float time : {0.0f, 0.05f, 0.11f, 0.2f}) {
        clip.sample(time, sampled); Joint mount;
        check(clip.sample_board(time, mount) && mount == sampled[skateboard_root_joint],
              "A native captured mount follows the same body sample through interpolation, without a separate board timestamp");
    }
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

// Smooth keys: a key that says nothing goes, the rest ease smoothly and are marked, and the brackets stay.
void smoothing_tidies_a_track() {
    Track track;
    track.joint = 343;
    const auto key_at = [&](float t, float y, Ease e = Ease::smooth) {
        Key k; k.time = t; k.offset = {0, y, 0}; k.ease = e; track.keys.push_back(k);
    };
    key_at(0.0f, 0.0f);          // the start bracket
    key_at(0.5f, 0.5f);
    key_at(1.0f, 1.0f, Ease::step); // straight between its neighbours: it says nothing
    key_at(1.5f, 1.5f);
    key_at(2.0f, 2.0f);          // the end bracket
    const auto before = track.keys.size();
    const auto changed = smooth_track(track, 0.0f, 2.0f);
    check(changed > 0, "Smoothing reports what it changed");
    check(track.keys.size() < before, "A key that says nothing is dropped");
    check(track.keys.front().time == 0.0f && track.keys.back().time == 2.0f, "The bracket keys stay where they are");
    check(near(track.keys.front().offset[1], 0.0f) && near(track.keys.back().offset[1], 2.0f), "and keep their values");
    for (const auto& k : track.keys) {
        check(k.ease == Ease::smooth, "Every key eases smoothly afterwards");
        check(k.smooth, "and is marked as smoothed");
    }
}

// Solo: turning a joint on its own. The joints hanging off it are turned back by as much, so they keep
// the way they face. This is the sum the studio does in set_offset, on the arm and its elbow and hand.
void solo_turn_holds_the_joints_below() {
    std::vector<Joint> native(skeleton_joints);
    for (std::size_t j = 0; j < native.size(); ++j) {
        native[j].rotation = from_euler({3.0f * static_cast<float>(j % 7) - 9, 5.0f * static_cast<float>(j % 5) - 10, 7.0f * static_cast<float>(j % 3)});
        native[j].position = {0.1f, 0.05f, -0.02f};
    }
    constexpr std::uint16_t arm = 47, elbow = 48, hand = 49;
    Animation edits;
    edits.end = -1;
    // The elbow and the hand already carry keys of their own: solo must build on those, not wipe them.
    const auto key_of = [&](std::uint16_t joint, const Vec3& degrees) {
        Key k;
        k.time = 0.5f;
        k.rotation = from_euler(degrees);
        Track track;
        track.joint = joint;
        track.keys.push_back(k);
        edits.tracks.push_back(track);
    };
    key_of(elbow, {12, -8, 4});
    key_of(hand, {-5, 15, 2});
    std::vector<Joint> before(native);
    apply_edits(edits, 0.5f, before);
    const auto faced_elbow = world(before, elbow).rotation, faced_hand = world(before, hand).rotation;

    // The arm is turned, as a drag would.
    Key turn;
    turn.time = 0.5f;
    turn.rotation = from_euler({0, 40, 0});
    Track arm_track;
    arm_track.joint = arm;
    arm_track.keys.push_back(turn);
    edits.tracks.push_back(arm_track);
    std::vector<Joint> swung(native);
    apply_edits(edits, 0.5f, swung);
    check(!same_turn(world(swung, elbow).rotation, faced_elbow), "Without solo the joints below swing with their parent");

    // Solo puts them back: a joint's place is the chain above it times its own key, so multiplying its key
    // by conj(where it faces now) * (where it faced) holds it. Only the joint hanging directly off the one
    // being turned is touched - the hand rides on the elbow, so holding the elbow holds the hand with it.
    check(parent(elbow) == arm && parent(hand) == elbow, "The arm, its elbow and its hand hang off one another");
    const auto back = normalised(multiply(conjugate(world(swung, elbow).rotation), faced_elbow));
    for (auto& track : edits.tracks)
        if (track.joint == elbow) track.keys.front().rotation = normalised(multiply(track.keys.front().rotation, back));
    std::vector<Joint> held(native);
    apply_edits(edits, 0.5f, held);
    check(same_turn(world(held, elbow).rotation, faced_elbow), "Solo holds the way the joint below the turn faces");
    check(same_turn(world(held, hand).rotation, faced_hand), "and the one below that, which rides on the held joint");
    check(same_turn(world(held, arm).rotation, world(swung, arm).rotation), "while the joint being turned keeps its turn");
    // Bones are rigid: the joints below still travel with the one that turned. Only their facing is held.
    check(length3_test(sub3_test(world(held, hand).position, world(before, hand).position)) > 1e-3f,
          "The joints below still travel with their parent: solo holds the facing, not the place");
}

void packs_select_variants_without_filesystem_order() {
    std::vector<Preset> presets{{"Z", "Kickflip", "mid"}, {"A", "kickflip", "mid"}, {"Any", "kickflip", ""}};
    select_presets(presets, std::nullopt);
    check(!presets[0].active && presets[1].active && presets[2].active, "Default choice is deterministic and separate for each pop");
    PresetPack pack;
    pack.name = "Test";
    pack.slots["kickflip|mid"] = {"Deleted", "Z", "A"};
    select_presets(presets, pack);
    check(presets[0].active && !presets[1].active && !presets[2].active, "Fixed pack skips missing variants and respects its chosen order");
    check(presets[0].included && presets[1].included && !presets[2].included, "Pack membership is distinct from the fixed choice");
    pack.alternate = true;
    select_presets(presets, read_pack_json(pack_json(pack)));
    check(presets[0].active && presets[1].active && !presets[2].active, "Alternate pack enables all included variants after round trip");
    pack.slots.clear();
    select_presets(presets, pack);
    check(!presets[0].active && !presets[1].active && !presets[2].active, "An empty pack leaves tricks native");
    bool refused = false;
    try { read_pack_json(dingosdk::Json::parse(R"({"format":"animator-preset-pack","version":1,"slots":{"ollie|":[42]}})")); }
    catch (const std::exception&) { refused = true; }
    check(refused, "Invalid pack members are refused");
    std::vector<Preset> stances{
        {"Normal A", "ollie", "", false, false, "goofy", false},
        {"Switch A", "ollie", "", false, false, "goofy", true},
        {"Switch Z", "ollie", "", false, false, "goofy", true},
        {"Regular", "ollie", "", false, false, "regular", false}};
    select_presets(stances, std::nullopt);
    check(stances[0].active && stances[1].active && !stances[2].active && stances[3].active,
          "Default selection retains a separate playable recording per base/switch stance");
    pack.alternate = false;
    pack.slots["ollie|"] = {"Normal A", "Switch Z", "Switch A", "Regular"};
    select_presets(stances, pack);
    check(stances[0].active && !stances[1].active && stances[2].active && stances[3].active,
          "Fixed packs choose their first included compatible variant for each stance");
}
void research_keeps_short_transitions_and_is_bounded() {
    ResearchBuffer buffer;
    ResearchSample sample;
    sample.physics = 200;
    buffer.append(sample);
    sample.seconds = 0.001;
    buffer.append(sample);
    check(buffer.samples.size() == 1, "Unchanged state is sampled periodically");
    sample.physics = 403;
    buffer.append(sample);
    sample.seconds = 0.002;
    sample.physics = 401;
    buffer.append(sample);
    check(buffer.samples.size() == 3, "Rail transitions between periodic samples are retained");
    sample.seconds = 0.003;
    sample.physics.reset();
    buffer.append(sample);
    check(!buffer.samples.back().physics, "Unavailable state is preserved, never guessed as ground");
    sample.seconds = 61;
    check(!buffer.append(sample) && buffer.samples.size() == 4, "Trace ends after its bounded duration");
    buffer.samples.clear();
    for (std::size_t i = 0; i < ResearchBuffer::capacity + 4; ++i) {
        sample.seconds = static_cast<double>(i) / 1000.0;
        sample.physics = static_cast<std::uint32_t>(i);
        buffer.append(sample);
    }
    check(buffer.samples.size() == ResearchBuffer::capacity, "Trace memory stays bounded even with a state change each tick");
}
void merged_library_and_timing() {
    check(episode_selection_changed("", "ollie") && episode_selection_changed("ollie", "kickflip"),
          "A first recognized trick and a late flip can select a preset");
    check(!episode_selection_changed("kickflip", "kickflip") && !episode_selection_changed("kickflip", ""),
          "Pop refinement and a blank name cannot restart the same trick's source clock");
    check(recorded_clip_identity("n_heelflip__high.clip") == std::pair{std::string("n_heelflip"), std::string("high")},
          "Recorded filenames preserve the game's underscored trick ID");
    for (const auto* file : {"../ollie__high.clip", "a\\ollie__high.clip", "a:ollie__high.clip", "ollie__any.clip", "__high.clip"})
        check(!recorded_clip_identity(file), "Recorded clips reject traversal and ambiguous identities");
    GameTrick low{"Flip", "Kickflip", "kickflip", "low", "native-low.clip", "asset-low"};
    auto high = low; high.pop = "high"; high.file = "native-high.clip"; high.asset = "asset-high";
    GameTrick grind{"Grind", "50-50 entry", "", "", "grind.clip", "grind-asset", true};
    auto take = high; take.file = "kickflip__high.clip"; take.asset.clear(); take.stance = "goofy"; take.switch_stance = true;
    const auto library = merge_game_library({low, high, grind}, {take});
    check(library.size() == 3, "A batch recording replaces only its matching native trick/pop slot");
    check(library[0].recorded && library[0].file == take.file && library[0].asset == "asset-high" &&
          library[0].stance == "goofy" && library[0].switch_stance.value_or(false),
          "High pop is shown first and retains native provenance plus recorded stance");
    check(!library[1].recorded && library[1].file == low.file && library[2].preview_only,
          "Other pop heights and grind previews stay available after Record all");
    check(playback_pop_rank("high", "high") == 0 && playback_pop_rank("high", "") == 1 &&
          playback_pop_rank("high", "low") < 0 && playback_pop_rank("mid", "low") < 0,
          "Playback accepts exact or explicitly generic pops, never a different source height");
    check(episode_selection_changed("360flip", "360flip", "low", "high") &&
          !episode_selection_changed("360flip", "360flip", "low", "low") &&
          !episode_selection_changed("360flip", "360flip", "", "high"),
          "A late high-pop grade invalidates a low binding without cycling a generic or matching binding");
    // Which foot leads, and what that means for a trick recorded in one stance being done in another.
    check(leads_right(true, false) && !leads_right(false, false), "Goofy leads with the right foot, regular with the left");
    check(leads_right(false, true) && !leads_right(true, true), "Riding switch turns that round");
    check(playback_lead_matches(true, false, false, true), "A goofy recording suits a regular skater riding switch as it is");
    check(playback_lead_matches(false, false, true, true), "and a regular one suits a goofy skater riding switch");
    check(!playback_lead_matches(true, false, false, false), "A goofy recording must be mirrored for a regular skater");
    check(!playback_lead_matches(true, false, true, true), "and for a goofy skater riding switch");
    check(!playback_lead_matches(true, false, false, std::nullopt), "Nothing matches while the switch state is unknown");
    GameplayClipClock clock;
    check(near(clock.advance_air(10, 0.3f, 1, 2), 0.3f), "Newly selected presets begin at takeoff");
    check(near(clock.advance_air(10.1, 0.3f, 1, 2), 0.4f), "Air source time advances incrementally");
    check(near(clock.advance_air(10.1, 0.3f, 2, 2), 0.4f), "Changing pop rate cannot move the playhead on the same callback");
    check(near(clock.advance_air(10.35, 0.3f, 2, 2), 0.9f), "A long callback integrates its elapsed time at the updated rate");
    check(near(clock.advance_air(10, 0.3f, 2, 2), 0.9f), "Backwards callbacks cannot rewind air playback");
    check(near(clock.advance_air(12, 0.3f, 2, 2), 2) && near(clock.air_rate, 0), "The landing marker holds until contact");
    // Moving a bracket carries that bracket's own key and nothing else: the keys between the brackets are
    // left exactly where they are, so the track keeps its whole frames instead of being stretched onto
    // fractions of one. (This replaces the window retiming, which stretched every key.)
    Animation animation; animation.start = 1; animation.end = 3;
    Track track{7, {}, false, false};
    track.keys = {{0, {}, {}, Ease::smooth}, {1, {}, {}, Ease::smooth}, {2, {}, {}, Ease::smooth}, {3, {}, {}, Ease::smooth}};
    animation.tracks.push_back(track); track.joint = 8; track.locked = true; animation.tracks.push_back(track);
    check(move_window_edge(animation, 1, 0.5f, 4), "A bracket accepts a place inside the window");
    check(near(animation.start, 0.5f), "and the window starts there");
    check(animation.tracks[0].keys.size() == 4 && near(animation.tracks[0].keys[1].time, 0.5f),
          "The key that was on the bracket goes with it");
    check(near(animation.tracks[0].keys[2].time, 2) && near(animation.tracks[0].keys[3].time, 3),
          "and the keys between the brackets do not move");
    check(near(animation.tracks[1].keys[1].time, 1), "A locked track is left alone");
    check(!move_window_edge(animation, 1, 3.5f, 4) && near(animation.start, 0.5f), "A bracket cannot be put past its other half");
    check(!move_window_edge(animation, 2, 0.2f, 4) && near(animation.end, 3), "and that holds for the end as well");
    check(move_window_edge(animation, 1, 0.0f, 4) && animation.tracks[0].keys.size() == 3 && near(animation.tracks[0].keys[0].time, 0),
          "Taken back onto a key that is already there, the two become one");
    // Smoothing puts keys back on whole frames, which is where an older stretched animation is mended.
    Track drifted{9, {}, false, false};
    drifted.keys = {{0, {}, {}, Ease::smooth}, {0.337f, {}, {0, 0.4f, 0}, Ease::step}, {0.681f, {}, {0, 0.1f, 0}, Ease::linear}, {1, {}, {}, Ease::smooth}};
    smooth_track(drifted, 0, 1);
    for (const auto& k : drifted.keys)
        check(near(k.time, on_a_frame(k.time), 1e-5f), "Every key sits on a whole frame after smoothing");
}
void preview_trajectory_uses_the_whole_clip() {
    Clip clip;
    clip.joints = 2; clip.times = {0, 0.25f, 0.5f, 0.75f, 1};
    clip.frames.resize(clip.times.size() * clip.joints); clip.takeoff = 0; clip.landing = 1;
    for (std::size_t i = 0; i < clip.times.size(); ++i)
        clip.frames[i * clip.joints + placement_joint].position[1] = 10 + clip.times[i];
    check(!clip_has_vertical_trajectory(clip), "A rising/sloped linear trajectory is not a jump relative to its endpoints");
    clip.frames[2 * clip.joints + placement_joint].position[1] += 0.3f;
    check(clip_has_vertical_trajectory(clip),
          "A captured jump uses its trajectory for the whole clip, including early/late frames below the old 12 cm switch threshold");
    clip.frames[2 * clip.joints + placement_joint].position[1] -= 0.3f;
    check(!clip_has_vertical_trajectory(clip), "An in-place clip can use the preview arc consistently");
    clip.landing = clip.takeoff;
    check(!clip_has_vertical_trajectory(clip), "Preview-only or invalid phase markers cannot divide by a zero airborne span");
}
void timing_and_stance_edge_cases() {
    check(!gameplay_body_joint(50) && !gameplay_body_joint(283), "Gameplay does not replace native hand attachment transforms with recorded rest positions");
    check(!gameplay_body_joint(placement_joint) && !gameplay_body_joint(board_joint), "Gameplay body ownership excludes placement and board");
    for (const auto& joint : editable_joints())
        if (joint.index != board_joint) check(gameplay_body_joint(joint.index), "Every editable body joint retains gameplay ownership");
    for (std::uint16_t j = 0; j < skeleton_joints; ++j)
        check(gameplay_body_joint(j) == gameplay_body_joint(mirror_joint[j]), "Gameplay ownership is symmetric across left and right");
    GameplayClipClock clock;
    clock.observe_air(10, 0.94f, 0.46f);
    const auto first_landing = clock.land(10.0163584, 1, 1.6675f, 1.01725f);
    check(first_landing > 0.94f && first_landing < 0.95f,
          "Landing continues the previous source time instead of the measured 77 ms contact-marker jump");
    const auto later = clock.land(10.1, 1, 1.6675f, 1.1f);
    check(near(later, 0.94f + 0.08f * (0.46f + 1) / 2 + 0.02f),
          "Landing integrates its smooth rate ramp exactly even across a long callback interval");
    check(near(clock.land(10.1, 1, 1.6675f, 1.1f), later), "A duplicate callback cannot advance the landing clock");
    check(near(clock.land(9, 1, 1.6675f, 1.1f), later), "A backwards timestamp cannot rewind landing playback");
    check(near(clock.land(12, 1, 1.6675f, 1.1f), 1.6675f), "Landing clock stays inside its clip");
    clock.reset(); clock.observe_air(20, 0.98f, 0);
    check(clock.land(20.016, 1, 2, 1.1f) < 0.981f, "Held airborne pose starts landing without a source-time snap");
    Track uneven{7, {}, false, false};
    for (const float t : {0.0f, 0.25f, 2.0f, 3.0f})
        uneven.keys.push_back({t, from_euler({t * 60, 0, 0}), {t, 0, 0}, Ease::smooth});
    constexpr float h = 0.002f;
    const auto left = evaluate(uneven, 0.25f - h), centre = evaluate(uneven, 0.25f), right = evaluate(uneven, 0.25f + h);
    check(near((centre.offset[0] - left.offset[0]) / h, (right.offset[0] - centre.offset[0]) / h, 0.03f),
          "Uneven key spacing must not introduce a position velocity jump");
    check(near((to_euler(centre.rotation)[0] - to_euler(left.rotation)[0]) / h,
               (to_euler(right.rotation)[0] - to_euler(centre.rotation)[0]) / h, 2.0f),
          "Uneven key spacing must not introduce an angular velocity jump");
    for (const double hz : {30.0, 60.0, 144.0}) {
        ContactWindow contact;
        check(!contact.observe(10, true), "A single contact sample is not a confirmed landing");
        bool confirmed = false;
        for (int i = 1; i < 20; ++i)
            if (contact.observe(10 + i / hz, true)) { confirmed = true; break; }
        check(confirmed && contact.since && *contact.since == 10, "Landing marker preserves first contact independent of sample frequency");
        contact.observe(11, false);
        check(!contact.since && !contact.observe(11.001, true), "A brief contact interrupted by air cannot accumulate into a false landing");
    }
    for (const bool recorded_goofy : {false, true})
        for (const bool recorded_switch : {false, true})
            for (const bool live_goofy : {false, true})
                for (const bool live_switch : {false, true}) {
                    const bool same_stance = recorded_goofy == live_goofy && recorded_switch == live_switch;
                    check(playback_stance_compatible(recorded_goofy, recorded_switch, live_goofy, live_switch) == same_stance,
                          "Only identical base stance and switch state may replay raw recorded bones");
                }
    check(!playback_stance_compatible(true, false, true, true), "A goofy normal recording cannot enter switch through raw bone mirroring");
    check(!playback_stance_compatible(false, true, true, false), "Equal lead feet do not make regular switch and goofy normal compatible");
    check(!playback_stance_compatible(true, false, true, std::nullopt), "Unknown live switch state keeps native animation");
    check(playback_stance_compatible(true, std::nullopt, true, false) &&
          !playback_stance_compatible(true, std::nullopt, true, true), "Legacy recordings are normal stance only");
    check(near(advance_time(0, 0.25f, 1, 2, false), 0.25f), "A 250 ms render stall must not discard 150 ms of playback");
    check(near(advance_time(0.8f, 0.7f, 2, 1, true), 0.2f), "Looping handles a frame spanning multiple loops");
    check(near(advance_time(0.8f, 0.7f, 2, 1, false), 1), "Non-looping playback ends at the last frame");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    check(near(advance_time(0.4f, nan, 1, 1, false), 0.4f), "Nonfinite elapsed time cannot corrupt the playhead");
    check(!fresh_sample(10, 9.99, 9.99), "A render pose cannot be recorded again with a new animation timestamp");
    check(!fresh_sample(10, 10.01, 9.99) && fresh_sample(10, 9.995, 9.99), "Render capture rejects future and accepts fresh timestamps");
    const auto a = variation_name(std::string(48, 'a'), 2), b = variation_name(std::string(48, 'a'), 3);
    check(a.size() <= 48 && file_safe(a) == a && file_safe(a) != file_safe(b), "Long variation names retain their unique suffix after filename sanitization");
    Animation animation;
    animation.stance = "goofy";
    animation.switch_stance = true;
    auto back = from_json(to_json(animation));
    check(back.switch_stance && *back.switch_stance, "Switch metadata survives profile serialization");
    animation.switch_stance = false;
    back = from_json(to_json(animation));
    check(back.switch_stance && !*back.switch_stance, "Normal stance is distinct from missing legacy metadata");
    const auto mirror = mirror_animation(animation);
    check(mirror.stance == "regular" && mirror.switch_stance == animation.switch_stance, "Mirrored animation metadata describes the opposite base stance");
    Clip clip;
    clip.joints = 1;
    clip.times = {0, 1};
    clip.frames.resize(2);
    clip.board.resize(2);
    std::vector<Joint> pose(1);
    pose[0].position = {17, 0, 0};
    clip.sample(nan, pose);
    check(pose[0].position[0] == 17 && !clip.sample_board(nan, pose[0]), "NaN sampling cannot index past a clip's final frame");
    clip.frames.clear();
    clip.sample(0.5f, pose);
    check(pose[0].position[0] == 17, "Incomplete in-memory clips never read nonexistent poses");
    bool refused = false;
    try { (void)clip_bytes(clip); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "Incomplete poses cannot be written as a valid-looking clip");
    Animation shifted;
    shifted.clip.times = {0, 1};
    set_key(shifted, 7, 0.2f, {}).offset = {0.1f, 0, 0};
    set_key(shifted, 7, 0.8f, {});
    set_key(shifted, 7, 1.0f, {});
    set_key(shifted, 49, 0.3f, {});
    shifted.track(49)->locked = true;
    shift_keys(shifted, 0.5f, 1.0f);
    const auto restored = from_json(to_json(shifted));
    check(restored.track(7)->keys.size() == 2 && near(restored.track(7)->keys.back().time, 1),
          "Moving a start bracket keeps saturated keys unique and the profile reloadable");
    check(near(restored.track(49)->keys[0].time, 0.3f), "Bracket shifts preserve locked tracks");
    auto track = *restored.track(7);
    move_key(track, 0, 1, 1);
    check(track.keys.size() == 1 && near(track.keys[0].offset[0], 0.1f), "Dropping a key on another frame replaces its pose instead of introducing a duplicate timestamp");
    Animation window;
    window.start = 0.2f;
    window.end = 0.8f;
    set_key(window, 7, 0, {from_euler({0, 30, 0}), {0.2f, 0, 0}});
    std::vector<Joint> body(skeleton_joints);
    apply_edits(window, 0.1f, body);
    check(near(body[7].position[0], 0) && animation_window_weight(window, 0.1f) == 0,
          "A bracket excludes both joint edits and in-game replacement outside its window");
    check(near(animation_window_weight(window, 0.24f), 0.5f) && near(animation_window_weight(window, 0.5f), 1) &&
          near(animation_window_weight(window, 0.76f), 0.5f), "Custom playback blends continuously at explicit bracket edges");
    for (const auto text : {
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"start":-1})",
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"start":0.8,"end":0.2})",
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"switch_stance":"yes"})"}) {
        refused = false;
        try { (void)from_json(text); } catch (const std::exception&) { refused = true; }
        check(refused, "Malformed timeline and stance metadata is rejected before reaching playback");
    }
}
void retargeted_edits_preserve_native_rig_and_convert_joint_frames() {
    std::vector<Joint> source(skeleton_joints), native(skeleton_joints);
    source[placement_joint].rotation = from_euler({0, 37, 0});
    native[placement_joint].rotation = from_euler({0, -125, 0});
    source[hips_joint].rotation = from_euler({12, 35, -8});
    native[hips_joint].rotation = from_euler({-10, -25, 17});
    source[8].rotation = from_euler({67, 11, -20});
    native[341].rotation = from_euler({-32, 48, 91});
    for (std::size_t i = 2; i < native.size(); ++i) {
        native[i].position = {0.013f * static_cast<float>(i % 5), 0.03f, -0.02f};
        native[i].scale = {1.1f, 1.1f, 1.1f};
    }
    Animation animation;
    animation.tracks.push_back({8, {{0, {0, 0, 0, 1}, {}, Ease::linear}}});
    auto target = native;
    retarget_edits(animation, 0, source, target, true);
    for (std::size_t i = 0; i < target.size(); ++i) {
        check(target[i].position == native[i].position && target[i].scale == native[i].scale,
              "Identity retarget never imports recorded bone positions or scale");
        for (int k = 0; k < 4; ++k) check(near(target[i].rotation[k], native[i].rotation[k]), "Identity edit leaves native switch pose unchanged");
    }
    const auto edit = from_euler({23, -11, 8});
    animation.tracks[0].keys[0].rotation = edit;
    retarget_edits(animation, 0, source, target, true);
    const auto sf = multiply(conjugate(world(source, placement_joint).rotation), world(source, 8).rotation);
    const auto tf = multiply(conjugate(world(native, placement_joint).rotation), world(native, 341).rotation);
    const auto local_delta = multiply(conjugate(native[341].rotation), target[341].rotation);
    const auto actual = normalised(multiply(multiply(tf, local_delta), conjugate(tf)));
    const auto expected = normalised(mirror_rotation(multiply(multiply(sf, edit), conjugate(sf))));
    float dot = 0;
    for (int k = 0; k < 4; ++k) dot += actual[k] * expected[k];
    check(std::abs(dot) > 0.99999f, "Edit rotates in the reflected placement frame even with asymmetric local bone bases");
    for (std::size_t i = 0; i < target.size(); ++i) {
        check(target[i].position == native[i].position && target[i].scale == native[i].scale && finite(target[i]),
              "Rotational retarget preserves all native local bone lengths and scales");
        if (i != 341) check(target[i].rotation == native[i].rotation, "Only the paired editable joint is changed");
    }
    animation.tracks[0].keys[0].offset = {0.03f, -0.1f, 0.02f};
    target = native;
    retarget_edits(animation, 0, source, target, true);
    const auto src_parent = world(source, hips_joint);
    const auto dst_parent = world(native, hips_joint);
    const auto source_frame = multiply(conjugate(world(source, placement_joint).rotation), src_parent.rotation);
    const auto target_frame = multiply(conjugate(world(native, placement_joint).rotation), dst_parent.rotation);
    const auto wanted_offset = mirror_vector(rotate(source_frame, animation.tracks[0].keys[0].offset));
    Vec3 local_offset{};
    for (int k = 0; k < 3; ++k) local_offset[k] = (target[341].position[k] - native[341].position[k]) * dst_parent.scale[0];
    const auto actual_offset = rotate(target_frame, local_offset);
    for (int k = 0; k < 3; ++k) check(near(actual_offset[k], wanted_offset[k]), "Offsets convert through parent frames and compensate target parent scale");
    animation.tracks[0].hidden = true;
    target = native;
    retarget_edits(animation, 0, source, target, true);
    check(target[341].rotation == native[341].rotation, "Hidden edits stay hidden in switch");
}
void switch_edits_follow_the_native_body_facing() {
    std::vector<Joint> source(skeleton_joints);
    source[placement_joint].rotation = from_euler({0, 37, 0});
    source[hips_joint].rotation = from_euler({12, 35, -8});
    source[341].rotation = from_euler({23, -17, 9});
    source[341].position = {0.1f, -0.2f, 0.03f};
    const auto rotation = from_euler({19, 11, -7});
    const Vec3 offset{0.03f, 0.02f, -0.01f};
    Animation animation;
    animation.tracks.push_back({341, {{0, rotation, offset, Ease::linear}}});
    for (const bool mirror : {false, true}) for (const bool reverse : {false, true}) {
        auto native = source;
        if (mirror) for (std::size_t j = 0; j < source.size(); ++j) native[mirror_joint[j]] = mirror_pose(source[j]);
        native[placement_joint].rotation = from_euler({0, -125, 0});
        if (reverse) native[hips_joint].rotation = multiply(Quat{0, 1, 0, 0}, native[hips_joint].rotation);
        auto target = native;
        retarget_edits(animation, 0, source, target, mirror, reverse);
        const auto joint = mirror ? mirror_joint[341] : 341;
        const auto expected_rotation = normalised(multiply(native[joint].rotation, mirror ? mirror_rotation(rotation) : rotation));
        const auto expected_offset = mirror ? mirror_vector(offset) : offset;
        float dot = 0;
        for (int k = 0; k < 4; ++k) dot += expected_rotation[k] * target[joint].rotation[k];
        check(std::abs(dot) > 0.99999f, "A native body half turn preserves anatomical rotation direction independently of limb mirroring");
        for (int k = 0; k < 3; ++k)
            check(near(target[joint].position[k] - native[joint].position[k], expected_offset[k]),
                  "Switch conversion keeps authored parent-local offset signs in the turned native body");
        for (std::size_t j = 0; j < native.size(); ++j) {
            check(target[j].scale == native[j].scale, "Facing conversion preserves native bone scales");
            if (j != joint) check(target[j] == native[j], "Facing conversion changes only the authored limb, never the native skeleton");
        }
    }
}
void diagnostic_history_is_bounded() {
    DiagnosticBuffer history;
    for (int i = 0; i < 5000; ++i) {
        DiagnosticSample sample; sample.at = i / 240.0; sample.sequence = i;
        // Deliberately submit poses at callback frequency: the history enforces its own cap.
        sample.native.resize(1); sample.desired.resize(1); sample.render_native.resize(1); sample.render_edited.resize(1);
        history.append(std::move(sample));
    }
    check(history.samples.size() <= DiagnosticBuffer::capacity &&
          history.samples.back().at - history.samples.front().at <= DiagnosticBuffer::duration,
          "Diagnostic callback history is bounded by both count and duration");
    std::size_t poses = 0;
    for (const auto& sample : history.samples) {
        if (!sample.native.empty()) ++poses;
        else check(sample.render_edited.empty(), "Post-edit diagnostic poses obey the same sampling/memory bound as native poses");
    }
    check(poses <= 151, "Diagnostic poses cannot grow past the 10 Hz time window even at 240 callbacks per second");
    const auto size = history.samples.size();
    DiagnosticSample bad; bad.at = std::numeric_limits<double>::quiet_NaN(); history.append(std::move(bad));
    check(history.samples.size() == size, "Invalid diagnostic timestamps cannot corrupt the ring");
    DiagnosticSample reset; reset.at = 0; history.append(std::move(reset));
    check(history.samples.size() == 1, "Clock reset starts a fresh diagnostic window");
    check(diagnostic_csv("a,\"b\"") == "\"a,\"\"b\"\"\"", "Diagnostic CSV quotes names correctly");
}
void authored_gameplay_matches_f8_limbs_and_source_key_times() {
    Animation animation;
    animation.stance = "regular"; animation.switch_stance = false;
    const auto edit = from_euler({23, -11, 8});
    const Vec3 offset{0.03f, -0.02f, 0.01f};
    animation.tracks.push_back({341, {{0, edit, offset, Ease::linear}}});
    animation.tracks.push_back({board_joint, {{0, edit, {10, 10, 10}, Ease::linear}}});
    for (const bool goofy : {false, true}) for (const bool switched : {false, true}) {
        std::vector<Joint> native(skeleton_joints);
        native[placement_joint].rotation = from_euler({0, goofy ? -125.0f : 37.0f, 0});
        native[hips_joint].rotation = from_euler({12, switched ? 180.0f : 0.0f, -8});
        native[341].rotation = from_euler({67, 11, -20});
        native[8].rotation = from_euler({-32, 48, 91});
        native[341].position = {0.1f, -0.2f, 0.03f};
        auto expected = native; apply_edits(animation, 0, expected);
        auto actual = native; apply_authored_edit_layer(animation, 0, actual, 1);
        check(actual[341] == expected[341], "Gameplay uses the exact F8 local edit on the physical left leg in every stance");
        for (std::size_t j = 0; j < native.size(); ++j)
            if (j != 341) check(actual[j] == native[j], "An authored left-leg track never swaps into the right leg or imports a source pose");
        actual = native; apply_authored_edit_layer(animation, 0, actual, 0);
        check(actual == native, "A zero-weight anatomical edit is a strict no-op");
        actual = native; apply_authored_edit_layer(animation, 0, actual, 0.5f);
        for (int k = 0; k < 3; ++k)
            check(near(actual[341].position[k], native[341].position[k] + offset[k] * 0.5f), "Authored offsets blend without changing local signs");
    }
    PlaybackEpisode episode;
    episode.observe(10, motion_state(100)); episode.observe(10.1, motion_state(200)); episode.select(10.2);
    const auto first = episode.sample(10.2, 0.2f, 0.75f, 2, 1, 0.3f, true);
    check(near(first.time, 0.3f), "Late trick recognition catches up from takeoff instead of restarting the edit timeline");
    const auto short_air = episode.sample(10.3, 0.2f, 0.75f, 2, 1, 0.3f, true);
    const auto long_air = episode.sample(10.3, 0.2f, 0.75f, 2, 1, 1.8f, true);
    check(near(short_air.time, 0.4f) && near(short_air.time, long_air.time), "Source key seconds are independent of ballistic airtime estimates");
    check(near(episode.sample(10.3, 0.2f, 0.75f, 2, 0.25f, 0.3f, true).time, 0.4f) &&
          near(episode.sample(10.3, 0.2f, 0.75f, 2, 4, 0.3f, true).time, 0.4f),
          "Quarter-speed and accelerated studio previews cannot retime gameplay edit keys");
    episode.select(10.31);
    check(near(episode.sample(10.32, 0.2f, 0.75f, 2, 1, 1, true).time, 0.42f), "Refining the named trick cannot reset source key time");
    episode.observe(10.6, motion_state(100)); episode.observe(10.64, motion_state(100));
    const auto reception = episode.sample(11.4, 0.2f, 0.75f, 2, 1, 1, true);
    check(near(reception.time, 1.5f) && reception.weight == 1 && !reception.finished,
          "Keys past one second remain active through source reception instead of fading at landing plus 600 ms");
    const auto finished = episode.sample(12, 0.2f, 0.75f, 2, 1, 1, true);
    check(finished.time == 2 && finished.weight == 0 && finished.finished, "Source playback ends and releases at clip duration without looping");
}
void native_playback_contracts() {
    EditTransition transition;
    std::array<Offset, 1> a{}, b{}; a[0].offset[0] = 0.4f; b[0].offset[0] = -0.4f;
    check(near(transition.evaluate(1, 1, 1, 10, a)[0].offset[0], 0), "The first edit layer fades from native motion");
    check(near(transition.evaluate(1.081, 1, 1, 10, a)[0].offset[0], 0.4f), "An established edit layer reaches its target");
    check(near(transition.evaluate(1.1, 1, 1, 11, b)[0].offset[0], 0.4f),
          "Ollie-to-flip selection starts from the previous edit delta instead of resetting the visible pose");
    check(near(transition.evaluate(1.14, 1, 1, 11, b)[0].offset[0], 0, 0.001f), "A changed preset blends smoothly between deltas");
    check(near(transition.evaluate(1.15, 2, 1, 11, b)[0].offset[0], 0), "Respawn cannot carry a previous entity's edit layer");
    PlaybackEpisode episode;
    // Real failed hardflip trace: riding -> five crouch samples -> pop -> air.
    // Production classification must retain ground entry through state 103.
    PlaybackEpisode crouched;
    crouched.observe(2.20, motion_state(100));
    for (int i = 0; i < 5; ++i) crouched.observe(2.2158 + i / 60.0, motion_state(103));
    check(crouched.observe(2.2998, motion_state(200)) && crouched.entry == EntryKind::ground,
          "Native 100/103/200 takeoff retains ground entry so a saved ground preset can be selected");
    const auto crouch_episode = crouched.id;
    check(!crouched.observe(2.3156, motion_state(201)) && crouched.id == crouch_episode && crouched.selection_open(2.3338),
          "200/201 remains one selectable airborne episode after the crouch");
    check(motion_state(std::nullopt) == MotionState::unknown && motion_state(102) == MotionState::unknown &&
          motion_state(300) == MotionState::offboard && motion_state(504) == MotionState::offboard &&
          motion_state(401) == MotionState::rail,
          "Recognizing grounded crouch does not guess other states or confuse bails and rails");
    check(!episode.observe(0, MotionState::riding) && episode.observe(0.1, MotionState::air), "Riding to air starts a new observed episode");
    check(episode.entry == EntryKind::ground, "Ground takeoff carries a ground binding");
    episode.select(0.1);
    (void)episode.sample(0.1, 0.2f, 1, 2, 1, 1);
    const auto air = episode.sample(0.2, 0.2f, 1, 2, 1, 1);
    const auto original_id = episode.id;
    episode.observe(0.21, MotionState::rail);
    const auto release = episode.sample(0.25, 0.2f, 1, 2, 1, 1);
    check(episode.phase == PlaybackPhase::rail_release && near(release.time, air.time) && release.weight < air.weight,
          "Rail entry releases to native constraints instead of advancing/holding a fake airborne grind");
    check(episode.sample(0.3, 0.2f, 1, 2, 1, 1).finished, "Rail release is bounded");
    episode.observe(0.31, MotionState::rail);
    check(episode.observe(0.4, MotionState::air) && episode.entry == EntryKind::rail && episode.id > original_id,
          "A grind pop-out is a fresh rail-entry episode");
    check(episode.sample(0.4, 0.2f, 1, 2, 1, 1).time < 0, "A new pop-out cannot inherit the entry preset or source clock");
    episode.observe(0.5, MotionState::unknown);
    check(episode.phase == PlaybackPhase::idle, "Unavailable native state cancels pending edits");
    episode.observe(1, MotionState::riding); episode.observe(1.1, MotionState::air); episode.select(1.1);
    (void)episode.sample(1.15, 0.2f, 1, 2, 1, 1);
    episode.observe(1.2, MotionState::riding); episode.observe(1.21, MotionState::air);
    check(episode.phase == PlaybackPhase::air, "A brief contact does not become a landing");
    episode.observe(1.3, MotionState::riding); episode.observe(1.334, MotionState::riding);
    check(episode.phase == PlaybackPhase::landing && episode.landing == 1.3, "Landing confirms elapsed contact while retaining first-contact time");
    episode.observe(1.35, MotionState::offboard);
    check(episode.phase == PlaybackPhase::idle && episode.clock.time < 0, "Bails and off-board states release the episode");

    PresentationLatch<int> cohort;
    auto first = std::make_shared<const int>(1), second = std::make_shared<const int>(2);
    check(*cohort.get(10, 1, first) == 1 && *cohort.get(10, 1, second) == 1,
          "Body and board in one presentation pin the same schedule despite a producer update");
    check(*cohort.get(11, 1, second) == 2 && !cohort.get(11, 2, {}), "The next presentation advances; invalidation releases an already pinned schedule");
    {
    struct Preview { Joint foot, board; };
    auto preview_before = std::make_shared<Preview>(), preview_after = std::make_shared<Preview>();
    preview_before->foot.position = {0, 0.1f, 0};
    preview_after->foot.position = {0, 0.8f, 0}; preview_after->board.position = {0, 0.7f, 0};
    PresentationLatch<Preview> preview;
    const auto body = preview.get(1, 0, preview_before);
    const auto board_sample = preview.get(1, 0, preview_after);
    check(body == board_sample && near(body->foot.position[1] - board_sample->board.position[1], 0.1f),
          "Preview body and board retain contact when preview_before producer publishes between their callbacks");
    const auto board_first = preview.get(2, 0, preview_after);
    check(board_first == preview.get(2, 0, preview_before) && near(board_first->foot.position[1] - board_first->board.position[1], 0.1f),
          "Preview sampling is coherent when board renders first and on repeated publications");
    check(!preview.get(2, 1, {}), "Closing or invalidating preview releases its current presentation");
    PresentationLatch<Preview> no_sample;
    check(!no_sample.get(3, 0, {}) && !no_sample.get(3, 0, preview_before),
          "A missing first preview sample cannot publish only one half later in the same presentation");
    }

    std::vector<Joint> native(skeleton_joints), source(skeleton_joints);
    native[7].position = {1, 2, 3}; native[7].scale = {1.2f, 0.8f, 1};
    source[7].position = {90, 80, 70}; source[7].rotation = from_euler({90, 30, 80});
    Animation keys; keys.tracks.push_back({7, {{0, {}, {}, Ease::linear}}, false, false});
    auto fresh = native;
    apply_native_edit_layer(keys, 0, source, fresh, false, 1);
    check(fresh == native, "An untouched source preset preserves the complete native pose exactly, regardless of its recorded coordinates");
    Joint board; board.position = {80, 60, 40}; board.rotation = from_euler({0, 70, 0});
    keys.tracks.push_back({board_joint, {{0, {}, {}, Ease::linear}}, false, false});
    check(board_edited(board, keys, 0, from_euler({0, 90, 0})) == board, "Neutral board keys preserve native physics exactly");
    keys.tracks[1].keys[0].offset = {0.2f, 0, 0};
    const auto board_edit = board_edited(board, keys, 0, from_euler({0, 90, 0}));
    check(near(board_edit.position[2], board.position[2] - 0.2f) && board_edit.rotation == board.rotation && board_edit.scale == board.scale,
          "A board offset uses its explicit edit frame and does not import source coordinates or modify rotation/scale");
    keys.tracks[0].keys[0].offset = {0.2f, 0, 0};
    apply_native_edit_layer(keys, 0, source, fresh, false, 0.5f);
    check(fresh[7].rotation == native[7].rotation, "A translation-only edit leaves native joint rotation exactly unchanged");
    check(fresh[7].scale == native[7].scale && fresh[placement_joint] == native[placement_joint] && fresh[50] == native[50],
          "Authored edits retain native scale, placement and attachment ownership");
    EditPublicationCache cache;
    cache.commit(1, 10, 100, native, fresh);
    auto repeated = fresh; cache.restore(1, 10, 100, repeated);
    check(repeated == native, "Repeated publication unwraps our prior writes before reapplying keys");
    apply_native_edit_layer(keys, 0, source, repeated, false, 0.5f);
    check(repeated == fresh, "Network capture of an already edited render pose applies the keys exactly once");
    const auto deltas = pose_edit_deltas(native, fresh);
    auto moved_native = native; moved_native[7].position[1] += 20;
    apply_pose_deltas(deltas, moved_native);
    check(near(moved_native[7].position[1], fresh[7].position[1] + 20),
          "Blended deltas follow a moving native pose without teleporting to an old absolute skeleton");
    auto changed_native = fresh; changed_native[7].position[1] += 1;
    const auto fresh_y = changed_native[7].position[1];
    cache.restore(1, 10, 100, changed_native);
    check(changed_native[7].position[1] == fresh_y && changed_native[7].position[0] == native[7].position[0],
          "A fresh Y correction survives while our exact previous X edit is unwrapped to avoid accumulation");
    repeated = fresh; cache.restore(1, 11, 100, repeated);
    check(repeated == fresh, "A new animation callback cannot reuse an earlier update's baseline");
    repeated = fresh; cache.restore(2, 10, 100, repeated);
    check(repeated == fresh, "Entity replacement invalidates repeated-publication recovery");

    Animation metadata; metadata.source = {"native_asset", "animation/dingo/test", "rig", "skater-placement"}; metadata.entry = "rail";
    metadata.source.position_codec = "native-piec-v1-extent";
    const auto back = from_json(to_json(metadata));
    check(back.source.kind == metadata.source.kind && back.source.asset == metadata.source.asset &&
          back.source.position_codec == metadata.source.position_codec && back.entry == "rail",
          "Source provenance and entry bindings survive save/load without becoming rendered captures");
    std::vector<Preset> presets{{"Ground", "ollie", "", false, false, "regular", false, "ground"},
                                {"Rail", "ollie", "", false, false, "regular", false, "rail"}};
    select_presets(presets, {});
    check(presets[0].active && presets[1].active, "Fixed selection preserves separate ground and rail-exit presets");
}
int main(int argc, char** argv) {
    try {
        timing_and_stance_edge_cases();
        merged_library_and_timing();
        preview_trajectory_uses_the_whole_clip();
        native_playback_contracts();
        diagnostic_history_is_bounded();
        retargeted_edits_preserve_native_rig_and_convert_joint_frames();
        switch_edits_follow_the_native_body_facing();
        authored_gameplay_matches_f8_limbs_and_source_key_times();
        packs_select_variants_without_filesystem_order();
        research_keeps_short_transitions_and_is_bounded();
        euler_numbers_read_back();
        native_names_and_search();
        manual_and_scripted_recording_keep_distinct_rules();
        mirror_pairs_and_comes_back();
        dragged_foot_reaches_the_drag();
        turn_keeps_its_speed_through_a_key();
        smoothing_tidies_a_track();
        solo_turn_holds_the_joints_below();
        animation_targets_keep_their_motion_and_hierarchy();
        slerp_meets_both_ends();
        keys_hold_outside_and_blend_between();
        keys_can_be_dragged_in_time();
        edits_are_on_top_of_the_pose_and_never_grow_it();
        joints_hang_from_their_parents();
        profiles_read_back_and_bad_ones_are_refused();
        clips_read_back_and_broken_ones_are_refused();
        if (argc == 3 && std::string_view(argv[1]) == "--diagnostic-export") {
            DiagnosticBuffer history;
            for (int i = 0; i < 3; ++i) {
                DiagnosticSample sample; sample.at = 100 + i * 0.1; sample.sequence = i; sample.physics = 200;
                sample.trick = "test,\"trick\""; sample.preset = "synthetic fixture";
                sample.live_switch = true; sample.recorded_switch = false; sample.retarget = sample.mirror = true;
                sample.native.resize(skeleton_joints); sample.desired = sample.native;
                sample.desired[7].position[0] = 0.1f;
                sample.desired[7].rotation = from_euler({30, 0, 0});
                sample.native_board = Joint{}; sample.desired_board = Joint{};
                sample.render_edited = sample.desired; sample.render_edited_at = sample.at;
                sample.render_edited_board = sample.desired_board; sample.render_edited_board_at = sample.at;
                sample.source = sample.native; sample.source_board = Joint{};
                sample.board_rig.resize(16); sample.render_board_at = sample.board_rig_at = sample.at;
                sample.observed_phase = "air"; sample.entry = "ground"; sample.source_kind = "native_asset";
                sample.episode = 1; sample.render_edit_sequence = sample.board_edit_sequence = i + 1;
                sample.render_presentation = sample.board_presentation = i + 10;
                history.append(std::move(sample));
            }
            auto summary = write_diagnostic_history(history, argv[2]);
            check(summary.at("samples").get<std::size_t>() == 3 &&
                  near(summary.at("max_local_position_delta").get<float>(), 0.1f) &&
                  near(summary.at("max_local_rotation_delta_degrees").get<float>(), 30.0f, 0.01f),
                  "Diagnostic text export measures known pose differences");
            summary["synthetic_fixture"] = true;
            const auto contents = [&](const char* file) {
                std::ifstream in(std::filesystem::path(argv[2]) / file);
                return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            check(contents("poses.csv").find(",source_sample,7,") != std::string::npos &&
                  contents("board.csv").find(",source_sample,900,") != std::string::npos &&
                  contents("board-rig.csv").find(",render_before_override,15,") != std::string::npos,
                  "Preview diagnostics export source poses and all sixteen physical board bones");
            std::ofstream out(std::filesystem::path(argv[2]) / "summary.json"); out << summary.dump(2);
            std::cout << "Synthetic diagnostic text export passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--animation-skeleton") {
            std::ifstream in(argv[2]);
            const auto document = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            const auto& joints = document.at("instances")[0].at("object").at("fields").at("Joints");
            check(joints.size() == skeleton_joints, "Native animation skeleton has the supported joint count");
            for (std::size_t i = 0; i < joints.size(); ++i)
                check(parent(static_cast<std::uint16_t>(i)) == joints[i].at("fields").at("Parent").get<int>(),
                      "Every studio parent must match the actual native AntSkeletonAsset");
            std::cout << "Native animation hierarchy: all 395 parents match\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-catalog") {
            const std::filesystem::path folder(argv[2]);
            std::ifstream in(folder / "catalog.json", std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            check(root.is_array() && root.size() > 0, "Exported native catalog must be nonempty");
            for (const auto& entry : root) {
                const auto name = entry.value("file", "");
                std::ifstream binary(folder / name, std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
                const auto clip = clip_from_bytes(bytes);
                check(clip.joints == skeleton_joints && !clip.empty(), "Native library clip must load on the skater rig");
                if (entry.value("preview_only", false))
                    check(clip.takeoff == -1 && clip.landing == -1, "Preview-only grind clips must not invent airborne markers");
                for (const auto& joint : clip.frames) check(finite(joint), "Native catalog clip has a broken joint");
            }
            std::cout << "Native catalog compatibility: " << root.size() << " clips passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-vector-coherence") {
            const std::filesystem::path folder(argv[2]);
            std::ifstream in(folder / "catalog.json");
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
            float maximum = 0;
            std::string worst;
            for (const auto& entry : root) {
                std::ifstream file(folder / entry.value("file", ""), std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
                const auto clip = clip_from_bytes(bytes);
                for (std::size_t f = 0; f < clip.times.size(); ++f) {
                    const auto pose = clip.frame(f);
                    for (const auto pair : {std::pair{343, 376}, std::pair{10, 377}, std::pair{343, 390}, std::pair{10, 391}}) {
                        const auto foot = world(pose, static_cast<std::uint16_t>(pair.first));
                        const auto target = world(pose, static_cast<std::uint16_t>(pair.second));
                        float square = 0;
                        for (std::size_t axis = 0; axis < 3; ++axis) square += std::pow(foot.position[axis]-target.position[axis], 2.0f);
                        const float distance = std::sqrt(square);
                        if (distance > maximum) { maximum = distance; worst = entry.value("file", ""); }
                    }
                }
            }
            std::cout << "Native FK/ankle-target maximum separation: " << maximum << " m in " << worst << '\n';
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-index") {
            std::ifstream in(argv[2], std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), dingosdk::JsonLimits{32 * 1024 * 1024, 48, 2 * 1024 * 1024});
            check(root.is_array() && root.size() > 0, "Native index must fit production JSON parser limits");
            for (const auto& entry : root)
                check((entry.value("asset", "").starts_with("animation/dingo/") || entry.value("asset", "").starts_with("_af/")) && !entry.value("category", "").empty(), "Indexed native asset requires category and stable source path");
            std::cout << "Native index compatibility: " << root.size() << " assets passed\n";
        }
        if (argc == 4 && std::string_view(argv[1]) == "--native-labels") {
            std::ifstream in(argv[2], std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), dingosdk::JsonLimits{32 * 1024 * 1024, 48, 2 * 1024 * 1024});
            check(root.is_array() && root.size() > 0, "Readable-name validation requires the actual native index");
            auto report = dingosdk::Json::array();
            std::vector<NamedNativeFixture> names;
            for (const auto& entry : root) {
                const auto asset = entry.value("asset", ""), type = entry.value("root_type", "");
                const auto name = native_asset_name(asset, type, entry.value("category", ""));
                const auto folder = native_asset_folder(asset, type, entry.value("category", ""));
                check(!name.label.empty() && !folder.empty() && !folder.starts_with('/') && !folder.ends_with('/') && folder.find("//") == std::string::npos,
                      "Every native asset needs a readable label and a valid folder");
                check(native_search_matches(name.search, native_search_terms(asset)), "Exact original names remain searchable");
                names.push_back({asset, name.label, folder, name.context, name.search, entry.value("state", "")});
            }
            native_disambiguate_names(names);
            for (const auto& name : names) {
                dingosdk::Json row = dingosdk::Json::object();
                row["asset"] = name.asset; row["label"] = name.label; row["category"] = name.category;
                row["context"] = name.context; row["state"] = name.state;
                report.push_back(std::move(row));
            }
            std::ofstream out(argv[3], std::ios::binary); out << report.dump(2); out.close();
            check(out.good(), "Readable inventory report must be written successfully");
            std::cout << "Native readable names and folders: " << root.size() << " assets passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-documents") {
            std::size_t count = 0;
            for (const auto& directory : {"native-documents", "native-resources"}) {
                for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(argv[2]) / directory)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                    std::ifstream in(entry.path(), std::ios::binary);
                    const auto document = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), dingosdk::JsonLimits{16 * 1024 * 1024, 64, 2 * 1024 * 1024});
                    check(document.is_object() && document.contains("asset") && document.at("asset").is_string() &&
                          document.contains("root_type") && document.at("root_type").is_string(), "Native inspector documents must fit the production reader and retain source identity/type");
                    ++count;
                }
            }
            check(count > 0, "Native inspector requires actual documents");
            std::cout << "Native inspector compatibility: " << count << " documents passed\n";
        }
        if (argc == 2) {
            std::size_t files = 0;
            for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                std::ifstream json(entry.path(), std::ios::binary);
                const auto animation = from_json(std::string((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>()));
                auto path = entry.path(); path.replace_extension(".clip");
                std::ifstream binary(path, std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
                const auto clip = clip_from_bytes(bytes);
                for (const auto& t : animation.tracks) for (const auto& k : t.keys)
                    check(k.time <= clip.duration() + 0.01f, "Saved user key is outside the actual clip");
                std::vector<Joint> sampled(clip.joints);
                for (float time = 0; time <= clip.duration(); time += 1.0f / 60) {
                    clip.sample(time, sampled);
                    apply_edits(animation, time, sampled);
                    for (const auto& joint : sampled) check(finite(joint), "Saved user animation produces a nonfinite joint");
                    if (clip.joints == skeleton_joints) {
                        std::vector<Joint> source(clip.joints), native(clip.joints);
                        clip.sample(time, source);
                        for (auto& joint : native) joint.rotation = from_euler({11, -29, 31});
                        retarget_edits(animation, time, source, native, true);
                        for (const auto& joint : native) check(finite(joint), "Saved user edits produce finite retargeted switch joints");
                    }
                }
                ++files;
            }
            std::cout << "Saved animation compatibility: " << files << " files passed\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "anim_core: all passed\n";
    return 0;
}
