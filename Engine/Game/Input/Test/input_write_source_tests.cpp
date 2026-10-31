#include "Extension/Input/native_input_write.h"
#include "Extension/Input/native_input_capture.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/input_source_environment.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace capture = dingosdk::input_lease_capture;
namespace tracked = dingosdk::input_tracked_write;
namespace {
source::environment::Facts facts{1000, 1, true, true, true, true};
unsigned checks{}, writes{};
enum class Sink { full, refused, partial } sink{Sink::full};
struct Buffer { std::uint32_t packet{}; lease::Pad pad{}; std::uint32_t sentinel{0xabcddcba}; };
lease::Pad offered{};
void check(bool value, const char* why) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
bool publish(void* output, std::uint32_t packet, const lease::Pad& pad) noexcept {
    check(source::publication_held(), "real provider gate covers the only bounded copy leaf");
    ++writes; offered = pad; auto& buffer = *static_cast<Buffer*>(output);
    if (sink == Sink::refused) return false;
    buffer.packet = packet;
    if (sink == Sink::partial) return false;
    buffer.pad = pad; return true;
}
Buffer finish(source::Poll& poll, lease::Pad raw = {}, lease::Pad returned = {},
    std::uint32_t raw_packet = 7, std::uint32_t returned_packet = 7,
    std::uint32_t result = 0, bool known = true, std::uint64_t staged_revision = 0) {
    Buffer out{returned_packet, returned}; ++facts.now_ms;
    const auto returned_result = source::finish_capture(poll, result, known, raw_packet, raw,
        result, known, returned_packet, returned,
        staged_revision ? staged_revision : source::publication_revision(), &out, &publish);
    check(returned_result == result, "tracked finalizer does not fabricate transport success");
    check(out.sentinel == 0xabcddcba && !source::publication_held(), "bounded copy preserves adjacent bytes and releases gate");
    return out;
}
Buffer report(lease::Pad raw = {}, lease::Pad returned = {}, std::uint32_t packet = 7,
    std::uint32_t result = 0, bool known = true) {
    source::Poll poll; source::begin(true, 0, poll);
    return finish(poll, raw, returned, packet, packet, result, known);
}
lease::Snapshot sample(lease::Handle h) {
    lease::Snapshot out; lease::Reason why;
    check(source::lease_read(1, h, out, why), "actual provider issues original raw snapshot"); return out;
}
lease::Handle prime() {
    source::tick(1, 2, true); report();
    lease::Handle h; lease::Reason why;
    check(source::lease_request(1, 0, h, why), "actual passive broker admits selected observed source");
    report(); (void)sample(h); return h;
}
lease::Pad desired() { lease::Pad p{}; p.buttons = 0x8000; p.left_x = 12345; return p; }
tracked::Version queue(lease::Handle h, lease::Pad pad = desired()) {
    tracked::Version v; tracked::Reason why;
    check(source::write_request(1, h, sample(h), pad, v, why) && why == tracked::Reason::none && v.generation && v.revision,
        "request returns exact nonzero query version, not a publication boolean"); return v;
}
tracked::Receipt state(lease::Handle h, tracked::Version v) {
    tracked::Receipt out; check(source::write_state(1, h, v, out), "exact current version remains readable");
    check(out.key.owner == 1 && out.key.lease_sequence == h.sequence && out.key.user == 0 &&
        out.version == v && out.desired_revision == v.revision, "coherent receipt retains exact key and current version");
    return out;
}
tracked::Version active(lease::Handle h) {
    const auto v = queue(h); const auto before = writes; const auto copied = report(); const auto r = state(h, v);
    check(copied.pad == desired() && writes == before + 1 && r.phase == tracked::Phase::active &&
        r.version_publications == 1 && r.published_revision == v.revision && r.output_debt &&
        !r.output_released && r.last_written_kind == tracked::WrittenKind::desired,
        "only full actual desired copy records exact version publication and debt"); return v;
}
void passive_close(lease::Handle h) {
    lease::Reason why; check(source::lease_release(1, h, why), "separate passive reservation releases");
    check(source::lease_acknowledge(1, h, why), "separate passive ACK follows writer ACK");
}
void close(lease::Handle h, tracked::Version v) {
    check(source::write_release(1, h), "explicit writer release accepted");
    const auto out = report({}, {}, 100); const auto r = state(h, v);
    check(out.packet == 100 && out.pad == lease::Pad{} && r.phase == tracked::Phase::complete &&
        r.output_released && !r.output_debt && !r.callbacks_unknown && !r.polls_in_flight && r.handback_publications == 1,
        "fresh noncolliding same-stream raw copy and callback drain complete handback");
    check(source::write_acknowledge(1, h), "only proved handback/drain ACKs writer"); passive_close(h);
}
void denial() {
    const auto h = prime(); const auto s = sample(h); tracked::Version v; tracked::Reason why; tracked::Receipt r;
    check(!source::write_request(2, h, s, desired(), v, why), "foreign owner cannot use issued source");
    auto forged = s; ++forged.observation_serial;
    check(!source::write_request(1, h, forged, desired(), v, why), "fabricated observation ordinal refuses");
    forged = s; forged.pad.buttons ^= 1;
    check(!source::write_request(1, h, forged, desired(), v, why), "fabricated raw sample refuses");
    facts.thread = 2; check(!source::write_request(1, h, s, desired(), v, why), "wrong client thread refuses"); facts.thread = 1;
    check(!source::write_state(1, h, {1, 1}, r) && !source::write_release(1, h) && !source::write_acknowledge(1, h),
        "unowned writer mode cannot be adopted by cleanup/query calls");
    check(writes == 0, "all admission denials leave output buffer untouched");
}
void pending() {
    const auto h = prime(); const auto v = queue(h); const auto r = state(h, v);
    check(r.phase == tracked::Phase::pending && !r.version_publications && !r.publications &&
        !r.published_revision && !r.output_debt && !r.output_released && writes == 0,
        "queued desired state is not output publication or terminal cleanup");
    const auto before = r.write_expires_ms; tracked::Version refused; tracked::Reason why;
    check(!source::write_request(1, h, sample(h), desired(), refused, why) && why == tracked::Reason::busy &&
        state(h, v).write_expires_ms == before && state(h, v).version == v,
        "unconfirmed desired revision cannot be replaced or extended");
}
void publication() {
    const auto h = prime(); const auto v = active(h); const auto raw = sample(h);
    check(raw.pad == lease::Pad{}, "desired output never seeds passive raw observation authority");
    const auto before = state(h, v); report(); const auto after = state(h, v);
    check(after.version_publications == before.version_publications + 1 && after.publications == before.publications + 1,
        "each protected full copy advances current desired publication only once"); close(h, v);
}
void versions() {
    const auto h = prime(); const auto first = active(h); const auto prior = state(h, first);
    const auto second = queue(h); const auto pending_row = state(h, second); tracked::Receipt stale;
    check(second.generation == first.generation && second.revision > first.revision &&
        !source::write_state(1, h, first, stale), "superseded opaque version is not historical authority");
    check(pending_row.phase == tracked::Phase::pending && pending_row.output_debt && !pending_row.version_publications &&
        pending_row.published_revision == first.revision && pending_row.publications == prior.publications,
        "pending replacement retains older debt without claiming current publication");
    report(); check(state(h, second).version_publications == 1, "only next actual copy confirms replacement"); close(h, second);
}
void source_age() {
    const auto h = prime(); const auto s = sample(h); tracked::Version v; tracked::Reason why;
    facts.now_ms = s.observed_ms + lease::maximum_age_ms + 1;
    check(!source::write_request(1, h, s, desired(), v, why) && writes == 0, "snapshot older than100ms refuses before output");
}
void source_age_edge() {
    const auto h = prime(); const auto s = sample(h); tracked::Version v; tracked::Reason why;
    facts.now_ms = s.observed_ms + lease::maximum_age_ms;
    check(source::write_request(1, h, s, desired(), v, why), "exact100ms observed source age remains admissible");
    check(state(h, v).write_expires_ms == facts.now_ms + tracked::write_lifetime_ms,
        "desired expiry begins at admission time, independently of snapshot age");
}
void expiry() {
    const auto h = prime(); const auto v = active(h); const auto deadline = state(h, v).write_expires_ms;
    lease::Reason renewal_reason;
    check(source::lease_renew(1, h, sample(h), renewal_reason), "passive renewal fixture remains available");
    check(state(h, v).write_expires_ms == deadline, "passive renewal never extends desired Pad lifetime");
    facts.now_ms = deadline; const auto r = state(h, v); const auto count = r.version_publications;
    check(r.phase == tracked::Phase::releasing && !r.output_released && r.output_debt,
        "exact100ms desired expiry stops assertions but retains debt");
    report(); check(state(h, v).version_publications == count, "expired output does not gain desired publication");
}
void capture_exclusion() {
    const auto h = prime(); const auto v = queue(h); capture::Reason why; capture::Receipt cap;
    check(!source::capture_request(1, h, sample(h), why) && !source::capture_state(1, h, cap) &&
        !source::capture_release(1, h) && !source::capture_acknowledge(1, h), "neutral capture cannot adopt retained writer mode");
    check(source::write_release(1, h) && state(h, v).phase == tracked::Phase::complete,
        "never-attempted writer cancellation needs no invented handback");
    check(!source::capture_request(1, h, sample(h), why), "unacknowledged complete writer still excludes capture");
    check(source::write_acknowledge(1, h), "never-written clean writer ACKs");
    tracked::Version next; tracked::Reason no;
    check(!source::write_request(1, h, sample(h), desired(), next, no) && !source::capture_request(1, h, sample(h), why),
        "consumed lease sequence cannot rearm either publication mode after ACK");
}
void capture_first() {
    const auto h = prime(); capture::Reason why; check(source::capture_request(1, h, sample(h), why), "capture owns shared slot first");
    tracked::Version v; tracked::Reason no; tracked::Receipt row;
    check(!source::write_request(1, h, sample(h), desired(), v, no) && !source::write_state(1, h, {1,1}, row) &&
        !source::write_release(1, h) && !source::write_acknowledge(1, h), "writer cannot adopt a neutral capture reservation");
}
void busy_original() {
    const auto h = prime(); const auto s = sample(h); source::Poll original; source::begin(true, 0, original);
    tracked::Version v; tracked::Reason why;
    check(!source::write_request(1, h, s, desired(), v, why) && writes == 0, "selected original already invoking blocks first writer");
    finish(original); (void)queue(h);
}
void filtered() {
    const auto h = prime(); const auto v = queue(h); lease::Pad filter{}; filter.buttons = 0x1000;
    const auto out = report({}, filter); const auto r = state(h, v);
    check(out.pad == filter && !r.version_publications && writes == 0,
        "another filter's changed Pad remains its own output, never our desired witness");
}
void staged_revision() {
    const auto h = prime(); const auto v = active(h); const auto before = state(h, v).version_publications;
    source::Poll call; source::begin(true, 0, call); const auto staged = source::publication_revision();
    { source::PublicationScope gate; source::publication_changed(); }
    finish(call, {}, {}, 7, 7, 0, true, staged);
    check(state(h, v).version_publications == before, "stale staged revision cannot confirm a current desired write");
}
void failed_sink(bool partial) {
    const auto h = prime(); const auto v = queue(h); sink = partial ? Sink::partial : Sink::refused;
    const auto out = report(); const auto r = state(h, v);
    check(r.phase == tracked::Phase::releasing && r.output_debt && r.quarantined && !r.output_released &&
        !r.version_publications && !r.publications && !source::write_acknowledge(1, h),
        "failed/partial protected copy retains unknown output duty without a publication");
    check(out.pad == lease::Pad{}, "failed desired copy did not claim a complete Pad mutation");
}
void selected_drain() {
    const auto h = prime(); const auto v = active(h); source::Poll first, second;
    source::begin(true, 0, first); source::begin(true, 0, second); check(source::write_release(1, h), "release overlapping callbacks");
    finish(first, {}, {}, 100, 100); const auto held = state(h, v);
    check(!held.output_released && held.output_debt && held.polls_in_flight == 1 && !source::write_acknowledge(1, h),
        "one returned callback cannot erase another exact selected callback");
    finish(second, {}, {}, 101, 101); const auto r = state(h, v);
    check(r.phase == tracked::Phase::complete && r.output_released && !r.polls_in_flight && r.handback_publications == 1,
        "last actual selected raw handback and drain establishes complete");
}
void other_user() {
    const auto h = prime(); const auto v = active(h); source::Poll other; source::begin(true, 1, other);
    close(h, v); const auto before = writes; finish(other);
    check(writes == before, "unselected user's independent callback neither blocks handback nor writes selected output");
}
void collision() {
    const auto h = prime(); const auto v = active(h); const auto confirmed = report();
    check(source::write_release(1, h), "begin packet collision handback");
    const auto bridge = report({}, {}, confirmed.packet); const auto pending_row = state(h, v);
    check(bridge.packet != confirmed.packet && bridge.pad == lease::Pad{} && pending_row.output_debt &&
        !pending_row.output_released && !pending_row.handback_publications && pending_row.cleanup_publications > 0,
        "colliding raw packet/different Pad needs confirmed translated bridge, not false handback");
    const auto raw_packet = bridge.packet + 1; const auto handed = report({}, {}, raw_packet); const auto r = state(h, v);
    check(handed.packet == raw_packet && handed.pad == lease::Pad{} && r.output_released && r.handback_publications == 1,
        "later noncolliding same-stream full raw copy establishes real handback");
}
void unknown_capacity() {
    const auto h = prime(); const auto v = active(h);
    { std::array<source::Poll, 129> calls; for (auto& p : calls) source::begin(true, 0, p); }
    const auto r = state(h, v);
    check(r.callbacks_unknown && r.quarantined && r.output_debt && !r.output_released && !r.polls_in_flight &&
        !source::write_acknowledge(1, h), "known callback drain cannot erase unregistered capacity duty");
}
void menu() {
    const auto h = prime(); const auto v = active(h); facts.menu_closed = false;
    const auto before = state(h, v).version_publications; report();
    check(state(h, v).phase == tracked::Phase::releasing && state(h, v).version_publications == before &&
        !source::write_acknowledge(1, h), "menu revokes assertions and cannot supply clean handback");
    facts.menu_closed = true; const auto raw = report({}, {}, 100); const auto r = state(h, v);
    check(raw.packet == 100 && r.output_released && r.handback_publications == 1,
        "same-stream qualified post-menu raw copy cleans up without rearming desired output");
}
void owner_close() {
    const auto h = prime(); const auto v = active(h); const auto issued = sample(h);
    source::lease_release_all(1); const auto r = state(h, v);
    check(r.orphaned && r.phase == tracked::Phase::releasing && r.output_debt && !r.output_released,
        "owner close revokes desired assertion while retaining exact orphan debt");
    tracked::Version next; tracked::Reason why;
    check(!source::write_request(2, h, issued, desired(), next, why), "new VM cannot adopt orphan reservation");
}
void post_ack() {
    const auto h = prime(); const auto v = active(h); close(h, v); const auto before = writes;
    lease::Pad raw{}; raw.buttons = 0x1000; const auto out = report(raw, raw, 123);
    tracked::Receipt row; check(writes == before && out.packet == 123 && out.pad == raw &&
        !source::write_state(1, h, v, row) && !source::write_release(1, h) && !source::write_acknowledge(1, h),
        "acknowledged old key never writes Pad or packet or recreates receipt");
}
void held_tail() {
    const auto h = prime(); const auto v = queue(h); lease::Pad held{}; held.buttons = 1; held.left_x = 25000;
    check(report(held, held).pad == desired() && state(h, v).version_publications == 1,
        "desired version publishes over the actual clean held raw stream");
    check(source::write_release(1, h), "release writer with physical controls still held");
    const auto suppressed = report(held, held, 20); const auto pending_row = state(h, v);
    check(suppressed.pad.buttons == 0 && suppressed.pad.left_x == 0 && pending_row.output_debt &&
        !pending_row.output_released && !source::write_acknowledge(1, h),
        "physical held controls cannot be handed back merely because desire cleared");
    const auto raw = report({}, {}, 100); const auto r = state(h, v);
    check(raw.packet == 100 && raw.pad == lease::Pad{} && r.output_released && r.handback_publications == 1,
        "actual same-stream rested controls permit bounded handback");
}
void handback_partial() {
    const auto h = prime(); const auto v = active(h); check(source::write_release(1, h), "release before partial raw handback");
    sink = Sink::partial; const auto before = writes; report({}, {}, 100); const auto r = state(h, v);
    check(writes == before + 1 && r.output_debt && r.quarantined && !r.output_released && !r.handback_publications &&
        !source::write_acknowledge(1, h), "partial handback packet copy is retained duty, never raw publication proof");
}
void packet_only_failure() {
    const auto h = prime(); (void)active(h); const auto v = queue(h, {}); sink = Sink::partial;
    const auto out = report(); const auto r = state(h, v);
    check(out.pad == lease::Pad{} && out.packet != 7 && writes == 2 && !r.version_publications && r.output_debt &&
        r.quarantined && !source::write_acknowledge(1, h),
        "equal Pad with only a partially written packet cannot impersonate a confirmed neutral desired version");
}
void cancel_drain() {
    const auto h = prime(); const auto v = queue(h); source::Poll admitted; source::begin(true, 0, admitted);
    check(source::write_release(1, h), "cancel before any attempted output"); const auto waiting = state(h, v);
    check(waiting.phase == tracked::Phase::releasing && !waiting.output_debt && waiting.polls_in_flight == 1 &&
        !waiting.output_released && !source::write_acknowledge(1, h), "never-written cancellation still waits its known callback");
    finish(admitted, {}, {}, 100, 100); const auto r = state(h, v);
    check(r.output_released && r.phase == tracked::Phase::complete && !r.publications && !r.handback_publications && writes == 0,
        "drained never-attempted cancellation completes without an invented owned copy");
}
void source_expiry() {
    const auto h = prime(); auto v = active(h); const auto source_deadline = state(h, v).source_expires_ms;
    for (unsigned i = 0; i != 3; ++i) {
        facts.now_ms += 70; v = queue(h); report();
    }
    const auto before = state(h, v);
    check(before.write_expires_ms > source_deadline && facts.now_ms < source_deadline,
        "fixture renews desired versions without extending separate250ms source lease");
    facts.now_ms = source_deadline; const auto expired = state(h, v);
    check(expired.phase == tracked::Phase::releasing && expired.output_debt && !expired.output_released &&
        expired.version_publications == before.version_publications,
        "source lease expiry revokes even a separately unexpired desired version");
    check(source::write_release(1, h), "retained writer cleanup remains available after source expiry");
}
void replacement() {
    const auto h = prime(); const auto v = active(h); const auto before = writes;
    report({}, {}, 7, 1167, false);
    for (unsigned i = 0; i != 3; ++i) report();
    facts.now_ms += tracked::cleanup_boundary_ms; const auto r = state(h, v);
    check(writes == before && r.phase == tracked::Phase::releasing && r.output_debt && r.quarantined &&
        !r.output_released && !source::write_acknowledge(1, h),
        "lost and replacement observed streams never stand in for original-stream handback");
}
}
namespace dingosdk::native_input_source::environment { Facts sample() noexcept { return facts; } }
int main(int argc, char** argv) {
    if (argc != 2) return 2; const std::string_view mode = argv[1];
    if (mode == "denial") denial(); else if (mode == "pending") pending();
    else if (mode == "publication") publication(); else if (mode == "versions") versions();
    else if (mode == "source_age") source_age(); else if (mode == "source_age_edge") source_age_edge();
    else if (mode == "expiry") expiry(); else if (mode == "capture_exclusion") capture_exclusion();
    else if (mode == "capture_first") capture_first(); else if (mode == "busy_original") busy_original();
    else if (mode == "filtered") filtered(); else if (mode == "staged_revision") staged_revision();
    else if (mode == "partial_sink") failed_sink(true); else if (mode == "refused_sink") failed_sink(false);
    else if (mode == "selected_drain") selected_drain(); else if (mode == "other_user") other_user();
    else if (mode == "collision") collision(); else if (mode == "unknown_capacity") unknown_capacity();
    else if (mode == "menu") menu(); else if (mode == "owner_close") owner_close();
    else if (mode == "post_ack") post_ack(); else if (mode == "held_tail") held_tail();
    else if (mode == "handback_partial") handback_partial(); else if (mode == "packet_only_failure") packet_only_failure();
    else if (mode == "cancel_drain") cancel_drain(); else if (mode == "source_expiry") source_expiry();
    else if (mode == "replacement") replacement(); else return 2;
    std::printf("PASS %s %u constructed real-provider checks; native publication/consumption unqualified\n", argv[1], checks);
    return 0;
}