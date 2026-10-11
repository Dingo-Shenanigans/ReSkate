// The Hall of Meat model, with made-up physics steps, and its score card.
#include "Extension/HallOfMeat/hall_of_meat_card.h"
#include "Extension/HallOfMeat/hall_of_meat_model.h"
#include "Extension/HallOfMeat/hall_of_meat_slow_motion.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::hall_of_meat;
using skater_body::Bone;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }
constexpr std::uint64_t t0 = 100000; // GetTickCount64() is never near 0

// The game stepping its physics at `speed` (1 full speed, break_game_speed in slow motion): each
// step as long in the game's time as the real time since the one before, times the speed.
struct Game : Tracker {
    float speed = 1.0f;
    std::uint64_t last{};
    bool step(std::uint64_t now, Step step, Summary* ended = nullptr) noexcept {
        step.seconds = last && now > last ? static_cast<float>(now - last) / 1000.0f * speed : 0.0f;
        last = now;
        return Tracker::step(now, step, ended);
    }
};

// A step of a skater in a ragdoll that touches nothing: a bail lying still.
Step lying() {
    Step step;
    step.ragdoll = true;
    return step;
}
// A step of a skater standing, or riding: no ragdoll.
Step standing_up() {
    Step step;
    step.ragdoll = false;
    return step;
}
// A ragdoll step in which `bone` hits the ground at `speed`.
Step hit(Bone bone, float speed, bool wipeout = false) {
    auto step = lying();
    step.wipeout = wipeout;
    auto& contact = step.body.bodies[skater_body::index(bone)];
    contact.touching = true;
    contact.impact = speed;
    contact.hit.world = true;
    return step;
}
// The same while riding: no ragdoll yet.
Step riding_hit(Bone bone, float speed) {
    auto step = hit(bone, speed);
    step.ragdoll = false;
    return step;
}
Step wipeout() {
    auto step = lying();
    step.wipeout = true;
    return step;
}
// A step in the air, hitting nothing: in a ragdoll, or on the board.
Step flying(bool ragdoll = true) {
    Step step;
    step.airborne = true;
    step.ragdoll = ragdoll;
    return step;
}
// A ragdoll step in which `bone` slides along the ground at `speed`, hitting nothing hard.
Step slide(Bone bone, float speed, bool wipeout = false) {
    auto step = lying();
    step.wipeout = wipeout;
    auto& contact = step.body.bodies[skater_body::index(bone)];
    contact.touching = true;
    contact.slide = {speed, 0, 0};
    contact.hit.world = true;
    return step;
}
// A ragdoll step at `speed`, touching nothing.
Step moving(float speed) {
    auto step = lying();
    step.velocity = game::Vec3{speed, 0, 0};
    return step;
}
float seconds(std::uint64_t ms) { return static_cast<float>(ms) / 1000.0f; }
// Steps `step` from `now` on, every 16 ms, for `ms`; returns the time after the last.
std::uint64_t keep(Game& tracker, std::uint64_t now, std::uint64_t ms, const Step& step) {
    for (const auto until = now + ms; now < until; now += 16) tracker.step(now, step);
    return now;
}

void impacts_count_only_during_a_bail() {
    Game tracker;
    check(!tracker.step(t0, riding_hit(Bone::neck1, 9.0f)), "a step without a bail ends nothing");
    check(tracker.view(t0).phase == Phase::riding, "nothing shows before a bail");
    const auto later = t0 + wipeout_after_impact_ms + 16;
    tracker.step(later, wipeout());
    check(tracker.phase(later) == Phase::bailing, "a wipeout starts a bail");
    check(tracker.view(later).injuries[skater_body::index(Bone::neck1)] == Injury::none,
        "an impact long before the bail is not counted");
}

void the_hit_that_causes_the_wipeout_counts() {
    // A drop from height: the thigh hits, the wipeout comes a step later.
    Game tracker;
    tracker.step(t0, riding_hit(Bone::right_upleg, 35.0f));
    tracker.step(t0 + 16, riding_hit(Bone::right_upleg, 12.0f));
    tracker.step(t0 + 32, wipeout());
    const auto view = tracker.view(t0 + 32);
    check(view.phase == Phase::bailing && view.injuries[skater_body::index(Bone::right_upleg)] == Injury::broken,
        "the hit before the wipeout breaks");
    check(view.tally.impacts == 1 && view.tally.hit_points == hit_points(35.0f), "at its hardest, once");
}

void nothing_shows_until_a_bone_is_hurt() {
    Game tracker;
    tracker.step(t0, wipeout());
    tracker.step(t0 + 16, hit(Bone::spine, hit_speed - 0.5f));
    check(tracker.phase(t0 + 16) == Phase::bailing && tracker.view(t0 + 16).phase == Phase::riding,
        "a bail that hurts nothing shows nothing");
    tracker.step(t0 + 32, hit(Bone::spine, hit_speed));
    const auto view = tracker.view(t0 + 32);
    check(view.phase == Phase::bailing && near(view.alpha, 1.0f) && view.tally.impacts == 1,
        "the first bruise brings the skeleton and the card");
    Game unhurt;
    unhurt.step(t0, hit(Bone::spine, hit_speed - 0.5f, true));
    Summary summary;
    check(unhurt.step(t0 + 16, standing_up(), &summary) && !summary.shown, "it ends unshown");
    check(unhurt.view(t0 + 32).phase == Phase::riding, "nor does it get a card");
}

