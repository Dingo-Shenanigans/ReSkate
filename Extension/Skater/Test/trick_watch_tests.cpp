// Reading tricks from the score HUD's values, apart from the game: a trick's start, its points
// once it lands, a slam, and a trick whose points never come.
#include "Extension/Skater/trick_watch.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dingosdk::tricks;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
TrickSignals tick(bool action, float score, float multiplier, bool slam = false, bool wipeout = false) {
    TrickSignals in;
    in.action = action;
    in.line_score = score;
    in.multiplier = multiplier;
    in.slam = slam;
    in.wipeout = wipeout;
    return in;
}
// The last event over `ticks` updates.
TrickEvent feed(TrickWatch& watch, const TrickSignals& in, int ticks) {
    TrickEvent last;
    for (int i = 0; i < ticks; ++i) {
        const auto e = watch.update(in);
        if (e.kind != TrickEvent::Kind::none) last = e;
    }
    return last;
}
using K = TrickEvent::Kind;

void a_trick_starts_then_lands_with_its_points() {
    TrickWatch w;
    feed(w, tick(false, 0, 0), 5);
    check(w.update(tick(true, 0, 0)).kind == K::started, "A trick starts the tick ActionActive goes on");
    check(feed(w, tick(true, 0, 0), 30).kind == K::none && w.in_trick(), "and says it once");
    check(w.update(tick(false, 0, 0)).kind == K::none && w.waiting(), "Its points are not in the tick it ends");
    const auto e = w.update(tick(false, 11, 1.5f));
    check(e.kind == K::landed && std::abs(e.points - 11) < 0.01f && std::abs(e.multiplier - 1.5f) < 0.01f,
          "Landed: 11 points, the first trick of a line");
}

void a_second_trick_in_a_line_scores_its_own_points() {
    TrickWatch w;
    feed(w, tick(false, 0, 0), 5);
    feed(w, tick(true, 0, 0), 30);
    w.update(tick(false, 0, 0));
    w.update(tick(false, 11, 1.5f));
    feed(w, tick(true, 11, 1.5f), 20);
    w.update(tick(false, 11, 1.5f));
    const auto f = w.update(tick(false, 708, 2.0f));
    const float points = (708 / 2.0f - 11 / 1.5f) * 2.0f;
    check(f.kind == K::landed && std::abs(f.points - points) < 0.01f, "The multiplier's rise scales only the new trick's points");
    feed(w, tick(true, 708, 2.0f), 20);
    w.update(tick(false, 708, 2.0f));
    const auto g = w.update(tick(false, 958, 2.0f));
    check(g.kind == K::landed && std::abs(g.points - 250) < 0.01f, "and the next one its own again");
}

void a_slam_ends_the_trick_for_nothing() {
    TrickWatch w;
    feed(w, tick(false, 0, 1), 2);
    feed(w, tick(true, 0, 1), 10);
    feed(w, tick(false, 300, 1), 2);
    feed(w, tick(true, 300, 1), 10);
    const auto e = w.update(tick(true, 300, 1, true));
    check(e.kind == K::broken && !w.in_trick(), "A slam ends the trick going on");
    check(feed(w, tick(false, 0, 0), 30).kind == K::none, "which scores nothing");
    check(feed(w, tick(false, 0, 0, false, true), 2).kind == K::none, "A wipeout with nothing going on says nothing");
    feed(w, tick(true, 0, 1), 5);
    check(w.update(tick(false, 0, 1, false, true)).kind == K::broken, "and one while the points are awaited breaks it");
}

void a_trick_whose_points_never_come_is_skipped() {
    TrickWatch w(5);
    feed(w, tick(false, 50, 1), 2);
    feed(w, tick(true, 50, 1), 10);
    const auto none = feed(w, tick(false, 50, 1), 10);
    check(!w.waiting(), "Waiting gives up after its ticks");
    feed(w, tick(true, 50, 1), 10);
    const auto e = feed(w, tick(false, 90, 1), 2);
    check(none.kind == K::none && e.kind == K::landed && std::abs(e.points - 40) < 0.01f, "and the next trick is still read");
}

void the_line_ending_is_not_a_trick() {
    TrickWatch w;
    feed(w, tick(false, 500, 2), 5);
    check(feed(w, tick(false, 0, 0), 5).kind == K::none, "The line's score going back to 0 is no event");
    check(feed(w, tick(false, 100, 1), 5).kind == K::none, "nor is a rise with no trick before it");
}
}

int main() {
    try {
        a_trick_starts_then_lands_with_its_points();
        a_second_trick_in_a_line_scores_its_own_points();
        a_slam_ends_the_trick_for_nothing();
        a_trick_whose_points_never_come_is_skipped();
        the_line_ending_is_not_a_trick();
        std::cout << "trick watch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trick watch test failed: " << error.what() << '\n';
        return 1;
    }
}
