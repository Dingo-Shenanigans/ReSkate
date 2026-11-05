#include "Extension/Input/native_input_capture.h"
#include "Engine/Game/Input/input_lease_capture_policy.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/input_source_environment.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace capture = dingosdk::input_lease_capture;
namespace {
source::environment::Facts facts{1000, 1, true, true, true, true};
unsigned checks{}, writes{};
lease::Pad offered_pad{};
enum class Write { full, refused, partial } write_mode{Write::full};
struct Buffer { std::uint32_t packet{}; lease::Pad pad{}; std::uint32_t tail{0xabcddcba}; };
void check(bool value, const char* why) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
bool publish(void* output, std::uint32_t packet, const lease::Pad& pad) noexcept {
    check(source::publication_held(), "actual provider holds gate at the bounded write");
    ++writes; offered_pad = pad; auto& buffer = *static_cast<Buffer*>(output);
    if (write_mode == Write::refused) return false;
    buffer.packet = packet;
    if (write_mode == Write::partial) return false;
    buffer.pad = pad;
    return true;
}
Buffer finish(source::Poll& poll, lease::Pad raw = {}, lease::Pad returned = {},
    std::uint32_t result = 0, bool raw_known = true, bool returned_known = true,
    std::uint32_t selected_result = 0, std::uint32_t original_packet = 7,
    std::uint32_t returned_packet = 7, std::uint64_t revision = 0) {
    Buffer buffer{returned_packet, returned};
    ++facts.now_ms;
    const auto selected = source::finish_capture(poll, result, raw_known, original_packet, raw,
        selected_result, returned_known, returned_packet, returned,
        revision ? revision : source::publication_revision(), &buffer, &publish);
    check(selected == selected_result, "finalizer never invents transport success");
    check(buffer.tail == 0xabcddcba, "publication touches only bounded report destination");
    check(!source::publication_held(), "actual finalizer releases publication gate");
    return buffer;
}
Buffer report(lease::Pad raw = {}, lease::Pad returned = {}, std::uint32_t result = 0,
    bool known = true, std::uint32_t returned_packet = 7) {
    source::Poll poll; source::begin(true, 0, poll);
    return finish(poll, raw, returned, result, known, known, result, 7, returned_packet);
}
lease::Snapshot sample(lease::Handle handle, std::uint64_t owner = 1) {
    lease::Reason reason; lease::Snapshot snapshot;
    check(source::lease_read(owner, handle, snapshot, reason), "actual broker issues snapshot");
    return snapshot;
}
lease::Handle lease_only() {
    source::tick(1, 2, true); report();
    lease::Handle handle; lease::Reason reason;
    check(source::lease_request(1, 0, handle, reason), "passive owner admitted");
    report(); (void)sample(handle); return handle;
}
lease::Handle arm() {
    const auto handle = lease_only(); const auto snapshot = sample(handle);
    capture::Reason reason;
    check(source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::none,
        "capture needs actual issued accepted source");
    return handle;
}
capture::Receipt state(lease::Handle handle) {
    capture::Receipt receipt;
    check(source::capture_state(1, handle, receipt), "actual capture receipt present");
    check(receipt.key.owner == 1 && receipt.key.lease_sequence == handle.sequence && receipt.key.user == 0,
        "receipt retains exact owner and lease identity");
    return receipt;
}
lease::Handle start() {
    const auto handle = arm();
    check(state(handle).phase == capture::Phase::waiting_neutral, "request does not capture immediately");
    const auto before = writes; const auto buffer = report();
    const auto receipt = state(handle);
    check(receipt.phase == capture::Phase::capturing && receipt.debt && receipt.publications == 1 &&
        !receipt.neutral_releases && writes == before + 1 && buffer.pad == lease::Pad{},
        "only a real full report write starts capture and records debt");
    return handle;
}
void close(lease::Handle handle) {
    if (state(handle).phase != capture::Phase::complete) {
        check(source::capture_release(1, handle), "owner requests release once"); report();
    }
    check(source::capture_acknowledge(1, handle), "capture ACK after real release/drain");
    lease::Reason reason; lease::Receipt passive;
    check(source::lease_state(1, handle, passive, reason), "passive receipt remains separate");
    if (!lease::terminal(passive.status)) check(source::lease_release(1, handle, reason), "end separate passive reservation");
    check(source::lease_acknowledge(1, handle, reason), "passive ACK after capture reservation cleared");
}
void denial() {
    const auto handle = lease_only(); const auto snapshot = sample(handle);
    capture::Reason reason; capture::Receipt receipt;
    check(!source::capture_request(2, handle, snapshot, reason), "foreign owner cannot adopt issued stream");
    auto forged = snapshot; ++forged.observation_serial;
    check(!source::capture_request(1, handle, forged, reason), "forged scalar snapshot refused");
    forged = snapshot; forged.pad.buttons = 1;
    check(!source::capture_request(1, handle, forged, reason), "forged raw pad refused");
    facts.thread = 2;
    check(!source::capture_request(1, handle, snapshot, reason), "wrong native client thread refused");
    facts.thread = 1;
    source::Poll predating; source::begin(true, 0, predating);
    check(!source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::busy,
        "capture cannot ignore an already invoking original");
    (void)finish(predating);
    const auto fresh = sample(handle);
    check(source::capture_request(1, handle, fresh, reason), "capture can use new exact issued observation after drain");
    check(!source::capture_state(2, handle, receipt) && !source::capture_release(2, handle) &&
        !source::capture_acknowledge(2, handle), "all cleanup remains exact owner scoped");
    close(handle);
}
void cold_original() {
    source::Poll cold; source::begin(true, 0, cold); // before verified client tick
    const auto handle = lease_only(); const auto snapshot = sample(handle); capture::Reason reason;
    check(!source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::busy,
        "cold original is included in capture admission drain");
    (void)finish(cold);
    check(source::capture_request(1, handle, snapshot, reason), "drained cold original cannot seed but no longer blocks");
    close(handle);
}
void publication() {
    const auto handle = arm(); lease::Pad pressed{}; pressed.buttons = 0x1000;
    const auto before = writes; auto buffer = report(pressed, pressed);
    check(buffer.pad == pressed && writes == before && state(handle).phase == capture::Phase::waiting_neutral,
        "waiting does not consume a nonneutral source");
    report(); buffer = report(pressed, pressed);
    const auto issued = sample(handle);
    check(buffer.pad == lease::Pad{} && issued.pad == pressed && state(handle).debt,
        "actual captured game buffer differs from unmodified issued raw observation");
    const auto publications = state(handle).publications;
    source::Poll other; source::begin(true, 1, other);
    const auto unselected = finish(other, pressed, pressed);
    check(unselected.pad == pressed && state(handle).publications == publications,
        "a different XInput user is never suppressed or counted as this capture");
    close(handle);
}
void late_drain() {
    const auto handle = start(); check(source::capture_release(1, handle), "explicit release accepted");
    source::Poll older, newer; source::begin(true, 0, older); source::begin(true, 0, newer);
    (void)finish(newer);
    auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.polls_in_flight == 1 &&
        !receipt.neutral_releases,
        "newer publication cannot hand back while an older callback can still write");
    check(!source::capture_acknowledge(1, handle), "ACK refuses retained duty before exact callback drain");
    lease::Reason reason;
    check(!source::lease_acknowledge(1, handle, reason), "passive ACK also respects captured callback");
    lease::Pad pressed{}; pressed.buttons = 0x8000; const auto buffer = finish(older, pressed, pressed);
    receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases && !receipt.polls_in_flight,
        "old completion maintains retained duty but cannot supply a fresh handback witness");
    report(); check(state(handle).neutral_releases == 1, "new later original alone supplies confirmed raw handback");
    close(handle);
}
void interference() {
    const auto handle = start(); const auto identity = state(handle).key;
    source::Poll retained; source::begin(true, 0, retained);
    source::PublishedAtomic<bool> writer{false}; writer.store(true);
    facts.menu_closed = false; facts.interference_clear = false;
    const auto menu_packet = std::uint32_t{7} ^ 0x80000000u;
    (void)report({}, {}, 0, true, menu_packet);
    (void)finish(retained, {}, {}, 0, true, true, 0, 7, menu_packet);
    facts.now_ms += 1100; source::tick(1, 2, true);
    (void)report({}, {}, 0, true, menu_packet);
    auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases && !receipt.polls_in_flight && !receipt.callbacks_unknown,
        "menu interference never fabricates completion; exact earlier callback has actually drained");
    lease::Reason reason; lease::Snapshot no_authority;
    check(!source::lease_read(1, handle, no_authority, reason), "menu revoked passive acquisition authority");
    writer.store(false); facts.menu_closed = true; facts.interference_clear = true;
    const auto before = writes; (void)report(); receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        receipt.neutral_releases == 1 && !receipt.polls_in_flight && !receipt.callbacks_unknown && writes == before + 1,
        "same selected stream plus ONE real clean neutral write and known drain complete retained menu cleanup");
    check(!source::lease_read(1, handle, no_authority, reason), "cleanup never reacquired passive input authority");
    close(handle);
}
void replacement() {
    const auto handle = start(); const auto identity = state(handle).key;
    check(source::capture_release(1, handle), "release before stream replacement");
    (void)report({}, {}, 1167, false);
    (void)report(); // reconnect creates another observed stream epoch
    auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        receipt.quarantined && !receipt.neutral_releases,
        "replacement stream neutral cannot prove release for old selected epoch");
    facts.now_ms += capture::Capture::maximum_release_ms;
    receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.quarantined && receipt.debt &&
        !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "1s replacement boundary retains unknown old-stream duty and cannot authorize ACK");
    lease::Reason reason; lease::Receipt passive;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released &&
        !source::lease_acknowledge(1, handle, reason), "replacement cannot bypass the retained capture through passive ACK");
}
void context_release() {
    const auto handle = start(); const auto identity = state(handle).key;
    source::tick(1, 2, false);
    (void)report(); // callback/readiness authority is gone; transport is still the same observed stream
    const auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        receipt.neutral_releases == 1 && !receipt.callbacks_unknown,
        "client context invalidation does not erase the old key or forbid same-stream cleanup-only publication");
    lease::Snapshot no_authority; lease::Reason reason;
    check(!source::lease_read(1, handle, no_authority, reason), "context cleanup does not authorize a new input sample");
    close(handle);
}
void staged_aba() {
    const auto handle = start(); source::Poll poll; source::begin(true, 0, poll);
    const auto staged = source::publication_revision(); source::PublishedAtomic<bool> writer{false};
    writer.store(true); writer.store(false);
    lease::Pad pressed{}; pressed.buttons = 0x8000;
    const auto buffer = finish(poll, pressed, pressed, 0, true, true, 0, 7, 7, staged);
    const auto receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases, "stale staged write gets only owed neutral fallback, no completion");
    report(); close(handle);
}
void failed_write() {
    const auto handle = arm(); write_mode = Write::partial;
    const auto buffer = report(); auto receipt = state(handle);
    check(buffer.packet != 7 && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.publications && !receipt.neutral_releases,
        "actual partial destination mutation retains unconfirmed debt even on first write");
    check(!source::capture_acknowledge(1, handle), "partial publication never ACKs");
    write_mode = Write::full; report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && !receipt.neutral_releases,
        "unknown partial output first needs one confirmed translated publication");
    report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "later confirmed raw handback resolves partial-write debt");
    close(handle);
}
void transport() {
    const auto handle = start(); const auto before = writes;
    report({}, {}, 1167, false);
    auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && writes == before,
        "failed original is not invented success or output");
    facts.now_ms += capture::Capture::maximum_release_ms;
    receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "lost-source 1s boundary retains unknown duty, never neutral consumption or ACK");
}
void raw_final() {
    const auto handle = start(); lease::Pad authored{}; authored.buttons = 0x8000;
    (void)report({}, authored); // first refusal establishes releasing duty
    const auto competing = report({}, authored);
    auto receipt = state(handle);
    check(competing.pad == authored && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases, "current independent candidate wins once held controls rest; no release is invented");
    (void)report({}, {}, 0, true, 8); receipt = state(handle);
    check(receipt.debt && !receipt.neutral_releases, "different raw/final packet cannot witness capture release");
    source::Poll unknown; source::begin(true, 0, unknown); const auto before = writes;
    (void)finish(unknown, {}, {}, 0, true, false);
    check(writes == before && state(handle).debt, "unknown final output is never confirmed");
    report(); close(handle);
}
void late_age() {
    const auto handle = start(); check(source::capture_release(1, handle), "release before aged callback");
    source::Poll slow; source::begin(true, 0, slow); facts.now_ms += 101;
    (void)finish(slow);
    const auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && !receipt.neutral_releases,
        "slow original/filter cannot become fresh merely at finalization");
    report(); close(handle);
}
void renewal() {
    const auto handle = start(); const auto issued = sample(handle); facts.now_ms += 37;
    lease::Reason reason;
    check(source::lease_renew(1, handle, issued, reason), "exact fresh issued snapshot renews both source and capture");
    check(state(handle).expires_ms == facts.now_ms + 250 && state(handle).expires_ms > issued.observed_ms + 250,
        "capture follows actual now-plus250 passive renewal, not projection time");
    check(!source::lease_renew(1, handle, issued, reason), "one observation cannot renew twice");
    close(handle);
}
void owner_close() {
    const auto handle = start(); source::Poll old; source::begin(true, 0, old);
    source::lease_release_all(1); source::lease_release_all(1);
    auto receipt = state(handle);
    check(receipt.orphaned && receipt.debt && receipt.polls_in_flight == 1, "owner close retains exact pending original and owed write");
    report(); check(state(handle).phase == capture::Phase::releasing && state(handle).debt,
        "orphan raw handback waits for the old selected-stream callback");
    lease::Pad pressed{}; pressed.buttons = 1; check(finish(old, pressed, pressed).pad == lease::Pad{}, "old orphan original stays neutral");
    report();
    check(!source::capture_state(1, handle, receipt), "orphan reclaimed only after real release and callback drain");
    report(); lease::Handle next; lease::Reason reason;
    check(source::lease_request(2, 0, next, reason), "different owner gets only a fresh passive handle after cleanup");
    check(next.sequence != handle.sequence, "owner cleanup never resets source handle sequence");
    source::lease_release_all(2);
}
void held_tail() {
    const auto handle = start(); const auto identity = state(handle).key;
    check(source::capture_release(1, handle), "release owns the same key throughout physical hold");
    lease::Pad pressed{}; pressed.buttons = 0x1000; pressed.left_trigger = 255; pressed.left_x = 20000;
    (void)report(pressed, pressed); facts.now_ms += capture::Capture::maximum_release_ms;
    (void)report(pressed, pressed); auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        receipt.quarantined && !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "1s keeps physically held capture tail reserved and refuses premature ACK");
    lease::Receipt passive; lease::Reason reason;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released &&
        !source::lease_acknowledge(1, handle, reason), "passive ACK cannot bypass tail work");
    const auto before = receipt.publications; write_mode = Write::partial;
    (void)report(pressed, pressed); receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        receipt.reason == capture::Reason::publication_unconfirmed && receipt.publications == before &&
        !source::capture_acknowledge(1, handle), "pre-ACK partial tail write is charged and cannot silently clear debt");
    write_mode = Write::full; (void)report(pressed, pressed);
    check(state(handle).publications == before + 1, "confirmed tail write remains part of original capture ledger");
    (void)report(); receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        !receipt.quarantined && receipt.neutral_releases == 1, "real physical rest plus raw handback finishes same key");
    close(handle);
}
void handback_collision() {
    const auto handle = start(); check(source::capture_release(1, handle), "request collision test release");
    Buffer confirmed;
    {
        // One actual older original prevents completion while the fresh report
        // establishes physical rest and a confirmed synthetic packet/neutral Pad.
        // Its destructor drains the ticket, but does not fabricate a publication.
        source::Poll blocker, fresh; source::begin(true, 0, blocker); source::begin(true, 0, fresh);
        confirmed = finish(fresh);
        check(state(handle).debt && !state(handle).neutral_releases, "callback fence retains the empty held set");
    }
    lease::Pad press{}; press.buttons = 0x4000;
    source::Poll collision; source::begin(true, 0, collision);
    const auto changed = finish(collision, press, press, 0, true, true, 0, confirmed.packet, confirmed.packet);
    auto receipt = state(handle);
    check(changed.packet != confirmed.packet && changed.pad == press && receipt.debt &&
        receipt.phase == capture::Phase::releasing && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "equal-packet changed Pad needs translated publication before raw handback");
    source::Poll next; source::begin(true, 0, next);
    const auto handed = finish(next, press, press, 0, true, true, 0, confirmed.packet, confirmed.packet);
    receipt = state(handle);
    check(handed.packet == confirmed.packet && handed.pad == press && receipt.phase == capture::Phase::complete &&
        !receipt.debt && receipt.neutral_releases == 1, "non-colliding next raw report confirms handback without resetting sequence");
    close(handle);
}
void handback_partial() {
    const auto handle = start(); check(source::capture_release(1, handle), "request partial handback release");
    const auto before = writes; write_mode = Write::partial; (void)report();
    auto receipt = state(handle);
    check(writes == before + 1 && receipt.debt && receipt.quarantined &&
        receipt.phase == capture::Phase::releasing && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "partial raw handback never stops translation or grants ACK");
    write_mode = Write::full; const auto translated = report();
    check(translated.packet != 7 && state(handle).debt, "unknown last output needs a complete translated bridge first");
    const auto raw = report(); receipt = state(handle);
    check(raw.packet == 7 && receipt.phase == capture::Phase::complete && !receipt.debt,
        "only a later confirmed same-stream raw handback ends output duty");
    close(handle);
}
void handback_race() {
    const auto handle = start(); check(source::capture_release(1, handle), "request callback-race release");
    source::Poll before, last; source::begin(true, 0, before); source::begin(true, 0, last);
    (void)finish(last); auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.polls_in_flight == 1 &&
        !receipt.neutral_releases, "raw endpoint cannot retire the same stream while another original is live");
    lease::Pad press{}; press.buttons = 0x2000; write_mode = Write::partial;
    (void)finish(before, press, press); receipt = state(handle);
    check(receipt.debt && receipt.quarantined && !receipt.polls_in_flight && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "late partial continuation stays charged to original unacknowledged key");
    write_mode = Write::full; report(); report(); close(handle);
}
void post_ack() {
    const auto handle = start(); const auto key = state(handle).key; close(handle);
    const auto before = writes; lease::Pad press{}; press.buttons = 0x8000; press.right_trigger = 200;
    for (std::uint32_t packet : {7u, 8u, 8u, 0xffffffffu, 0u}) {
        source::Poll call; source::begin(true, 0, call);
        const auto buffer = finish(call, press, press, 0, true, true, 0, packet, packet);
        check(buffer.packet == packet && buffer.pad == press && writes == before,
            "after ACK no capture-owned Pad OR packet-only write survives, including wrap/packet repeats");
    }
    source::PublishedAtomic<bool> later_writer{false}; later_writer.store(true); later_writer.store(false);
    (void)report(press, press);
    check(writes == before, "post-ACK publication interference cannot resurrect an old capture write");
    capture::Receipt absent;
    check(!source::capture_state(1, handle, absent) && !source::capture_release(1, handle) &&
        !source::capture_acknowledge(1, handle), "acknowledged key cannot be resurrected or reused");
    source::tick(1, 2, true); report(); lease::Handle next; lease::Reason reason;
    check(source::lease_request(2, 0, next, reason) && next.sequence != key.lease_sequence,
        "later owner only acquires a fresh monotonic handle");
    source::lease_release_all(2);
}
void packet_only_failure() {
    const auto handle = start(); lease::Pad independent{}; independent.buttons = 0x8000;
    (void)report({}, independent); // revoke authority; the next fresh original can establish physical rest
    const auto before = state(handle).publications;
    write_mode = Write::partial; const auto buffer = report({}, independent);
    auto receipt = state(handle);
    check(buffer.packet != 7 && offered_pad == independent && receipt.phase == capture::Phase::releasing &&
        receipt.debt && receipt.quarantined && receipt.reason == capture::Reason::publication_unconfirmed &&
        receipt.publications == before && !source::capture_acknowledge(1, handle), "packet-only partial publication is charged even without a suppression plan");
    write_mode = Write::full; report(); report(); close(handle);
}
void handback_equal_pad() {
    const auto handle = start(); const auto confirmed = report();
    check(source::capture_release(1, handle), "request identical Pad collision release");
    source::Poll call; source::begin(true, 0, call);
    const auto handed = finish(call, {}, {}, 0, true, true, 0, confirmed.packet, confirmed.packet);
    const auto receipt = state(handle);
    check(handed.packet == confirmed.packet && handed.pad == confirmed.pad &&
        receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "equal raw/last packet with identical confirmed Pad is a safe actual handback");
    close(handle);
}
void replacement_no_write() {
    const auto handle = start(); check(source::capture_release(1, handle), "request old-stream release");
    (void)report({}, {}, 1167, false); const auto before = writes;
    lease::Pad press{}; press.buttons = 1;
    for (unsigned i = 0; i != 3; ++i) (void)report(press, press);
    facts.now_ms += capture::Capture::maximum_release_ms;
    const auto receipt = state(handle);
    check(writes == before && receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        !source::capture_acknowledge(1, handle), "replacement is neither normalized/suppressed nor old-stream handback proof");
}
void capacity() {
    const auto handle = start();
    {
        std::array<source::Poll, 129> calls;
        for (auto& call : calls) source::begin(true, 0, call);
        lease::Pad pressed{}; pressed.buttons = 1;
        check(finish(calls.back(), pressed, pressed).pad == lease::Pad{}, "unrecorded owned original gets only protected neutral fallback");
        const auto receipt = state(handle);
        check(receipt.callbacks_unknown && receipt.quarantined && receipt.debt && receipt.polls_in_flight == 128,
            "capacity failure preserves explicit unknown duty and bounded known count");
    }
    const auto receipt = state(handle);
    check(receipt.callbacks_unknown && receipt.debt && !receipt.polls_in_flight &&
        !source::capture_acknowledge(1, handle), "known callback drain cannot erase unknown capacity duty");
    lease::Receipt passive; lease::Reason reason;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released,
        "passive source release stays false under capture unknown");
    check(!source::lease_acknowledge(1, handle, reason), "passive ACK cannot bypass unknown capture");
}
}
namespace dingosdk::native_input_source::environment { Facts sample() noexcept { return facts; } }
int main(int argc, char** argv) {
    if (argc != 2) return 2; const std::string_view mode = argv[1];
    if (mode == "denial") denial(); else if (mode == "cold_original") cold_original();
    else if (mode == "publication") publication(); else if (mode == "late_drain") late_drain();
    else if (mode == "interference") interference(); else if (mode == "replacement") replacement();
    else if (mode == "context_release") context_release(); else if (mode == "staged_aba") staged_aba();
    else if (mode == "failed_write") failed_write(); else if (mode == "transport") transport();
    else if (mode == "raw_final") raw_final(); else if (mode == "late_age") late_age();
    else if (mode == "renewal") renewal(); else if (mode == "owner_close") owner_close();
    else if (mode == "capacity") capacity(); else if (mode == "held_tail") held_tail();
    else if (mode == "handback_collision") handback_collision(); else if (mode == "handback_partial") handback_partial();
    else if (mode == "handback_race") handback_race(); else if (mode == "post_ack") post_ack();
    else if (mode == "replacement_no_write") replacement_no_write();
    else if (mode == "packet_only_failure") packet_only_failure();
    else if (mode == "handback_equal_pad") handback_equal_pad(); else return 2;
    std::printf("PASS %s %u constructed provider checks; no native-consumption proof\n", argv[1], checks);
    return 0;
}