void hard_hits_break_and_light_ones_bruise() {
    Game tracker;
    tracker.step(t0, hit(Bone::left_forearm, hit_speed + 0.1f, true));
    tracker.step(t0 + 16, hit(Bone::neck1, broken_speed + 0.1f));
    tracker.step(t0 + 32, hit(Bone::spine, hit_speed - 0.1f));
    tracker.step(t0 + 48, hit(Bone::left_forearm, 1.0f)); // a later, softer hit keeps the worst
    const auto view = tracker.view(t0 + 48);
    check(view.injuries[skater_body::index(Bone::left_forearm)] == Injury::hit, "a light hit bruises");
    check(view.injuries[skater_body::index(Bone::neck1)] == Injury::broken, "a hard hit breaks");
    check(view.injuries[skater_body::index(Bone::spine)] == Injury::none, "a graze below the threshold leaves no mark");
    check(view.flashes[skater_body::index(Bone::neck1)] > 0.9f, "a fresh hit flashes");
    check(tracker.view(t0 + 16 + flash_ms).flashes[skater_body::index(Bone::neck1)] == 0, "the flash fades");
}

void a_flump_starts_no_bail() {
    // Rolling along the ground and jumping off a height while rolling: a ragdoll slamming the ground,
    // which the game never wipes out.
    Game tracker;
    auto now = keep(tracker, t0, 1500, flying());
    for (int i = 0; i < 60; ++i, now += 16) {
        auto step = hit(i % 2 ? Bone::left_arm : Bone::spine, broken_speed + 5.0f);
        step.velocity = game::Vec3{8, -6, 0};
        tracker.step(now, step);
    }
    check(tracker.view(now).phase == Phase::riding && tracker.view(now).tally.score == 0, "nothing shows, nothing counts");
    check(!tracker.step(now, standing_up()), "and no bail ends");
}

void a_break_pulses_the_screen_edge() {
    Game tracker;
    tracker.step(t0, hit(Bone::left_hand, hit_speed + 1.0f, true));
    check(tracker.view(t0 + 16).break_pulse == 0.0f, "a bruise does not pulse");
    tracker.step(t0 + 16, hit(Bone::left_forearm, broken_speed + 1.0f));
    const auto broke = t0 + 16;
    check(near(tracker.view(broke + break_rise_ms / 2).break_pulse, 0.5f), "a break pulses up");
    check(near(tracker.view(broke + break_rise_ms).break_pulse, 1.0f), "to its full strength");
    check(near(tracker.view(broke + break_rise_ms + break_hold_ms - 16).break_pulse, 1.0f), "holds it");
    check(near(tracker.view(broke + break_rise_ms + break_hold_ms + break_fall_ms / 2).break_pulse, 0.5f), "and fades");
    check(tracker.view(broke + break_effect_ms).break_pulse == 0.0f, "then it is gone");
    tracker.step(broke + break_effect_ms, hit(Bone::left_forearm, broken_speed + 5.0f));
    check(tracker.view(broke + break_effect_ms + break_rise_ms).break_pulse == 0.0f, "a broken bone hit again does not pulse");
    tracker.step(broke + break_effect_ms + 16, hit(Bone::right_leg, broken_speed + 1.0f));
    check(near(tracker.view(broke + break_effect_ms + 16 + break_rise_ms).break_pulse, 1.0f), "the next break pulses again");
}

void a_break_slows_the_game_with_its_red_edge() {
    Game tracker;
    check(tracker.game_speed(t0) == 1.0f, "full speed without a bail");
    tracker.step(t0, hit(Bone::left_hand, hit_speed + 1.0f, true));
    check(tracker.game_speed(t0 + 16) == 1.0f, "a bruise keeps the speed");
    tracker.step(t0 + 16, hit(Bone::left_forearm, broken_speed + 1.0f));
    const auto broke = t0 + 16;
    check(tracker.game_speed(broke) == 1.0f && near(tracker.game_speed(broke + break_rise_ms), break_game_speed),
        "a break slows the game down");
    check(near(tracker.game_speed(broke + break_rise_ms + break_hold_ms - 16), break_game_speed), "holds it slow");
    check(near(tracker.game_speed(broke + break_rise_ms + break_hold_ms + break_fall_ms / 2), (1.0f + break_game_speed) / 2),
        "then lets it go");
    check(tracker.game_speed(broke + break_effect_ms) == 1.0f, "back to full speed as the red edge is gone");
    for (auto at = broke; at < broke + break_effect_ms; at += 10)
        check(near(tracker.game_speed(at), 1.0f - (1.0f - break_game_speed) * tracker.view(at).break_pulse),
            "the slow motion and the red edge are one");
    // A further break while it lets go slows the game again, with no jump on the way.
    const auto again = broke + break_rise_ms + break_hold_ms + break_fall_ms / 2;
    tracker.step(again, hit(Bone::right_leg, broken_speed + 1.0f));
    float previous = tracker.game_speed(again - 1), steepest = 0;
    for (auto at = again; at < again + break_rise_ms + 16; ++at) {
        const float speed = tracker.game_speed(at);
        steepest = std::max(steepest, std::abs(speed - previous));
        previous = speed;
    }
    check(steepest < 0.02f, "the speed never jumps");
    check(near(tracker.game_speed(again + break_rise_ms), break_game_speed), "the further break slows it down again");
    tracker.lose(again + 16);
    check(tracker.game_speed(again + 32) == 1.0f, "a skater gone takes the slow motion with them");
}

