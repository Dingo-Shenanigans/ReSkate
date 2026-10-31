#include "Engine/Game/World/owned_body.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dingosdk::owned_body;
namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
Epoch next(Epoch epoch) { ++epoch.serial; epoch.time += epoch.seconds; return epoch; }
Command request() { Command c; c.epoch = {7, 11, 100, 3, static_cast<float>(native_seconds)}; c.force = {0, 20, 0}; c.impulse = {0, 2, 0}; c.impulse_id = 1; return c; }
void definitions_are_bounded_and_have_no_gameplay_defaults() {
    Definition d;
    check(!valid(d), "empty definition activated");
    d.count = 1;
    d.capsules[0] = {{0, 0, 0}, {0, 1, 0}, .1f};
    d.mass = 12;
    d.inertia = {2, 3, 4};
    d.rotation = {0, 0, 0, 1};
    check(valid(d), "generic capsule rejected");
    auto bad = d;
    bad.count = maximum_capsules + 1;
    check(!valid(bad), "capsule quota escaped");
    bad = d;
    bad.mass = 0;
    check(!valid(bad), "zero mass admitted");
    bad = d;
    bad.inertia[1] = std::numeric_limits<float>::quiet_NaN();
    check(!valid(bad), "NaN inertia admitted");
    bad = d;
    bad.capsules[0].radius = 2.1f;
    check(!valid(bad), "radius bound escaped");
    bad = d;
    bad.capsules[0].a[0] = 11;
    check(!valid(bad), "local shape extent escaped");
    bad = d;
    bad.rotation = {0, 0, 0, 2};
    check(!valid(bad), "nonunit rotation admitted");
}
void uncertain_allocations_stay_quarantined_after_confirmed_subset_cleanup() {
    AllocationProgress allocation;
    check(allocation.quarantine(false) && allocation.quarantine(true), "unknown body allocation called cleanly parked");
    allocation.body_confirmed = true;
    check(!allocation.quarantine(true), "confirmed body-only subset cannot be parked");
    ++allocation.shapes_attempted;
    check(allocation.quarantine(true), "constructor exception/wrong return/unknown out handle lost quarantine");
    ++allocation.shapes_confirmed;
    check(!allocation.quarantine(true) && allocation.quarantine(false), "confirmed shape cleanup ignores actual park failure");
    ++allocation.shapes_attempted;
    check(allocation.quarantine(true), "second uncertain constructor erased by successful cleanup of first shape");
}
void commands_bound_the_combined_impulse_and_force() {
    auto c = request();
    check(valid(c), "bounded force and impulse rejected");
    c.impulse_id = 0;
    check(!valid(c), "anonymous impulse admitted");
    c = request();
    c.force = {30000, 30000, 0};
    check(!valid(c), "force norm escaped via separate axes");
    c = request();
    c.force = {0, 10000, 0};
    c.impulse = {0, 600, 0};
    check(!valid(c), "combined force plus J/h escaped");
    c = request();
    c.torque[0] = std::numeric_limits<float>::infinity();
    check(!valid(c), "infinite torque admitted");
    c = request();
    c.epoch.seconds = .02f;
    check(!valid(c), "unsupported native step admitted");
    c = request();
    c.epoch.serial = std::numeric_limits<std::uint64_t>::max();
    check(!valid(c), "target epoch overflow admitted");
}
void observed_predecessor_targets_only_the_next_real_callback() {
    Mailbox box;
    const auto c = request();
    Command produced;
    check(box.submit(c, c.epoch, 1000), "observed predecessor N not queued");
    check(!box.submit(c, c.epoch, 1001), "pending packet overwritten");
    check(!box.consume(c.epoch, 1002, produced) && box.pending, "same-N callback consumed future packet");
    const auto target = next(c.epoch);
    check(box.consume(target, 1010, produced) && !box.pending && produced.impulse_id == 1, "N+1 callback did not consume exact packet");
    check(!box.consume(target, 1011, produced), "duplicate wrapper replayed a consumed command");
    box.finish(true, true, true);
    check(box.oldest().outcome == Outcome::applied && box.oldest().produced == target, "setter receipt lost exact producer epoch");
    check(!box.acknowledge(box.oldest().sequence), "setter success invented a measured successor");
    auto later = c;
    later.epoch = next(target);
    later.impulse = {};
    later.impulse_id = 0;
    check(box.count == 1 && box.oldest().sequence == 1, "unacknowledged result was overwritten");
    Sample measured;
    measured.epoch = later.epoch;
    auto wrong = measured;
    ++wrong.epoch.world;
    check(!box.observe_successor(wrong), "replacement world supplied a successor");
    wrong = measured;
    ++wrong.epoch.body;
    check(!box.observe_successor(wrong), "replacement body supplied a successor");
    wrong = measured;
    wrong.epoch = target;
    check(!box.observe_successor(wrong), "producer epoch was called a successor");
    check(box.observe_successor(measured), "N+2 measured successor not attached");
    check(!box.acknowledge(box.oldest().sequence + 1), "wrong receipt acknowledged");
    check(box.acknowledge(box.oldest().sequence), "measured terminal result not acknowledged");
    check(!box.acknowledge(box.oldest().sequence), "receipt acknowledged twice");
    check(!box.submit(c, c.epoch, 1030), "already submitted serial replayed");
    check(box.submit(later, later.epoch, 1030), "next measured command blocked after acknowledgement");
}
void owner_world_clock_and_body_changes_never_become_a_race_retry() {
    const auto c = request();
    for (unsigned field = 0; field < 5; ++field) {
        Mailbox box;
        auto observed = c.epoch;
        if (field == 0) ++observed.world;
        if (field == 1) ++observed.body;
        if (field == 2) ++observed.serial;
        if (field == 3) observed.time += 1;
        if (field == 4) observed.seconds = .02f;
        check(!box.submit(c, observed, 0), "changed observed epoch queued");
        check(box.submit(c, c.epoch, 0), "original epoch refused");
        auto current = next(c.epoch);
        if (field == 0) ++current.world;
        if (field == 1) ++current.body;
        if (field == 2) current.serial -= 2;
        if (field == 3) current.time += 1;
        if (field == 4) current.seconds = .02f;
        Command out;
        check(!box.consume(current, 1, out) && !box.pending && box.oldest().outcome == Outcome::rejected, "changed owner/body/clock consumed or stayed retryable");
    }
}
void skipped_steps_and_the_deadline_expire_without_setters() {
    const auto c = request();
    Command out;
    Mailbox skipped;
    check(skipped.submit(c, c.epoch, 100), "queue failed");
    check(!skipped.consume(next(next(c.epoch)), 110, out) && skipped.oldest().outcome == Outcome::expired, "skipped N+1 relabeled as a later step");
    check(!skipped.oldest().force_attempted && !skipped.oldest().torque_attempted, "expiry invented native setters");
    Mailbox timed;
    check(timed.submit(c, c.epoch, 100), "queue failed");
    check(!timed.consume(next(c.epoch), 200, out) && timed.oldest().outcome == Outcome::expired, "deadline equality admitted");
    check(timed.acknowledge(timed.oldest().sequence), "expired receipt cannot be acknowledged");
    check(!timed.submit(c, c.epoch, 201), "expired serial silently retried");
}
void partial_native_failures_cannot_replay_impulses() {
    auto c = request();
    Mailbox box;
    Command out;
    check(box.submit(c, c.epoch, 0) && box.consume(next(c.epoch), 1, out), "partial fixture not consumed");
    box.finish(true, false, false);
    check(box.oldest().outcome == Outcome::partial && box.failed && box.last_impulse == 1, "uncertain force call was reported as clean rejection");
    check(box.acknowledge(box.oldest().sequence), "partial receipt could not be acknowledged");
    c.epoch = next(next(c.epoch));
    ++c.impulse_id;
    check(!box.submit(c, c.epoch, 10), "failed native owner became retryable after acknowledgement");
    Mailbox complete;
    c = request();
    check(complete.submit(c, c.epoch, 0) && complete.consume(next(c.epoch), 1, out), "complete fixture not consumed");
    complete.finish(true, true, true);
    Sample measured;
    measured.epoch = next(next(c.epoch));
    check(complete.observe_successor(measured) && complete.acknowledge(1), "complete ack failed");
    c.epoch = next(next(c.epoch));
    check(!complete.submit(c, c.epoch, 10), "one-shot impulse event replayed on a later step");
    ++c.impulse_id;
    check(complete.submit(c, c.epoch, 10), "new explicit impulse event refused");
    complete.cancel();
    check(complete.oldest().outcome == Outcome::rejected && !complete.pending, "release left pending input active");
}
void consecutive_steps_pipeline_without_waiting_for_earlier_acknowledgement() {
    Mailbox box;
    auto c = request();
    Command out;
    for (unsigned i = 0; i < 3; ++i) {
        check(box.submit(c, c.epoch, 1000 + i * 16), "previous result blocked next native-step command");
        check(box.consume(next(c.epoch), 1001 + i * 16, out), "consecutive N+1 command missed its exact target");
        check(out.epoch == c.epoch && out.impulse_id == i + 1, "immutable packet changed while older receipt waited");
        check(box.finish(true, true, true), "consumed result did not finish");
        Sample sample;
        sample.epoch = next(c.epoch);
        sample.position = {static_cast<float>(i), 0, 0};
        (void)box.observe_successor(sample);
        if (i == 0) check(!box.oldest().successor_available, "producer sample invented its own successor");
        c.epoch = next(c.epoch);
        ++c.impulse_id;
    }
    check(box.count == 3 && box.last_serial == 102, "consecutive producer records were overwritten");
    Receipt first, second, third;
    check(box.find(1, first) && box.find(2, second) && box.find(3, third), "retained sequence disappeared");
    check(first.produced.serial == 101 && second.produced.serial == 102 && third.produced.serial == 103, "60Hz packets collapsed or skipped a step");
    check(first.successor.epoch.serial == 102 && first.successor.position[0] == 1 && second.successor.epoch.serial == 103 &&
        second.successor.position[0] == 2 && !third.successor_available, "first independent successor not attached to each record");
    Sample later;
    later.epoch = next(c.epoch);
    later.position = {99, 0, 0};
    check(box.observe_successor(later), "last record did not acquire its successor");
    check(box.find(1, first) && first.successor.epoch.serial == 102 && first.successor.position[0] == 1, "later observation rewrote first successor");
    check(!box.acknowledge(2), "out-of-order acknowledgement skipped retained proof");
    check(box.acknowledge(1) && box.acknowledge(2) && box.acknowledge(3), "ordered terminal receipts could not be freed");
    check(!box.count && box.oldest().outcome == Outcome::none && !box.find(1, first), "acknowledged record remained selectable");
}
void full_ring_backpressure_reuses_only_acknowledged_slots() {
    Mailbox box;
    auto c = request();
    Command out;
    for (std::size_t i = 0; i < receipt_capacity; ++i) {
        check(box.submit(c, c.epoch, 0) && box.consume(next(c.epoch), 1, out) && box.finish(true, true, true), "ring fixture failed");
        Sample sample;
        sample.epoch = next(c.epoch);
        (void)box.observe_successor(sample);
        c.epoch = next(c.epoch);
        ++c.impulse_id;
    }
    check(box.count == receipt_capacity && !box.submit(c, c.epoch, 2), "unacknowledged ring overflowed");
    check(box.last_submitted == 103 && box.sequence == 4, "backpressure consumed an unpublished serial or ID");
    check(box.acknowledge(1) && box.submit(c, c.epoch, 2), "freed head did not permit one immutable packet");
    check(box.count == receipt_capacity && box.oldest().sequence == 2, "ring wrap overwrote an older unacknowledged record");
    check(!box.submit(c, c.epoch, 3), "pending inbox was overwritten at ring wrap");
    box.cancel();
    Receipt canceled;
    check(box.find(5, canceled) && canceled.outcome == Outcome::rejected && !box.pending && box.failed, "unload left wrapped inbox active");
    c.epoch = next(c.epoch);
    ++c.impulse_id;
    check(!box.submit(c, c.epoch, 4), "canceled owner admitted later control");
}
void later_failure_is_selectable_without_waiving_older_successor_proof() {
    Mailbox box;
    auto c = request();
    Command out;
    check(box.submit(c, c.epoch, 0, 10) && box.consume(next(c.epoch), 1, out) && box.finish(true, true, true), "first control did not apply");
    c.epoch = next(c.epoch);
    ++c.impulse_id;
    check(box.submit(c, c.epoch, 2, 20) && box.consume(next(c.epoch), 3, out) && box.finish(true, false, false), "later partial fixture failed");
    Receipt failure;
    check(box.oldest().sequence == 10 && !box.oldest().successor_available && !box.acknowledge(10), "partial failure invented earlier motion proof");
    check(box.find(20, failure) && failure.outcome == Outcome::partial && failure.force_attempted && !failure.torque_attempted,
        "later partial result hidden behind an unacknowledgeable head");
    check(!box.acknowledge(20), "selected inspection allowed unordered acknowledgement");
    c.epoch = next(c.epoch);
    ++c.impulse_id;
    check(box.failed && !box.submit(c, c.epoch, 4, 30) && !box.consume(next(c.epoch), 5, out), "partial owner retried after failure");
    Mailbox other;
    auto foreign = request();
    check(other.submit(foreign, foreign.epoch, 0, 30), "foreign fixture failed");
    check(!box.find(30, failure) && !other.find(20, failure), "receipt from another body selected a local record");
}
void expiry_and_release_keep_their_own_records_while_older_results_wait() {
    Mailbox box;
    auto c = request();
    Command out;
    check(box.submit(c, c.epoch, 0) && box.consume(next(c.epoch), 1, out) && box.finish(true, true, true), "first packet failed");
    c.epoch = next(c.epoch);
    ++c.impulse_id;
    check(box.submit(c, c.epoch, 2) && !box.consume(next(c.epoch), 102, out), "deadline failed to expire pipelined packet");
    Receipt expired;
    check(box.find(2, expired) && expired.outcome == Outcome::expired && !expired.force_attempted, "expiry changed the wrong record");
    check(!box.submit(c, c.epoch, 103), "expired submitted serial replayed");
    c.epoch = next(c.epoch);
    check(box.submit(c, c.epoch, 103), "unconsumed impulse event could not be republished with a fresh epoch");
    box.cancel();
    Receipt released;
    check(box.find(3, released) && released.outcome == Outcome::rejected && box.find(2, expired) && expired.outcome == Outcome::expired,
        "release overwrote old expiry or lost its pending packet");
    check(box.oldest().outcome == Outcome::applied && !box.oldest().successor_available, "release fabricated an older successor");
}
}
int main() {
    try {
        definitions_are_bounded_and_have_no_gameplay_defaults();
        uncertain_allocations_stay_quarantined_after_confirmed_subset_cleanup();
        commands_bound_the_combined_impulse_and_force();
        observed_predecessor_targets_only_the_next_real_callback();
        owner_world_clock_and_body_changes_never_become_a_race_retry();
        skipped_steps_and_the_deadline_expire_without_setters();
        partial_native_failures_cannot_replay_impulses();
        consecutive_steps_pipeline_without_waiting_for_earlier_acknowledgement();
        full_ring_backpressure_reuses_only_acknowledged_slots();
        later_failure_is_selectable_without_waiving_older_successor_proof();
        expiry_and_release_keep_their_own_records_while_older_results_wait();
        std::cout << "owned body contracts passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
