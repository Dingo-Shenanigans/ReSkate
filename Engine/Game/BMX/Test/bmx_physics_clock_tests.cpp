#include "Engine/Game/BMX/physics_clock.h"
#include "Engine/Game/BMX/bmx_model.h"
#include <cstdio>
#include <limits>

int main() {
    using namespace dingosdk::bmx;
    int errors{};
    const auto check = [&](bool ok, const char* text) { if (!ok) { ++errors; std::printf("FAIL: %s\n", text); } };
    PhysicsClock clock;
    check(clock.sample(100, .01) == 0, "first native observation only establishes a baseline");
    check(clock.sample(100, .01) == 0, "render duplicates do not advance native time");
    check(std::abs(clock.sample(102, .01) - .02) < 1e-9, "native step count uses scaled simulation time");
    check(clock.sample(103, 0) == 0, "pause cannot advance gestures or motion");
    check(std::abs(clock.sample(104, .005) - .005) < 1e-9, "resuming slow motion does not catch up pause time");
    check(clock.sample(500, .01) == 0, "respawn or large gap is discarded");
    check(clock.sample(501, std::numeric_limits<double>::quiet_NaN()) == 0, "invalid native time is rejected");
    clock.reset();
    check(clock.sample(UINT32_MAX, .01) == 0 && std::abs(clock.sample(0, .01) - .01) < 1e-9,
          "32-bit native simulation counter wrap preserves one step");
    clock.reset();
    check(clock.sample_time(1, 10) == 0, "native observer time establishes a baseline");
    check(std::abs(clock.sample_time(3, 10.015) - .015) < 1e-9,
          "two physics steps with different scaled durations use their sum");
    check(clock.sample_time(3, 10.015) == 0 && !clock.discontinuity(), "same observer step is idle, not an interruption");
    check(clock.sample_time(4, 10.015) == 0 && !clock.discontinuity(), "zero-time physics step cannot move the bike");
    check(clock.sample_time(100, 11) == 0 && clock.discontinuity(), "native simulation gap requires gesture rearming");
    check(clock.sample_time(1, 0) == 0 && clock.discontinuity(), "owner/time reset cannot retain old input momentum");
    Model bike;
    Contacts ground{{QueryState::hit, 0}, {QueryState::hit, 0}};
    bike.advance(.1, {0, 1}, ground);
    const double speed = bike.state().speed;
    check(bike.reconcile_position(1, 0, 2) && bike.state().x == 1 && bike.state().z == 2 && bike.state().speed == speed,
          "native collision position is authoritative without resetting steering or speed");
    check(!bike.reconcile_position(std::numeric_limits<double>::infinity(), 0, 0) && bike.state().x == 1,
          "bad native position cannot corrupt the model");
    bike.stop_horizontal();
    check(bike.state().speed == 0, "a chassis obstacle consumes forward speed");
    std::printf("BMX native clock/reconciliation: %s\n", errors ? "FAILED" : "passed");
    return errors ? 1 : 0;
}
