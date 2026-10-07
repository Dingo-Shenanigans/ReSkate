// The fall-through guard's decision (trainer_fall_guard.h): when a fall is suspected of going
// through the map, and when a spot counts as solid ground to come back to.
#include "Extension/Trainer/trainer_fall_guard.h"

#include <iostream>
#include <string>

namespace {
using namespace dingosdk::trainer::fall_guard;
int failures = 0;
void check(bool condition, const std::string &message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
} // namespace

int main() {
    check(!suspect(40.0f, -3.0f, 50.0f), "a gentle drop is nothing");
    check(!suspect(40.0f, -20.0f, 50.0f), "a fast drop a little under the last spot is a jump");
    check(suspect(10.0f, -20.0f, 50.0f), "a fast drop far under the last spot is suspicious");
    check(!suspect(10.0f, -20.0f, std::nullopt), "no safe spot yet: nothing to compare against");
    check(!suspect(10.0f, 5.0f, 50.0f), "going up is never a fall");
    check(suspect(-2500.0f, 0.0f, std::nullopt), "below any map is always a fall through it");
    check(steady(0.2f, false), "rolling along is steady");
    check(!steady(0.2f, true), "in the air is not");
    check(!steady(-4.0f, false), "dropping is not");
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "fall guard: all checks passed\n";
    return failures ? 1 : 0;
}
