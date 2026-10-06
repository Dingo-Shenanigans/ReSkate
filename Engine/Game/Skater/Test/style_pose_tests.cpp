// Style layer maths: rotations compose as expected and a pose the game left alone is not adjusted twice.
#include "Engine/Game/Skater/style_pose.h"
#include <iostream>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
bool near(const Quat &a, const Quat &b) {
    for (std::size_t i = 0; i < 4; ++i)
        if (std::abs(a[i] - b[i]) > 1e-5f) return false;
    return true;
}
} // namespace

int main() {
    check(near(from_degrees(0, 0, 0), {0, 0, 0, 1}), "no angles give the identity");
    check(near(from_degrees(90, 0, 0), {0.70710678f, 0, 0, 0.70710678f}), "90 degrees about X");
    check(near(from_degrees(500, 0, 0), from_degrees(max_degrees, 0, 0)), "angles are clamped");
    for (const auto &angles : {std::array<float, 3>{30, 0, 0}, std::array<float, 3>{0, -70, 0}, std::array<float, 3>{0, 0, 110},
                               std::array<float, 3>{25, -40, 60}, std::array<float, 3>{-100, 15, -80}}) {
        const auto back = to_degrees(from_degrees(angles[0], angles[1], angles[2]));
        check(std::abs(back[0] - angles[0]) < 0.01f && std::abs(back[1] - angles[1]) < 0.01f && std::abs(back[2] - angles[2]) < 0.01f,
              "angles come back from the rotation they make");
    }
    const auto halfway = mix(from_degrees(40, 20, -30), from_degrees(-10, 60, 80), 0.5f);
    const auto as_angles = to_degrees(halfway);
    check(near(from_degrees(as_angles[0], as_angles[1], as_angles[2]), halfway), "a blend of two rotations has angles that make it again");
    check(near(from_degrees(std::nanf(""), 0, 0), {0, 0, 0, 1}), "a non-finite angle is ignored");
    check(near(normalized({0, 0, 0, 0}), {0, 0, 0, 1}), "a zero quaternion normalizes to the identity");

    const Quat game{0, 0.38268343f, 0, 0.92387953f}, delta = from_degrees(30, 0, 0);
    std::array<Quat, 4> pose{};
    pose.fill(game);
    PoseTracker tracker(pose.size());
    const auto pass = [&](std::initializer_list<std::uint16_t> joints) {
        unsigned reused{};
        tracker.begin();
        for (const auto joint : joints) {
            bool ours{};
            pose[joint] = tracker.adjust(joint, pose[joint], delta, &ours);
            reused += ours;
        }
        tracker.end([&](std::uint16_t joint) { return pose[joint]; },
                    [&](std::uint16_t joint, const Quat &base) { pose[joint] = base; });
        return reused;
    };
    const auto expected = normalized(multiply(game, delta));
    check(pass({1, 2}) == 0 && near(pose[1], expected), "a fresh pose is adjusted from the game's rotation");
    // The game did not rewrite the pose: the same result, not the delta applied twice.
    check(pass({1, 2}) == 2 && near(pose[1], expected), "a pose left alone is not adjusted twice");
    check(tracker.base(1) && near(*tracker.base(1), game), "the game's rotation is remembered");
    check(tracker.base(1, pose[1]) && !tracker.base(1, game), "but only offered while the pose still holds what was written");
    // The game evaluated a new pose.
    const Quat moved{0.5f, 0.5f, 0.5f, 0.5f};
    pose[1] = moved;
    check(pass({1, 2}) == 1 && near(pose[1], normalized(multiply(moved, delta))), "a new pose becomes the new base");
    // Joint 2 is no longer adjusted and the game has not rewritten it: the tracker restores its rotation.
    check(pass({1}) == 1 && near(pose[2], game) && !tracker.base(2), "a released joint gets the game's rotation back");
    check(tracker.adjusted().size() == 1, "only the adjusted joint stays tracked");
    // Released after the game rewrote it: left alone.
    pose[1] = moved;
    check(pass({}) == 0 && near(pose[1], moved) && tracker.adjusted().empty(), "a rewritten joint is not restored");
    check(parse_target("Riding") == Target{false, 0} && parse_target("offboard") == Target{false, 2}, "state families parse");
    check(parse_target("kickflip") == Target{true, 2} && parse_target("FSPopShuvit") == Target{true, 7} &&
              parse_target("nollie") == Target{true, 16},
          "flip tricks parse to the game's numbering");
    check(!parse_target("none") && !parse_target("spin"), "unknown targets are refused");
    // Blending: a rotation eases in, and eases to identity when it is no longer wanted.
    Blender blender(4);
    const std::vector<JointDelta> wanted{{1, from_degrees(90, 0, 0)}};
    const auto &first = blender.step(wanted, 0.5f);
    check(first.size() == 1 && first[0].rotation[3] > from_degrees(90, 0, 0)[3] && first[0].rotation[3] < 1,
          "a rotation starts part of the way in");
    for (int i = 0; i < 40; ++i) (void)blender.step(wanted, 0.5f);
    check(near(blender.step(wanted, 0.5f)[0].rotation, from_degrees(90, 0, 0)), "and arrives");
    check(blender.step({}, 0.5f).size() == 1, "a dropped rotation eases out rather than vanishing");
    for (int i = 0; i < 60; ++i) (void)blender.step({}, 0.5f);
    check(blender.step({}, 0.5f).empty(), "and is released once it is back");
    check(ease_amount(0.016f, 0.15f) > 0.2f && ease_amount(0.016f, 0.15f) < 0.4f, "easing follows the frame time");
    // A flip trick's timeline, from the game's signals.
    TrickTracker trick;
    check(trick.step(-1, true, true, true, 0).trick == 0, "no trick while riding");
    check(trick.step(2, true, true, true, 1000).time == 0 && trick.step(2, false, true, true, 1150).time == 0.5f, "the flick starts the timeline");
    check(trick.step(2, false, true, true, 5000).time < 1, "the pop waits for the catch however long it takes");
    check(trick.step(-1, false, true, true, 5100).time == 1, "the game dropping the trick in the air is the catch");
    check(trick.step(0, false, true, true, 5225).time == 1.5f, "the fall runs to the ground");
    const auto landed = trick.step(-1, true, true, true, 5300);
    check(landed.trick == 2 && landed.time == 2, "touching down starts the landing");
    check(trick.step(-1, true, true, true, 5300 + TrickTracker::landing_ms).time == 3, "which runs to the end of the timeline");
    check(trick.step(-1, true, true, true, 5301 + TrickTracker::landing_ms).trick == 0, "and then the trick is over");
    // The pop took 1500 ms (the maximum) and the fall 200 ms. The next trick uses these durations.
    check(trick.step(2, false, true, true, 10000).time == 0 && trick.step(2, false, true, true, 10750).time == 0.5f, "the last pop sets the pace");
    check(trick.step(-1, false, true, true, 10800).time == 1 && trick.step(-1, false, true, true, 10900).time == 1.5f, "and so does the last fall");
    check(trick.step(-1, false, false, true, 10950).trick == 0, "a slam ends the trick at once");
    check(trick.step(99, true, true, true, 12000).trick == 0, "a number that is not a flip trick is ignored");
    // Flipping into a grind: the grind is the landing, and the trick then ends.
    TrickTracker into;
    (void)into.step(2, false, true, true, 0);
    (void)into.step(-1, false, true, true, 200);
    check(into.step(-1, false, true, false, 400).time == 2, "landing in a grind is the touchdown");
    check(into.step(-1, false, true, false, 401 + TrickTracker::landing_ms).trick == 0, "and the trick ends while still grinding");
    TrickTracker stuck;
    (void)stuck.step(2, false, true, true, 0);
    check(stuck.step(2, false, true, true, TrickTracker::longest_ms + 1).trick == 0, "a trick the game never ends is given up on");
    check(ease_amount(0, 0.15f) == 0, "no time passed moves nothing");
    // Keyframes: the pose goes from the game's pose through each keyframe and back.
    const Quat bent = from_degrees(90, 0, 0), turned = from_degrees(0, 60, 0);
    const std::vector<Key> keys{{1.0f, {{5, bent}}}, {2.0f, {{5, turned}, {6, bent}}}};
    std::vector<JointDelta> at;
    evaluate(keys, 0.0f, at);
    check(at.size() == 1 && near(at[0].rotation, identity), "the timeline starts at the game's pose");
    evaluate(keys, 0.5f, at);
    check(at.size() == 1 && near(at[0].rotation, mix(identity, bent, 0.5f)), "and moves toward the first keyframe");
    evaluate(keys, 1.0f, at);
    check(near(at[0].rotation, bent), "reaching it at its time");
    evaluate(keys, 1.5f, at);
    check(at.size() == 2 && at[1].joint == 6 && near(at[1].rotation, mix(identity, bent, 0.5f)), "between keyframes each joint moves from one to the next");
    evaluate(keys, 2.0f, at);
    check(at.size() == 2 && near(at[0].rotation, turned) && near(at[1].rotation, bent), "and reaches the next keyframe at its time");
    evaluate(keys, 2.5f, at);
    check(at.size() == 2 && near(at[0].rotation, mix(turned, identity, 0.5f)), "after the last keyframe it returns to the game's pose");
    evaluate({}, 1.0f, at);
    check(at.empty(), "a trick with no keyframes changes nothing");
    // The angle of joint 5 about X, at a timeline time.
    const auto angle = [](const std::vector<Key> &keyframes, float time, const Pace &pace) {
        std::vector<JointDelta> rotations;
        evaluate(keyframes, time, rotations, pace);
        return rotations.empty() ? 0.0f : 2 * std::atan2(rotations[0].rotation[0], rotations[0].rotation[3]) * 57.29578f;
    };
    // A joint that keeps turning one way goes through a keyframe with no corner: its speed is the same on both sides.
    const std::vector<Key> onward{{1.0f, {{5, from_degrees(30, 0, 0)}}}, {2.0f, {{5, from_degrees(60, 0, 0)}}}, {2.9f, {{5, from_degrees(70, 0, 0)}}}};
    for (const auto &pace : {even_pace, Pace{440, 290, 450}}) {
        const float step = 0.002f, scale_before = pace[0], scale_after = pace[1];
        const float before = (angle(onward, 1.0f, pace) - angle(onward, 1.0f - step, pace)) / (step * scale_before);
        const float after = (angle(onward, 1.0f + step, pace) - angle(onward, 1.0f, pace)) / (step * scale_after);
        check(before > 0 && std::abs(before - after) < 0.05f * before, "a joint goes through a keyframe at one speed, also when the parts have different lengths");
    }
    // A joint that turns back at a keyframe slows to a stop there, and does not go past it.
    const float peak = angle(keys, 1.0f, even_pace);
    check(std::abs(angle(keys, 0.99f, even_pace) - peak) < 0.05f && angle(keys, 1.01f, even_pace) <= peak + 1e-3f, "a joint that turns back stops at the keyframe");
    for (float time = 0; time <= trick_end; time += 0.01f)
        if (angle(onward, time, Pace{440, 290, 450}) > 70.001f) {
            check(false, "the curve never goes past a keyframe");
            break;
        }
    // Takes: a replayed frame is recognised by its pose, and restyled from what was shown.
    Takes takes;
    const auto shown_at = [](float degrees) {
        Signature signature;
        signature.fill(from_degrees(degrees, degrees * 0.5f, 0));
        return signature;
    };
    for (int i = 0; i < 60; ++i) takes.add({2, static_cast<float>(i) * 0.05f, shown_at(static_cast<float>(i)), {{5, bent}}});
    float distance{};
    const auto *found = takes.find(shown_at(30.2f), &distance);
    check(found && found->trick == 2 && found->time == 1.5f && distance < Takes::tolerance, "a replayed pose finds the frame that showed it");
    check(takes.find(shown_at(3.0f)) && takes.find(shown_at(3.0f))->time == 0.15f, "also after a jump to another part of the replay");
    check(!takes.find(shown_at(-70.0f)), "a pose no recorded frame showed is not matched");
    int calls{};
    while (calls < 8 && !takes.find(shown_at(55.0f))) ++calls;
    check(calls < 8, "after a miss, a replay far from the last match is still found within 8 calls");
    std::vector<JointDelta> change;
    restyle({{5, bent}}, {{5, turned}, {6, bent}}, change);
    check(change.size() == 2 && near(normalized(multiply(bent, change[0].rotation)), turned) && change[1].joint == 6 && near(change[1].rotation, bent),
          "restyling takes the old rotation out and puts the new one in");
    restyle({{5, bent}}, {{5, bent}}, change);
    check(near(change[0].rotation, identity), "an unchanged style leaves a replayed frame as it was");
    restyle({{5, bent}}, {}, change);
    check(near(normalized(multiply(bent, change[0].rotation)), identity), "a rotation since removed is taken out");
    check(preset_name("street_2-a") && !preset_name("") && !preset_name(std::string(41, 'a')) && !preset_name("a b") && !preset_name("..") &&
              !preset_name("caf\xc3\xa9"),
          "a preset name is ASCII letters, digits, '-' and '_'");
    check(!preset_name("CON") && !preset_name("nul") && !preset_name("Com1") && !preset_name("LPT9") && preset_name("console") && preset_name("com10"),
          "and not a Windows device name");
    check(same_text("Street", "street") && !same_text("street", "streets"), "two names that differ only in case name the same file");
    if (!failures) std::cout << "style pose tests passed\n";
    return failures ? 1 : 0;
}
