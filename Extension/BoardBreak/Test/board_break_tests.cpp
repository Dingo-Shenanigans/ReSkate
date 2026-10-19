// Board Break's rules, apart from the game: what a break leaves of a board, how likely a slam
// breaks it, and where the parts of a broken board lie.
#include "Extension/BoardBreak/board_break_model.h"
#include "Extension/BoardBreak/board_break_feel.h"
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

using namespace dingosdk::board_break;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b, float within = 1e-4f) { return std::abs(a - b) < within; }
bool near(Vec a, Vec b, float within = 1e-4f) { return length(a - b) < within; }
// Where a point of a part ends up after the part's motion.
Vec moved(const Rigid& motion, Vec point) { return turn(motion.facing, point) + motion.at; }
constexpr Quat level{};
// The rig's mount: a quarter turn about the upright axis.
const Quat mount = about({0, 1, 0}, 1.5707963f);

void a_break_leaves_two_pieces_or_one_that_hangs_together() {
    for (const auto place : {Place::nose, Place::mid, Place::tail}) {
        const auto clean = broken_at(place, false);
        check(clean.hinge == Hinge::none && clean.loose, "A clean break has a piece that comes off");
        check((clean.kept | clean.loose) == whole_board && !(clean.kept & clean.loose), "and the two are the whole board, each part once");
        const auto joined = broken_at(place, true);
        check(joined.kept == whole_board && !joined.loose && joined.hinge != Hinge::none, "Held by the grip tape, nothing comes off");
    }
    check(broken_at(Place::nose, false).loose == nose && broken_at(Place::tail, false).loose == tail, "An end comes off by itself");
    const auto halves = broken_at(Place::mid, false);
    check(halves.kept == (nose | front | front_truck) && halves.loose == (back | tail | back_truck),
          "In the middle the board keeps its front half, and each half its truck");
    check(broken_at(Place::nose, true).hinge == Hinge::nose && broken_at(Place::mid, true).hinge == Hinge::fold &&
              broken_at(Place::tail, true).hinge == Hinge::tail,
          "and each place hangs together its own way");
}

void a_harder_slam_breaks_a_board_more_often() {
    check(break_chance(10.0f, 10.0f) == 1.0f && break_chance(30.0f, 10.0f) == 1.0f, "A heavy slam always breaks it");
    check(break_chance(0.0f, 10.0f) == 0.0f && break_chance(6.0f, 10.0f) == 0.0f, "A light one never does");
    check(near(break_chance(8.0f, 10.0f), 0.5f), "and in between it rises smoothly");
    float before{};
    for (float force = 0; force <= 12.0f; force += 0.5f) {
        const auto chance = break_chance(force, 10.0f);
        check(chance >= before && chance <= 1.0f, "It never falls with more force");
        before = chance;
    }
}

void the_size_of_a_break_picks_its_sound() {
    check(break_size(Place::mid, false, 0.0f) == Size::heavy, "A board that comes apart in the middle is the biggest break");
    check(break_size(Place::mid, true, 0.9f) == Size::heavy && break_size(Place::mid, true, 0.4f) == Size::medium,
          "A fold right down is as big, a shallow one in between");
    check(break_size(Place::nose, false, 0.0f) == Size::medium && break_size(Place::tail, false, 0.0f) == Size::medium, "An end that comes off is in between");
    check(break_size(Place::nose, true, 0.0f) == Size::light && break_size(Place::tail, true, 0.0f) == Size::light, "One that hangs on is a small one");
}

void a_whole_board_keeps_its_stretches_away() {
    const auto bones = bones_of({});
    for (std::size_t i = 0; i < 3; ++i) check(near(bones[i].at, away), "A stretch that does not show goes far away");
    check(near(bones[3].at, Vec{}) && near(bones[3].facing.w, 1.0f), "and the tail's, which hangs on the front's, goes with that one");
}