void a_break_just_before_the_bail_counts_from_the_bail() {
    // The wipeout comes a few steps after the impact that broke the bone: the slow motion and the
    // red edge run from the step that saw the break, not from the impact.
    Game tracker;
    tracker.step(t0, riding_hit(Bone::right_upleg, broken_speed + 5.0f));
    const auto wiped_out = t0 + wipeout_after_impact_ms - 16;
    tracker.step(wiped_out, wipeout());
    check(near(tracker.game_speed(wiped_out + break_rise_ms), break_game_speed), "the slow motion at its full strength");
    check(near(tracker.view(wiped_out + break_rise_ms).break_pulse, 1.0f), "the red edge is at its full strength");
}

void the_board_and_bad_values_are_ignored() {
    Game tracker;
    auto step = wipeout();
    step.body.bodies[skater_body::index(Bone::board_root)].impact = 20.0f;
    step.body.bodies[skater_body::index(Bone::hips)].impact = std::nanf("");
    step.body.bodies[skater_body::index(Bone::spine1)].impact = -8.0f;
    tracker.step(t0, step);
    for (const auto injury : tracker.view(t0).injuries) check(injury == Injury::none, "board, NaN and negative peaks hurt nothing");
}

void a_bail_lasts_until_the_skater_gets_up() {
    Game tracker;
    tracker.step(t0, hit(Bone::hips, hit_speed + 1.0f, true));
    // Rolling, then lying however long: the bail goes on, its card with it.
    auto now = keep(tracker, t0 + 16, 3000, hit(Bone::spine2, 2.0f));
    now = keep(tracker, now, 6000, lying());
    tracker.step(now, hit(Bone::neck1, hit_speed + 1.0f)); // a car finds the body on the ground
    now = keep(tracker, now + 16, 1000, lying());
    const auto down = tracker.view(now);
    check(down.phase == Phase::bailing && near(down.alpha, 1.0f) && down.tally.impacts == 2, "the bail and its card go on");
    Summary summary;
    check(tracker.step(now, standing_up(), &summary), "standing up ends the bail");
    check(near(summary.tally.seconds, seconds(now - t0)) && near(summary.peaks[skater_body::index(Bone::hips)], hit_speed + 1.0f) && summary.shown,
        "the summary holds the duration and the worst hits");
    const auto lingering = tracker.view(now + linger_ms - 16);
    check(lingering.phase == Phase::getting_up && near(lingering.alpha, 1.0f), "the skeleton and the card stay a while");
    const auto fading = tracker.view(now + linger_ms + fade_ms / 2);
    check(fading.phase == Phase::getting_up && near(fading.alpha, 0.5f), "then fade out together");
    check(fading.tally.impacts == 2, "with the bail's tally");
    check(tracker.view(now + linger_ms + fade_ms).phase == Phase::riding, "then they are gone");
    // The next wipeout starts afresh.
    const auto later = now + linger_ms + fade_ms + 16;
    tracker.step(later, wipeout());
    check(tracker.view(later).injuries[skater_body::index(Bone::hips)] == Injury::none, "a new bail starts clean");
}

