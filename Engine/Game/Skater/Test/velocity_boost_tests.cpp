#include "Engine/Game/Skater/velocity_boost.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
bool near(float a, float b) { return std::abs(a - b) < .001f; }
}

int main() {
    using dingosdk::VelocityBoostDirection;
    using dingosdk::velocity_boost_delta;
    constexpr auto forward = VelocityBoostDirection::forward;
    constexpr auto up = VelocityBoostDirection::up;
    std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Rotate through world headings; changing preferred stance does not change
    // this contract: facing-aligned travel boosts forward, fakie travel reverses it.
    for (int heading = 0; heading < 360; heading += 15) {
        const float angle = static_cast<float>(heading) * 3.14159265f / 180.f;
        transform[8] = std::sin(angle);
        transform[10] = std::cos(angle);
        for (const float speed : {1.f, 75.f, 300.f}) {
            for (const float travel : {12.f, -12.f}) {
                const std::array<float, 3> motion{travel * transform[8], 0, travel * transform[10]};
                const auto delta = velocity_boost_delta(transform, speed, forward, motion);
                check(delta.has_value(), "Valid riding boost rejected");
                const float sign = travel < 0 ? -1.f : 1.f;
                check(near((*delta)[0], sign * speed * transform[8]) && near((*delta)[2], sign * speed * transform[10]),
                    "Boost opposes riding direction");
                check(near(std::hypot((*delta)[0], (*delta)[2]), speed), "Configured boost magnitude changed");
            }
        }
    }
    transform[8] = 0; transform[10] = 1;
    for (const auto motion : {std::array<float, 3>{0, 0, 0}, {0, 0, -.05f}, {12, 0, 0}, {0, -50, 0}, {0, 50, 0}}) {
        const auto delta = velocity_boost_delta(transform, 75, forward, motion);
        check(delta && near((*delta)[2], 75), "Stationary, sideways or vertical motion reversed boost");
    }
    // Preserve the original three-dimensional facing axis and magnitude on ramps.
    transform[9] = .6f; transform[10] = .8f;
    const auto ramp = velocity_boost_delta(transform, 75, forward, {0, -100, -12});
    check(ramp && near((*ramp)[1], -45) && near((*ramp)[2], -60), "Fakie ramp direction or magnitude changed");
    const auto rising = velocity_boost_delta(transform, 75, forward, {0, -100, 12});
    check(rising && near((*rising)[1], 45) && near((*rising)[2], 60), "Vertical motion overrode forward travel");
    const auto upward = velocity_boost_delta(transform, 25, up, {0, -100, -12});
    check(upward && *upward == std::array<float, 3>{0, 25, 0}, "Up Boost changed in fakie");
    for (const float speed : {0.f, 301.f, std::numeric_limits<float>::quiet_NaN()})
        check(!velocity_boost_delta(transform, speed, forward), "Invalid forward speed accepted");
    check(!velocity_boost_delta(transform, 26, up), "Invalid up speed accepted");
    check(!velocity_boost_delta({}, 75, forward), "Degenerate facing accepted");
    check(!velocity_boost_delta(transform, 75, forward, {0, 0, std::numeric_limits<float>::infinity()}),
        "Invalid travel velocity accepted");
    std::cout << "Velocity boost regressions passed.\n";
}
