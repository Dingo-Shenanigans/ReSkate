#pragma once
#include "hall_of_meat_model.h"
#include <cstdint>
#include <string>
#include <vector>

// Hall of Meat's score card, as in skate. 3: a row per stat, with skate.'s own icon for it
// (hall_of_meat_overlay.cpp draws it), then the Meat. No game access.
namespace dingosdk::hall_of_meat {
// When a stat comes onto the card: once the bail has done something worth showing, so a small
// bail's card stays short and a big one grows as it goes. The time shows from the start; the
// Meat counts every stat whether shown or not.
inline constexpr int hits_shown = 5, broken_shown = 5;
inline constexpr float road_rash_shown_feet = 15.0f, airtime_shown_seconds = 3.0f, fall_shown_feet = 100.0f,
    speed_shown_mph = 20.0f, rotations_shown = 2.0f;
// skate. shows lengths in feet and speeds in miles per hour.
inline constexpr float feet_per_metre = 3.28084f;
inline constexpr float mph_per_metre_per_second = 2.23694f;

// Each stat's row, by the icon it shows, in the card's order.
enum class Stat : std::uint8_t { time, hits, broken, road_rash, airtime, fall, speed, rotations };
struct CardRow {
    Stat stat{};
    std::string value; // what was measured, as shown: "6.2 s"
    int points{};
};
struct Card {
    float opacity{}; // 0: no card
    std::vector<CardRow> rows;
    int total{};       // the Meat
    std::string badge; // beside the title: "NEW BEST" or "BEST 12,345"; empty for none
    bool highlight{};  // the badge marks a record
};
// The card for what the overlay draws now, against the map's best before the bail. No card while
// riding.
Card card(const View& view, const Standing& against);
// 12,345 and -1,234, as skate. shows its numbers.
std::string grouped(long long value);
}