void a_rest_ends_the_counting() {
    Game tracker;
    tracker.step(t0, hit(Bone::hips, hit_speed + 1.0f, true));
    auto now = keep(tracker, t0 + 16, 1000, moving(4.0f));
    const auto went_still = now;
    now = keep(tracker, now, rest_ms - 16, moving(0.2f));
    check(tracker.phase(now - 16) == Phase::bailing, "not resting yet");
    now = keep(tracker, now, 32, moving(0.1f));
    check(tracker.phase(now) == Phase::down, "a still body rests");
    check(near(tracker.view(now).tally.seconds, seconds(went_still - t0)), "its time stopped when it went still");
    auto twitch = hit(Bone::left_hand, hit_speed + 1.0f);
    twitch.velocity = game::Vec3{0.3f, 0, 0}; // a bone twitches, the body barely moves
    tracker.step(now, twitch);
    check(tracker.phase(now) == Phase::down && tracker.view(now).tally.impacts == 1, "a twitch at rest scores nothing");
    const auto rested = tracker.view(now).tally;
    const auto later = now + 3000;
    check(near(tracker.view(later).tally.seconds, seconds(went_still - t0)), "however long it lies");
    // Getting up starts inside the ragdoll and moves the body fast again: nothing more counts.
    auto getting_up = hit(Bone::left_foot, hit_speed + 1.0f);
    getting_up.velocity = game::Vec3{0, 3.0f, 2.0f};
    now = keep(tracker, later, 600, getting_up);
    const auto final = tracker.view(now).tally;
    check(tracker.phase(now) == Phase::down && final.score == rested.score && final.impacts == rested.impacts &&
              near(final.seconds, rested.seconds) && near(final.top_speed, rested.top_speed),
        "getting up adds no time and no points");
    Summary summary;
    tracker.step(now, standing_up(), &summary);
    check(summary.tally.score == rested.score && near(summary.tally.seconds, seconds(went_still - t0)),
        "the bail ends as it was when the body came to rest");
    check(tracker.view(now + linger_ms + fade_ms / 2).tally.score == rested.score, "and fades out so");
    Game unknown;
    unknown.step(t0, hit(Bone::hips, hit_speed + 1.0f, true));
    check(unknown.phase(keep(unknown, t0 + 16, 2000, lying())) == Phase::bailing, "an unknown speed never rests");
}

void a_fall_from_height_bails_at_its_wipeout() {
    // Thrown off the board from height: the ragdoll begins in the air, the wipeout comes with the impact.
    Game tracker;
    auto now = keep(tracker, t0, 500, flying(false));
    now = keep(tracker, now, 2000, flying());
    check(tracker.phase(now) == Phase::riding, "a ragdoll in the air is no bail yet");
    tracker.step(now, hit(Bone::neck1, 20.0f, true));
    check(tracker.view(now).phase == Phase::bailing && tracker.view(now).tally.impacts == 1, "the wipeout starts it with the impact");
    check(std::abs(tracker.view(now).tally.airtime - 2.5f) < 0.02f, "and the whole flight's airtime");
    now = keep(tracker, now + 16, 1000, lying());
    Summary summary;
    check(tracker.step(now, standing_up(), &summary) && summary.tally.impacts == 1, "and ends when the skater stands up");
}

void a_stumble_without_a_ragdoll_ends() {
    Game tracker;
    auto stumble = wipeout();
    stumble.ragdoll = false;
    tracker.step(t0, stumble);
    const auto now = keep(tracker, t0 + 16, ragdoll_wait_ms - 32, standing_up());
    check(tracker.phase(now) == Phase::bailing, "the ragdoll is given a moment to begin");
    check(tracker.step(t0 + ragdoll_wait_ms, standing_up()), "then the bail ends");
}

void an_unknown_skater_state_ends_nothing() {
    Game tracker;
    tracker.step(t0, hit(Bone::hips, hit_speed + 1.0f, true));
    const auto now = keep(tracker, t0 + 16, 2000, Step{}); // the skater state could not be read
    check(tracker.phase(now) == Phase::bailing, "the bail goes on");
    check(tracker.step(now, standing_up()), "until the skater is seen standing");
    Game riding;
    riding.step(t0, Step{});
    check(riding.phase(t0) == Phase::riding, "nor does it start one");
}

void a_lost_skater_ends_the_bail() {
    Game tracker;
    check(!tracker.lose(t0), "no bail, nothing to end");
    tracker.step(t0, hit(Bone::hips, hit_speed + 1.0f, true));
    tracker.step(keep(tracker, t0 + 16, 480, lying()) + 4, lying()); // the last step at t0 + 500
    Summary summary;
    check(tracker.lose(t0 + 500, &summary) && near(summary.tally.seconds, 0.5f) && summary.shown, "a respawn ends the bail");
    check(tracker.view(t0 + 500).phase == Phase::getting_up, "and it fades out");
    check(!tracker.lose(t0 + 516), "once");
    // A teleport in mid-air: the flight before it is not the next bail's.
    Game teleported;
    const auto now = keep(teleported, t0, 1000, flying(false));
    teleported.lose(now);
    teleported.step(now + 16, hit(Bone::hips, hit_speed + 1.0f, true));
    check(teleported.view(now + 16).tally.airtime == 0, "a flight before a teleport is forgotten");
}

void a_reading_clock_behind_the_physics_one_is_harmless() {
    Game tracker;
    tracker.step(t0, hit(Bone::neck, hit_speed + 0.5f, true));
    const auto view = tracker.view(t0 - 5);
    check(view.phase == Phase::bailing && near(view.alpha, 1.0f) && near(view.flashes[skater_body::index(Bone::neck)], 1.0f),
        "a view a moment before the step still shows it");
}

