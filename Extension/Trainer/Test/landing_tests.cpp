// The decisions behind a ground-safe arrival (trainer_landing.h): where the downward ray
// starts, the watch that puts a skater who sinks back on the surface, and when a pause-map
// waypoint is remembered or forgotten.
#include "Extension/Trainer/trainer_landing.h"

#include <iostream>
#include <string>

namespace {
using namespace dingosdk::trainer::landing;
int failures = 0;
void check(bool condition, const std::string &message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
Sample at(std::uint64_t now, Vec3 position, std::optional<float> ground, float vertical = 0.0f, bool may_resend = true) {
    return Sample{.now = now, .position = position, .vertical = vertical, .ground = ground, .may_resend = may_resend};
}
} // namespace

int main() {
    // ---- the ray --------------------------------------------------------------------------
    check(first_ray(12.0f).top == ray_ceiling && first_ray(12.0f).bottom == ray_floor, "a low hint starts at the usual ceiling");
    check(first_ray(1800.0f).top == 2300.0f, "a high hint starts 500 above it");
    check(std::isfinite(first_ray(std::nanf("")).top), "a broken hint still gives a usable ray");
    // A waypoint at Y 12 over a custom map's ground at Y 1939 (seen in game): the first ray
    // starts below that ground and misses, so the retry covers what lies above the first one.
    const auto retry = retry_ray(first_ray(12.0f));
    check(retry && retry->top >= 1939.0f && retry->bottom == ray_ceiling, "a miss is cast again from far above");
    check(!retry_ray(Ray{ray_retry_ceiling, 0.0f}), "nothing to retry from the top of the world");

    // ---- the landing watch ----------------------------------------------------------------
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        check(step(w, at(1000, {0, 0, 0}, 50.0f)) == Step::wait, "the teleport gets a moment to arrive");
        check(step(w, at(2600, {100.0f, 50.6f, 200.0f}, 50.0f)) == Step::landed, "standing on the surface ends the watch");
        check(!w.active, "a landed watch is over");
        check(step(w, at(2800, {100.0f, -20.0f, 200.0f}, 50.0f)) == Step::wait, "an ended watch does nothing");
    }
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        check(step(w, at(2600, {100.0f, 30.0f, 200.0f}, 50.0f)) == Step::put_back, "sinking under the surface puts the skater back");
        check(w.fixes == 1, "a correction is counted");
        check(step(w, at(2700, {100.0f, 30.0f, 200.0f}, 50.0f)) == Step::wait, "the next look waits for the correction to arrive");
        check(step(w, at(4200, {100.0f, 50.4f, 200.0f}, 50.0f)) == Step::landed, "then it lands");
    }
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        check(step(w, at(2600, {100.0f, 30.0f, 200.0f}, std::nullopt)) == Step::wait, "no ground yet: collision is streaming, wait");
        check(w.active, "still watching");
        check(step(w, at(2800, {100.0f, 49.0f, 200.0f}, 50.0f, -8.0f)) == Step::wait, "still falling onto it: not landed yet");
    }
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        check(step(w, at(2600, {0, 0, 0}, 50.0f)) == Step::wait, "not arrived, too early to resend");
        check(step(w, at(3600, {0, 0, 0}, 50.0f, 0.0f, false)) == Step::wait, "never resend while the game ignores teleports");
        check(step(w, at(3800, {0, 0, 0}, 50.0f)) == Step::resend, "a teleport that never arrived is sent again");
        check(w.resends == max_resends - 1, "the resend is counted");
        check(step(w, at(4000, {0, 0, 0}, 50.0f)) == Step::wait, "resends are spaced out");
        int sent = 1;
        for (std::uint64_t now = 4000; now < 60000 && w.active; now += 200)
            if (step(w, at(now, {0, 0, 0}, 50.0f)) == Step::resend) ++sent;
        check(sent == max_resends, "at most three resends");
        check(!w.active, "a teleport that never arrives ends the watch");
    }
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        int fixes = 0;
        Step last{};
        for (std::uint64_t now = 2600; now < 60000 && w.active; now += 100)
            if ((last = step(w, at(now, {100.0f, 0.0f, 200.0f}, 50.0f))) == Step::put_back) ++fixes;
        check(fixes == max_fixes && last == Step::ended, "a skater who keeps sinking is corrected six times, then left alone");
    }
    {
        auto w = start(100.0f, 200.0f, 50.5f, 1000);
        check(step(w, at(1000 + watch_ms + 1, {100.0f, 70.0f, 200.0f}, 50.0f)) == Step::ended, "the watch times out");
    }

    // ---- the remembered waypoint ----------------------------------------------------------
    const std::optional<Vec3> old = Vec3{1.0f, 2.0f, 3.0f};
    const Vec3 fresh{4.0f, 5.0f, 6.0f};
    check(remembered(old, Reading{.read = true, .typed = 58, .waypoint = fresh}) == fresh, "a placed waypoint replaces the old one");
    check(!remembered(old, Reading{.read = true, .typed = 58, .waypoint = std::nullopt}), "a filled map without one: it was removed");
    check(remembered(old, Reading{.read = true, .typed = 0, .waypoint = std::nullopt}) == old, "an empty map says nothing: keep it");
    check(remembered(old, Reading{}) == old, "an unreadable registry says nothing: keep it");
    check(!remembered(std::nullopt, Reading{}), "nothing seen, nothing remembered");

    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "landing: all checks passed\n";
    return failures ? 1 : 0;
}
