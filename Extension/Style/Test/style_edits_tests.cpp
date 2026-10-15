#include "Extension/Style/style_edits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
RotationKey at(std::uint8_t trick, std::uint8_t key, const char *joint) { return {Target{true, trick, key}, joint}; }
} // namespace

int main() {
    std::string error;
    Style style;
    check(edits::add_key(style, 2, 0.5f, even_pace, error) == 0 && edits::add_key(style, 2, 1.0f, even_pace, error) == 1 &&
              edits::add_key(style, 2, 1.5f, even_pace, error) == 2,
          "keyframes get the next number");
    check(edits::set_joint(style, Target{true, 2, 2}, "Head", 10, 0, 0, error), "a keyframe's joint is set");
    check(edits::set_joint(style, Target{true, 2, 2}, "head", 12, 0, 0, error) &&
              style.rotations.at(at(2, 2, "Head"))[0] == 12,
          "a joint named in another case is set under the editor's name");
    check(edits::set_blend_out(style, 2, 2, 300, error), "a keyframe gets a blend-out");
    check(edits::delete_key(style, 2, 1, error), "a keyframe is deleted");
    check(style.keys(2).size() == 2 && style.rotations.contains(at(2, 1, "Head")) && !style.rotations.contains(at(2, 2, "Head")),
          "the later keyframe's rotation moves down one number");
    check(style.blend_outs.contains(Target{true, 2, 1}) && style.blend_outs.at(Target{true, 2, 1}) == 300.0f,
          "and so does its blend-out");

    {
        const Pace measured{440, 290, 450};
        Style seeded;
        (void)edits::add_key(seeded, 3, 0.5f, measured, error);
        (void)edits::set_joint(seeded, Target{true, 3, 0}, "Spine1", 0, 25, 0, error);
        (void)edits::set_joint(seeded, Target{true, 3, 0}, "LeftArm", 0, 40, 0, error);
        (void)edits::set_blend_out(seeded, 3, 0, 300, error);
        (void)edits::add_key(seeded, 3, 2.0f, measured, error);
        (void)edits::set_joint(seeded, Target{true, 3, 1}, "Head", 30, 0, 0, error);
        std::vector<JointDelta> shown, after;
        evaluate(trick_keys(seeded, 3), 0.8f, shown, measured);
        const auto added = edits::add_key(seeded, 3, 0.8f, measured, error);
        const auto played = trick_keys(seeded, 3);
        evaluate(played, 0.8f, after, measured);
        bool same = added == 2 && shown.size() == 2 && played.size() == 3 && played[1].time == 0.8f && played[1].joints.size() == 2;
        // A joint that only the next keyframe turns stays at the game's pose.
        for (const auto &now : after) {
            const auto was = std::ranges::find(shown, now.joint, &JointDelta::joint);
            same = same && std::abs(std::abs(dot(now.rotation, was == shown.end() ? identity : was->rotation)) - 1) < 1e-4f;
        }
        for (const auto &delta : shown) {
            const auto expected = to_degrees(delta.rotation);
            const auto found = seeded.rotations.find(at(3, 2, std::string(editable_joints[delta.joint]).c_str()));
            same = same && found != seeded.rotations.end() && std::abs(found->second[0] - expected[0]) < 0.01f &&
                   std::abs(found->second[1] - expected[1]) < 0.01f && std::abs(found->second[2] - expected[2]) < 0.01f &&
                   std::ranges::find(after, delta.joint, &JointDelta::joint) != after.end();
        }
        check(same, "a new keyframe holds the shown pose, so the pose does not change");
    }
    {
        Style tilted;
        (void)edits::add_key(tilted, 4, 0.5f, even_pace, error);
        (void)edits::set_joint(tilted, Target{true, 4, 0}, "Spine1", 0, 100, 0, error);
        std::vector<JointDelta> shown, after;
        evaluate(trick_keys(tilted, 4), 0.51f, shown, even_pace);
        (void)edits::add_key(tilted, 4, 0.51f, even_pace, error);
        evaluate(trick_keys(tilted, 4), 0.51f, after, even_pace);
        check(shown.size() == 1 && after.size() == 1 && std::abs(std::abs(dot(after[0].rotation, shown[0].rotation)) - 1) < 1e-4f,
              "a new keyframe holds a shown tilt past 90 degrees");
    }

    {
        Style two;
        (void)edits::add_key(two, 2, 1.0f, even_pace, error);
        (void)edits::add_key(two, 5, 1.0f, even_pace, error);
        (void)edits::set_joint(two, Target{true, 5, 0}, "Head", 5, 0, 0, error);
        edits::clear(two, Target{true, 2});
        check(two.keys(2).empty() && two.keys(5).size() == 1 && two.rotations.contains(at(5, 0, "Head")),
              "clearing a trick leaves the others");
        edits::clear(two, std::nullopt);
        check(two == Style{}, "clearing all leaves nothing");
    }

    {
        Style blends;
        (void)edits::add_key(blends, 2, 1.0f, even_pace, error);
        (void)edits::set_blend_out(blends, 2, 0, 10, error);
        check(blends.blend_outs.at(Target{true, 2, 0}) == min_blend_out_ms, "a short blend-out is raised to the least");
        (void)edits::set_blend_out(blends, 2, 0, 5000, error);
        check(blends.blend_outs.at(Target{true, 2, 0}) == max_blend_out_ms, "a long one is cut to the most");
        (void)edits::set_blend_out(blends, 2, 0, 0, error);
        check(!blends.blend_outs.contains(Target{true, 2, 0}), "0 removes it");
    }

    {
        Style full;
        for (int i = 0; i < static_cast<int>(max_keys); ++i)
            (void)edits::add_key(full, 4, 0.1f + 0.2f * static_cast<float>(i), even_pace, error);
        const auto before = full;
        error.clear();
        check(edits::add_key(full, 4, 2.9f, even_pace, error) == -1 && !error.empty() && full == before,
              "a keyframe past the most is refused");
    }

    {
        Style unchanged;
        (void)edits::add_key(unchanged, 2, 1.0f, even_pace, error);
        const auto before = unchanged;
        check(edits::add_key(unchanged, 0, 1.0f, even_pace, error) == -1 && unchanged == before, "no trick 0");
        check(!edits::move_key(unchanged, 2, 9, 1.0f, error) && unchanged == before, "no move of a keyframe the trick lacks");
        check(!edits::delete_key(unchanged, 2, 9, error) && unchanged == before, "no delete of one either");
        check(!edits::set_blend_out(unchanged, 2, 9, 300, error) && unchanged == before, "nor a blend-out");
        check(!edits::set_joint(unchanged, Target{true, 2, 0}, "Tail", 1, 0, 0, error) && unchanged == before, "no joint outside the list");
        check(!edits::set_joint(unchanged, Target{true, 2, 0}, "Head", NAN, 0, 0, error) && unchanged == before, "angles must be numbers");
    }
    {
        Style apart;
        (void)edits::add_key(apart, 2, 1.0f, even_pace, error);
        (void)edits::add_key(apart, 2, 1.0f, even_pace, error);
        check(apart.keys(2) == std::vector<float>{1.0f, 1.0f + key_nudge}, "a keyframe added at another's time is nudged later");
        (void)edits::add_key(apart, 2, 2.0f, even_pace, error);
        check(edits::move_key(apart, 2, 2, 1.0f, error) && apart.keys(2)[2] == 1.0f - key_nudge,
              "a keyframe moved onto two others goes to the nearest free time");
        (void)edits::move_key(apart, 2, 0, 9.0f, error);
        (void)edits::move_key(apart, 2, 1, 9.0f, error);
        check(apart.keys(2)[0] == last_key_time && apart.keys(2)[1] == last_key_time - key_nudge,
              "two keyframes dragged to the end stay apart inside the timeline");
        check(edits::move_key(apart, 2, 2, 1.5f, error) && apart.keys(2)[2] == 1.5f, "a free time is kept");
    }
    {
        Style moved;
        (void)edits::add_key(moved, 2, 0.5f, even_pace, error);
        (void)edits::add_key(moved, 2, 1.0f, even_pace, error);
        (void)edits::set_joint(moved, Target{true, 2, 0}, "Head", 10, 0, 0, error);
        (void)edits::set_joint(moved, Target{true, 2, 1}, "Head", 20, 0, 0, error);
        (void)edits::set_blend_out(moved, 2, 0, 100, error);
        (void)edits::set_blend_out(moved, 2, 1, 200, error);
        check(edits::move_key(moved, 2, 0, 1.5f, error) && moved.keys(2) == std::vector<float>{1.5f, 1.0f},
              "the moved keyframe keeps its ID");
        check(moved.rotations.at(at(2, 0, "Head"))[0] == 10 && moved.rotations.at(at(2, 1, "Head"))[0] == 20 &&
                  moved.blend_outs.at(Target{true, 2, 0}) == 100.0f && moved.blend_outs.at(Target{true, 2, 1}) == 200.0f,
              "each ID keeps its rotation and blend-out");
        check(key_place(moved.keys(2), 0) == 1 && key_place(moved.keys(2), 1) == 0, "the place in time gives the new order");
    }
    if (!failures) std::cout << "style edits tests passed\n";
    return failures ? 1 : 0;
}