void one_contact_is_one_impact() {
    Game tracker;
    constexpr float light = hit_speed + 0.5f, harder = broken_speed - 1.0f;
    tracker.step(t0, hit(Bone::spine, light, true));
    tracker.step(t0 + 16, hit(Bone::spine, harder)); // the same contact, harder
    tracker.step(t0 + 32, hit(Bone::spine, 2.0f)); // easing off: no hit
    auto tally = tracker.view(t0 + 32).tally;
    check(tally.impacts == 1 && tally.damage == hit_points(harder), "a contact counts once, as its hardest step");
    tracker.step(t0 + 32 + impact_gap_ms + 16, hit(Bone::spine, light)); // hitting the ground again
    tally = tracker.view(t0 + 32 + impact_gap_ms + 16).tally;
    check(tally.impacts == 2 && tally.damage == hit_points(harder) + hit_points(light), "a new contact is a new impact");
    check(hit_points(hit_speed - 0.1f) == 0 && hit_points(hit_speed) == 100, "only hits score, the lightest 100");
    check(hit_points(30.0f) == 887, "a slam scores more than its speed, less than its energy");
}

void the_meat_adds_up() {
    Game tracker;
    constexpr float breaks = broken_speed + 1.0f, bruises = hit_speed + 1.0f;
    tracker.step(t0, hit(Bone::neck1, breaks, true)); // the head breaks
    tracker.step(t0 + 16, hit(Bone::left_hand, bruises)); // a hand is bruised
    const auto tally = tracker.view(t0 + 16).tally;
    check(tally.broken == 1 && tally.impacts == 2, "one bone broken, two hits");
    check(tally.hit_points == hit_points(breaks) + hit_points(bruises) && tally.head_bonus == hit_points(breaks),
        "a hit to the head counts double");
    check(tally.score == 2 * hit_points(breaks) + hit_points(bruises) + points_per_break + tally.time_points,
        "the Meat is the hits, the breaks and the time");
}

void a_vehicle_adds_half_again() {
    Game tracker;
    auto car = hit(Bone::spine, 10.0f, true);
    car.body.bodies[skater_body::index(Bone::spine)].hit.vehicle = true;
    tracker.step(t0, car);
    tracker.step(t0 + 16, hit(Bone::spine, 12.0f)); // the same contact, harder: still the car's
    const auto tally = tracker.view(t0 + 16).tally;
    check(tally.impacts == 1 && tally.vehicle_bonus == static_cast<int>(hit_points(12.0f) * 0.5f + 0.5f), "half again");
    check(impact_points({skater_body::index(Bone::neck1), 10.0f, true}) ==
              hit_points(10.0f) * 2 + static_cast<int>(hit_points(10.0f) * 0.5f + 0.5f),
        "the head and a vehicle together");
}

void sliding_along_the_ground_is_road_rash() {
    Game tracker;
    tracker.step(t0, slide(Bone::hips, 5.0f, true));
    auto now = t0;
    for (int i = 0; i < 50; ++i) tracker.step(now += 20, slide(Bone::hips, 5.0f)); // a second at 5 m/s
    const auto view = tracker.view(now);
    check(view.phase == Phase::bailing, "a sliding bail goes on");
    check(std::abs(view.tally.scraped - 5.0f) < 1e-3f && view.tally.scrape_points == static_cast<int>(5.0f * points_per_scraped_metre + 0.5f), "5 m of road rash");
    check(view.injuries[skater_body::index(Bone::hips)] == Injury::hit, "it bruises and shows");
    check(view.tally.score == view.tally.scrape_points + view.tally.time_points && view.tally.impacts == 0, "it scores without a hit");

    // The whole body slides as one: the road rash is how far it went, not the bodies' sum.
    Game body;
    auto all = slide(Bone::hips, 4.0f, true);
    for (const auto bone : {Bone::spine, Bone::spine1, Bone::left_upleg})
        all.body.bodies[skater_body::index(bone)] = all.body.bodies[skater_body::index(Bone::hips)];
    all.body.bodies[skater_body::index(Bone::left_hand)] = slide(Bone::left_hand, 8.0f).body.bodies[skater_body::index(Bone::left_hand)];
    body.step(t0, all);
    all.wipeout = false;
    for (now = t0; now < t0 + 1000;) body.step(now += 20, all);
    const auto tally = body.view(now).tally;
    check(std::abs(tally.scraped - 4.8f) < 1e-3f, "the average of the scraping bodies' slides, over a second");

    Game board;
    auto on_board = slide(Bone::spine, 5.0f, true);
    auto& contact = on_board.body.bodies[skater_body::index(Bone::spine)];
    contact.hit = {};
    contact.hit.board = true;
    board.step(t0, on_board);
    board.step(t0 + 1000, on_board);
    board.step(t0 + 1020, slide(Bone::left_foot, 5.0f));
    board.step(t0 + 1040, slide(Bone::spine, scrape_speed - 0.1f));
    check(board.view(t0 + 1040).tally.scraped == 0.0f, "the board, the feet and a slow slip do not scrape");
}

