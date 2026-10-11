// The flip trick gestures read from the game's data match the stick moves that do those tricks in the game.
#include "Extension/Skater/trick_gestures.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
} // namespace

int main(int argc, char **argv) try {
    if (argc < 2 || !std::filesystem::exists(std::filesystem::path(argv[1]) / "Data")) {
        std::cout << "No game root given; skipped.\n";
        return 0;
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
    const auto near = [](float angle, float target) { return std::abs(std::remainder(angle - target, 360.0f)) < 50; };
    // Measured in the game: these moves do these tricks.
    const auto *ollie = find(1), *kickflip = find(2), *heelflip = find(3), *nollie = find(16);
    check(ollie && near(middle(ollie->arcs.front()), 180) && near(middle(ollie->arcs.back()), 0), "an ollie is the stick down, then up");
    check(nollie && nollie->nollie && near(middle(nollie->arcs.front()), 0) && near(middle(nollie->arcs.back()), 180), "a nollie is the stick up, then down");
    check(kickflip && near(middle(kickflip->arcs.back()), 315), "a kickflip flicks up and to the left");
    check(heelflip && near(middle(heelflip->arcs.back()), 45), "a heelflip flicks up and to the right");
    for (const auto &gesture : gestures) check(gesture.hold_magnitude > 0.5f && !gesture.arcs.empty(), "each gesture has a crouch and a move");
    if (!failures) std::cout << "trick gestures tests passed (" << gestures.size() << " tricks)\n";
    return failures ? 1 : 0;
} catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
