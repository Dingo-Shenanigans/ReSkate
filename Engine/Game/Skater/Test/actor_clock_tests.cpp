#include "Engine/Game/Skater/actor_clock.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace c = dingosdk::actor_clock;
namespace {
unsigned checks{};
void check(bool value, const char* why) {
    ++checks;
    if (!value) { std::cerr << "actor clock: " << why << '\n'; std::exit(1); }
}
struct Actor {
    std::uint64_t entity{}, world{}, lifetime{}, ticket{};
    bool operator==(const Actor&) const = default;
};
using Key = c::Key<Actor>;
using Feed = c::Feed<Actor>;
using Clock = c::Ledger<Actor>;
const Key first{7, 11, 19, {23, 29, 31, 19}};
bool update(Clock& clock, const Key& key, double dt, std::uint64_t now) {
    Feed feed;
    return clock.begin(key, feed) && clock.publish(feed, dt, now);
}
void lifecycle_and_freshness() {
    Clock clock; c::Sample sample{9, 9, 9}; Feed absent;
    check(!clock.read(first, 100, sample) && !sample.serial && !sample.observed_ms && sample.seconds == 0, "disabled read clears output");
    check(!clock.begin(first, absent), "disabled cannot reserve a feed");
    check(clock.enable(first), "claim explicitly enables");
    check(!clock.read(first, 100, sample), "enable does not invent a first sample");
    check(update(clock, first, .05, 100), "positive native dt publishes");
    check(clock.read(first, 200, sample) && sample.serial == 1 && sample.observed_ms == 100 && sample.seconds == .05,
        "exact freshness boundary is accepted");
    check(!clock.read(first, 201, sample) && !sample.serial, "age beyond boundary refuses");
    check(!clock.read(first, 99, sample), "reversed wall clock refuses");
    check(clock.enable(first) && clock.read(first, 101, sample) && sample.serial == 1 && sample.seconds == .05,
        "idempotent enable preserves accumulated sample");
    check(clock.disable(first) && clock.disable(first), "matching disable is idempotent");
    check(!clock.enable(first) && !clock.read(first, 101, sample), "disabled ticket cannot be rearmed");
}
void exact_keys_and_delayed_callbacks() {
    Clock clock; c::Sample sample;
    check(clock.enable(first), "first claim enables");
    Feed old;
    check(clock.begin(first, old), "original callback reserves old token");
    for (unsigned lane = 0; lane < 7; ++lane) {
        auto wrong = first;
        if (lane == 0) ++wrong.owner;
        if (lane == 1) ++wrong.sequence;
        if (lane == 2) ++wrong.generation;
        if (lane == 3) ++wrong.actor.entity;
        if (lane == 4) ++wrong.actor.world;
        if (lane == 5) ++wrong.actor.lifetime;
        if (lane == 6) ++wrong.actor.ticket;
        Feed refused;
        check(!clock.begin(wrong, refused) && !refused.serial, "wrong exact feed key refuses");
        check(!clock.disable(wrong), "wrong key cannot disable owner");
        check(!clock.read(wrong, 100, sample), "wrong key cannot read sample");
    }
    check(clock.disable(first) && !clock.publish(old, .01, 100), "feed returning after disable cannot publish");
    auto next = first; ++next.sequence; ++next.generation; next.actor.ticket = next.generation;
    check(clock.enable(next), "same native actor with a new claim enables");
    check(!clock.publish(old, .01, 101) && !clock.disable(first) && !clock.enable(first),
        "old feed/disable/enable cannot affect new claim");
    check(!clock.read(next, 101, sample), "new claim cannot inherit old time");
    check(update(clock, next, .02, 102) && clock.read(next, 102, sample) && sample.seconds == .02,
        "new claim starts its own observed duration");
    Feed earlier, later;
    check(clock.begin(next, earlier) && clock.begin(next, later), "overlapping callbacks reserve ordered tokens");
    check(clock.publish(later, .03, 103) && !clock.publish(earlier, .04, 104), "late older callback cannot overwrite newer sample");
    check(!clock.publish(later, .03, 104), "one feed cannot publish twice");
    check(clock.read(next, 104, sample) && sample.seconds == .02 + .03 && sample.observed_ms == 103,
        "refused stale publications do not refresh or accumulate");
}
void invalid_native_values_do_not_refresh() {
    const std::array invalid{0.0, -.01, .100001, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto dt : invalid) {
        Clock clock; c::Sample sample;
        check(clock.enable(first) && update(clock, first, .01, 100), "real baseline exists");
        check(!update(clock, first, dt, 150), "invalid or zero dt refuses");
        check(clock.read(first, 200, sample) && sample.observed_ms == 100 && sample.seconds == .01,
            "invalid value never refreshes old sample");
        check(!clock.read(first, 201, sample), "invalid updates cannot prevent staleness");
        check(update(clock, first, .02, 202), "a separately qualified valid update may resume");
        check(clock.read(first, 202, sample) && sample.seconds == .01 + .02,
            "missing observations are not synthesized");
    }
    Clock wall; c::Sample sample;
    check(wall.enable(first) && update(wall, first, .01, 100), "wall baseline exists");
    check(!update(wall, first, .01, 99) && !update(wall, first, .01, 0), "wall regression and zero observation refuse");
    check(wall.read(first, 100, sample) && sample.seconds == .01, "wall failure preserves baseline");
    check(update(wall, first, static_cast<double>(.1f), 101), "native float upper step boundary remains valid after promotion");
}
void exhaustion_and_bad_admission() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Clock exhausted(maximum); Feed feed; c::Sample sample;
    check(exhausted.enable(first) && !exhausted.begin(first, feed) && !feed.serial, "serial cannot wrap");
    check(!exhausted.read(first, 100, sample), "serial exhaustion is unavailable");
    auto next = first; ++next.generation; ++next.sequence;
    check(!exhausted.enable(next), "new claim cannot reset exhausted process serial");
    Clock last(maximum - 1);
    check(last.enable(first) && last.begin(first, feed) && feed.serial == maximum && last.publish(feed, .01, 100),
        "final representable reservation is valid");
    check(last.read(first, 100, sample) && sample.serial == maximum, "final serial is retained exactly");
    check(!last.begin(first, feed) && !last.read(first, 100, sample), "next reservation fails closed without stale refresh");
    for (const auto seed : {std::numeric_limits<double>::max(), std::numeric_limits<double>::infinity()}) {
        Clock precision(0, seed);
        check(precision.enable(first) && !update(precision, first, .01, 100), "nonadvancing/nonfinite sum refuses");
        check(!precision.read(first, 100, sample) && !precision.begin(first, feed), "exhausted time remains unavailable");
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        Clock zero; auto bad = first;
        if (lane == 0) bad.owner = 0;
        if (lane == 1) bad.sequence = 0;
        if (lane == 2) bad.generation = 0;
        check(!zero.enable(bad), "zero authority key refuses");
        check(zero.enable(first), "failed admission does not consume a real key");
    }
}
}
int main() {
    lifecycle_and_freshness(); exact_keys_and_delayed_callbacks();
    invalid_native_values_do_not_refresh(); exhaustion_and_bad_admission();
    std::cout << "actor_clock checks=" << checks << " native_calls=0\n";
}