void the_tail_is_placed_from_the_front_stretch() {
    // The tail's bone hangs on the front stretch's: wherever the two lie, the front's motion
    // followed by what goes into the tail's bone is the tail's own motion.
    const Rigid front_motion{about({1, 0, 0}, 0.3f), {0.01f, 0.02f, -0.03f}};
    const Rigid tail_motion{unit(about({0, 1, 0}, 1.1f) * about({1, 0, 0}, -0.4f)), {0.5f, -0.2f, 0.7f}};
    for (const bool front_shown : {true, false}) {
        std::array<Stretch, 4> stretch{};
        stretch[1] = {front_shown, front_motion};
        stretch[3] = {true, tail_motion};
        const auto bones = bones_of(stretch);
        const Rigid parent = front_shown ? front_motion : Rigid{{}, away};
        for (const Vec point : {Vec{0, 0.09f, -0.3f}, Vec{0.1f, 0.1f, -0.4f}})
            check(near(moved(parent, moved(bones[3], point)), moved(tail_motion, point), 2e-3f), "The tail lies where its piece does");
    }
}

void a_piece_shows_only_its_own_stretches() {
    const auto clean = broken_at(Place::mid, false);
    const auto layout = lay_out(clean.kept, clean.hinge, 0.0f, 1.0f, mount);
    check(layout.stretch[0].shown && layout.stretch[1].shown && !layout.stretch[2].shown && !layout.stretch[3].shown,
          "The front half is the nose and the front stretch");
    check(layout.front_truck && !layout.back_truck, "on its front truck");
    const auto bones = bones_of(layout.stretch);
    check(near(bones[2].at, away), "and the back stretch is nowhere to be seen");
    // Half a board on one truck tips onto its broken end while it is the right way up, and not on its grip.
    const auto broken_end = moved(layout.stretch[1].motion, turn(mount, Vec{0, 0.09f, mid_row}));
    check(broken_end.y < 0.09f - 0.03f, "The half tips onto its broken end");
    const auto upside_down = lay_out(clean.kept, clean.hinge, 0.0f, -1.0f, mount);
    check(near(upside_down.stretch[1].motion.at, Vec{}), "Lying on its grip it stays as it is");
}

void a_folded_board_meets_at_the_break() {
    for (const float depth : {0.2f, 0.6f, 1.0f}) {
        Rigid front_half, back_half;
        folded(depth, front_half, back_half);
        // The grip of both halves at the break: the same height, a crack's width apart.
        const Vec grip{0, 0.095f, mid_row};
        const auto ahead = moved(front_half, grip), behind = moved(back_half, grip);
        check(near(ahead.y, behind.y, 1e-3f), "Both halves' grip is at one height at the break");
        check(near(ahead.z - behind.z, crack, 1e-3f), "with the crack between them");
        check(ahead.y < grip.y, "and the break has sagged");
        // Each half turns about its own axle: the wheels stay where they were, but for the slide along the board.
        check(near(moved(front_half, {0, axle_height, front_axle}).y, axle_height, 1e-4f) &&
                  near(moved(back_half, {0, axle_height, back_axle}).y, axle_height, 1e-4f),
              "The axles keep their height");
    }
    const auto whole = lay_out(whole_board, Hinge::fold, 0.8f, 1.0f, mount);
    for (const auto& one : whole.stretch) check(one.shown, "A folded board shows all four stretches");
    check(!near(whole.stretch[1].motion.at, whole.stretch[2].motion.at), "its two halves moved apart");
    check(near(whole.stretch[0].motion.at, whole.stretch[1].motion.at) && near(whole.stretch[2].motion.at, whole.stretch[3].motion.at),
          "and each end with its half");
}

void a_hanging_end_turns_about_the_break() {
    const auto layout = lay_out(whole_board, Hinge::nose, 0.5f, 1.0f, mount);
    check(near(layout.stretch[1].motion.at, Vec{}) && near(layout.stretch[2].motion.at, Vec{}) && near(layout.stretch[3].motion.at, Vec{}),
          "The rest of the board stays where it is");
    const auto tip = moved(layout.stretch[0].motion, turn(mount, Vec{0, 0.14f, 0.41f}));
    check(tip.y < 0.14f - 0.05f, "and the nose droops");
    const auto hinge = moved(hung_on_grip(nose_row, 1.0f, 0.5f), {0, grip_height - 0.004f, nose_row});
    check(near(hinge, Vec{0, grip_height - 0.004f, nose_row + crack * 0.5f}), "It turns about the break line, a hair away from the board");
}

