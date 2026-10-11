// LinearTransform math: points placed, directions turned, transforms composed.
#include "Engine/Game/Abi/linear_transform.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dingosdk::game;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

void a_point_is_placed_in_its_transform() {
    // Turned 90 degrees about +Z (x -> y), standing at (1, 2, 3).
    const LinearTransform transform{{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {1, 2, 3, 1}}};
    const auto point = place({2, 0, 1}, transform);
    check(near(point[0], 1) && near(point[1], 4) && near(point[2], 4), "offsets turn with the transform and move with it");
    const auto direction = turn({2, 0, 1}, transform);
    check(near(direction[0], 0) && near(direction[1], 2) && near(direction[2], 1), "a direction only turns");
}

void transforms_compose_inner_first() {
    const LinearTransform outer{{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {1, 2, 3, 1}}};
    const LinearTransform inner{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 5, 1}}}; // 5 along z
    const auto both = compose(inner, outer);
    const auto through = place(place({2, 0, 1}, inner), outer), at_once = place({2, 0, 1}, both);
    for (std::size_t i = 0; i < 3; ++i) check(near(through[i], at_once[i]), "one transform does what both do");
}

void lengths() {
    check(near(length({3, 4, 0}), 5.0f) && length({}) == 0.0f && near(length({0.1f, 0, 0}), 0.1f), "lengths");
}
}

int main() {
    try {
        a_point_is_placed_in_its_transform();
        transforms_compose_inner_first();
        lengths();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Linear transform tests passed.\n";
    return 0;
}
