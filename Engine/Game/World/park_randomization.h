#pragma once
#include "park_rotation.h"
#include <random>
#include <utility>

namespace dingosdk {
// Sample layouts, rather than families, so every authored layout has equal weight.
// Empty lots are deliberately excluded from the random pool.
template<class Generator> ParkChoices random_park_choices(Generator& generator) {
    ParkChoices choices;
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
        unsigned count{};
        for (const auto variants : park_lots[lot].counts) count += variants;
        auto selected = std::uniform_int_distribution<unsigned>(0, count - 1)(generator);
        for (unsigned family = 0; family < park_families.size(); ++family) {
            const auto variants = park_lots[lot].counts[family];
            if (selected < variants) { choices[lot] = park_id(family, selected + 1); break; }
            selected -= variants;
        }
    }
    return choices;
}
inline ParkChoices random_park_choices() {
    thread_local std::mt19937 generator{std::random_device{}()};
    return random_park_choices(generator);
}

struct ParkLaunchRandomization {
    bool pending{};
    // Waiting for content does not consume the launch. Joining a host does:
    // leaving that session or reconstructing the controller must not reroll.
    bool consume(bool ready, bool controlled_by_host) {
        if (controlled_by_host) { pending = false; return false; }
        if (!ready || !std::exchange(pending, false)) return false;
        return true;
    }
};
}
