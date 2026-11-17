#include "Engine/Game/Input/input_lease_policy.h"
#include "Extension/Input/native_input_source.h"
#include "Extension/Input/native_input_capture.h"
#include "Extension/Input/native_menu_observation.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/input_source_environment.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <atomic>
#include <chrono>
#include <thread>
#include <Windows.h>

namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace {
source::environment::Facts facts{1000, 1, true, true, true, true};
unsigned checks{};
std::atomic<unsigned> sample_calls{};
std::atomic<bool> threaded_mode{}, stop_poller{}, press_mode{}, capture_writes{}, thread_violation{};
std::atomic<unsigned> threaded_writes{}, threaded_polls{};
struct Menu { bool known{}, open{}; };
void test_tick(std::uintptr_t base, std::uintptr_t client, bool ready, Menu menu = {}) {
    using namespace dingosdk::native_menu_observation;
    publish(menu.known ? (menu.open ? State::open : State::closed) : State::unknown, facts.now_ms);
    source::tick(base, client, ready);
}

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
void setup() { test_tick(1, 2, true, {true, false}); report(); }
lease::Handle request(std::uint64_t owner = 1, std::uint32_t user = 0) {
    lease::Handle handle; lease::Reason reason;
    if (!source::lease_request(owner, user, handle, reason) && (reason == lease::Reason::source_unavailable || reason == lease::Reason::source_stale)) {
        report(user);
        check(source::lease_request(owner, user, handle, reason), "request accepted after registered poll");
    } else check(handle.sequence != 0, "request accepted");
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
    // reskate.input.write replaces a report like this and a lease does not reserve against it:
    // the replacement wins, and no lease can stand beside it until it ends.
    close(handle); report(); handle = request();
    report(0, 9, {}, changed);
    check(state(handle).reason == lease::Reason::source_unavailable, "a replaced report revokes as unavailable");
    close(handle); report(0, 10, {}, changed);
    check(!source::lease_request(1, 0, handle, reason) && reason == lease::Reason::source_unavailable, "no lease on a replaced stream");
    report(); handle = request();
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
    test_tick(1, 3, true, {true, false});
    check(state(handle).reason == lease::Reason::context_changed, "context replacement revokes");
    close(handle); report(); handle = request();
    test_tick(1, 3, false);
    check(lease::terminal(state(handle).status), "not-ready cleanup retained");
    close(handle);
}
void lifetime() {
    setup(); auto handle = request();
    facts.now_ms += 101;
    test_tick(1, 2, true, {true, false});
    check(state(handle).status == lease::Status::expired, "warming stale boundary expires");
    close(handle); test_tick(1, 2, true, {true, false}); report(); handle = request();
    const auto admitted = state(handle);
    for (unsigned i = 0; i < 3; ++i) { facts.now_ms += 79; test_tick(1, 2, true, {true, false}); report(0, i); }
    facts.now_ms = admitted.expires_ms;
    check(state(handle).reason == lease::Reason::deadline, "exact TTL closes despite recent samples");
    close(handle); test_tick(1, 2, true, {true, false}); report(); handle = request();
    --facts.now_ms;
    check(state(handle).reason != lease::Reason::stale_epoch, "older sample cannot revoke a lease by itself");
    ++facts.now_ms;
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
    lease::Handle blocked;
    check(!source::lease_request(1, 0, blocked, reason) && reason == lease::Reason::busy,
        "unarmed original blocks admission instead of warming a later lease");
    ++facts.now_ms; source::finish(predating, 0, true, 23, {}, 0, true, 23, {});
    report(); handle = request();
    check(!source::lease_read(1, handle, sample, reason), "admission remains a warming boundary");
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
void idle_poll() {
    test_tick(1, 2, true, {true, false});
    const auto sampled = sample_calls.load();
    for (unsigned i = 0; i < 1000; ++i) test_tick(1, 2, true, {true, false});
    check(sample_calls.load() == sampled, "unarmed client ticks do not sample the environment");
    const auto gates = source::gate_entries();
    for (unsigned i = 0; i < 1000; ++i) {
        source::Poll poll; source::begin(true, 0, poll);
        source::finish(poll, 0, true, i, {}, 0, true, i, {});
    }
    check(sample_calls.load() == sampled && source::gate_entries() == gates,
        "unarmed polls take no environment sample or publication gate");
    source::PublishedAtomic<bool> same{false};
    same.store(false); (void)same.exchange(false);
    check(source::gate_entries() == gates, "unchanged publications take no gate");
    source::Poll pending; source::begin(true, 0, pending);
    lease::Handle handle; lease::Reason reason;
    check(!source::lease_request(1, 0, handle, reason) && reason == lease::Reason::busy,
        "request cannot arm across an unregistered original in flight");
    source::finish(pending, 0, true, 7, {}, 0, true, 7, {});
    report();
    check(source::lease_request(1, 0, handle, reason), "registered poll permits a later request");
    close(handle);
    test_tick(1, 3, true, {true, false});
    lease::Handle stale;
    check(!source::lease_request(2, 0, stale, reason) && reason == lease::Reason::source_unavailable,
        "context changed during an unarmed stretch refuses an old stream");
}
struct ThreadBuffer { std::uint32_t packet{}; lease::Pad pad{}; std::uint32_t tail{0xabcddcba}; };
bool thread_publish(void* output, std::uint32_t packet, const lease::Pad& pad) noexcept {
    auto& buffer = *static_cast<ThreadBuffer*>(output);
    if (!source::publication_held() || buffer.tail != 0xabcddcba ||
        (capture_writes.load(std::memory_order_relaxed) && (pad.buttons & ~std::uint16_t{0x10})))
        thread_violation.store(true, std::memory_order_relaxed);
    buffer.packet = packet; buffer.pad = pad;
    threaded_writes.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void threaded_poll() {
    threaded_mode.store(true);
    auto tick = [] {
        dingosdk::native_menu_observation::publish(dingosdk::native_menu_observation::State::closed, GetTickCount64());
        source::tick(1, 2, true);
    };
    tick();
    std::thread poller([] {
        std::uint32_t packet{1};
        while (!stop_poller.load(std::memory_order_acquire)) {
            source::Poll poll; source::begin(true, 0, poll);
            lease::Pad raw{};
            if (press_mode.load(std::memory_order_acquire)) raw.buttons = 0x1011;
            ThreadBuffer buffer{packet, raw};
            const auto result = source::finish_capture(poll, 0, true, packet, raw,
                0, true, packet, raw, source::publication_revision(), &buffer, &thread_publish);
            if (result || buffer.tail != 0xabcddcba) thread_violation.store(true);
            ++packet;
            threaded_polls.fetch_add(1, std::memory_order_relaxed);
            // No sleep: the poller runs as fast as it can against the client thread's requests.
            std::this_thread::yield();
        }
    });
    lease::Handle handle; lease::Reason reason{}; lease::Snapshot snapshot;
    dingosdk::input_lease_capture::Reason capture_reason{};
    dingosdk::input_lease_capture::Receipt receipt;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto wait_for = [&](auto predicate, const char* why) {
        while (std::chrono::steady_clock::now() < until) {
            tick();
            if (predicate()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)source::capture_state(1, handle, receipt);
        lease::Receipt passive; lease::Reason passive_reason{};
        (void)source::lease_state(1, handle, passive, passive_reason);
        std::fprintf(stderr, "timeout: lease=%u capture=%u phase=%u receipt_reason=%u passive=%u passive_reason=%u polls=%u writes=%u violation=%u\n",
            static_cast<unsigned>(reason), static_cast<unsigned>(capture_reason),
            static_cast<unsigned>(receipt.phase), static_cast<unsigned>(receipt.reason),
            static_cast<unsigned>(passive.status), static_cast<unsigned>(passive.reason),
            threaded_polls.load(), threaded_writes.load(), thread_violation.load());
        stop_poller.store(true); poller.join(); check(false, why);
    };
    wait_for([&] { return source::lease_request(1, 0, handle, reason); }, "threaded lease admission");
    wait_for([&] { return source::lease_read(1, handle, snapshot, reason); }, "threaded lease sample");
    wait_for([&] {
        if (!source::lease_read(1, handle, snapshot, reason)) return false;
        return source::capture_request(1, handle, snapshot, capture_reason);
    }, "threaded capture admission");
    wait_for([&] { return source::capture_state(1, handle, receipt) &&
        receipt.phase == dingosdk::input_lease_capture::Phase::capturing; }, "threaded capture publication");
    const auto before_pressed = threaded_writes.load();
    capture_writes.store(true); press_mode.store(true);
    wait_for([&] { return threaded_writes.load() > before_pressed + 3; }, "threaded captured writes");
    press_mode.store(false); capture_writes.store(false);
    check(source::capture_release(1, handle), "threaded capture release");
    wait_for([&] { return source::capture_state(1, handle, receipt) &&
        receipt.phase == dingosdk::input_lease_capture::Phase::complete; }, "threaded handback");
    stop_poller.store(true); poller.join();
    check(!thread_violation.load() && threaded_writes.load() > 0,
        "threaded writes hold publication, keep buffer tail and mask pressed controls except Start");
    check(source::capture_acknowledge(1, handle), "threaded capture ACK");
    lease::Receipt passive;
    check(source::lease_state(1, handle, passive, reason), "threaded passive receipt");
    check(passive.reason != lease::Reason::stale_epoch, "racing tick and poll clocks never revoke stale epoch");
    if (!lease::terminal(passive.status)) check(source::lease_release(1, handle, reason), "threaded lease release");
    check(source::lease_acknowledge(1, handle, reason), "threaded lease ACK");
    tick();
    lease::Handle fresh;
    if (!source::lease_request(2, 0, fresh, reason) && reason == lease::Reason::source_unavailable) {
        report();
        check(source::lease_request(2, 0, fresh, reason), "fresh lease after registered cleanup poll");
    }
    check(fresh.sequence, "fresh lease after threaded cleanup");
    check(source::lease_release(2, fresh, reason) && source::lease_acknowledge(2, fresh, reason),
        "fresh threaded lease cleans");
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
std::uint64_t test_tick_now() noexcept { return threaded_mode.load(std::memory_order_relaxed) ? GetTickCount64() : facts.now_ms; }
std::uint32_t test_tick_thread() noexcept { return threaded_mode.load(std::memory_order_relaxed) ? GetCurrentThreadId() : facts.thread; }
Facts sample() noexcept {
    sample_calls.fetch_add(1, std::memory_order_relaxed);
    if (threaded_mode.load(std::memory_order_relaxed)) return {GetTickCount64(), GetCurrentThreadId(), true, true, true, true};
    return facts;
}
bool trusted_capture() noexcept { return false; }
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
    else if (mode == "idle_poll") idle_poll();
    else if (mode == "threaded_poll") threaded_poll();
    else return 2;
    std::printf("PASS %s %u constructed source checks\n", argv[1], checks);
    return 0;
}
