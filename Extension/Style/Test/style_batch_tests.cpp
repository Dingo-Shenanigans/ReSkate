#include "Extension/Style/style_batch.h"
#include <iostream>
#include <string_view>

namespace {
int failures{};
void check(bool ok, std::string_view what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
} // namespace

int main() {
    using namespace dingosdk::style;
    TrickSet all;
    for (std::uint8_t id = 1; id < flip_trick_count; ++id) all.set(id);
    check(tricks_to_record(all, {}).none(), "a trick with a current clip is not recorded");
    {
        auto on_disk = all;
        on_disk.reset(2);
        TrickSet outdated;
        outdated.set(5);
        const auto wanted = tricks_to_record(on_disk, outdated);
        check(wanted.test(2) && wanted.test(5) && wanted.count() == 2, "a missing clip and an outdated clip are recorded");
    }
    {
        const auto wanted = tricks_to_record({}, {});
        check(wanted.count() == last_capturable && !wanted.test(31) && !wanted.test(32) && !wanted.test(0),
              "with no clips, every capturable trick is recorded, never the quick ollie or quick nollie");
        const auto order = in_order(wanted);
        check(order.size() == last_capturable && order.front() == 1 && order.back() == last_capturable,
              "the tricks are in trick order");
    }
    check(record_text(30, 7000) == "Your tricks need recording: 30 tricks, about 4 min. Your skater goes to a practice stage.",
          "the note gives the count and the minutes, rounded up");
    check(record_text(1, 7000) == "Your tricks need recording: 1 trick, about 1 min. Your skater goes to a practice stage.",
          "one trick takes at least a minute in the note");
    check(progress_text(7, 30, 2, false) == "Recording your tricks: 7 of 30 (Kickflip). Esc or Back: stop after this trick",
          "the progress names the trick and the stop keys");
    check(progress_text(7, 30, 2, true) == "Recording your tricks: 7 of 30 (Kickflip). Stopping after this trick",
          "a stop that was asked for shows");
    check(summary_text(30, 30, {}, false) == "your 30 tricks are recorded", "all kept");
    check(summary_text(1, 1, {}, false) == "the trick is recorded", "one kept");
    check(summary_text(30, 28, {{14, "the skater did no flip trick"}, {29, "the stick did not move"}}, false) ==
              "28 of 30 tricks are recorded; not recorded: the Laserflip (the skater did no flip trick), "
              "the Nollie 360 Flip (the stick did not move)",
          "the tricks that were not kept are named with their reasons");
    check(summary_text(30, 7, {{14, "x"}}, true) == "stopped: 7 of 30 tricks are recorded; not recorded: the Laserflip (x)",
          "a stop lists the tricks that failed");
    check(summary_text(1, 0, {{3, "test failure"}}, false) == "the Heelflip was not recorded: test failure", "one trick not kept");
    check(summary_text(30, 7, {}, true) == "stopped: 7 of 30 tricks are recorded", "a stop says how many were recorded");
    if (!failures) std::cout << "style batch tests passed\n";
    return failures ? 1 : 0;
}
