// A draw packet's bones: unpacked and placed in the world.
#include "Engine/Game/Rendering/draw_packet.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dingosdk;
using namespace dingosdk::draw_packet;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

void a_packed_bone_unpacks() {
    // Turned 90 degrees about +Z (x -> y), at (1, 2, 3): the columns of that matrix, as packed.
    const PackedBone packed{0, -1, 0, 1, 1, 0, 0, 2, 0, 0, 1, 3};
    const auto transform = unpack(packed);
    const auto point = game::place({1, 0, 0}, transform);
    check(near(point[0], 1) && near(point[1], 3) && near(point[2], 3), "the rows come back from the columns");
}

void bones_are_placed_by_root_and_actor() {
    const PackedBone origin{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const LinearTransform actor{{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {10, 0, 0, 1}}};
    LinearTransform out{};
    check(skin(origin, placement({100, 200, 300, 0}, false, actor), out) && near(out[3][0], 100) && near(out[3][2], 300),
        "the root adds the whole metres back");
    check(skin(origin, placement({1, 0, 0, 0}, true, actor), out), "relative");
    const auto point = game::place({0, 0, 0}, out);
    check(near(point[0], 10) && near(point[1], 1), "a relative packet then goes into its actor's transform");
    PackedBone bad = origin;
    bad[5] = std::numeric_limits<float>::quiet_NaN();
    check(!skin(bad, placement({}, false, actor), out), "a bone that is not finite is refused");
}
}

int main() {
    try {
        a_packed_bone_unpacks();
        bones_are_placed_by_root_and_actor();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Draw packet tests passed.\n";
    return 0;
}
