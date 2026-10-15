#include "Extension/Style/style_pose.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
                               std::array<float, 3>{25, -40, 60}, std::array<float, 3>{-100, 15, -80}, std::array<float, 3>{0, 100, 0},
                               std::array<float, 3>{20, 110, -30}, std::array<float, 3>{60, -105, 10}}) {
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
        tracker.begin();
        for (const auto joint : joints) pose[joint] = tracker.adjust(joint, pose[joint], delta);
        tracker.end([&](std::uint16_t joint) -> std::optional<Quat> { return pose[joint]; },
                    [&](std::uint16_t joint, const Quat &base) { pose[joint] = base; });
    };
    const auto expected = normalized(multiply(game, delta));
    pass({1, 2});
    check(near(pose[1], expected), "a fresh pose is adjusted from the game's rotation");
    pass({1, 2});
    check(near(pose[1], expected), "a pose left alone is not adjusted twice");
    check(tracker.base(1, pose[1]) && near(*tracker.base(1, pose[1]), game), "the game's rotation is remembered");
    check(tracker.base(1, pose[1]) && !tracker.base(1, game), "but only offered while the pose still holds what was written");
    const Quat moved{0.5f, 0.5f, 0.5f, 0.5f};
    pose[1] = moved;
    pass({1, 2});
    check(near(pose[1], normalized(multiply(moved, delta))), "a new pose becomes the new base");
    pass({1});
    check(near(pose[2], game) && !tracker.base(2, pose[2]), "a released joint gets the game's rotation back");
    check(tracker.adjusted().size() == 1, "only the adjusted joint stays tracked");
    pose[1] = moved;
    pass({});
    check(near(pose[1], moved) && tracker.adjusted().empty(), "a rewritten joint is not restored");
    {
        PoseTracker lost(4);
        std::array<Quat, 4> held{};
        held.fill(game);
        lost.begin();
        held[1] = lost.adjust(1, held[1], delta);
        held[2] = lost.adjust(2, held[2], delta);
        lost.begin();
        unsigned restored{};
        lost.end([&](std::uint16_t joint) -> std::optional<Quat> { return joint == 1 ? std::nullopt : std::optional(held[joint]); },
                 [&](std::uint16_t joint, const Quat &base) {
                     held[joint] = base;
                     ++restored;
                 });
        check(restored == 1 && near(held[2], game) && lost.adjusted().empty(), "an unreadable joint is dropped, a readable one restored");
        held[3] = lost.adjust(3, game, delta);
        lost.clear();
        check(lost.adjusted().empty() && !lost.base(3, held[3]), "clear forgets every adjusted joint");
    }
    Blender blender(4);
    const std::vector<SkeletonDelta> wanted{{1, from_degrees(90, 0, 0)}};
    const auto &first = blender.step(wanted, 0.5f);
    check(first.size() == 1 && first[0].rotation[3] > from_degrees(90, 0, 0)[3] && first[0].rotation[3] < 1,
          "a rotation starts part of the way in");
    for (int i = 0; i < 40; ++i) (void)blender.step(wanted, 0.5f);
    check(near(blender.step(wanted, 0.5f)[0].rotation, from_degrees(90, 0, 0)), "and arrives");
    check(blender.step({}, 0.5f).size() == 1, "a dropped rotation eases out rather than vanishing");
    for (int i = 0; i < 60; ++i) (void)blender.step({}, 0.5f);
    check(blender.step({}, 0.5f).empty(), "and is released once it is back");
    check(ease_amount(0.016f, 0.15f) > 0.2f && ease_amount(0.016f, 0.15f) < 0.4f, "easing follows the frame time");
    TrickTracker trick;
    check(trick.step(-1, true, true, true, 0).trick == 0, "no trick while riding");
    check(trick.step(2, true, true, true, 1000).time == 0 && trick.step(2, false, true, true, 1150).time == 0.5f,
          "the flick starts the timeline");
    check(trick.step(2, false, true, true, 5000).time < 1, "the pop waits for the catch however long it takes");
    check(trick.step(-1, false, true, true, 5100).time == 1, "the game dropping the trick in the air is the catch");
    check(trick.step(0, false, true, true, 5225).time == 1.5f, "the fall runs to the ground");
    const auto landed = trick.step(-1, true, true, true, 5300);
    check(landed.trick == 2 && landed.time == 2, "touching down starts the landing");
    check(trick.step(-1, true, true, true, 5300 + TrickTracker::landing_ms).time == 3, "which runs to the end of the timeline");
    check(trick.step(-1, true, true, true, 5301 + TrickTracker::landing_ms).trick == 0, "and then the trick is over");
    // The pop took 1500 ms (the maximum) and the fall 200 ms. The next trick uses these durations.
    check(trick.step(2, false, true, true, 10000).time == 0 && trick.step(2, false, true, true, 10750).time == 0.5f,
          "the last pop sets the pace");
    check(trick.step(-1, false, true, true, 10800).time == 1 && trick.step(-1, false, true, true, 10900).time == 1.5f,
          "and so does the last fall");
    check(trick.step(-1, false, false, true, 10950).trick == 0, "a slam ends the trick at once");
    check(trick.step(99, true, true, true, 12000).trick == 0, "a number that is not a flip trick is ignored");
    TrickTracker into;
    (void)into.step(2, false, true, true, 0);
    (void)into.step(-1, false, true, true, 200);
    check(into.step(-1, false, true, false, 400).time == 2, "landing in a grind is the touchdown");
    check(into.step(-1, false, true, false, 401 + TrickTracker::landing_ms).trick == 0, "and the trick ends while still grinding");
    TrickTracker stuck;
    (void)stuck.step(2, false, true, true, 0);
    check(stuck.step(2, false, true, true, TrickTracker::longest_ms + 1).trick == 0, "a trick the game never ends is given up on");
    check(ease_amount(0, 0.15f) == 0, "no time passed moves nothing");
    const Quat bent = from_degrees(90, 0, 0), turned = from_degrees(0, 60, 0);
    const Pace measured{440, 290, 450};
    const std::vector<Key> keys{{1.0f, {{5, bent}}}, {2.0f, {{5, turned}, {6, bent}}}}, caught{{1.0f, {{5, bent}}}};
    const std::vector<Key> hit{{1.0f, {{5, bent}}, 100}, {2.0f, {{5, bent}}}}, last{{1.0f, {{5, bent}}, 100}},
        late{{2.9f, {{5, bent}}, max_blend_out_ms}};
    const float released = 2.0f + release_ms / even_pace[2], out_at = 1.0f + 100 / even_pace[1];
    // One joint at a timeline time: at `expected` when `same`, else not at it; `count` joints have a rotation (0: any number).
    struct Moment {
        const std::vector<Key> &keys;
        float time;
        Pace pace;
        std::uint8_t joint;
        Quat expected;
        bool same;
        std::size_t count;
        const char *what;
    };
    const Moment moments[]{
        {keys, 0.0f, even_pace, 5, identity, true, 1, "the timeline starts at the game's pose"},
        {keys, 0.5f, even_pace, 5, mix(identity, bent, 0.5f), true, 1, "and moves toward the first keyframe"},
        {keys, 1.0f, even_pace, 5, bent, true, 0, "reaching it at its time"},
        {keys, 1.5f, even_pace, 6, mix(identity, bent, 0.5f), true, 2, "between keyframes each joint moves from one to the next"},
        {keys, 2.0f, even_pace, 5, turned, true, 2, "and reaches the next keyframe at its time"},
        {keys, 2.0f, even_pace, 6, bent, true, 2, "and reaches the next keyframe at its time"},
        {keys, (2.0f + released) * 0.5f, even_pace, 5, mix(turned, identity, 0.5f), true, 2,
         "after the last keyframe it returns to the game's pose"},
        {keys, released, even_pace, 5, identity, true, 2, "in release_ms"},
        {keys, released, even_pace, 6, identity, true, 2, "in release_ms"},
        {keys, 2.9f, even_pace, 5, identity, true, 0, "and stays there to the end of the landing"},
        {caught, 1.0f + (release_ms - 10) / measured[1], measured, 5, identity, false, 0, "the return takes release_ms in real time"},
        {caught, 1.0f + release_ms / measured[1], measured, 5, identity, true, 0, "also when the part has a different length"},
        {hit, (1.0f + out_at) * 0.5f, even_pace, 5, mix(bent, identity, 0.5f), true, 0, "a blend out leaves the keyframe"},
        {hit, out_at, even_pace, 5, identity, true, 0, "and is at the game's pose after its time"},
        {hit, (out_at + 2.0f) * 0.5f, even_pace, 5, mix(identity, bent, 0.5f), true, 0, "then the pose moves to the next keyframe"},
        {last, 1.0f + 90 / measured[1], measured, 5, identity, false, 0, "the last keyframe's blend out replaces release_ms"},
        {last, 1.0f + 100 / measured[1], measured, 5, identity, true, 0, "and is in real time"},
        {late, 2.95f, even_pace, 5, identity, false, 0, "a blend out longer than the trick"},
        {late, trick_end, even_pace, 5, identity, true, 0, "ends with the trick"},
    };
    std::vector<JointDelta> at;
    for (const auto &moment : moments) {
        evaluate(moment.keys, moment.time, at, moment.pace);
        const auto found = std::ranges::find(at, moment.joint, &JointDelta::joint);
        check(found != at.end() && near(found->rotation, moment.expected) == moment.same && (!moment.count || at.size() == moment.count),
              moment.what);
    }
    evaluate({}, 1.0f, at);
    check(at.empty(), "a trick with no keyframes changes nothing");
    const std::vector<Key> too_long{{1.0f, {{5, bent}}, 600}, {2.0f, {{5, turned}}}}, straight{{1.0f, {{5, bent}}}, {2.0f, {{5, turned}}}};
    std::vector<JointDelta> direct;
    for (const float time : {1.2f, 1.5f, 1.9f}) {
        evaluate(too_long, time, at);
        evaluate(straight, time, direct);
        check(near(at[0].rotation, direct[0].rotation), "a blend out that does not end before the next keyframe goes straight to it");
    }
    // The angle of joint 5 about X, at a timeline time.
    const auto angle = [](const std::vector<Key> &keyframes, float time, const Pace &pace) {
        std::vector<JointDelta> rotations;
        evaluate(keyframes, time, rotations, pace);
        return rotations.empty() ? 0.0f : 2 * std::atan2(rotations[0].rotation[0], rotations[0].rotation[3]) * 57.29578f;
    };
    const std::vector<Key> onward{
        {1.0f, {{5, from_degrees(30, 0, 0)}}}, {2.0f, {{5, from_degrees(60, 0, 0)}}}, {2.9f, {{5, from_degrees(70, 0, 0)}}}};
    for (const auto &pace : {even_pace, measured}) {
        const float step = 0.002f, scale_before = pace[0], scale_after = pace[1];
        const float before = (angle(onward, 1.0f, pace) - angle(onward, 1.0f - step, pace)) / (step * scale_before);
        const float after = (angle(onward, 1.0f + step, pace) - angle(onward, 1.0f, pace)) / (step * scale_after);
        check(before > 0 && std::abs(before - after) < 0.05f * before,
              "a joint goes through a keyframe at one speed, also when the parts have different lengths");
    }
    const float peak = angle(keys, 1.0f, even_pace);
    check(std::abs(angle(keys, 0.99f, even_pace) - peak) < 0.05f && angle(keys, 1.01f, even_pace) <= peak + 1e-3f,
          "a joint that turns back stops at the keyframe");
    for (float time = 0; time <= trick_end; time += 0.01f)
        if (angle(onward, time, measured) > 70.001f) {
            check(false, "the curve never goes past a keyframe");
            break;
        }
    check(preset_name("street_2-a") && !preset_name("") && !preset_name(std::string(41, 'a')) && !preset_name("a b") &&
              !preset_name("..") &&
              !preset_name("caf\xc3\xa9"),
          "a preset name is ASCII letters, digits, '-' and '_'");
    check(!preset_name("CON") && !preset_name("nul") && !preset_name("Com1") && !preset_name("LPT9") && preset_name("console") &&
              preset_name("com10"),
          "and not a Windows device name");
    Style styled;
    styled.times[2] = {0.5f, 2.0f};
    styled.rotations[{Target{true, 2, 0}, "Hips"}] = {60, 0, 0};
    styled.rotations[{Target{true, 2, 1}, "LeftArm"}] = {0, 40, 0};
    styled.blend_outs[Target{true, 2, 0}] = 120;
    const auto played = trick_keys(styled, 2);
    check(played.size() == 2 && played[0].time == 0.5f && played[0].blend_out_ms == 120 && played[1].joints.size() == 1,
          "a trick's keyframes are played with their joints and blend outs");
    Style other;
    other.times[2] = {0.5f};
    other.rotations[{Target{true, 3, 0}, "Hips"}] = {60, 0, 0};
    other.rotations[{Target{false, 2, 0}, "Hips"}] = {60, 0, 0};
    check(trick_keys(other, 2).size() == 1 && trick_keys(other, 2)[0].joints.empty(),
          "a rotation of another trick or of a family is not a joint of this trick's keyframe");
    Style one, two, three;
    two.times[2] = {1.0f};
    three.times[2] = {1.5f};
    History history;
    check(!history.undo(one) && !history.redo(one), "an empty history changes nothing");
    history.record(one, "add keyframe");
    history.record(two, "move keyframe");
    check(history.undo_name() && *history.undo_name() == "move keyframe", "the step to undo has the edit's name");
    const auto back = history.undo(three);
    check(back && *back == two && history.redo_name() && *history.redo_name() == "move keyframe",
          "undo gives the style before the last edit");
    check(history.redo(two) == three, "redo gives it back");
    (void)history.undo(three);
    history.record(two, "delete keyframe");
    check(!history.redo(one), "a new edit after an undo ends the redo steps");
    History grouped;
    grouped.group(true);
    grouped.record(one, "move keyframe");
    grouped.record(two, "move keyframe");
    grouped.group(false);
    grouped.record(three, "blend out");
    (void)grouped.undo(one);
    check(grouped.undo(three) == one && !grouped.undo(one), "the edits of one group are one step");
    History deep;
    for (std::size_t i = 0; i < History::depth + 5; ++i) deep.record(one, "edit");
    std::size_t steps{};
    while (deep.undo(one)) ++steps;
    check(steps == History::depth, "the history keeps a limited number of steps");
    history.clear();
    check(!history.undo(one) && !history.redo(one), "a cleared history has no steps");
    const std::pair<bool, const char *> lookups[]{
        {parse_target("Riding") == Target{false, 0} && parse_target("offboard") == Target{false, 2}, "state families parse"},
        {parse_target("kickflip") == Target{true, 2} && parse_target("FSPopShuvit") == Target{true, 7} &&
             parse_target("nollie") == Target{true, 16},
         "flip tricks parse to the game's numbering"},
        {!parse_target("none") && !parse_target("spin"), "unknown targets are refused"},
        {dingosdk::console::equal("Street", "street") && !dingosdk::console::equal("street", "streets"), "two names that differ only in case name the same file"},
        {!is_flip_trick(0) && is_flip_trick(1) && is_flip_trick(32) && !is_flip_trick(33) && !is_flip_trick(-1),
         "a flip trick is 1 to 32"},
        {std::ranges::distance(flip_trick_ids()) == 32 && *flip_trick_ids().begin() == 1, "the ids run from 1 to 32"},
        {parse_flip_trick("Kickflip") == 2 && parse_flip_trick("NOLLIEPOP") == 32 && !parse_flip_trick("none") &&
             !parse_flip_trick("riding"),
         "a trick name in any case gives its id"},
        {editable_joint("Hips") == 0 && editable_joint("leftarm") == 9 && !editable_joint("Reference") && !editable_joint(""),
         "a joint name in any case gives its editable index"},
        {sentence("the menu did not open") == "The menu did not open." && sentence("").empty(), "a reason becomes one sentence"},
        {sentence("the Kickflip was not captured: the skater did a Heelflip") ==
             "The Kickflip was not captured: the skater did a Heelflip.",
         "a reason inside a reason stays as it is"},
        {blend_out_ms(0, true) == release_ms && blend_out_ms(0, false) == 0 && blend_out_ms(120, false) == 120,
         "a keyframe blends out by its own time, the last by release_ms"},
        {part_names[static_cast<std::size_t>(Part::fall)] == "fall" && next_part(Part::pop) == Part::fall &&
             part_start(Part::landing) == 2,
         "the middle part is the fall"},
        {std::abs(dot(from_degrees(30, 0, 0), from_degrees(30, 0, 0)) - 1) < 1e-5f && near(conjugate(Quat{1, 2, 3, 4}), {-1, -2, -3, 4}),
         "the dot product and the conjugate"},
    };
    for (const auto &[ok, what] : lookups) check(ok, what);
    check(sentence("Recorded.") == "Recorded." && sentence("3 tricks") == "3 tricks.", "no second full stop, and a digit stays");
    const std::vector<float> close_keys{1.01f, 1.0f};
    // An order that is not a reversal, so a place in time is not an ID at a place.
    const std::vector<float> rotated{2.0f, 0.5f, 1.0f}, tied{1.0f, 1.0f};
    check(key_place(rotated, 0) == 2 && key_place(rotated, 1) == 0 && key_place(rotated, 2) == 1 && key_place(tied, 0) == 0 &&
              key_place(tied, 1) == 1 && key_place(close_keys, 0) == 1 && key_place(close_keys, 1) == 0,
          "a keyframe's place in time, with a tie by ID and with keyframes 0.01 s apart");
    check(blend_outs_of(BlendOuts{{Target{true, 2, 1}, 300.0f}, {Target{true, 3, 0}, 80.0f}}, 2, 3) == std::vector<float>{0, 300, 0},
          "a trick's blend outs by key number");
    if (!failures) std::cout << "style pose tests passed\n";
    return failures ? 1 : 0;
}