void a_truck_goes_with_its_piece() {
    // The deck's bone is moved away to hide the real deck, and the truck hangs on it: what goes into
    // the truck's bone takes that move back and adds the piece's motion.
    const Vec mount_at{0, 0.0856f, 0};
    const auto still = truck_bone({}, mount, mount_at, level, front_truck_place);
    check(near(still.at + away, front_truck_place) && near(still.facing.w, 1.0f), "A piece that has not moved leaves its truck where it was");
    const Rigid lifted{{}, {0, 0.25f, 0}};
    const auto up = truck_bone(lifted, mount, mount_at, level, front_truck_place);
    check(near(up.at + away, front_truck_place + Vec{0, 0.25f, 0}), "A piece that is lifted takes its truck up with it");
}
void a_baseplate_stays_on_the_deck_while_its_truck_steers() {
    // A baseplate sits on the truck's effector, a child of the truck's bone. Whatever the truck's
    // bone does, the truck's bone followed by the effector's is what it is at rest.
    for (const auto& rest : {back_truck_rest, front_truck_rest}) {
        const auto still = baseplate_bone({rest.facing, rest.at}, rest);
        check(near(still.at, rest.effector) && near(std::abs(still.facing.w), 1.0f), "At rest the effector is where the rig has it");
        const Vec bolt{0.03f, 0.08f, rest.at.z + 0.02f};
        const auto on_deck = moved({rest.facing, rest.at}, moved({{}, rest.effector}, bolt));
        for (const float lean : {-0.4f, 0.15f, 0.6f}) {
            const Rigid steered{unit(rest.facing * about({0, 0, 1}, lean)), rest.at};
            const auto effector = baseplate_bone(steered, rest);
            check(near(moved(steered, moved(effector, bolt)), on_deck, 1e-4f), "Steered, the baseplate has not moved on the deck");
        }
    }
}

void the_settings_keep_to_their_ranges() {
    Settings s;
    check(set_setting_value(s, "sensitivity", 9999) && s.sensitivity == sensitivity_max, "A percent is kept within its range");
    check(set_setting_value(s, "always", 5) && s.always && setting_value(s, "always") == 1, "a switch is on or off");
    check(set_setting_value(s, "joined", -3) && s.joined == 0, "and nothing goes below its least");
    check(!set_setting_value(s, "nonsense", 1) && setting_value(s, "nonsense") == -1, "An unknown name is no setting");
    for (const auto& info : setting_info) check(setting_value(s, info.name) >= 0, "Every setting has a value");
}

void a_worn_or_touchy_board_breaks_sooner() {
    check(slam_chance(heavy_slam, 100, 0.0f) == 1.0f, "A heavy slam always breaks a new board");
    check(slam_chance(6.0f, 100, 0.0f) < slam_chance(6.0f, 100, 1.0f), "A worn board breaks from less");
    check(slam_chance(heavy_slam * 0.5f, 200, 0.0f) == 1.0f, "At 200% half the force always breaks it");
    check(slam_chance(8.0f, 50, 0.0f) < slam_chance(8.0f, 100, 0.0f), "at 50% it takes more");
    check(slam_chance(2.0f, 100, 0.0f) == 0.0f && slam_chance(2.0f, 100, 0.6f) == 0.0f, "A stumble never breaks a board that is not done");
    check(slam_chance(0.0f, 400, 1.0f) == 0.0f, "nor does a bail whose force is not known, at any setting");
}

void every_hit_wears_a_board() {
    check(landing_wear(3.0f) == 0.0f && landing_wear(5.0f) > 0.0f && landing_wear(9.0f) > landing_wear(5.0f), "Harder landings wear more, light ones none");
    check(rail_wear(3.0f, true) > rail_wear(3.0f, false), "A slide wears the deck more than a grind on the trucks");
    check(grind_wear_per_second(true) > grind_wear_per_second(false), "and sliding more than grinding");
    check(slam_wear(2.0f) == 0.0f && slam_wear(12.0f) > slam_wear(7.0f), "A stumble costs nothing, a hard slam a lot");
    check(add_wear(0.5f, 0.1f, 0) == 0.5f && near(add_wear(0.5f, 0.1f, 200), 0.7f) && add_wear(0.95f, 1.0f, 100) == 1.0f,
          "Wear goes at the player's rate, and never past worn out");
}

