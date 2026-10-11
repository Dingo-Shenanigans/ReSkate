// The step length the game keeps for a simulation rate, and the rate back from it.
#include "Engine/Game/Settings/simulation_time.h"
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}
} // namespace

int main() {
    using namespace dingosdk::simulation_time;
    check(std::bit_cast<std::uint32_t>(step_seconds(60)) == 0x3c888888, "60 a second: the game's 1/60, from whole nanoseconds");
    check(step_seconds(200) == 0.005f, "200 a second: 5 ms");
    check(step_seconds(0) == 0.0f, "no rate, no step");
    check(rate_of(step_seconds(60)) == 60 && rate_of(step_seconds(200)) == 200 && rate_of(step_seconds(1200)) == 1200,
        "the rate back from its step");
    check(rate_of(0.0f) == 0 && rate_of(std::numeric_limits<float>::quiet_NaN()) == 0, "no step, no rate");
    if (failures) return 1;
    std::cout << "Simulation time tests passed.\n";
    return 0;
}
