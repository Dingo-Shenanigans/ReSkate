#pragma once
#include "Engine/Game/Skater/skater_body.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Road Rash's rules, apart from the game: which side of the body a bail hit, how many bails it
// takes for a limb's marks to show and to get worse, what the items that draw them are called and
// where on a limb they sit. Plain data and pure functions (Test/road_rash_tests.cpp).
namespace dingosdk::road_rash {
// Where marks can sit: the skater has one tattoo layer per arm and leg, and one moles layer on the face.
enum class Part : std::uint8_t { left_leg, left_arm, right_leg, right_arm, face };
inline constexpr std::size_t part_count = 5;
inline constexpr std::array<Part, part_count> parts{Part::left_leg, Part::left_arm, Part::right_leg, Part::right_arm, Part::face};
inline constexpr std::array<std::string_view, part_count> part_names{"left leg", "left arm", "right leg", "right arm", "face"};
constexpr std::size_t index(Part part) noexcept { return static_cast<std::size_t>(part); }
constexpr bool leg(Part part) noexcept { return part == Part::left_leg || part == Part::right_leg; }
constexpr bool left(Part part) noexcept { return part == Part::left_leg || part == Part::left_arm; }

// A limb's marks have three levels, the face's two.
inline constexpr int worst_limb = 3, worst_face = 2;
constexpr int worst(Part part) noexcept { return part == Part::face ? worst_face : worst_limb; }
// The bails it takes for the first marks, the bigger ones and the worst ones.
inline constexpr std::array<unsigned, 3> steps{10, 14, 20};
// The face follows the worst level of the limbs, and gets worse four bails later.
inline constexpr unsigned face_later = 4;
// How many bails after the leg of the side that took more falls a limb follows: that side's arm,
// then the other side's leg and arm.
inline constexpr unsigned arm_later = 1, other_side_later = 4;

// ---- a fall -------------------------------------------------------------------------------

enum class Side : std::uint8_t { both, left, right };
// How hard the limbs of each side hit something in a fall, summed over its physics steps: the speed
// into the surface and the speed along it, metres per second.
struct Fall {
    float left{}, right{};
};
// The side a body is on; feet and toes (they touch the ground all the time), the trunk and the
// head count for neither.
constexpr Side side_of(skater_body::Bone bone) noexcept {
    using skater_body::Bone;
    switch (bone) {
    case Bone::left_hand: case Bone::left_forearm: case Bone::left_arm: case Bone::left_shoulder:
    case Bone::left_leg: case Bone::left_upleg: return Side::left;
    case Bone::right_hand: case Bone::right_forearm: case Bone::right_arm: case Bone::right_shoulder:
    case Bone::right_leg: case Bone::right_upleg: return Side::right;
    default: return Side::both;
    }
}
// Adds one physics step's contacts. A body that only touches the skater's own board hit nothing.
inline void add(Fall& fall, const skater_body::Contacts& contacts) noexcept {
    for (std::size_t i = 0; i < skater_body::count; ++i) {
        const auto& body = contacts.bodies[i];
        const auto side = side_of(static_cast<skater_body::Bone>(i));
        if (side == Side::both || !body.touching || (body.hit.board && !body.hit.world && !body.hit.vehicle)) continue;
        const float hit = body.impact + game::length(body.slide);
        (side == Side::left ? fall.left : fall.right) += hit;
    }
}
// The side that took the fall: the one whose limbs took three fifths of it or more. A fall the
// limbs hardly felt, or felt alike, is on both.
inline Side side(const Fall& fall) noexcept {
    const float total = fall.left + fall.right;
    if (!(total >= 2.0f)) return Side::both;
    const float share = fall.left / total;
    return share >= 0.6f ? Side::left : share <= 0.4f ? Side::right : Side::both;
}

// ---- the session --------------------------------------------------------------------------

// The level `bails` give a part, when the left (or the right) side took more of them.
constexpr int level_for(Part part, unsigned bails, bool left_first) noexcept {
    if (part == Part::face) return bails >= steps[2] + face_later ? 2 : bails >= steps[2] ? 1 : 0;
    const unsigned later = (left(part) == left_first ? 0 : other_side_later) + (leg(part) ? 0 : arm_later);
    if (bails < later) return 0;
    const unsigned count = bails - later;
    return count >= steps[2] ? 3 : count >= steps[1] ? 2 : count >= steps[0] ? 1 : 0;
}
// Bails counted since the skater was last clean, and the marks they left. A mark never gets
// smaller: only healing takes it off.
struct Tally {
    unsigned bails{};
    float left{}, right{}; // falls on each side; one on both counts half for each
    bool last_left{};      // the side of the last one-sided fall: it breaks a tie
    std::array<int, part_count> levels{};
};
constexpr bool left_first(const Tally& tally) noexcept {
    return tally.left > tally.right || (tally.left == tally.right && tally.last_left);
}
constexpr void grow(Tally& tally) noexcept {
    const bool first = left_first(tally);
    for (const auto part : parts)
        tally.levels[index(part)] = (std::max)(tally.levels[index(part)], level_for(part, tally.bails, first));
}
constexpr void count(Tally& tally, Side side) noexcept {
    ++tally.bails;
    if (side == Side::both) {
        tally.left += 0.5f;
        tally.right += 0.5f;
    } else {
        (side == Side::left ? tally.left : tally.right) += 1.0f;
        tally.last_left = side == Side::left;
    }
    grow(tally);
}

// ---- the items ----------------------------------------------------------------------------

// The marks are cosmetic items of a content mod: tattoo items for the limbs, moles items for the
// face, found by the end of their names: RoadRash_<Set>_[<design>_]<level>[_Dry|_Blood].
//   Set     Leg, Arm, Face; LegR is the left leg's set mirrored, for the right leg: a decal sits
//           on a limb like a sticker read from the front, so only a mirrored one puts the marks
//           of the left leg's outer side on the right leg's outer side.
//   design  nothing for the first, B_ and C_ for the others: each part wears one design for as
//           long as its marks last.
//   level   1 .. the part's worst; the worst comes dry (scabs) and with fresh blood.
inline constexpr std::string_view item_prefix = "roadrash_";
inline constexpr std::size_t design_count = 3;
inline constexpr std::array<std::string_view, design_count> design_letters{"", "b_", "c_"};
// In lower case, as names are compared. `mirrored` asks for the right leg's own set.
inline std::string item_name(Part part, bool mirrored, std::size_t design, int level, bool blood) {
    std::string name(item_prefix);
    name += part == Part::face ? "face_" : !leg(part) ? "arm_" : mirrored ? "legr_" : "leg_";
    name += design_letters[design < design_count ? design : 0];
    name += std::to_string(level);
    if (level >= worst(part)) name += blood ? "_blood" : "_dry";
    return name;
}

// ---- where a limb's decal goes ------------------------------------------------------------

// A tattoo slot places its item with five numbers: x across and y up the limb's piece of the
// body's tattoo layout (0..1), its size, its turn (0.5 is upright) and one unused.
using Placement = std::array<float, 5>;
// The middle of the kneecap or the point of the elbow on a left limb (the right one mirrors x),
// how much y changes per centimetre down the limb, and how many centimetres a size of 1 covers.
struct Limb {
    float x, y, y_per_cm, cm_per_size;
};
inline constexpr Limb leg_spot{0.37f, 0.59f, 0.01073f, 76.0f}, arm_spot{0.635f, 0.505f, 0.01192f, 51.0f};
// A level's picture covers `side` centimetres with its centre `down` centimetres below that point.
struct Cut {
    float side, down;
};
inline constexpr std::array<Cut, 3> leg_cuts{{{20, 0}, {27, 2}, {38, 7}}}, arm_cuts{{{16, 0}, {24, 3}, {46, 16}}};
constexpr Placement placement(Part part, int level) noexcept {
    const auto& spot = leg(part) ? leg_spot : arm_spot;
    const auto& cut = (leg(part) ? leg_cuts : arm_cuts)[static_cast<std::size_t>(std::clamp(level, 1, worst_limb) - 1)];
    return {left(part) ? spot.x : 1.0f - spot.x, spot.y - cut.down * spot.y_per_cm, cut.side / spot.cm_per_size, 0.5f, 0.5f};
}
// A slot's parameters are words that hold floats. The game works them out again when it updates the
// skater: what it hands back is as good as what it was given, but not equal to the last bit.
inline bool same_parameters(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i] && !(std::abs(std::bit_cast<float>(a[i]) - std::bit_cast<float>(b[i])) < 0.002f)) return false;
    return true;
}
}
