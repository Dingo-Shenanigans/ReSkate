#include "Extension/Style/trick_gestures.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
} // namespace

int main(int argc, char **argv) try {
    using dingosdk::style::flip_trick_type, dingosdk::style::stick_axis;
    check(flip_trick_type(1) == std::uint8_t{1} && flip_trick_type(2) == std::uint8_t{2} && flip_trick_type(63) == std::uint8_t{63},
          "trick numbers 1 to 63 are kept");
    check(!flip_trick_type(0) && !flip_trick_type(64) && !flip_trick_type(-1), "other numbers are no trick");
    check(stick_axis(1.0f) == 32767 && stick_axis(-1.0f) == -32767 && stick_axis(0.0f) == 0 && stick_axis(4.0f) == 32767 &&
              stick_axis(-4.0f) == -32767,
          "the stick axis is clamped to the pad's range");
    check(stick_axis(std::nanf("")) == 0 && stick_axis(INFINITY) == 0 && stick_axis(-INFINITY) == 0,
          "NaN and infinity leave the stick in the middle");
    using dingosdk::style::flick, dingosdk::style::TrickGesture, dingosdk::style::max_flick_ms;
    const auto ticks_of = [](const std::vector<dingosdk::overlay::PadStep> &steps) {
        std::vector<std::uint32_t> ticks;
        for (const auto &step : steps) ticks.push_back(step.tick);
        return ticks;
    };
    check(flick(TrickGesture{}, 24).empty(), "a gesture with no arcs gives no steps");
    // A crouch straight down at full reach, the stick let go in the middle, then a flick up and to the right.
    const TrickGesture up_right{2, false, 170, 190, 0.9f, {{170, 190, 0.9f, 1, 0}, {350, 10, 0.1f, 0.2f, 0}, {30, 60, 0.8f, 1, 0}}};
    const auto fast = flick(up_right, 0);
    check(ticks_of(fast) == std::vector<std::uint32_t>{0, 18, 19, 23, 55},
          "a crouch of 18 ticks, a tick for each move, the release 4 ticks after the last move and the end 36 ticks after it");
    check(fast[0].pad.sThumbRX == 0 && fast[0].pad.sThumbRY == -32767, "the crouch is at the first arc, at full reach");
    check(fast[1].pad.sThumbRX == 0 && fast[1].pad.sThumbRY == 0, "an arc near the middle is the stick let go");
    check(fast[2].pad.sThumbRX > 20000 && fast[2].pad.sThumbRY > 20000, "the flick ends up and to the right");
    check(fast[3].pad.sThumbRY == 0 && fast[4].pad.sThumbRY == 0, "then the stick is let go");
    check(ticks_of(flick(up_right, 100)) == std::vector<std::uint32_t>{0, 18, 19, 20, 21, 22, 23, 27, 59},
          "a 100 ms flick shares its 6 ticks between the moves");
    check(ticks_of(flick(up_right, 60000)) == ticks_of(flick(up_right, max_flick_ms)), "a flick longer than max_flick_ms is cut to it");
    // From the right to down and to the left: a turn of 135 degrees.
    const TrickGesture turning{3, false, 170, 190, 0.9f, {{170, 190, 0.9f, 1, 0}, {80, 100, 0.8f, 1, 0}, {215, 235, 0.8f, 1, 0}}};
    check(ticks_of(flick(turning, 0)) == std::vector<std::uint32_t>{0, 18, 19, 20, 24, 56}, "a turn of 135 degrees gets two ticks");
    check(std::ranges::is_sorted(ticks_of(flick(turning, 200))), "the steps' ticks rise");
    const TrickGesture broken{4, false, 0, 0, 0.9f, {{NAN, NAN, 0.9f, 1, 0}, {0, 0, 0.8f, 1, 0}}};
    const auto calm = flick(broken, 0);
    check(ticks_of(calm) == std::vector<std::uint32_t>{0, 18, 22, 54} && calm[0].pad.sThumbRX == 0 && calm[0].pad.sThumbRY == 0 &&
              calm[1].pad.sThumbRX == 0,
          "an angle that is not a number leaves the stick in the middle");
    if (argc < 2 || !std::filesystem::exists(std::filesystem::path(argv[1]) / "Data")) {
        std::cout << "No game root given; skipped.\n";
        return failures ? 1 : 0;
    }
    const auto gestures = dingosdk::style::read_trick_gestures(argv[1]);
    check(gestures.size() >= 30, "nearly every flip trick has a gesture");
    const auto find = [&](std::uint8_t trick) {
        const auto found = std::ranges::find(gestures, trick, &dingosdk::style::TrickGesture::trick);
        return found == gestures.end() ? nullptr : &*found;
    };
    // The middle of an arc, in degrees clockwise from up.
    const auto middle = [](const dingosdk::style::GestureArc &arc) {
        const float to = arc.to < arc.from ? arc.to + 360 : arc.to;
        return std::fmod((arc.from + to) / 2, 360.0f);
    };
    const auto close_to = [](float angle, float target) { return std::abs(std::remainder(angle - target, 360.0f)) < 50; };
    // Measured in the game: these moves do these tricks.
    const auto *ollie = find(1), *kickflip = find(2), *heelflip = find(3), *nollie = find(16);
    check(ollie && close_to(middle(ollie->arcs.front()), 180) && close_to(middle(ollie->arcs.back()), 0),
          "an ollie is the stick down, then up");
    check(nollie && nollie->nollie && close_to(middle(nollie->arcs.front()), 0) && close_to(middle(nollie->arcs.back()), 180),
          "a nollie is the stick up, then down");
    check(kickflip && close_to(middle(kickflip->arcs.back()), 315), "a kickflip flicks up and to the left");
    check(heelflip && close_to(middle(heelflip->arcs.back()), 45), "a heelflip flicks up and to the right");
    for (const auto &gesture : gestures)
        check(gesture.hold_magnitude > 0.5f && !gesture.arcs.empty(), "each gesture has a crouch and a move");
    if (!failures) std::cout << "trick gestures tests passed (" << gestures.size() << " tricks)\n";
    return failures ? 1 : 0;
} catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
