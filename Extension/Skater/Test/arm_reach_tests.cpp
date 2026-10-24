// An arm reaching apart from the game: the hand joint lands on a point in reach, stops at the arm's
// length short of one out of reach, turns to a rotation asked for, and leaves a broken pose alone.
#include "Extension/Skater/arm_reach.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::arm_reach;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
float distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
bool same_turn(const Quat& a, const Quat& b) { return std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) > 0.9999f; }
bool same_pose(const std::vector<Joint>& a, const std::vector<Joint>& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_turn(a[i].rotation, b[i].rotation) || distance(a[i].position, b[i].position) > 1e-6f) return false;
    return true;
}

// A skater standing with both arms straight out: upper arms 0.3 m, forearms 0.28 m.
constexpr float upper_length = 0.3f, fore_length = 0.28f;
std::vector<Joint> t_pose() {
    std::vector<Joint> pose(395);
    pose[0].position = {10, 0, 5};
    pose[1].position = {0, 1.0f, 0};
    pose[45].position = {0, 0.4f, 0};
    pose[0].rotation = anim_studio::from_euler({0, 30, 0}); // the skater turned, to keep the maths honest
    for (const auto hand : {Hand::right, Hand::left}) {
        const auto joints = chain(hand);
        const float x = hand == Hand::left ? -1.0f : 1.0f;
        pose[joints[7]].position = {0.15f * x, 0, 0};
        pose[joints[8]].position = {0.1f * x, 0, 0};
        pose[joints[9]].position = {upper_length * x, 0, 0};
        pose[joints[10]].position = {fore_length * x, 0, 0};
    }
    return pose;
}
Vec3 joint_at(const std::vector<Joint>& pose, Hand hand, std::size_t index) {
    std::array<Placed, 11> w{};
    check(compose(pose, chain(hand), w), "The arm composes");
    return w[index].position;
}

void a_point_in_reach_is_reached() {
    for (const auto side : {Hand::right, Hand::left}) {
        auto pose = t_pose();
        const Vec3 s = joint_at(pose, side, 8);
        Goal goal;
        goal.hand = side;
        goal.at = {s[0] + 0.1f, s[1] - 0.25f, s[2] + 0.3f};
        const auto r = reach(pose, goal);
        check(r.valid && r.reached, "A point in reach is reached");
        check(distance(r.hand.position, goal.at) < 2e-3f, "and the hand is on it");
        check(distance(hand_of(pose, side).hand.position, goal.at) < 2e-3f, "as the pose now has it");
        check(std::abs(distance(joint_at(pose, side, 8), joint_at(pose, side, 9)) - upper_length) < 1e-4f, "The upper arm keeps its length");
        check(std::abs(distance(joint_at(pose, side, 9), joint_at(pose, side, 10)) - fore_length) < 1e-4f, "and the forearm its length");
    }
}

void a_point_out_of_reach_is_pointed_at() {
    auto pose = t_pose();
    const Vec3 s = joint_at(pose, Hand::right, 8);
    Goal goal;
    goal.at = {s[0], s[1] + 3.0f, s[2]};
    const auto r = reach(pose, goal);
    check(r.valid && !r.reached, "A point out of reach is not reached");
    check(std::abs(distance(r.hand.position, s) - (upper_length + fore_length) * 0.995f) < 2e-3f, "The arm stops just short of straight");
    check(r.hand.position[1] - s[1] > 0.5f, "pointing at it");
}

void a_rotation_asked_for_is_turned_to() {
    auto pose = t_pose();
    const Vec3 s = joint_at(pose, Hand::right, 8);
    Goal goal;
    goal.at = {s[0] + 0.2f, s[1] - 0.2f, s[2] + 0.2f};
    goal.rotation = anim_studio::from_euler({20, -70, 45});
    const auto r = reach(pose, goal);
    check(r.valid && same_turn(r.hand.rotation, *goal.rotation), "The hand turns to the rotation asked for");
    check(distance(r.hand.position, goal.at) < 2e-3f, "without leaving the point");
}

void no_weight_is_the_animation() {
    auto pose = t_pose();
    const auto before = pose;
    Goal goal;
    goal.at = joint_at(pose, Hand::right, 8);
    goal.at[2] += 0.4f;
    goal.weight = 0;
    check(reach(pose, goal).valid && same_pose(pose, before), "A weight of 0 leaves the arm as it was");
}

void only_the_arm_is_written() {
    auto pose = t_pose();
    const auto before = pose;
    Goal goal;
    goal.hand = Hand::left;
    goal.at = joint_at(pose, Hand::left, 8);
    goal.at[1] -= 0.4f;
    goal.rotation = anim_studio::from_euler({0, 0, 90});
    check(reach(pose, goal).valid, "The left arm reaches");
    for (std::size_t i = 0; i < pose.size(); ++i)
        if (i < first_written(Hand::left) || i > last_written(Hand::left))
            check(same_turn(pose[i].rotation, before[i].rotation), "A reach writes only the joints it says it writes");
}

void a_broken_pose_is_left_alone() {
    auto pose = t_pose();
    pose[48].rotation[0] = NAN;
    const auto before = pose;
    Goal goal;
    goal.at = {10, 1, 5};
    check(!reach(pose, goal).valid && !hand_of(pose, Hand::right).valid, "A broken arm is not reached with");
    for (std::size_t i = 0; i < pose.size(); ++i)
        if (i != 48) check(same_turn(pose[i].rotation, before[i].rotation), "and nothing is written");
    auto short_pose = t_pose();
    short_pose.resize(40);
    check(!reach(short_pose, goal).valid, "A pose without the arm's joints is refused");
    auto stretched = t_pose();
    stretched[48].position = {3, 0, 0};
    check(!reach(stretched, goal).valid, "An arm not an arm's length is refused");
    goal.at = {NAN, 0, 0};
    auto fine = t_pose();
    check(!reach(fine, goal).valid, "A point that is not a number is refused");
}
} // namespace

int main() {
    try {
        a_point_in_reach_is_reached();
        a_point_out_of_reach_is_pointed_at();
        a_rotation_asked_for_is_turned_to();
        no_weight_is_the_animation();
        only_the_arm_is_written();
        a_broken_pose_is_left_alone();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "arm_reach: all passed\n";
    return 0;
}
