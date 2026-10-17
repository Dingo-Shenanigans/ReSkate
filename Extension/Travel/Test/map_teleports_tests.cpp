// Map teleport zones' rules, with made-up zones: riding through a thin panel, where and how the
// skater comes out, and two zones that send to each other.
#include "Extension/Travel/map_teleports_model.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dingosdk::map_teleports;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b, float within = 1e-3f) { return std::abs(a - b) < within; }
bool near(const Vec3& a, const Vec3& b, float within = 1e-3f) {
    return near(a[0], b[0], within) && near(a[1], b[1], within) && near(a[2], b[2], within);
}
// A panel 4 m wide and 3 m high, 2 cm thick, across the z axis at z = 10.
Zone panel(float yaw = 0) {
    Zone zone;
    zone.name = "pipe";
    zone.center = {0, 1.5f, 10};
    zone.half = {2, 1.5f, 0.01f};
    zone.yaw = yaw;
    zone.target = {100, 5, -50};
    zone.heading = 90;
    return zone;
}

void a_thin_panel_is_hit_between_two_looks() {
    const auto zone = panel();
    check(!inside(zone, {0, 1, 9.5f}) && !inside(zone, {0, 1, 10.5f}), "Neither look is inside the panel");
    check(crossed(zone, {0, 1, 9.5f}, {0, 1, 10.5f}), "Riding through the panel was missed");
    check(crossed(zone, {0, 1, 10.5f}, {0, 1, 9.5f}), "Riding back through the panel was missed");
    check(!crossed(zone, {3, 1, 9.5f}, {3, 1, 10.5f}), "Passing beside the panel fired");
    check(!crossed(zone, {0, 4, 9.5f}, {0, 4, 10.5f}), "Jumping over the panel fired");
    check(!crossed(zone, {0, 1, 8}, {0, 1, 9.9f}), "Stopping short of the panel fired");
}

void a_turned_box_turns_its_sides() {
    // Turned 90 degrees the panel lies along z, across x.
    const auto zone = panel(90);
    check(crossed(zone, {-0.5f, 1, 10}, {0.5f, 1, 10}), "Riding across the turned panel was missed");
    check(!crossed(zone, {1, 1, 9.5f}, {1, 1, 10.5f}), "The turned panel kept its first sides");
    check(inside(zone, {0, 1, 11.9f}) && !inside(zone, {0, 1, 12.1f}), "The turned panel is not 4 m long along z");
}

void the_skater_leaves_along_the_spots_heading_at_its_own_speed() {
    const auto zone = panel();
    // Rolling forward along +z at 12 m/s, falling at 1 m/s.
    const auto out = exit_for(zone, 0, {0, -1, 12});
    check(out.carry, "Speed was not carried");
    check(near(out.position, zone.target), "The skater came out somewhere else");
    check(near(out.heading, 90), "The skater does not face the spot's heading");
    check(near(out.velocity, {12, -1, 0}), "The speed was not turned onto the spot's heading");
    check(near(ground_speed(out.velocity), 12), "The speed changed");
}

void fakie_stays_fakie_and_an_angle_stays_an_angle() {
    const auto zone = panel();
    // Rolling along +z but facing -z: fakie.
    const auto fakie = exit_for(zone, 180, {0, 0, 8});
    check(near(travel_heading(fakie.velocity), 90), "Fakie travel does not follow the spot");
    check(near(fakie.heading, 270), "Fakie was not kept");
    // Facing 20 degrees off the line.
    const auto angled = exit_for(zone, 20, {0, 0, 8});
    check(near(angled.heading, 110), "The angle to the line was not kept");
}

void a_still_skater_faces_the_spot_and_gets_no_speed() {
    auto zone = panel();
    const auto still = exit_for(zone, 45, {0.1f, 0, 0.1f});
    check(!still.carry && near(still.heading, 90), "A still skater was not turned to the spot");
    zone.keep_speed = false;
    const auto off = exit_for(zone, 0, {0, 0, 12});
    check(!off.carry && near(off.heading, 90), "A zone without kept speed carried it");
}

void a_zone_without_orient_keeps_the_skaters_line() {
    auto zone = panel();
    zone.orient = false;
    const auto out = exit_for(zone, 30, {0, 0, 12});
    check(out.carry && near(out.heading, 30), "The skater was turned");
    check(near(out.velocity, {0, 0, 12}), "The skater's line was turned");
}

void two_zones_that_send_to_each_other_do_not_bounce() {
    auto a = panel();
    auto b = panel();
    b.center = {100, 1.5f, -50};
    b.target = a.center; // b sends back into a
    a.target = b.center; // a sends into b
    const std::vector<Zone> zones{a, b};
    Tracker tracker;
    std::uint64_t now = 1000;
    check(!tracker.step(zones, {0, 1, 9}, now), "The first look fired");
    const auto fired = tracker.step(zones, {0, 1, 11}, now += 16);
    check(fired && *fired == 0, "Riding through zone a was missed");
    tracker.teleported(zones, a.target, now);
    // Out inside zone b, still inside it after the settle time: b holds.
    check(!tracker.step(zones, {100, 1.5f, -50}, now += 16), "The zone fired before the skater settled");
    check(!tracker.step(zones, {100, 1.5f, -50.005f}, now += 1000), "The zone the skater came out in fired");
    // Leaving b, then riding back through it, fires it.
    check(!tracker.step(zones, {100, 1.5f, -48}, now += 16), "Leaving the zone fired");
    const auto back = tracker.step(zones, {100, 1.5f, -51}, now += 16);
    check(back && *back == 1, "Riding back through zone b was missed");
}

void a_jump_across_the_map_is_not_a_ride() {
    const std::vector<Zone> zones{panel()};
    Tracker tracker;
    (void)tracker.step(zones, {0, 1, -100}, 0);
    check(!tracker.step(zones, {0, 1, 100}, 16), "A respawn across the zone fired it");
}

void bad_zones_are_refused() {
    auto zone = panel();
    check(valid(zone), "A good zone was refused");
    zone.half[2] = 0;
    check(!valid(zone), "A flat box was taken");
    zone = panel();
    zone.target[1] = std::nanf("");
    check(!valid(zone), "A target that is not a number was taken");
}
}

int main() {
    try {
        a_thin_panel_is_hit_between_two_looks();
        a_turned_box_turns_its_sides();
        the_skater_leaves_along_the_spots_heading_at_its_own_speed();
        fakie_stays_fakie_and_an_angle_stays_an_angle();
        a_still_skater_faces_the_spot_and_gets_no_speed();
        a_zone_without_orient_keeps_the_skaters_line();
        two_zones_that_send_to_each_other_do_not_bounce();
        a_jump_across_the_map_is_not_a_ride();
        bad_zones_are_refused();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "map teleports: all checks passed\n";
    return 0;
}
