// The skater skeleton from the game's data has every editable joint, and matches joints measured on a live skater.
// Usage: dingosdk_style_skeleton_tests [Skate folder]
#include "Extension/Skater/style_skeleton.h"
#include "Engine/Game/Skater/style_pose.h"
#include <algorithm>
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
} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || !*argv[1]) {
        std::cout << "style skeleton tests skipped: no game folder given\n";
        return 0;
    }
    try {
        const auto joints = read_game_skeleton(argv[1]);
        check(joints.size() == 395, "the skeleton has 395 joints");
        const auto index = [&](std::string_view name) {
            return std::ranges::find(joints, name, &SkeletonJoint::name) - joints.begin();
        };
        check(index("Reference") == 0 && index("AITrajectory") == 1, "the placement joints come first");
        check(index("Hips") == 7 && index("Head") == 103 && index("Face") == 107, "the head chain matches first person");
        check(joints[103].parent == 102 && joints[7].parent == 1, "parents are read");
        for (const auto name : editable_joints) {
            const auto at = static_cast<std::size_t>(index(name));
            check(at < joints.size() && at >= 7 && at < 375, "an editable joint is a body joint of the skeleton");
            check(std::ranges::count(joints, name, &SkeletonJoint::name) == 1, "an editable joint's name is unique");
        }
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "style skeleton tests passed\n";
    return failures ? 1 : 0;
}
