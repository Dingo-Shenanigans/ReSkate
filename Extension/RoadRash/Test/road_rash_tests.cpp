// Road Rash's rules, with made-up falls: the side of a fall, the marks a count of bails leaves, the
// names of the items that draw them and where a limb's decal goes.
#include "Extension/RoadRash/road_rash_model.h"
#include <bit>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::road_rash;
using skater_body::Bone;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

// One physics step in which `bone` hits the ground this hard and slides along it this fast.
skater_body::Contacts hit(Bone bone, float impact, float slide = 0.0f) {
    skater_body::Contacts contacts;
    auto& body = contacts.bodies[skater_body::index(bone)];
    body.touching = true;
    body.impact = impact;
    body.slide = {slide, 0.0f, 0.0f};
    body.hit.world = true;
    return contacts;
}
int level(const Tally& tally, Part part) { return tally.levels[index(part)]; }
Tally after(unsigned bails, Side side) {
    Tally tally;
    for (unsigned i = 0; i < bails; ++i) count(tally, side);
    return tally;
}

void a_fall_is_on_the_side_whose_limbs_took_it() {
    Fall fall;
    check(side(fall) == Side::both, "A fall nothing felt is on neither side");
    add(fall, hit(Bone::left_forearm, 3.0f, 2.0f));
    add(fall, hit(Bone::left_upleg, 2.0f));
    add(fall, hit(Bone::right_hand, 1.0f));
    check(near(fall.left, 7.0f) && near(fall.right, 1.0f), "Impact and slide of each side's limbs add up");
    check(side(fall) == Side::left, "The left limbs took most of it");
    Fall other;
    add(other, hit(Bone::right_leg, 4.0f));
    add(other, hit(Bone::right_arm, 1.0f, 3.0f));
    check(side(other) == Side::right, "and the right ones of this one");
    Fall even;
    add(even, hit(Bone::left_leg, 3.0f));
    add(even, hit(Bone::right_leg, 2.6f));
    check(side(even) == Side::both, "A fall both sides felt alike is on both");
    Fall light;
    add(light, hit(Bone::left_hand, 0.5f));
    check(side(light) == Side::both, "A touch is no fall on a side");
}

void feet_trunk_head_and_the_own_board_count_for_no_side() {
    Fall fall;
    for (const auto bone : {Bone::left_foot, Bone::left_toe, Bone::right_foot, Bone::right_toe, Bone::hips, Bone::spine2, Bone::neck1})
        add(fall, hit(bone, 9.0f, 9.0f));
    check(fall.left == 0.0f && fall.right == 0.0f, "Feet stand on the ground all the time; trunk and head have no side");
    auto board = hit(Bone::left_hand, 5.0f);
    board.bodies[skater_body::index(Bone::left_hand)].hit = {};
    board.bodies[skater_body::index(Bone::left_hand)].hit.board = true;
    add(fall, board);
    check(fall.left == 0.0f, "A hand on the skater's own board hit nothing");
    auto untouched = hit(Bone::right_leg, 5.0f);
    untouched.bodies[skater_body::index(Bone::right_leg)].touching = false;
    add(fall, untouched);
    check(fall.right == 0.0f, "A body that touches nothing hit nothing");
}

void marks_come_with_the_bails() {
    check(after(9, Side::left).levels == std::array<int, part_count>{}, "Nine bails leave no mark");
    const auto ten = after(10, Side::left);
    check(level(ten, Part::left_leg) == 1 && level(ten, Part::left_arm) == 0 && level(ten, Part::right_leg) == 0,
        "The tenth marks the leg of the side that took them");
    const auto eleven = after(11, Side::left);
    check(level(eleven, Part::left_arm) == 1 && level(eleven, Part::right_leg) == 0, "the arm follows one bail later");
    const auto fourteen = after(14, Side::left);
    check(level(fourteen, Part::left_leg) == 2 && level(fourteen, Part::right_leg) == 1 && level(fourteen, Part::right_arm) == 0,
        "At fourteen the leg is worse and the other side's leg starts");
    const auto twenty = after(20, Side::left);
    check(level(twenty, Part::left_leg) == 3 && level(twenty, Part::left_arm) == 2 && level(twenty, Part::face) == 1,
        "At twenty the leg is at its worst and the face shows it");
    const auto all = after(25, Side::left);
    check(level(all, Part::left_leg) == 3 && level(all, Part::left_arm) == 3 && level(all, Part::right_leg) == 3 &&
        level(all, Part::right_arm) == 3 && level(all, Part::face) == 2, "At twenty-five everything is at its worst");
    check(after(200, Side::both).levels == all.levels, "and stays there");
}

