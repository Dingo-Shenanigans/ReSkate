// Exercise the bail feed against a fake clock. No game, hooks or native
// memory are used; hall_of_meat.cpp's GetTickCount64 is substituted.
#include <Windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

static ULONGLONG fixture_now = 1000;
static ULONGLONG fixture_clock() { return fixture_now; }
#define GetTickCount64 fixture_clock
#include "Extension/Skater/hall_of_meat.cpp"
#undef GetTickCount64

using namespace dingosdk::hall_of_meat;

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void advance(ULONGLONG ms) { fixture_now += ms; }

void records_a_wipeout_with_its_causes() {
    forget();
    observe_cause(13, 4.5f);
    observe_cause(9, 2);
    Bail bail;
    check(observe_wipeout(true, &bail), "the first wipeout opens a bail");
    check(bail.body_contact, "the body-contact flag is carried");
    check(bail.impact_count == 2, "both causes are kept");
    check(bail.impacts[0].reason == 13 && bail.impacts[0].magnitude == 4.5f, "causes stay in arrival order");
    check(std::abs(bail.magnitude - 6.5f) < 0.001f, "magnitudes are added up");
    check(observe_wipeout(true, nullptr) == false, "the ragdoll's follow-up step is folded in");
    check(recent().size() == 1, "one bail is kept");
}

void a_stumble_expires_before_the_next_crash() {
    forget();
    observe_cause(14, 1);
    advance(pending_ttl_ms + 1);
    Bail bail;
    check(observe_wipeout(false, &bail), "the later crash still records");
    check(bail.impact_count == 0 && bail.magnitude == 0, "the expired stumble is left out");
    check(!bail.body_contact, "the body-contact flag follows the crash, not the stumble");
}

void history_keeps_the_newest_and_honours_the_debounce() {
    forget();
    for (int index = 0; index < 12; ++index) {
        advance(bail_debounce_ms + 1);
        observe_cause(2, 1);
        observe_wipeout(false, nullptr);
    }
    const auto bails = recent();
    check(bails.size() == kept_bails, "the ring keeps its bound");
    for (std::size_t index = 1; index < bails.size(); ++index)
        check(bails[index - 1].at > bails[index].at, "newest first");
    forget();
    check(recent().empty(), "forget clears the history");
}

void absurd_magnitudes_are_dropped() {
    forget();
    observe_cause(5, std::nanf(""));
    Bail bail;
    check(observe_wipeout(false, &bail), "the wipeout still records");
    check(bail.impact_count == 0, "the non-finite magnitude is not kept");
}

int main() {
    try {
        records_a_wipeout_with_its_causes();
        a_stumble_expires_before_the_next_crash();
        history_keeps_the_newest_and_honours_the_debounce();
        absurd_magnitudes_are_dropped();
        std::cout << "hall of meat tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
