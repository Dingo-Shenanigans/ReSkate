// The local player's open challenge attempts (Extension/Progression/challenge_attempts.h), apart from
// the game: what a plugin's score line holds back for.
#include "Extension/Progression/challenge_attempts.h"
#include <iostream>
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
} // namespace

int main() {
    try {
        a_begun_attempt_is_open_until_it_ends();
        attempts_the_game_keeps_count_too();
        an_unknown_begin_lasts_until_any_end();
        a_level_change_ends_every_attempt();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "challenge attempts tests passed\n";
    return 0;
}
