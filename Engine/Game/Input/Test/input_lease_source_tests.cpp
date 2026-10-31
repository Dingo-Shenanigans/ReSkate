#include "Engine/Game/Input/input_lease_policy.h"
#include "Extension/Input/native_input_source.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/input_source_environment.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace {
source::environment::Facts facts{1000, 1, true, true, true, true};
unsigned checks{};
void check(bool value, const char* name) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
void report(std::uint32_t user = 0, std::uint32_t packet = 7,
    lease::Pad raw = {}, lease::Pad returned = {}, std::uint32_t result = 0,
    bool raw_known = true, bool returned_known = true, std::uint32_t returned_result = 0) {
    source::Poll poll;
    source::begin(true, user, poll);
    ++facts.now_ms;
    source::finish(poll, result, raw_known, packet, raw,
        returned_result, returned_known, packet, returned);
}
void setup() { source::tick(1, 2, true); report(); }
lease::Handle request(std::uint64_t owner = 1, std::uint32_t user = 0) {
    lease::Handle handle; lease::Reason reason;
    check(source::lease_request(owner, user, handle, reason), "request accepted");
    check(handle.sequence && reason == lease::Reason::none, "request nonzero exact handle");
    return handle;
}
lease::Receipt state(lease::Handle handle, std::uint64_t owner = 1) {
    lease::Receipt receipt; lease::Reason reason;
    check(source::lease_state(owner, handle, receipt, reason), "receipt accepted");
    check(receipt.handle == handle && reason == lease::Reason::none, "receipt exact handle");
    return receipt;
}
void close(lease::Handle handle, std::uint64_t owner = 1) {
    lease::Reason reason;
    auto receipt = state(handle, owner);
    if (!lease::terminal(receipt.status)) check(source::lease_release(owner, handle, reason), "release accepted");
    check(source::lease_acknowledge(owner, handle, reason), "terminal ack accepted");
}
lease::Receipt warming_copy_;
void admission() {
    lease::Handle handle; lease::Reason reason; lease::Snapshot snapshot;
    check(!source::lease_request(1, 0, handle, reason), "cold request denied");
    setup();
    check(!source::lease_request(0, 0, handle, reason) && reason == lease::Reason::invalid_owner, "zero owner denied");
    check(!source::lease_request(1, 4, handle, reason) && reason == lease::Reason::invalid_source, "user four denied");
    handle = request();
    const auto warming = state(handle);
    check(warming.status == lease::Status::warming && !warming.observed_ms && !warming.last_observation &&
        warming.source_epoch && warming.context_generation && !warming.source_released, "warming is boundary not sample");
    check(!source::lease_read(1, handle, snapshot, reason), "cached admission cannot warm");
    check(!source::lease_read(2, handle, snapshot, reason) && reason == lease::Reason::unavailable, "cross owner denied");
    report(0, 8);
    check(source::lease_read(1, handle, snapshot, reason) && snapshot.observation_serial && snapshot.source_epoch, "later original supplies sample");
    auto forged = snapshot; ++forged.observation_serial;
    check(!source::lease_renew(1, handle, forged, reason), "forged snapshot denied");
    check(source::lease_renew(1, handle, snapshot, reason), "exact issued observation renews");
    check(!source::lease_renew(1, handle, snapshot, reason), "same observation cannot renew twice");
    facts.thread = 2;
    check(!source::lease_state(1, handle, warming_copy_, reason) && reason == lease::Reason::wrong_thread, "wrong API thread denied");
    facts.thread = 1; close(handle);
}
void raw() {
    setup(); auto handle = request(); lease::Reason reason; lease::Snapshot sample;
    source::Poll before;
    source::begin(false, 0, before);
    source::finish(before, 0, true, 2, {}, 0, true, 2, {});
    check(!source::lease_read(1, handle, sample, reason), "nonouter poll cannot warm");
    lease::Pad changed{}; changed.buttons = 1;
    report(0, 9, {}, changed);
    check(state(handle).status == lease::Status::revoked, "transformed pad revokes");
    close(handle); report(); handle = request();
    report(0, 9, {}, {}, 0, false, true);
    check(state(handle).status == lease::Status::revoked, "unknown raw revokes");
    close(handle); report(); handle = request();
    report(0, 9, {}, {}, 1167, false, false, 1167);
    check(state(handle).status == lease::Status::revoked, "disconnect revokes");
    close(handle); report(); handle = request();
    report(0, 9, {}, {}, 0, true, true, 1167);
    check(state(handle).status == lease::Status::revoked, "returned result mismatch revokes");
    close(handle); report(); handle = request();
    source::Poll packet_mismatch; source::begin(true, 0, packet_mismatch);
    ++facts.now_ms;
    source::finish(packet_mismatch, 0, true, 9, {}, 0, true, 10, {});
    check(state(handle).status == lease::Status::revoked, "returned packet mismatch revokes");
    close(handle); report(); handle = request();
    report(0, 9, {}, {}, 0, true, false);
    check(state(handle).status == lease::Status::revoked, "unknown returned output revokes");
    close(handle);
}
void cleanup() {
    setup(); auto handle = request(); lease::Reason reason;
    source::Poll pending; source::begin(true, 0, pending);
    check(source::lease_release(1, handle, reason), "release while original active");
    auto receipt = state(handle);
    check(receipt.status == lease::Status::released && receipt.polls_in_flight == 1 && !receipt.source_released, "terminal reservation retained");
    check(!source::lease_acknowledge(1, handle, reason) && reason == lease::Reason::busy, "ack waits exact callback drain");
    lease::Handle other;
    check(!source::lease_request(2, 0, other, reason) && reason == lease::Reason::busy, "replacement waits old drain");
    ++facts.now_ms; source::finish(pending, 0, true, 9, {}, 0, true, 9, {});
    receipt = state(handle);
    check(receipt.source_released && !receipt.polls_in_flight, "actual finish drains reservation");
    check(source::lease_acknowledge(1, handle, reason), "drained ack accepted");
    handle = request();
    {
        source::Poll abandoned; source::begin(true, 0, abandoned);
        facts.thread = 2; source::lease_release_all(1); source::lease_release_all(1); facts.thread = 1;
        check(!source::lease_state(1, handle, receipt, reason), "closed owner cannot reacquire orphan");
        check(!source::lease_request(2, 0, other, reason), "orphan still reserves old callback");
    }
    report(); other = request(2);
    lease::Snapshot snapshot;
    check(!source::lease_read(2, other, snapshot, reason), "abandoned old poll cannot seed replacement");
    close(other, 2);
}
void publication() {
    setup(); const auto handle = request();
    source::PublishedAtomic<bool> interference{false};
    source::Poll pending; source::begin(true, 0, pending);
    const auto before = source::publication_revision();
    interference.store(true); interference.store(false);
    check(source::publication_revision() > before && !interference.load(), "ABA writer revision advances");
    ++facts.now_ms; source::finish(pending, 0, true, 9, {}, 0, true, 9, {});
    const auto receipt = state(handle);
    check(receipt.status == lease::Status::revoked && receipt.reason == lease::Reason::interference &&
        receipt.source_released && !receipt.last_observation, "ABA completion never publishes");
    close(handle);
}
void environment() {
    setup(); auto handle = request();
    facts.focused = false;
    check(state(handle).reason == lease::Reason::focus_lost, "focus stop recorded");
    facts.focused = true; close(handle); report(); handle = request();
    facts.gameplay = false;
    check(state(handle).reason == lease::Reason::gameplay_inactive, "gameplay stop recorded");
    facts.gameplay = true; close(handle); report(); handle = request();
    facts.menu_closed = false;
    check(state(handle).reason == lease::Reason::menu_open, "menu stop recorded");
    facts.menu_closed = true; close(handle); report(); handle = request();
    facts.interference_clear = false;
    check(state(handle).reason == lease::Reason::interference, "interference stop recorded");
    facts.interference_clear = true; close(handle); report(); handle = request();
    source::tick(1, 3, true);
    check(state(handle).reason == lease::Reason::context_changed, "context replacement revokes");
    close(handle); report(); handle = request();
    source::tick(1, 3, false);
    check(lease::terminal(state(handle).status), "not-ready cleanup retained");
    close(handle);
}
void lifetime() {
    setup(); auto handle = request();
    facts.now_ms += 101;
    check(state(handle).status == lease::Status::expired, "warming stale boundary expires");
    close(handle); source::tick(1, 2, true); report(); handle = request();
    const auto admitted = state(handle);
    for (unsigned i = 0; i < 3; ++i) { facts.now_ms += 79; source::tick(1, 2, true); report(0, i); }
    facts.now_ms = admitted.expires_ms;
    check(state(handle).reason == lease::Reason::deadline, "exact TTL closes despite recent samples");
    close(handle); source::tick(1, 2, true); report(); handle = request();
    --facts.now_ms;
    check(state(handle).reason == lease::Reason::stale_epoch, "wall clock regression closes");
    close(handle);
}
void overlap() {
    setup(); auto handle = request(); lease::Reason reason; lease::Snapshot sample;
    source::Poll older, newer;
    source::begin(true, 0, older); source::begin(true, 0, newer);
    ++facts.now_ms; source::finish(newer, 0, true, 22, {}, 0, true, 22, {});
    ++facts.now_ms; source::finish(older, 1167, false, 0, {}, 1167, false, 0, {});
    check(source::lease_read(1, handle, sample, reason) && sample.packet == 22, "old disconnect cannot overwrite newer completion");
    close(handle);
    source::Poll predating; source::begin(true, 0, predating);
    handle = request(); ++facts.now_ms;
    source::finish(predating, 0, true, 23, {}, 0, true, 23, {});
    check(!source::lease_read(1, handle, sample, reason), "old original cannot warm newer lease");
    close(handle);
}
void capacity() {
    setup(); std::array<lease::Handle, lease::retained_capacity> handles{}; lease::Reason reason;
    for (auto& handle : handles) {
        report(); handle = request(); check(source::lease_release(1, handle, reason), "retain terminal slot");
    }
    lease::Handle extra;
    check(!source::lease_request(1, 0, extra, reason) && reason == lease::Reason::capacity, "retained table bounded");
    check(source::lease_acknowledge(1, handles[0], reason), "ack frees exact slot");
    extra = request();
    {
        std::array<source::Poll, 129> callbacks;
        for (auto& callback : callbacks) source::begin(true, 0, callback);
        auto receipt = state(extra);
        check(receipt.status == lease::Status::revoked && receipt.reason == lease::Reason::capacity &&
            receipt.polls_in_flight == 128 && !receipt.source_released, "callback storage exhausts closed and bounded");
    }
    check(state(extra).source_released, "all real callback destructors drain");
    close(extra);
    for (std::size_t i = 1; i < handles.size(); ++i) check(source::lease_acknowledge(1, handles[i], reason), "other retained receipts intact");
}
void policy() {
    lease::Pad pad; pad.left_x = static_cast<std::int16_t>(lease::left_stick_rest);
    check(lease::neutral(pad), "XInput circular dead-zone boundary accepted");
    pad.left_y = 1; check(!lease::neutral(pad), "diagonal outside radius not neutral");
    pad = {}; pad.left_trigger = 31; check(!lease::neutral(pad), "trigger above threshold not neutral");
    pad = {}; pad.buttons = 1; check(!lease::neutral(pad), "buttons never neutral");
    lease::Broker broker{~std::uint64_t{0}};
    lease::Source observed{0, 1, 1, 1, 1000, 1, {}, true, true};
    lease::Guard guard{1000, 1, true, true, true, true, true};
    lease::Handle last; lease::Reason reason;
    check(broker.request(1, 0, observed, guard, false, last, reason), "last sequence admitted");
    check(broker.release(1, last) && broker.acknowledge(1, last), "last sequence cleans");
    check(!broker.request(1, 0, observed, guard, false, last, reason) && reason == lease::Reason::sequence_exhausted, "sequence cannot wrap");
}
}
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept { return facts; }
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "admission") admission();
    else if (mode == "raw") raw();
    else if (mode == "cleanup") cleanup();
    else if (mode == "publication") publication();
    else if (mode == "environment") environment();
    else if (mode == "lifetime") lifetime();
    else if (mode == "overlap") overlap();
    else if (mode == "capacity") capacity();
    else if (mode == "policy") policy();
    else return 2;
    std::printf("PASS %s %u constructed source checks\n", argv[1], checks);
    return 0;
}
