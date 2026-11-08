// The local player's open challenge attempts (Extension/Progression/challenge_attempts.h), apart from
// the game: what a plugin's score line holds back for.
#include "Extension/Progression/challenge_attempts.h"
#include <iostream>
#include <new>
#include <stdexcept>

using dingosdk::profile_runtime::OpenAttempts;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void a_begun_attempt_is_open_until_it_ends() {
    OpenAttempts attempts;
    check(!attempts.open(), "nothing begun, yet an attempt was open");
    attempts.begin("Challenge.Gap.01");
    check(attempts.open(), "a begun attempt was not open");
    attempts.end("Challenge.Other");
    check(attempts.open(), "another challenge's end closed this one");
    attempts.end("Challenge.Gap.01");
    check(!attempts.open(), "an ended attempt stayed open");
}

void attempts_the_game_keeps_count_too() {
    // Requests handed back to the game unchanged (not a local challenge) count the same.
    OpenAttempts attempts;
    attempts.begin("Native.Activity");
    attempts.begin("Local.Challenge");
    attempts.end("Local.Challenge");
    check(attempts.open(), "one attempt's end closed both");
    attempts.end("Native.Activity");
    check(!attempts.open(), "both ended, yet one stayed open");
}

void an_unknown_begin_lasts_until_any_end() {
    OpenAttempts attempts;
    attempts.begin_unknown();
    check(attempts.open(), "a begin whose id could not be read was not open");
    attempts.end("Challenge.Gap.01");
    check(!attempts.open(), "an end did not close an unknown begin");
    attempts.begin_unknown();
    attempts.begin("Challenge.Gap.01");
    attempts.end({});
    check(attempts.open(), "an end whose id could not be read closed a known attempt");
    attempts.end("Challenge.Gap.01");
    check(!attempts.open(), "the known attempt did not end");
}

void a_level_change_ends_every_attempt() {
    OpenAttempts attempts;
    attempts.begin("Challenge.Gap.01");
    attempts.begin_unknown();
    attempts.clear();
    check(!attempts.open(), "an attempt stayed open past a level change");
}

void prepared_originals_do_not_prove_a_completed_watch() {
    using dingosdk::profile_runtime::challenge_requests_watched;
    check(!challenge_requests_watched(false, true, true), "retained prepared originals passed after hook installation failed");
    check(!challenge_requests_watched(true, false, true) && !challenge_requests_watched(true, true, false),
        "an incomplete watch passed without both request originals");
    check(challenge_requests_watched(true, true, true), "a completed watch with both originals was refused");
}

void a_failed_observation_remains_unknown_until_an_end() {
    OpenAttempts attempts;
    volatile bool fail = true;
    const auto failure = [&](std::string&) { if (fail) throw std::bad_alloc(); return false; };
    attempts.observe(false, failure);
    check(attempts.open(), "a failed begin observation was lost");
    attempts.observe(true, [](std::string&) { return false; });
    check(!attempts.open(), "an observed end did not clear the lost begin");
    attempts.begin("Challenge.Gap.01");
    attempts.observe(true, failure);
    check(attempts.open(), "a failed end observation cleared a known attempt");
    attempts.end("Challenge.Other");
    check(attempts.open(), "an unrelated end cleared a known attempt after an observation failure");
    attempts.end("Challenge.Gap.01");
    check(!attempts.open(), "a matching end did not clear the known attempt");
    attempts.observe(false, failure);
    attempts.clear();
    check(!attempts.open(), "a level clear kept an observation failure");
}

void an_outer_tracker_failure_is_sticky_until_a_level_clear() {
    dingosdk::profile_runtime::AttemptFallback fallback;
    check(!fallback.unknown(), "an unused fallback started unknown");
    volatile bool fail = true;
    fallback.guard([&] { if (fail) throw std::bad_alloc(); });
    check(fallback.unknown(), "a tracker allocation failure did not hold the display back");
    bool ran{};
    fallback.guard([&] { ran = true; });
    check(ran && fallback.unknown(), "a later successful observation forgot the lost request");
    fallback.guard([&] { fallback.cleared(); if (fail) throw std::runtime_error("clear failed"); });
    check(fallback.unknown(), "a failed level clear claimed tracking was known");
    fallback.guard([&] { fallback.cleared(); });
    check(!fallback.unknown(), "a successful level clear did not reset the fallback");
}
} // namespace

int main() {
    try {
        a_begun_attempt_is_open_until_it_ends();
        attempts_the_game_keeps_count_too();
        an_unknown_begin_lasts_until_any_end();
        a_level_change_ends_every_attempt();
        prepared_originals_do_not_prove_a_completed_watch();
        a_failed_observation_remains_unknown_until_an_end();
        an_outer_tracker_failure_is_sticky_until_a_level_clear();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "challenge attempts tests passed\n";
    return 0;
}
