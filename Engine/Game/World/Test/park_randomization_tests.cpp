#include "Engine/Game/World/park_randomization.h"
#include <iostream>
#include <map>
#include <set>

namespace {
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}
int main() {
    using namespace dingosdk;
    std::mt19937 generator{12345};
    std::array<std::map<std::string, unsigned>, park_lots.size()> seen;
    constexpr unsigned rolls = 30000;
    for (unsigned roll = 0; roll < rolls; ++roll) {
        const auto choices = random_park_choices(generator);
        for (unsigned lot = 0; lot < choices.size(); ++lot) {
            check(!choices[lot].empty() && choices[lot] != "empty" && valid_park(lot, choices[lot]),
                  "random layouts are non-empty and supported by their lot");
            ++seen[lot][choices[lot]];
        }
    }
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
        std::set<std::string> expected;
        for (unsigned family = 0; family < park_families.size(); ++family)
            for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant)
                expected.insert(park_id(family, variant));
        check(seen[lot].size() == expected.size(), "every supported layout can be selected");
        const auto average = rolls / static_cast<unsigned>(expected.size());
        for (const auto& layout : expected) {
            const auto count = seen[lot][layout];
            // Broad deterministic bound catches equal-family weighting without
            // making this a fragile statistical test or fixing a particular RNG sequence.
            check(count > average * 3 / 4 && count < average * 5 / 4,
                  "layouts have equal weight regardless of family size");
        }
    }
    ParkLaunchRandomization launch;
    check(!launch.consume(true, false), "launch randomization is off by default");
    launch.pending = true;
    check(!launch.consume(false, false) && launch.pending, "wait for park content before consuming launch");
    check(launch.consume(true, false) && !launch.pending, "randomize once when the controller becomes ready");
    check(!launch.consume(false, false) && !launch.consume(true, false), "map reloads do not reroll");
    launch.pending = true;
    check(!launch.consume(true, true) && !launch.pending, "guests never randomize host-controlled parks");
    check(!launch.consume(true, false), "leaving a host does not defer startup randomization");
    launch.pending = true;
    check(!launch.consume(false, true) && !launch.pending,
          "joining a host cancels startup randomization before park content is ready");
    check(!launch.consume(true, false), "leaving an unready host session cannot trigger a deferred roll");
    if (failures) return 1;
    std::cout << "park randomization: ok\n";
    return 0;
}
