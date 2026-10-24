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

// With a plugin's camera in the cycle: high -> low -> first person -> the plugin's camera -> high, and
// without first person (or on foot, where it cannot start) high -> low -> the plugin's camera -> high.
void plugin() {
    for (const bool with_first_person : {true, false}) {
        bool high = true, first_person = false, on = false;
        const auto press = [&] {
            CycleState state{with_first_person, first_person, !first_person && !on, true, on, !on};
            const auto write = cycle_height_write(state, !high);
            high = write.high;
            if (write.step == CycleStep::enter) first_person = true;
            if (write.step == CycleStep::leave) first_person = false;
            if (write.plugin == CycleStep::enter) on = true;
            if (write.plugin == CycleStep::leave) on = false;
        };
        for (int round = 0; round < 3; ++round) {
            expect(high && !first_person && !on, "each round starts on the high camera");
            press();
            expect(!high && !first_person && !on, "first press: low camera");
            if (with_first_person) {
                press();
                expect(!high && first_person && !on, "second press: first person");
            }
            press();
            expect(!high && !first_person && on, "then the plugin's camera, the low camera behind it");
            press();
        }
        expect(high && !first_person && !on, "out of the plugin's camera: the high camera");
    }
    // No plugin in the cycle: the first-person cycle as before.
    expect(same(cycle_height_write({true, true, false, false, false, true}, true), true, CycleStep::leave),
        "without a plugin's camera, first person -> high");
    // Turned on by the plugin itself over the high camera: the button still takes it out to high.
    const auto out = cycle_height_write({false, false, true, true, true, false}, false);
    expect(out.high && out.plugin == CycleStep::leave, "a plugin's camera on by itself: the button leaves it for high");
}
} // namespace

int main() {
    off();
    steps();
    presses();
    plugin();
    if (failures) {
        std::printf("%d first-person cycle test(s) failed\n", failures);
        return 1;
    }
    std::printf("first-person cycle tests passed\n");
    return 0;
}