void the_view_follows_the_bail() {
    Game tracker;
    check(tracker.phase(t0) == Phase::riding, "riding");
    tracker.step(t0, hit(Bone::left_hand, hit_speed + 1.0f, true));
    tracker.step(keep(tracker, t0 + 16, 480, lying()) + 4, lying()); // the last step at t0 + 500
    auto view = tracker.view(t0 + 500);
    check(view.phase == Phase::bailing && near(view.tally.seconds, 0.5f), "bailing");
    check(view.injuries[skater_body::index(Bone::left_hand)] == Injury::hit && view.tally.impacts == 1, "the hand's hit");
    const auto now = keep(tracker, t0 + 516, 1000, lying());
    tracker.step(now, standing_up());
    view = tracker.view(now + 100);
    check(view.phase == Phase::getting_up && near(view.tally.seconds, seconds(now - t0)), "getting up, with how long the bail lasted");
    check(tracker.phase(now + linger_ms + fade_ms) == Phase::riding, "then riding again");
}

void airtime_is_the_bails_time_in_the_air() {
    // A fall from height on the board: 4 s in the air, the wipeout a step after the impact.
    Game tracker;
    std::uint64_t now = t0;
    for (; now <= t0 + 4000; now += 16) tracker.step(now, flying(false));
    tracker.step(now, standing_up());                       // the touch-down
    tracker.step(now + 16, hit(Bone::hips, hit_speed, true)); // the impact's wipeout
    tracker.step(now + 32, flying());                       // the ragdoll bounces: +16 ms
    tracker.step(now + 48, lying());                        // and lies
    check(std::abs(tracker.view(now + 48).tally.airtime - (4000 + 16) / 1000.0f) < 1e-4f,
        "the flight before the wipeout and the bail's own flights count");
    const auto air = tracker.view(now + 48).tally;
    check(air.airtime_points == static_cast<int>(air.airtime * points_per_air_second + 0.5f), "airtime scores by the second");

    // A clean landing, then a bail later on the ground: that flight is not the bail's.
    Game later;
    for (now = t0; now <= t0 + 1000; now += 16) later.step(now, flying(false));
    later.step(now, standing_up());
    later.step(now + wipeout_after_impact_ms + 16, hit(Bone::hips, hit_speed, true));
    check(later.view(now + wipeout_after_impact_ms + 16).tally.airtime == 0, "a bail on the ground has no airtime");
}

void a_bail_lasts_through_its_flights() {
    // The first impact throws the body off a ledge: seconds in the air, then the second one.
    Game tracker;
    auto now = keep(tracker, t0, 1000, flying(false));
    tracker.step(now, hit(Bone::hips, broken_speed + 1.0f, true));
    const auto thrown = now;
    now = keep(tracker, now + 16, 3000, flying());
    check(tracker.view(now).phase == Phase::bailing, "a body in the air is still bailing");
    tracker.step(now, hit(Bone::neck1, broken_speed + 1.0f, true)); // the second impact, wiping out again
    const auto tally = tracker.view(now).tally;
    check(tracker.view(now).phase == Phase::bailing && tally.impacts == 2 && tally.broken == 2, "the same bail, both impacts");
    check(std::abs(tally.airtime - static_cast<float>(1000 + now - thrown - 16) / 1000.0f) < 0.02f,
        "the flight before the bail and the one inside it add up");
    Summary summary;
    tracker.step(now + 16, standing_up(), &summary);
    check(summary.tally.impacts == 2 && std::abs(summary.tally.airtime - tally.airtime) < 1e-4f,
        "the airtime holds until the bail is over");
}

void the_slow_motion_keeps_the_steps() {
    check(slow_motion_rate(60, 1.0f) == 60, "full speed keeps the rate");
    check(slow_motion_rate(60, break_game_speed) == 200, "a break's slow motion steps more often: 200 a second of game time is 60 a real one");
    check(slow_motion_rate(60, 0.5f) == 120, "half speed doubles it");
    check(slow_motion_rate(60, 2.0f) == 60, "never faster than the game's own");
    check(slow_motion_rate(60, 0.0f) == highest_simulation_rate && slow_motion_rate(60, -1.0f) == highest_simulation_rate,
        "a stop is the slowest it can follow");
    check(slow_motion_rate(60, std::numeric_limits<float>::quiet_NaN()) == 60, "a broken speed is full speed");
    // Every break runs the same speeds, but never faster than the game ran (the trainer's game speed).
    check(slow_motion_time_scale(1.0f, break_game_speed) == break_game_speed, "at the game's own speed, the break's");
    check(slow_motion_time_scale(0.5f, break_game_speed) == break_game_speed, "with the trainer at half speed, the same");
    check(!slow_motion_time_scale(0.5f, 0.7f), "but not where the curve is faster than the trainer");
    check(!slow_motion_time_scale(0.1f, break_game_speed), "a trainer slower than the break is never sped up");
    check(!slow_motion_time_scale(0.0f, break_game_speed), "a paused game stays paused");
    check(!slow_motion_time_scale(1.0f, 1.0f) && !slow_motion_time_scale(1.0f, 2.0f), "full speed is none");
    check(slow_motion_time_scale(1.0f, 0.0f) == slowest_speed, "no slower than the steps can follow");
    check(!slow_motion_time_scale(1.0f, std::numeric_limits<float>::quiet_NaN()), "a broken speed is none");
}

