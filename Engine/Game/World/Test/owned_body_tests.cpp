#include "Engine/Game/World/owned_body.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

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
// A parked body may come back only for a definition with its exact capsules, mass and inertia.
void reuse_compares_the_shape_bit_for_bit_and_names_its_outcomes() {
    Definition d;
    d.count = 2;
    d.capsules[0] = {{0, 0, 0}, {0, 1, 0}, .1f};
    d.capsules[1] = {{.2f, 0, -.3f}, {.2f, .4f, -.3f}, .05f};
    d.mass = 12;
    d.inertia = {2, 3, 4};
    d.position = {1, 2, 3};
    d.rotation = {0, 0, 0, 1};
    check(same_shape(d, d), "a definition differs from itself");
    auto moved = d;
    moved.position = {-40, 7, 900};
    moved.rotation = {0, .7071068f, 0, .7071068f};
    check(same_shape(d, moved) && same_shape(moved, d), "a new position or rotation stopped a parked body coming back");
    // Capsules past count are not part of the body.
    moved.capsules[5] = {{9, 9, 9}, {1, 1, 1}, 2};
    check(same_shape(d, moved), "an unused capsule entry was compared");
    const auto differs = [&](auto change, const char* why) {
        auto other = d;
        change(other);
        check(!same_shape(d, other) && !same_shape(other, d), why);
    };
    differs([](Definition& x) { x.count = 1; }, "a different capsule count matched");
    differs([](Definition& x) { x.capsules[1].a[2] = -.30001f; }, "a moved capsule endpoint matched");
    differs([](Definition& x) { x.capsules[0].b[1] = 1.0000001f; }, "a one-ulp endpoint change matched");
    differs([](Definition& x) { x.capsules[1].radius = .051f; }, "a different radius matched");
    differs([](Definition& x) { x.mass = 12.5f; }, "a different mass matched");
    for (std::size_t axis = 0; axis < 3; ++axis)
        differs([axis](Definition& x) { x.inertia[axis] += .25f; }, "a different inertia axis matched");
    // Bit for bit, not ==: a negative zero is a different value here.
    differs([](Definition& x) { x.capsules[0].a[0] = -0.f; }, "a negative zero endpoint matched as equal");
    check(std::string_view(name(Origin::allocated)) == "allocated" && std::string_view(name(Origin::reused)) == "reused",
        "create's origin names changed");
    const std::pair<ReuseMiss, std::string_view> misses[] = {{ReuseMiss::none, "none"}, {ReuseMiss::none_parked, "none_parked"},
        {ReuseMiss::definition_differs, "definition_differs"}, {ReuseMiss::world_changed, "world_changed"},
        {ReuseMiss::quarantined, "quarantined"}, {ReuseMiss::parked_body_changed, "parked_body_changed"}};
    for (const auto& [miss, text] : misses) check(name(miss) == text, "a reuse miss name changed");
    const CreateReport report;
    check(report.origin == Origin::allocated && report.miss == ReuseMiss::none, "a default report claims a reuse or a miss");
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
// The half-rate order. In some rides the plugin's tick falls between the clock edge and the
// synchronizer's calls on the body, so the tick that reads sample N+1 still finds its command
// for N queued (counted in the game on 2026-11-14: no listed call between the edge and the tick,
// three after it). The inbox holds the command for N+1 behind the one for N, and each is still
// taken only by a call that reads the step after its own epoch.
Command after(const Command& previous, std::uint32_t impulse_id = 0) {
    auto c = previous;
    c.epoch = next(previous.epoch);
    c.impulse = impulse_id ? Vec{0, 2, 0} : Vec{};
    c.impulse_id = impulse_id;
    return c;
}
void the_next_steps_command_queues_behind_a_pending_one() {
    Mailbox box;
    const auto first = request(), second = after(first, 2);
    Command out;
    check(box.submit(first, first.epoch, 1000), "the command for N was not queued");
    check(!box.consume(first.epoch, 1001, out) && box.pending, "a call that still read N took the command for N");
    check(box.submit(second, second.epoch, 1016), "the command for N+1 was refused while the one for N was pending");
    Receipt head, behind;
    check(box.count == 2 && box.oldest().sequence == 1 && box.find(1, head) && box.find(2, behind) &&
        head.outcome == Outcome::queued && behind.outcome == Outcome::queued && head.requested == first.epoch &&
        behind.requested == second.epoch, "two pending commands did not each keep a queued receipt, in order");
    const auto third = after(second, 3);
    check(!box.submit(third, third.epoch, 1017) && box.count == 2 && box.sequence == 2 && box.last_submitted == 101,
        "a third command was queued behind two, or its refusal spent a sequence");
    check(box.consume(second.epoch, 1017, out) && out.epoch == first.epoch && out.impulse_id == 1,
        "the call that read N+1 did not take the command for N");
    check(!box.consume(second.epoch, 1017, out), "another wrapper took a command while the first was being produced");
    check(box.finish(true, true, true), "the first command did not finish");
    check(box.find(1, head) && head.outcome == Outcome::applied && head.produced == second.epoch && box.find(2, behind) &&
        behind.outcome == Outcome::queued && behind.produced == Epoch{}, "producing the first command changed the second's receipt");
    check(!box.consume(second.epoch, 1018, out) && box.pending && box.find(2, behind) && behind.outcome == Outcome::queued,
        "the command for N+1 was taken or dropped by a call that still read N+1");
    const auto later = next(second.epoch);
    check(box.consume(later, 1033, out) && out.epoch == second.epoch && out.impulse_id == 2 && box.finish(true, true, true),
        "the call that read N+2 did not take the command for N+1");
    check(!box.consume(later, 1034, out) && !box.pending, "a consumed command was replayed");
    check(box.find(2, behind) && behind.outcome == Outcome::applied && behind.produced == later && box.last_serial == 101 &&
        box.last_impulse == 2, "the second command's receipt lost its own producer epoch");
}
void a_second_pending_command_must_be_for_exactly_the_next_step() {
    const auto first = request(), exact = after(first, 2);
    Mailbox queued;
    check(queued.submit(first, first.epoch, 1000), "the command for N was not queued");
    for (unsigned field = 0; field < 6; ++field) {
        auto box = queued;
        auto wrong = exact;
        if (field == 0) ++wrong.epoch.world;
        if (field == 1) ++wrong.epoch.body;
        if (field == 2) wrong.epoch = next(wrong.epoch); // N+2: the step between would go without a command
        if (field == 3) wrong.epoch.time += wrong.epoch.seconds; // N+1 by serial, on another time base
        if (field == 4) wrong.epoch.seconds = std::nextafter(wrong.epoch.seconds, 1.f); // calibrated, but not the same step
        if (field == 5) wrong.epoch.time = std::nextafter(wrong.epoch.time, 1e9); // one ulp from the first's target
        check(valid(wrong), "the wrong second command was not a valid command on its own");
        check(!box.submit(wrong, wrong.epoch, 1016) && box.pending && box.count == 1 && box.sequence == 1 &&
            box.last_submitted == 100 && box.oldest().outcome == Outcome::queued,
            "a second command that is not exactly the next step was queued, or its refusal changed the inbox");
        check(box.submit(exact, exact.epoch, 1016), "a refused second command blocked the exact next step's");
    }
    auto box = queued;
    Command out;
    check(box.submit(exact, exact.epoch, 1016) && box.consume(exact.epoch, 1017, out), "the first of two commands was not taken at N+1");
    const auto third = after(exact, 3);
    check(!box.submit(third, third.epoch, 1018), "a command was queued while another was being produced");
    check(box.finish(true, true, true) && !box.submit(exact, exact.epoch, 1018), "a submitted serial was queued twice");
    check(box.submit(third, third.epoch, 1033), "the place behind the pending command did not free when the first was taken");
    const auto fourth = after(third, 4);
    check(!box.submit(fourth, fourth.epoch, 1034) && box.count == 3, "a third pending command was queued");
    // The receipt ring still bounds everything: three unacknowledged results and one pending command fill it.
    Mailbox full;
    auto c = request();
    for (unsigned i = 0; i < 3; ++i) {
        check(full.submit(c, c.epoch, 0) && full.consume(next(c.epoch), 1, out) && full.finish(true, true, true), "ring fixture failed");
        c = after(c, c.impulse_id + 1);
    }
    check(full.submit(c, c.epoch, 2) && full.count == receipt_capacity, "the fourth row was not reserved");
    const auto overflow = after(c, c.impulse_id + 1);
    check(!full.submit(overflow, overflow.epoch, 3) && full.count == receipt_capacity && full.sequence == 4 && full.oldest().sequence == 1,
        "a second pending command overflowed the receipt ring");
}
void release_rejects_both_pending_commands() {
    const auto first = request(), second = after(first, 2);
    Command out;
    Receipt a, b;
    Mailbox box;
    check(box.submit(first, first.epoch, 0) && box.submit(second, second.epoch, 16), "two pending commands were not queued");
    box.cancel();
    check(box.failed && !box.pending && box.find(1, a) && box.find(2, b) && a.outcome == Outcome::rejected &&
        b.outcome == Outcome::rejected && a.produced == Epoch{} && b.produced == Epoch{}, "release left one of two pending commands active");
    check(!box.consume(second.epoch, 17, out) && !box.consume(next(second.epoch), 33, out) && box.find(2, b) &&
        b.outcome == Outcome::rejected, "a released command was produced");
    // The stopped ring frees in order, as one rejected row does.
    check(!box.acknowledge(2) && box.acknowledge(1) && box.acknowledge(2) && !box.count,
        "two released commands could not be acknowledged in order");
    const auto third = after(second, 3);
    check(!box.submit(third, third.epoch, 40), "a released owner admitted later control");
    // Released while the first is being produced: the first keeps its own result, the second never runs.
    Mailbox producing;
    check(producing.submit(first, first.epoch, 0) && producing.submit(second, second.epoch, 16) &&
        producing.consume(second.epoch, 17, out), "the first of two commands was not taken at N+1");
    producing.cancel();
    check(producing.finish(true, true, true) && producing.find(1, a) && a.outcome == Outcome::applied && producing.find(2, b) &&
        b.outcome == Outcome::rejected && !producing.pending && !producing.consume(next(second.epoch), 33, out),
        "release during production lost the first result or left the second active");
    // A native setter that fails on the first stops the second too.
    Mailbox partial;
    check(partial.submit(first, first.epoch, 0) && partial.submit(second, second.epoch, 16) &&
        partial.consume(second.epoch, 17, out) && partial.finish(true, false, false), "the partial fixture was not produced");
    check(partial.failed && partial.find(1, a) && a.outcome == Outcome::partial && partial.find(2, b) &&
        b.outcome == Outcome::rejected && !partial.pending && !partial.consume(next(second.epoch), 33, out) &&
        partial.last_impulse == 1, "a command ran after a native setter failed");
}
// A command is only ever produced by a call that reads the step after its own epoch. Each of two
// pending commands is judged by that rule alone and gets its own receipt. When the first's step
// was missed the first expires; the second is still taken if, and only if, the clock reads the
// step after its own epoch. Expiring the second with the first would report a missed step that
// was not missed, and neither rule can put a command on another step.
void each_pending_command_gets_its_own_verdict() {
    const auto first = request(), second = after(first, 2);
    Command out;
    Receipt a, b;
    const auto two = [&](std::uint64_t at, std::uint64_t then) {
        Mailbox box;
        check(box.submit(first, first.epoch, at) && box.submit(second, second.epoch, then), "two pending commands were not queued");
        return box;
    };
    // No call read N+1, so the first's step is gone. This call reads N+2, the second's own step.
    auto skipped = two(1000, 1016);
    check(skipped.consume(next(second.epoch), 1033, out) && out.epoch == second.epoch && out.impulse_id == 2,
        "the second command was not taken at the step after its own epoch");
    check(skipped.find(1, a) && a.outcome == Outcome::expired && a.produced == Epoch{} && !a.force_attempted && !a.torque_attempted,
        "a first command whose step was skipped was relabelled or produced");
    check(skipped.finish(true, true, true) && skipped.find(2, b) && b.outcome == Outcome::applied && b.produced == next(second.epoch) &&
        skipped.last_impulse == 2 && !skipped.pending, "the second command's own result was not recorded");
    // Two steps on, both steps are gone.
    auto late = two(1000, 1016);
    check(!late.consume(next(next(second.epoch)), 1050, out) && !late.pending && late.find(1, a) && late.find(2, b) &&
        a.outcome == Outcome::expired && b.outcome == Outcome::expired && b.produced == Epoch{} && late.last_impulse == 0,
        "a command was produced two steps after its epoch");
    check(!late.acknowledge(2) && late.acknowledge(1) && late.acknowledge(2), "two expired commands were not acknowledged in order");
    auto again = second;
    again.epoch = next(next(second.epoch));
    again.impulse_id = 1;
    check(late.submit(again, again.epoch, 1050), "unconsumed impulse events could not be republished after both expired");
    // The first's deadline passes while the clock reads N+1: the second waits there, as it would alone.
    auto timed = two(1000, 1090);
    check(!timed.consume(second.epoch, 1100, out) && timed.pending && timed.find(1, a) && a.outcome == Outcome::expired &&
        timed.find(2, b) && b.outcome == Outcome::queued, "the first command's deadline expired the second, or the second left its step");
    check(timed.consume(next(second.epoch), 1189, out) && out.epoch == second.epoch && timed.finish(true, true, true),
        "the second command was not taken inside its own deadline");
    auto stale = two(1000, 1090);
    check(!stale.consume(next(second.epoch), 1190, out) && !stale.pending && stale.find(1, a) && a.outcome == Outcome::expired &&
        stale.find(2, b) && b.outcome == Outcome::expired, "deadline equality admitted the second command");
    // Another world at the first's step: neither command matches it, and neither is retried.
    auto moved = two(1000, 1016);
    auto elsewhere = second.epoch;
    ++elsewhere.world;
    check(!moved.consume(elsewhere, 1017, out) && !moved.pending && !moved.failed && moved.find(1, a) && moved.find(2, b) &&
        a.outcome == Outcome::rejected && b.outcome == Outcome::rejected && !moved.consume(next(second.epoch), 1033, out),
        "a changed world consumed a pending command or left one retryable");
}
// The receipt path and a synchronizer call on an unlisted wrapper have no clock reading to
// judge a command by, only the time. expire() is their whole verdict: a deadline, never a step.
void deadlines_alone_expire_each_pending_command_by_its_own() {
    static_assert(pending_capacity == 2, "the inbox holds the command for N and the one for N+1");
    const auto first = request(), second = after(first, 2);
    Command out;
    Receipt a, b;
    Mailbox box;
    check(box.submit(first, first.epoch, 1000) && box.submit(second, second.epoch, 1090), "two pending commands were not queued");
    box.expire(1099);
    check(box.pending && box.find(1, a) && box.find(2, b) && a.outcome == Outcome::queued && b.outcome == Outcome::queued &&
        box.command.epoch == first.epoch, "a command expired before its deadline");
    box.expire(1100);
    check(box.pending && !box.failed && box.find(1, a) && box.find(2, b) && a.outcome == Outcome::expired &&
        b.outcome == Outcome::queued && box.command.epoch == second.epoch && box.deadline == 1190,
        "the first command's deadline did not expire it alone, or expire gave the second a verdict before its own deadline");
    check(box.consume(next(second.epoch), 1189, out) && out.epoch == second.epoch && box.finish(true, true, true),
        "the second command was not produced in its own step after the first expired");
    Mailbox both;
    check(both.submit(first, first.epoch, 1000) && both.submit(second, second.epoch, 1090), "two pending commands were not queued");
    both.expire(1190);
    check(!both.pending && !both.failed && both.find(1, a) && both.find(2, b) && a.outcome == Outcome::expired &&
        b.outcome == Outcome::expired && b.produced == Epoch{} && !both.consume(next(second.epoch), 1191, out),
        "two commands past their deadlines were not both expired");
    // One pending command, as before: nothing behind it to promote.
    Mailbox single;
    check(single.submit(first, first.epoch, 1000), "the command was not queued");
    single.expire(1100);
    check(!single.pending && single.oldest().outcome == Outcome::expired && !single.consume(second.epoch, 1101, out),
        "a single command did not expire at its deadline");
    Mailbox stopped;
    check(stopped.submit(first, first.epoch, 1000) && stopped.submit(second, second.epoch, 1016), "two pending commands were not queued");
    stopped.cancel();
    stopped.expire(5000);
    check(stopped.find(1, a) && stopped.find(2, b) && a.outcome == Outcome::rejected && b.outcome == Outcome::rejected,
        "a released command was relabelled as expired");
}
// consume judges by the clock reading it was handed, whatever object the caller read it from.
// Before expire() existed the service's receipt path passed the pending command's own epoch
// member (box.command.epoch) to expire it by deadline. Promoting the second command rewrites
// that member, so a consume that kept a reference to it would go on to judge the second against
// the second's own epoch, and the second would wait: a verdict against a reading no caller
// passed. The reading is taken by value, so the member gives exactly what a copy of it gives.
void consume_judges_by_the_reading_it_was_handed() {
    const auto first = request(), second = after(first, 2);
    Command out;
    Receipt a, b, c, d;
    const auto two = [&] {
        Mailbox box;
        check(box.submit(first, first.epoch, 1000) && box.submit(second, second.epoch, 1090), "two pending commands were not queued");
        return box;
    };
    // One pending command and its own epoch: it waits before its deadline and expires at it.
    Mailbox single;
    check(single.submit(first, first.epoch, 1000) && !single.consume(single.command.epoch, 1099, out) && single.pending &&
        single.oldest().outcome == Outcome::queued, "a command did not wait at a reading of its own epoch");
    check(!single.consume(single.command.epoch, 1100, out) && !single.pending && !single.failed &&
        single.oldest().outcome == Outcome::expired, "a command did not expire at its deadline at a reading of its own epoch");
    // Two pending, the first still in time: the reading is the first's own epoch, and both wait.
    auto waiting = two();
    check(!waiting.consume(waiting.command.epoch, 1099, out) && waiting.pending && waiting.next_pending &&
        waiting.find(1, a) && waiting.find(2, b) && a.outcome == Outcome::queued && b.outcome == Outcome::queued,
        "two commands did not wait at a reading of the first's epoch");
    // Two pending, the first past its deadline. The reading is step N both times: once read from
    // the member the call is about to rewrite, once from a copy.
    auto member = two(), copied = two();
    const Epoch reading = copied.command.epoch;
    const bool by_member = member.consume(member.command.epoch, 1100, out);
    const bool by_copy = copied.consume(reading, 1100, out);
    check(member.find(1, a) && member.find(2, b) && copied.find(1, c) && copied.find(2, d), "a receipt row was lost");
    check(by_member == by_copy && a.outcome == c.outcome && b.outcome == d.outcome && member.pending == copied.pending &&
        member.failed == copied.failed, "the reading changed under consume when it was read from the pending command itself");
    // The clock reads N. The first expires by its deadline. The second, measured at N+1 and due
    // at N+2, is neither waiting at its own epoch nor at its step: it is rejected and never retried.
    check(!by_member && a.outcome == Outcome::expired && b.outcome == Outcome::rejected && b.produced == Epoch{} &&
        !member.pending && !member.next_pending && !member.failed && member.last_impulse == 0 &&
        !member.consume(next(second.epoch), 1101, out), "a reading of step N did not expire the first command and reject the second");
    check(!member.acknowledge(2) && member.acknowledge(1) && member.acknowledge(2) && !member.count,
        "the two verdicts could not be acknowledged in order");
}
void impulse_events_are_never_replayed_across_two_pending_commands() {
    auto first = request();
    first.impulse_id = 5;
    Command out;
    // last_impulse moves only when a command is taken, so the pending one's event is compared too.
    for (const std::uint32_t id : {5U, 4U, 1U}) {
        Mailbox box;
        const auto replay = after(first, id);
        check(box.submit(first, first.epoch, 0) && box.last_impulse == 0 && !box.submit(replay, replay.epoch, 16) && box.count == 1,
            "the pending command's impulse event was queued again behind it");
    }
    Mailbox box;
    const auto second = after(first, 6);
    check(box.submit(first, first.epoch, 0) && box.submit(second, second.epoch, 16), "a new impulse event was refused behind a pending one");
    check(box.consume(second.epoch, 17, out) && out.impulse_id == 5 && box.finish(true, true, true) && box.last_impulse == 5,
        "the first impulse event was not produced once");
    const auto replayed = after(second, 6), fresh = after(second, 7);
    check(!box.submit(replayed, replayed.epoch, 33) && box.submit(fresh, fresh.epoch, 33),
        "the event now pending first was not compared, or a new event was refused");
    check(box.consume(fresh.epoch, 34, out) && out.impulse_id == 6 && box.finish(true, true, true) &&
        box.consume(next(fresh.epoch), 50, out) && out.impulse_id == 7 && box.finish(true, true, true) && box.last_impulse == 7,
        "three impulse events were not each produced once, in order");
    // With or without an impulse on either command.
    auto none = request();
    none.impulse = {};
    none.impulse_id = 0;
    Mailbox plain;
    const auto with = after(none, 1), without = after(first);
    check(plain.submit(none, none.epoch, 0) && plain.submit(with, with.epoch, 16), "an impulse was refused behind a command without one");
    Mailbox mixed;
    check(mixed.submit(first, first.epoch, 0) && mixed.submit(without, without.epoch, 16), "a command without an impulse was refused behind one with");
    // An event already produced still bounds the second command when the first carries none.
    Mailbox earlier;
    const auto a = request(), b = after(a), old = after(b, 1), next_event = after(b, 2);
    check(earlier.submit(a, a.epoch, 0) && earlier.consume(b.epoch, 1, out) && earlier.finish(true, true, true) &&
        earlier.submit(b, b.epoch, 16), "the earlier impulse fixture failed");
    check(!earlier.submit(old, old.epoch, 33) && earlier.submit(next_event, next_event.epoch, 33),
        "a produced impulse event was replayed behind a pending command");
}
void two_pending_commands_are_acknowledged_in_the_order_they_were_submitted() {
    Mailbox box;
    const auto first = request(), second = after(first, 2);
    Command out;
    Receipt a, b;
    check(box.submit(first, first.epoch, 0) && box.submit(second, second.epoch, 16), "two pending commands were not queued");
    check(!box.acknowledge(1) && !box.acknowledge(2), "a queued command was acknowledged");
    check(box.consume(second.epoch, 17, out) && box.finish(true, true, true) && !box.acknowledge(1) && !box.acknowledge(2),
        "an applied result without a successor, or the command queued behind it, was acknowledged");
    Sample measured;
    measured.epoch = next(second.epoch);
    check(box.consume(measured.epoch, 33, out) && box.finish(true, true, true) && box.observe_successor(measured) && box.find(1, a) &&
        box.find(2, b) && a.successor_available && a.successor.epoch == measured.epoch && !b.successor_available,
        "the sample the second command was produced in was not the first's successor alone");
    check(!box.acknowledge(2) && box.acknowledge(1) && !box.acknowledge(2), "the second result was acknowledged before its own successor");
    measured.epoch = next(measured.epoch);
    check(box.observe_successor(measured) && box.acknowledge(2) && !box.count, "the second result could not be acknowledged in turn");
}
// One frame of a ride as the game orders it, counted there per frame: the clock edge, then either
// three calls on listed wrappers, the plugin's tick and three more calls (the full-rate order),
// or the tick and then three calls (the half-rate order).
struct Ride {
    Mailbox box;
    Epoch clock = request().epoch;
    std::uint64_t now = 1000, first_produced{}, last_produced{};
    unsigned applied{}, ticked{};
    std::size_t most_rows{};
    // The tick reads the sample, acknowledges every finished result and sends one command.
    void tick() {
        Sample sample;
        sample.epoch = clock;
        (void)box.observe_successor(sample);
        while (box.count) {
            const auto head = box.oldest();
            if (head.outcome == Outcome::queued || (head.outcome == Outcome::applied && !head.successor_available)) break;
            check(head.outcome == Outcome::applied && head.produced.serial == head.requested.serial + 1 &&
                head.successor.epoch.serial > head.produced.serial, "a command expired, was rejected or ran on another step");
            check(box.acknowledge(head.sequence), "a measured result could not be acknowledged");
        }
        Command c;
        c.epoch = clock;
        c.force = {0, 20, 0};
        check(box.submit(c, clock, now), "a tick's command was refused");
        ++ticked;
        most_rows = (std::max)(most_rows, box.count);
    }
    // Three calls on listed wrappers. At most the first takes a command.
    void synchronize() {
        for (unsigned call = 0; call < 3; ++call) {
            Command out;
            if (!box.consume(clock, now + call, out)) continue;
            check(out.epoch.serial + 1 == clock.serial && (!last_produced || clock.serial == last_produced + 1),
                "a step went without its command, or took another step's");
            last_produced = clock.serial;
            if (!first_produced) first_produced = clock.serial;
            ++applied;
            check(box.finish(true, true, true), "a consumed command did not finish");
        }
    }
    // The calls after a full-rate frame's tick read the epoch that tick just commanded, and wait.
    void frame(bool tick_first) {
        clock = next(clock);
        now += 16;
        if (!tick_first) synchronize();
        tick();
        const auto before = applied;
        synchronize();
        check(tick_first || applied == before, "a call after the tick took the command for the step it read");
    }
};
void a_command_is_applied_on_every_step_in_either_order_and_across_flips() {
    std::size_t full_rate_rows{};
    for (const std::string_view order : {"ffffffffffff", "hhhhhhhhhhhh", "hhhhffffhhhhffff", "ffffhhhhffffhhhh", "fhfhfhhffhhfhf"}) {
        Ride ride;
        const auto start = ride.clock.serial;
        for (const char kind : order) ride.frame(kind == 'h');
        check(ride.ticked == order.size() && ride.applied == order.size() - 1 && ride.first_produced == start + 2 &&
            ride.last_produced == ride.clock.serial, "a ride did not get one command applied on every step after its first");
        // After each acknowledgement and submit the ring holds what a full-rate ride holds.
        if (!full_rate_rows) full_rate_rows = ride.most_rows;
        check(ride.most_rows == full_rate_rows && ride.most_rows == 2 && ride.box.count <= 2,
            "the receipt ring held more rows than a full-rate ride's");
    }
}
}
int main() {
    try {
        definitions_are_bounded_and_have_no_gameplay_defaults();
        reuse_compares_the_shape_bit_for_bit_and_names_its_outcomes();
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
        the_next_steps_command_queues_behind_a_pending_one();
        a_second_pending_command_must_be_for_exactly_the_next_step();
        release_rejects_both_pending_commands();
        each_pending_command_gets_its_own_verdict();
        deadlines_alone_expire_each_pending_command_by_its_own();
        consume_judges_by_the_reading_it_was_handed();
        impulse_events_are_never_replayed_across_two_pending_commands();
        two_pending_commands_are_acknowledged_in_the_order_they_were_submitted();
        a_command_is_applied_on_every_step_in_either_order_and_across_flips();
        std::cout << "owned body contracts passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
