// Guided controller setup (drone_radio.h: Calibration): which axis each stick is on, which way
// round, and the arm and reset switches, from a simulated radio.
#include "Extension/Skater/drone_radio.h"
#include <cstdio>
#include <cstdlib>
using namespace dingosdk::drone::radio;
using Step = Calibration::Step;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } } while (0)
// Hold axes for `t` seconds at 60 fps.
static void hold(Calibration& c, std::array<float,6> a, std::uint32_t b, float t) { for (float s = 0; s < t; s += 1.f/60) c.update(a, b, 1.f/60); }
int main() {
  // A Mode 2 EdgeTX radio in AETR order: roll 1, pitch 2, throttle 3, yaw 4; switches on 5 and 6.
  { Calibration c; c.start(); CHECK(c.step()==Step::rest, "starts at rest");
    std::array<float,6> rest{0,0,-1,0,-1,-1};
    c.capture_rest(rest, 0); CHECK(c.step()==Step::throttle, "throttle after rest");
    auto a = rest; a[2] = 1; hold(c, a, 0, 0.1f); CHECK(c.step()==Step::throttle, "short push is not enough");
    hold(c, a, 0, 0.4f); CHECK(c.waiting_for_return(), "waits for throttle down");
    a[0] = 0.3f; hold(c, a, 0, 0.5f); // a small wobble elsewhere is ignored
    hold(c, rest, 0, 0.1f); CHECK(c.step()==Step::yaw, "yaw next");
    a = rest; a[3] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f); CHECK(c.step()==Step::pitch, "pitch next");
    a = rest; a[1] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);
    a = rest; a[0] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f); CHECK(c.step()==Step::arm, "arm next");
    a = rest; a[4] = 1; hold(c, a, 0, 0.4f); CHECK(c.step()==Step::reset, "arm stays on, reset next");
    auto b = a; b[5] = 1; hold(c, b, 0, 0.4f); CHECK(c.step()==Step::reset && c.waiting_for_return(), "reset must be let go");
    hold(c, a, 0, 0.1f); CHECK(c.step()==Step::done, "done");
    auto m = c.mapping(); CHECK(m==(std::array<int,4>{1,2,3,4}), "AETR mapping");
    CHECK(c.arm_axis()==5 && c.reset_axis()==6, "switch axes");
    CHECK(pick_axis(a, c.arm_axis()) > 0.5f, "armed side reads high");
  }
  // Shuffled and reversed: throttle on 1 reversed, yaw 2, pitch 4 reversed, roll 3; buttons for switches.
  { Calibration c; c.start(); std::array<float,6> rest{1,0,0,0,0,0}; c.capture_rest(rest, 0);
    auto a = rest; a[0] = -1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);
    a = rest; a[1] = 0.9f; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);
    a = rest; a[3] = -0.95f; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);
    a = rest; a[2] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);
    auto m = c.mapping(); CHECK(m==(std::array<int,4>{3,-4,-1,2}), "shuffled mapping");
    auto cmd = map_axes(std::array<float,6>{-1,0,0,0,0,0}, m); CHECK(cmd.throttle > 0.99f, "reversed throttle reads full when pushed");
    cmd = map_axes(rest, m); CHECK(cmd.throttle < 0.01f, "reversed throttle reads zero at rest");
    hold(c, rest, 1u<<6, 0.4f); CHECK(c.arm_button()==7 && c.step()==Step::reset, "arm button");
    hold(c, rest, (1u<<6)|(1u<<1), 0.4f); CHECK(c.reset_button()==2, "reset button");
    hold(c, rest, 1u<<6, 0.1f); CHECK(c.step()==Step::done, "done after reset released");
  }
  // Skips, and a button held at rest never counts.
  { Calibration c; c.start(); std::array<float,6> rest{0,0,-1,0,0,0}; c.capture_rest(rest, 1u);
    for (int i : {2,3,1,0}) { auto a = rest; a[i] = 1.f; hold(c, a, 1u, 0.4f); hold(c, rest, 1u, 0.1f); }
    hold(c, rest, 1u, 1.0f); CHECK(c.step()==Step::arm, "held-at-rest button ignored");
    c.skip(); c.skip(); CHECK(c.step()==Step::done && c.arm_axis()==0 && c.arm_button()==0, "skipped switches");
    c.cancel(); CHECK(!c.active() && c.step()==Step::idle, "cancel");
  }
  // An Xbox pad through Windows' joystick API: sticks rest in the middle, triggers on 5 and 6,
  // and up on a stick reads negative. The throttle springs back, so the middle has to hover.
  { Calibration c; c.start(); std::array<float,6> rest{0,0,0,0,-1,-1}; c.capture_rest(rest, 0);
    auto a = rest; a[1] = -1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);  // throttle: left stick up
    a = rest; a[0] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);       // yaw: left stick right
    a = rest; a[3] = -1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);      // pitch: right stick up
    a = rest; a[2] = 1; hold(c, a, 0, 0.4f); hold(c, rest, 0, 0.1f);       // roll: right stick right
    c.skip(); c.skip();
    CHECK(c.throttle_centred(), "a pad's throttle is found to spring back");
    const auto m = c.mapping(); CHECK(m==(std::array<int,4>{3,-4,-2,1}), "pad mapping");
    CHECK(std::abs(map_axes(rest, m, 0.12f, true, 0.35f).throttle - 0.35f) < 1e-4f, "middle of a spring throttle hovers");
    a = rest; a[1] = -1; CHECK(map_axes(a, m, 0.12f, true, 0.35f).throttle > 0.999f, "stick up is full throttle");
    a[1] = 1; CHECK(map_axes(a, m, 0.12f, true, 0.35f).throttle < 0.001f, "stick down is no throttle");
    a[1] = -0.05f; CHECK(std::abs(map_axes(a, m, 0.12f, true, 0.35f).throttle - 0.35f) < 1e-4f, "drift inside the dead zone still hovers");
  }
  // A radio's throttle rests at the bottom: not a spring throttle.
  { Calibration c; c.start(); std::array<float,6> rest{0,0,-1,0,0,0}; c.capture_rest(rest, 0);
    auto a = rest; a[2] = 1; hold(c, a, 0, 0.4f); CHECK(!c.throttle_centred(), "a radio throttle is not a spring throttle"); }
  std::printf(fails ? "%d failure(s)\n" : "all calibration checks passed\n", fails);
  return fails ? 1 : 0;
}