void the_card_shows_the_bail() {
    check(grouped(0) == "0" && grouped(999) == "999" && grouped(1000) == "1,000" && grouped(1234567) == "1,234,567",
        "thousands grouped");
    check(grouped(-1234) == "-1,234", "a negative number too");
    Game tracker;
    check(card(tracker.view(t0), {}).opacity == 0 && card(tracker.view(t0), {}).rows.empty(), "no card while riding");
    tracker.step(t0, hit(Bone::neck1, broken_speed + 1.0f, true)); // the head breaks
    tracker.step(t0 + 16, hit(Bone::left_hand, hit_speed + 1.0f));
    tracker.step(keep(tracker, t0 + 32, 1456, lying()) + 12, lying()); // the last step at t0 + 1500
    const auto view = tracker.view(t0 + 1500);
    auto shown = card(view, standing(0, view.tally.score));
    check(near(shown.opacity, 1.0f) && shown.total == view.tally.score, "the Meat, every stat counted");
    check(shown.rows.size() == 1 && shown.rows[0].stat == Stat::time && shown.rows[0].value == "1.5 s" &&
              shown.rows[0].points == static_cast<int>(1.5f * points_per_second + 0.5f),
        "a small bail shows its time alone");
    check(shown.badge == "NEW BEST" && shown.highlight, "the map's first bail sets its best");
    shown = card(view, standing(99999, view.tally.score));
    check(shown.badge == "BEST 99,999" && !shown.highlight, "a smaller one shows the best");
    check(card(view, {}).badge.empty(), "an unknown best shows none");
    tracker.step(t0 + 1500, standing_up());
    check(near(card(tracker.view(t0 + 1500 + linger_ms / 2), {}).opacity, 1.0f), "the card stays once the skater gets up");
    check(near(card(tracker.view(t0 + 1500 + linger_ms + fade_ms / 2), {}).opacity, 0.5f), "then fades");

    // A big one: off a roof for 3.5 s, five bones broken at 20 m/s, a slide of 5 m.
    Game big;
    auto falling = flying(false);
    falling.velocity = game::Vec3{0, -10, 0};
    auto now = keep(big, t0, 3500, falling);
    for (const auto bone : {Bone::hips, Bone::spine, Bone::left_leg, Bone::right_leg, Bone::left_arm}) {
        auto impact = hit(bone, broken_speed + 1.0f, true);
        impact.velocity = game::Vec3{20, 0, 0};
        big.step(now += 16, impact);
    }
    auto sliding = slide(Bone::spine1, 5.0f);
    sliding.velocity = game::Vec3{5, 0, 0};
    sliding.spin = 3.0f * radians_per_rotation; // tumbling over three times a second
    now = keep(big, now + 16, 1000, sliding);
    shown = card(big.view(now), {});
    std::vector<Stat> stats;
    for (const auto& row : shown.rows) stats.push_back(row.stat);
    check(stats == std::vector<Stat>{Stat::time, Stat::hits, Stat::broken, Stat::road_rash, Stat::airtime, Stat::fall, Stat::speed,
                                     Stat::rotations},
        "a big bail shows every stat, in its order");
    check(shown.rows[2].value == "5 broken" && shown.rows[2].points == 5 * points_per_break, "the broken bones");
    check(shown.rows[6].value == "44.7 MPH" && shown.rows[6].points == static_cast<int>(20.0f * points_per_speed + 0.5f),
        "the top speed, in miles per hour");
    check(shown.rows[7].value == "3.0 rotations", "the rotations");
}

void the_fall_and_the_top_speed_score() {
    // Off a roof on the board: falling at 10 m/s for 2 s, then the impact's wipeout at 20 m/s.
    Game tracker;
    auto falling = flying(false);
    falling.velocity = game::Vec3{3, -10, 0};
    auto now = keep(tracker, t0, 2000, falling);
    auto impact = hit(Bone::hips, hit_speed + 2.0f, true);
    impact.velocity = game::Vec3{12, -16, 0};
    tracker.step(now, impact);
    auto rolling = lying();
    rolling.velocity = game::Vec3{0, 2, 4}; // rolling on, bouncing up: it goes no further down
    now = keep(tracker, now + 16, 500, rolling);
    const auto t = tracker.view(now).tally;
    check(std::abs(t.fallen - 19.84f) < 0.01f, "the flight's fall is the bail's");
    check(t.fall_points == static_cast<int>(t.fallen * points_per_metre_fallen + 0.5f), "a metre fallen scores");
    check(near(t.top_speed, 20.0f) && t.speed_points == static_cast<int>(20.0f * points_per_speed + 0.5f), "the top speed scores");
    check(t.score == t.damage + t.broken * points_per_break + t.time_points + t.airtime_points + t.fall_points +
              t.speed_points + t.rotation_points,
        "the Meat is every stat's points");
}

