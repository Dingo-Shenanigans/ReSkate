#include "Engine/Game/Skater/first_person_cycle.h"

#include <cstdio>
#include <string_view>

// First person in the camera cycle (first_person::cycle_height_write): the game's camera height
// button flips UseHighCam; the option turns that into high -> low -> first person -> high.
namespace {
using namespace dingosdk::first_person;
int failures = 0;

void expect(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
}
bool same(const CycleWrite& write, bool high, CycleStep step) { return write.high == high && write.step == step; }

void off() {
    for (const bool first_person : {false, true})
        for (const bool high : {false, true})
            expect(same(cycle_height_write({false, first_person, true}, high), high, CycleStep::none),
                "with the option off the game's height passes through and first person is left alone");
}

void steps() {
    expect(same(cycle_height_write({true, false, true}, false), false, CycleStep::none), "high -> low is the game's own step");
    expect(same(cycle_height_write({true, false, true}, true), false, CycleStep::enter),
        "low -> first person keeps the low camera behind it");
    expect(same(cycle_height_write({true, true, false}, true), true, CycleStep::leave),
        "first person (low behind it) -> the high camera");
    expect(same(cycle_height_write({true, true, false}, false), true, CycleStep::leave),
        "first person switched on in the menu over the high camera still lands on high");
    expect(same(cycle_height_write({true, false, false}, true), true, CycleStep::none),
        "where first person cannot start (on foot, Freecam) low -> high is the game's own step");
}

// The game reads the height it keeps and writes the opposite on every press.
void presses() {
    for (const bool start_high : {true, false}) {
        bool high = start_high, first_person = false;
        const auto press = [&] {
            const auto write = cycle_height_write({true, first_person, !first_person}, !high);
            high = write.high;
            if (write.step == CycleStep::enter) first_person = true;
            if (write.step == CycleStep::leave) first_person = false;
        };
        if (!start_high) press(); // low -> first person
        if (!start_high) press(); // first person -> high
        for (int round = 0; round < 3; ++round) {
            expect(high && !first_person, "the cycle starts each round on the high camera");
            press();
            expect(!high && !first_person, "first press: low camera");
            press();
            expect(!high && first_person, "second press: first person over the low camera");
            press();
        }
        expect(high && !first_person, "the third press of each round returns to the high camera");
    }
}
} // namespace

int main() {
    off();
    steps();
    presses();
    if (failures) {
        std::printf("%d first-person cycle test(s) failed\n", failures);
        return 1;
    }
    std::printf("first-person cycle tests passed\n");
    return 0;
}