void the_side_that_took_more_goes_first_and_no_mark_gets_smaller() {
    const auto right = after(10, Side::right);
    check(level(right, Part::right_leg) == 1 && level(right, Part::left_leg) == 0, "Falls on the right mark the right leg first");
    // Ten on the left, then the right side overtakes it: the left keeps what it has.
    auto tally = after(10, Side::left);
    for (int i = 0; i < 11; ++i) count(tally, Side::right);
    check(!left_first(tally) && tally.bails == 21, "The right side took more by now");
    check(level(tally, Part::right_leg) == 3 && level(tally, Part::left_leg) >= 2, "It is ahead now, and the left leg kept its marks");
    // Falls on both sides count half for each; the last one-sided fall breaks the tie.
    Tally even;
    count(even, Side::right);
    count(even, Side::left);
    for (int i = 0; i < 8; ++i) count(even, Side::both);
    check(near(even.left, even.right) && left_first(even) && level(even, Part::left_leg) == 1 && level(even, Part::right_leg) == 0,
        "A tie goes to the side of the last one-sided fall");
}

void items_are_named_by_set_design_and_level() {
    check(item_name(Part::left_leg, false, 0, 1, true) == "roadrash_leg_1", "The first design has no letter");
    check(item_name(Part::right_leg, true, 0, 2, true) == "roadrash_legr_2", "The right leg has its own, mirrored set");
    check(item_name(Part::right_leg, false, 0, 2, true) == "roadrash_leg_2", "or shares the left one's");
    check(item_name(Part::left_arm, false, 1, 3, true) == "roadrash_arm_b_3_blood", "The worst level comes with blood");
    check(item_name(Part::right_arm, true, 2, 3, false) == "roadrash_arm_c_3_dry", "or dry; arms share one set");
    check(item_name(Part::face, false, 0, 1, true) == "roadrash_face_1" && item_name(Part::face, false, 1, 2, false) == "roadrash_face_b_2_dry",
        "The face's worst level is its second");
    check(item_name(Part::left_leg, false, 9, 1, true) == "roadrash_leg_1", "An unknown design is the first");
}

void a_limbs_decal_sits_on_its_knee_or_elbow() {
    const auto left = placement(Part::left_leg, 1), right = placement(Part::right_leg, 1);
    check(near(left[0] + right[0], 1.0f) && near(left[1], right[1]) && near(left[2], right[2]), "The right limb mirrors the left across the layout");
    check(near(left[2], 20.0f / 76.0f) && near(left[3], 0.5f), "A level's size is its centimetres over the limb's scale, upright");
    const auto worst = placement(Part::left_leg, 3);
    check(worst[2] > left[2] && worst[1] < left[1], "A worse level is bigger and reaches further down the limb");
    check(placement(Part::left_arm, 3)[2] > placement(Part::left_arm, 1)[2] && placement(Part::left_arm, 9)[2] == placement(Part::left_arm, 3)[2],
        "Arms likewise; a level past the worst is the worst");
}

void parameters_the_game_worked_out_again_are_the_same() {
    const auto words = [](std::initializer_list<float> values) {
        std::vector<std::uint32_t> out;
        for (const float value : values) out.push_back(std::bit_cast<std::uint32_t>(value));
        return out;
    };
    const auto given = words({0.365f, 0.505f, 16.0f / 51.0f, 0.5f, 0.5f});
    auto back = given;
    back[2] = std::bit_cast<std::uint32_t>(80.0f / 255.0f) + 1; // the same size, one bit off
    check(same_parameters(given, given) && same_parameters(given, back), "A last bit of difference is no change");
    check(!same_parameters(given, words({0.365f, 0.515f, 16.0f / 51.0f, 0.5f, 0.5f})), "A centimetre down the limb is one");
    check(!same_parameters(given, words({0.365f, 0.505f, 16.0f / 51.0f, 0.5f})), "and so is another count of them");
}
}

int main() {
    try {
        a_fall_is_on_the_side_whose_limbs_took_it();
        feet_trunk_head_and_the_own_board_count_for_no_side();
        marks_come_with_the_bails();
        the_side_that_took_more_goes_first_and_no_mark_gets_smaller();
        items_are_named_by_set_design_and_level();
        a_limbs_decal_sits_on_its_knee_or_elbow();
        parameters_the_game_worked_out_again_are_the_same();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Road Rash tests passed.\n";
    return 0;
}