void the_body_turning_over_scores() {
    Game tracker;
    auto spinning = hit(Bone::hips, hit_speed + 1.0f, true);
    spinning.spin = -1.0f; // not a speed: nothing
    tracker.step(t0, spinning);
    auto tumbling = moving(4.0f);
    tumbling.spin = 2.0f * radians_per_rotation; // twice a second, any way round
    auto now = keep(tracker, t0 + 16, 1504, tumbling);
    auto t = tracker.view(now).tally;
    check(std::abs(t.rotations - 3.0f) < 0.01f, "three rotations in a second and a half");
    check(t.rotation_points == static_cast<int>(t.rotations * points_per_rotation + 0.5f), "a rotation scores");
    auto lying_turning = moving(0.1f);
    lying_turning.spin = radians_per_rotation; // lying still: the body comes to rest, whatever turns
    now = keep(tracker, now, rest_ms + 32, lying_turning);
    check(tracker.phase(now) == Phase::down, "the body rests");
    const auto rested = tracker.view(now).tally.rotations;
    now = keep(tracker, now, 1000, tumbling); // getting up
    check(near(tracker.view(now).tally.rotations, rested), "after the rest nothing turns any more");
}

void a_bail_runs_on_the_games_clock() {
    // A break slows the game down: its physics steps as often each real second as before, each the
    // shorter. In play on 2026-10-08 the ragdoll came later than ragdoll_wait_ms in real time, so the
    // bail ended in mid-air and the next impact began another: the bones hurt before were gone.
    Game slow;
    slow.speed = break_game_speed;
    auto thrown = hit(Bone::left_leg, hit_speed + 1.0f, true);
    thrown.ragdoll = false; // the bail's first part: off the board, the ragdoll still to come
    slow.step(t0, thrown);
    auto falling = flying(false);
    falling.velocity = game::Vec3{0, -10, 0};
    auto now = keep(slow, t0 + 16, 1000, falling); // a real second, under a third of the game's
    check(slow.phase(now) == Phase::bailing, "the ragdoll still has the game's moment to begin");
    falling.ragdoll = true;
    now = keep(slow, now, 1000, falling);
    slow.step(now, hit(Bone::neck1, broken_speed + 1.0f, true)); // a further impact, wiping out again
    const auto view = slow.view(now);
    check(view.phase == Phase::bailing && near(view.alpha, 1.0f) && view.tally.impacts == 2 &&
              view.injuries[skater_body::index(Bone::left_leg)] == Injury::hit,
        "one bail, the skeleton whole: the leg bruised before is still bruised");
    check(std::abs(view.tally.airtime - 0.6f) < 0.01f && std::abs(view.tally.fallen - 6.0f) < 0.1f,
        "the airtime and the fall are the game's: two real seconds at 0.3");
}

void a_bail_sets_a_best_only_by_beating_it() {
    check(standing(0, 300).new_best && standing(0, 300).best == 300, "the first bail on a map sets its best");
    check(!standing(500, 300).new_best && standing(500, 300).best == 500, "a smaller bail keeps it");
    check(!standing(500, 500).new_best, "matching it does not beat it");
}
}

int main() {
    try {
        impacts_count_only_during_a_bail();
        the_hit_that_causes_the_wipeout_counts();
        nothing_shows_until_a_bone_is_hurt();
        hard_hits_break_and_light_ones_bruise();
        a_break_pulses_the_screen_edge();
        a_break_slows_the_game_with_its_red_edge();
        a_break_just_before_the_bail_counts_from_the_bail();
        a_flump_starts_no_bail();
        the_board_and_bad_values_are_ignored();
        a_bail_lasts_until_the_skater_gets_up();
        a_rest_ends_the_counting();
        a_fall_from_height_bails_at_its_wipeout();
        a_stumble_without_a_ragdoll_ends();
        an_unknown_skater_state_ends_nothing();
        a_lost_skater_ends_the_bail();
        a_reading_clock_behind_the_physics_one_is_harmless();
        one_contact_is_one_impact();
        the_meat_adds_up();
        a_vehicle_adds_half_again();
        sliding_along_the_ground_is_road_rash();
        the_view_follows_the_bail();
        airtime_is_the_bails_time_in_the_air();
        a_bail_lasts_through_its_flights();
        the_fall_and_the_top_speed_score();
        the_body_turning_over_scores();
        a_bail_runs_on_the_games_clock();
        a_bail_sets_a_best_only_by_beating_it();
        the_card_shows_the_bail();
        the_slow_motion_keeps_the_steps();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Hall of Meat tests passed.\n";
    return 0;
}
