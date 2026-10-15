// Usage: dingosdk_style_skeleton_tests [Skate folder]
#include "Extension/Style/style_skeleton.h"
#include "Extension/Style/style_pose.h"
#include <algorithm>
#include <iostream>
#include <string_view>

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
        const auto skeleton = read_game_skeleton(argv[1]);
        check(skeleton.names.size() == 395, "the skeleton has 395 joints");
        const auto index = [&](std::string_view name) {
            return std::ranges::find(skeleton.names, name) - skeleton.names.begin();
        };
        check(index("Reference") == 0 && index("AITrajectory") == 1, "the placement joints come first");
        check(index("Hips") == 7 && index("Head") == 103 && index("Face") == 107, "the head chain matches first person");
        check(skeleton.parents[103] == 102 && skeleton.parents[7] == 1, "parents are read");
        for (const auto name : editable_joints) {
            const auto at = static_cast<std::size_t>(index(name));
            check(at < skeleton.names.size() && at >= 7 && at < 375, "an editable joint is a body joint of the skeleton");
            check(std::ranges::count(skeleton.names, name) == 1, "an editable joint's name is unique");
        }
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "style skeleton tests passed\n";
    return failures ? 1 : 0;
}