void a_worn_board_handles_worse() {
    check(handling(0.0f) == Handling{} && handling(0.2f) == Handling{}, "A board under a fifth worn handles as new");
    const auto half = handling(0.6f), done = handling(1.0f);
    check(half.steering < 1.0f && done.steering < half.steering && done.spring < half.spring && done.twist > half.twist,
          "The more worn, the lazier the turn and the softer the trucks");
    check(done.steering > 0.5f && done.spring > 0.2f, "but it can still be ridden");
    check(!gives_way_on_landing(20.0f, 400, done_wear - 0.01f), "A board that is not done never gives way on a landing");
    check(!gives_way_on_landing(6.0f, 100, done_wear) && gives_way_on_landing(6.5f, 100, done_wear),
          "A board just done goes on a big drop");
    check(!gives_way_on_landing(3.0f, 100, 1.0f) && gives_way_on_landing(3.5f, 100, 1.0f),
          "one worn right out on any real drop, but not on an ollie on flat ground");
    check(gives_way_on_landing(4.0f, 200, done_wear) && !gives_way_on_landing(4.0f, 100, done_wear), "Sensitivity makes it touchier");
}

void a_board_across_its_way_slides() {
    const Vec level_up{0, 1, 0}, ahead{0, 0, 1}, across{1, 0, 0}, upside_down{0, -1, 0}, going{0, -1, 5};
    check(contact_of(ahead, level_up, going) == Contact::grind, "Along the rail it grinds on its trucks");
    check(contact_of(across, level_up, going) == Contact::slide, "Across it, it slides on the deck");
    check(contact_of(across, upside_down, going) == Contact::darkslide, "On its grip it is a darkslide");
    check(contact_of(across, level_up, {0, -2, 0.1f}) == Contact::grind, "A board that hardly moves along counts as grinding");
    check(rail_chance(9.0f, Contact::grind, 100, 0.0f) == 0.0f, "A grind on the trucks never snaps the board");
    check(rail_chance(2.0f, Contact::slide, 100, 0.0f) == 0.0f && rail_chance(6.0f, Contact::slide, 100, 0.0f) == 1.0f,
          "A hop onto a rail is safe, a drop onto it snaps the board");
    check(rail_chance(3.5f, Contact::darkslide, 100, 0.0f) > rail_chance(3.5f, Contact::slide, 100, 0.0f), "a darkslide sooner");
}

void a_board_breaks_where_it_hit() {
    check(place_of_break(0.8f, false, 0.1f) == Place::nose && place_of_break(-0.8f, false, 0.1f) == Place::tail, "Nose first at the nose, tail first at the tail");
    check(place_of_break(0.8f, true, 0.1f) == Place::mid, "Across a rail in the middle");
    check(place_of_break(0.0f, false, 0.2f) == Place::mid && place_of_break(0.0f, false, 0.9f) == Place::tail, "Flat mostly in the middle");
    check(body_loudness(2.0f) == 0.0f && body_loudness(body_full) == 1.0f && body_loudness(5.0f) < body_loudness(9.0f), "Harder hits sound louder");
}
} // namespace

int main() {
    try {
        a_break_leaves_two_pieces_or_one_that_hangs_together();
        a_harder_slam_breaks_a_board_more_often();
        the_size_of_a_break_picks_its_sound();
        a_whole_board_keeps_its_stretches_away();
        the_tail_is_placed_from_the_front_stretch();
        a_piece_shows_only_its_own_stretches();
        a_folded_board_meets_at_the_break();
        a_hanging_end_turns_about_the_break();
        a_truck_goes_with_its_piece();
        a_baseplate_stays_on_the_deck_while_its_truck_steers();
        the_settings_keep_to_their_ranges();
        a_worn_or_touchy_board_breaks_sooner();
        every_hit_wears_a_board();
        a_worn_board_handles_worse();
        a_board_across_its_way_slides();
        a_board_breaks_where_it_hit();
        std::cout << "board break tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "board break test failed: " << error.what() << '\n';
        return 1;
    }
}
