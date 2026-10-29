#include "native_input_source.h"
#include "native_input_capture.h"
#include "Engine/Game/Input/input_lease_capture_policy.h"
#include "input_publication.h"
#include "input_source_environment.h"
#include "Engine/Game/Input/input_lease_policy.h"
#include <array>
#include <atomic>
#include <limits>
#include <mutex>

namespace dingosdk::native_input_source {
namespace {
namespace lease = input_lease;
namespace capture = input_lease_capture;
constexpr std::size_t callback_capacity = 128;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
struct Reservation {
    std::uint64_t owner{};
    lease::Handle handle{};
    std::uint32_t user{4}, callbacks{};
    bool orphaned{};
};
struct PacketState {
    bool seen{}, active{};
    std::uint32_t packet{}, original{}, candidate{};
    std::uint64_t epoch{}, publication{}, capture_revision{};
    lease::Pad raw{}, final{};
    bool confirmed_known{};
    std::uint32_t confirmed_packet{};
    std::uint64_t confirmed_epoch{};
    lease::Pad confirmed_pad{};
    bool can_handback(std::uint64_t stream, std::uint32_t raw_packet,
        const lease::Pad& source) const noexcept {
        // Equal packet numbers must not hide a changed Pad at raw handback.
        // A partial write first needs another fully confirmed publication.
        return active && confirmed_known && confirmed_epoch == stream &&
            (raw_packet != confirmed_packet || source == confirmed_pad);
    }
    void confirm(std::uint64_t stream, std::uint32_t output_packet,
        const lease::Pad& pad, bool published) noexcept {
        confirmed_known = published;
        if (published) { confirmed_epoch = stream; confirmed_packet = output_packet; confirmed_pad = pad; }
    }
    void handback(std::uint64_t stream, std::uint64_t pub, std::uint64_t revision,
        std::uint32_t raw_packet, const lease::Pad& pad) noexcept {
        // Called only after the ONE complete raw output write and policy confirm.
        // Preserve history/sequence values; only future translation ownership ends.
        active = false; seen = true; epoch = stream; publication = pub; capture_revision = revision;
        packet = original = candidate = raw_packet; raw = final = pad;
    }
    std::uint32_t translate(std::uint64_t stream, std::uint64_t pub, std::uint64_t revision,
        std::uint32_t source_packet, std::uint32_t returned_packet,
        const lease::Pad& source, const lease::Pad& returned, bool activate) noexcept {
        if (!active) {
            if (activate) { packet = seen ? packet + 1 : returned_packet + 1; active = true; }
            else packet = returned_packet;
        } else if (epoch != stream || publication != pub || capture_revision != revision ||
            original != source_packet || candidate != returned_packet || raw != source || final != returned) ++packet;
        seen = true; epoch = stream; publication = pub; capture_revision = revision;
        original = source_packet; candidate = returned_packet; raw = source; final = returned;
        return packet;
    }
};
struct Source {
    lease::Source observation{};
    std::uint64_t latest_start{}, epoch{};
    bool connected{};
    PacketState packets;
};
struct Callback {
    std::uint64_t ticket{}, revision{}, context{}, ordinal{}, started_ms{};
    lease::Handle handle{};
    std::uint32_t user{4};
    capture::Key capture_key{};
    capture::Ticket capture_ticket{};
};
struct State {
    std::mutex gate;
    lease::Broker leases;
    capture::Capture capture;
    bool capture_orphaned{}, callbacks_unknown{};
    std::array<Source, 4> sources{};
    std::array<Reservation, lease::retained_capacity> reservations{};
    std::array<Callback, callback_capacity> callbacks{};
    std::atomic<std::uint64_t> revision{1};
    std::uint64_t next_ticket{1}, next_epoch{1}, next_ordinal{1};
    std::uint64_t context{}, last_now{}, eligible_at{};
    std::uintptr_t base{}, client{};
    std::uint32_t client_thread{};
    bool ready{}, failed{};
};
State& state() noexcept { static State value; return value; }
thread_local bool publication_gate_held{};
std::uint64_t issue(std::uint64_t& next) noexcept {
    const auto result = next;
    if (next) next = next == maximum ? 0 : next + 1;
    return result;
}
void reset_sources() noexcept {
    for (auto& source : state().sources) source.observation = {};
}
void invalidate(lease::Reason reason) noexcept {
    state().leases.invalidate(reason);
    state().capture.revoke(reason == lease::Reason::context_changed ? capture::Reason::context_changed :
        reason == lease::Reason::stale_epoch ? capture::Reason::clock_regressed :
        reason == lease::Reason::interference ? capture::Reason::interference : capture::Reason::authority_lost);
    reset_sources();
}
void advance_revision() noexcept {
    auto& s = state();
    const auto revision = s.revision.load(std::memory_order_relaxed);
    if (revision == maximum) s.failed = true;
    else s.revision.store(revision + 1, std::memory_order_release);
}
struct Sample {
    std::uint64_t revision{};
    environment::Facts facts{};
};
Sample sample() noexcept {
    Sample result;
    result.revision = publication_revision();
    result.facts = environment::sample();
    return result;
}
lease::Guard guard(const Sample& sample) noexcept {
    auto& s = state();
    const auto now = sample.facts.now_ms;
    const bool stable = sample.revision == s.revision.load(std::memory_order_relaxed);
    if (!now || now < s.last_now) {
        invalidate(lease::Reason::stale_epoch);
        s.ready = false;
    } else s.last_now = now;
    const bool ready = !s.failed && stable && s.ready && s.base && s.client && s.context &&
        s.eligible_at && now >= s.eligible_at && now - s.eligible_at <= lease::lifetime_ms;
    if (!ready || !sample.facts.focused || !sample.facts.gameplay ||
        !sample.facts.menu_closed || !sample.facts.interference_clear) reset_sources();
    return {now, s.context, ready, sample.facts.focused, sample.facts.gameplay,
        sample.facts.menu_closed, stable && sample.facts.interference_clear};
}
Reservation* reservation(lease::Handle handle) noexcept {
    if (!handle.sequence) return nullptr;
    for (auto& entry : state().reservations) if (entry.handle == handle) return &entry;
    return nullptr;
}
std::uint32_t captured_callbacks(const capture::Key& key) noexcept {
    std::uint32_t count{};
    if (!key.owner) return count;
    for (const auto& callback : state().callbacks)
        if (callback.ticket && callback.capture_key == key) ++count;
    return count;
}
bool stream_in_flight(std::uint32_t user, std::uint64_t except = 0) noexcept {
    for (const auto& callback : state().callbacks)
        if (callback.ticket && callback.ticket != except && callback.user == user) return true;
    return false;
}
bool capture_busy(std::uint64_t owner = 0, lease::Handle handle = {}) noexcept {
    const auto& s = state(); const auto& receipt = s.capture.state();
    if (receipt.phase == capture::Phase::idle ||
        (owner && (receipt.key.owner != owner || receipt.key.lease_sequence != handle.sequence))) return false;
    return s.capture.reserved() || s.callbacks_unknown || captured_callbacks(receipt.key) ||
        (receipt.key.user < 4 && s.sources[receipt.key.user].packets.active);
}
void capture_cleanup() noexcept {
    auto& s = state(); const auto& receipt = s.capture.state();
    if (s.capture_orphaned && receipt.phase == capture::Phase::complete && !receipt.debt &&
        !s.callbacks_unknown && !captured_callbacks(receipt.key) && receipt.key.user < 4 &&
        !s.sources[receipt.key.user].packets.active) {
        (void)s.capture.acknowledge(receipt.key); s.capture_orphaned = false;
    }
}
void reclaim_orphans() noexcept {
    auto& s = state(); capture_cleanup();
    for (auto& entry : s.reservations)
        if (entry.owner && entry.orphaned && !entry.callbacks && !capture_busy(entry.owner, entry.handle)) {
            (void)s.leases.acknowledge(entry.owner, entry.handle);
            entry = {};
        }
}
void capture_sync(const lease::Guard& guard) noexcept {
    auto& s = state(); const auto& receipt = s.capture.state();
    if (receipt.phase != capture::Phase::idle && receipt.phase != capture::Phase::complete &&
        receipt.phase != capture::Phase::releasing) {
        lease::Receipt passive;
        if (!s.leases.state(receipt.key.owner, {receipt.key.lease_sequence}, passive) || lease::terminal(passive.status))
            s.capture.revoke(capture::Reason::authority_lost);
        else if (!guard.ready) s.capture.revoke(capture::Reason::authority_lost);
        else if (!guard.focused) s.capture.revoke(capture::Reason::focus_lost);
        else if (!guard.gameplay) s.capture.revoke(capture::Reason::gameplay_inactive);
        else if (!guard.menu_closed) s.capture.revoke(capture::Reason::menu_open);
        else if (!guard.interference_clear) s.capture.revoke(capture::Reason::interference);
        else if (receipt.key.context_generation != guard.context_generation) s.capture.revoke(capture::Reason::context_changed);
        else if (guard.now_ms >= receipt.expires_ms) s.capture.revoke(capture::Reason::deadline);
    }
    if (receipt.phase == capture::Phase::releasing && receipt.key.user < 4)
        s.capture.sync(guard.now_ms, s.sources[receipt.key.user].connected &&
            s.sources[receipt.key.user].epoch == receipt.key.source_epoch);
    // No timer, observed raw report, or VM close resolves an unrecorded callback.
    if (receipt.phase != capture::Phase::idle &&
        (receipt.generation == maximum || receipt.revision == maximum)) {
        s.failed = true; s.callbacks_unknown = true;
    }
    if (s.callbacks_unknown && receipt.phase != capture::Phase::idle)
        (void)s.capture.retain_neutralization(receipt.key);
    reclaim_orphans();
}
capture::Input capture_input(const Source& source, const lease::Guard& guard,
    const lease::Pad& raw, const lease::Pad& returned, std::uint64_t ordinal,
    bool known, std::uint32_t original_result, std::uint32_t returned_result, bool same_packet) noexcept {
    const auto& key = state().capture.state().key;
    capture::Input result;
    result.user = key.user; result.source_epoch = source.epoch; result.context_generation = guard.context_generation;
    result.observation_serial = ordinal; result.observed_ms = source.observation.observed_ms;
    result.now_ms = guard.now_ms; result.original_result = original_result; result.returned_result = returned_result;
    result.raw = raw; result.returned = returned; result.outer_game_poll = true;
    result.has_output = known; result.source_connected = source.connected;
    result.safety = {state().leases.accepted(key.owner, {key.lease_sequence}, source.observation),
        guard.ready, guard.focused, guard.gameplay, guard.menu_closed, guard.interference_clear && same_packet};
    return result;
}
void callback_failure(std::uint32_t user) noexcept {
    auto& s = state(); s.failed = true;
    invalidate(lease::Reason::capacity);
    const auto& receipt = s.capture.state();
    if (receipt.phase != capture::Phase::idle && receipt.key.user == user) {
        s.callbacks_unknown = true;
        (void)s.capture.retain_neutralization(receipt.key);
    }
}
bool client(const Sample& sample, lease::Reason& reason) noexcept {
    if (!sample.facts.thread || sample.facts.thread != state().client_thread) {
        reason = lease::Reason::wrong_thread;
        return false;
    }
    return true;
}
bool owned(std::uint64_t owner, lease::Handle handle, Reservation*& entry,
    lease::Reason& reason) noexcept {
    if (!owner) { reason = lease::Reason::invalid_owner; return false; }
    entry = reservation(handle);
    if (!entry || entry->owner != owner || entry->orphaned) {
        reason = lease::Reason::unavailable; return false;
    }
    return true;
}
void drain(std::uint64_t ticket) noexcept {
    for (auto& callback : state().callbacks) if (callback.ticket == ticket) {
        if (auto* entry = reservation(callback.handle); entry && entry->callbacks) --entry->callbacks;
        callback = {};
        reclaim_orphans();
        return;
    }
}
}

PublicationScope::PublicationScope(bool enabled) noexcept {
    if (!enabled || publication_gate_held) return;
    state().gate.lock();
    publication_gate_held = owns_ = true;
}
PublicationScope::~PublicationScope() {
    if (!owns_) return;
    publication_gate_held = false;
    state().gate.unlock();
}
bool publication_held() noexcept { return publication_gate_held; }
std::uint64_t publication_revision() noexcept { return state().revision.load(std::memory_order_acquire); }
void publication_changed() noexcept {
    if (!publication_gate_held) return;
    advance_revision();
    invalidate(lease::Reason::interference);
}

void tick(std::uintptr_t base, std::uintptr_t native_client, bool ready) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) return;
    auto& s = state();
    const bool publication_stable = sampled.revision == s.revision.load(std::memory_order_relaxed);
    const bool changed = base != s.base || native_client != s.client || ready != s.ready ||
        sampled.facts.thread != s.client_thread;
    if (changed) {
        advance_revision();
        invalidate(lease::Reason::context_changed);
        if (s.context == maximum) { s.context = 0; s.failed = true; }
        else if (!s.failed) ++s.context;
    }
    s.base = base; s.client = native_client; s.client_thread = sampled.facts.thread;
    s.ready = ready && base && native_client && sampled.facts.thread && !s.failed;
    s.eligible_at = s.ready ? sampled.facts.now_ms : 0;
    // This tick itself owns the context transition above, not an external writer.
    auto current = sampled;
    if (publication_stable) current.revision = s.revision.load(std::memory_order_relaxed);
    const auto facts = guard(current);
    s.leases.tick(facts);
    capture_sync(facts);
}

bool lease_request(std::uint64_t owner, std::uint32_t user,
    lease::Handle& handle, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    auto& s = state();
    const auto current = guard(sampled);
    if (!owner) { reason = lease::Reason::invalid_owner; return false; }
    if (user > 3) { reason = lease::Reason::invalid_source; return false; }
    for (const auto& entry : s.reservations) if (entry.callbacks) {
        reason = lease::Reason::busy; return false;
    }
    if (capture_busy()) { reason = lease::Reason::busy; return false; }
    Reservation* vacant{};
    for (auto& entry : s.reservations) if (!entry.owner) { vacant = &entry; break; }
    if (!vacant) { reason = lease::Reason::capacity; return false; }
    if (!s.leases.request(owner, user, s.sources[user].observation, current, false, handle, reason)) return false;
    *vacant = {owner, handle, user, 0, false};
    return true;
}
bool lease_read(std::uint64_t owner, lease::Handle handle,
    lease::Snapshot& snapshot, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    const auto current = guard(sampled); state().leases.tick(current); capture_sync(current);
    return state().leases.read(owner, handle, current, snapshot, reason);
}
bool lease_renew(std::uint64_t owner, lease::Handle handle, const lease::Snapshot& snapshot,
    lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    const auto current = guard(sampled); auto& s = state();
    s.leases.tick(current); capture_sync(current);
    const bool result = s.leases.renew(owner, handle, snapshot, current, reason);
    const auto& receipt = s.capture.state();
    if (result && receipt.key.owner == owner && receipt.key.lease_sequence == handle.sequence &&
        (receipt.phase == capture::Phase::waiting_neutral || receipt.phase == capture::Phase::capturing)) {
        lease::Receipt renewed;
        if (!s.leases.state(owner, handle, renewed)) return false;
        const auto& source = s.sources[receipt.key.user];
        const auto input = capture_input(source, current, source.observation.pad, source.observation.pad,
            source.observation.observation_serial, true, 0, 0, true);
        if (!s.capture.renew(receipt.key, renewed.expires_ms, input)) {
            reason = lease::Reason::unavailable; return false;
        }
    }
    return result;
}
bool lease_release(std::uint64_t owner, lease::Handle handle, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    const auto current = guard(sampled); state().leases.tick(current); capture_sync(current);
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (!state().leases.release(owner, handle)) { reason = lease::Reason::unavailable; return false; }
    const auto& held = state().capture.state();
    if (held.key.owner == owner && held.key.lease_sequence == handle.sequence) {
        (void)state().capture.release(held.key); advance_revision();
    }
    reason = lease::Reason::none;
    return true;
}
bool lease_state(std::uint64_t owner, lease::Handle handle,
    lease::Receipt& receipt, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    const auto current = guard(sampled); state().leases.tick(current); capture_sync(current);
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (!state().leases.state(owner, handle, receipt)) { reason = lease::Reason::unavailable; return false; }
    receipt.polls_in_flight = entry->callbacks;
    receipt.capture_reserved = capture_busy(owner, handle);
    receipt.source_released = lease::terminal(receipt.status) && !entry->callbacks && !receipt.capture_reserved;
    receipt.orphaned = entry->orphaned;
    reason = lease::Reason::none;
    return true;
}
bool lease_acknowledge(std::uint64_t owner, lease::Handle handle, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    const auto current = guard(sampled); state().leases.tick(current); capture_sync(current);
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (entry->callbacks || capture_busy(owner, handle)) { reason = lease::Reason::busy; return false; }
    if (!state().leases.acknowledge(owner, handle)) { reason = lease::Reason::unavailable; return false; }
    *entry = {};
    reason = lease::Reason::none;
    return true;
}
void lease_release_all(std::uint64_t owner) noexcept {
    if (!owner) return;
    PublicationScope scope;
    if (!scope.owns()) return;
    const auto& held = state().capture.state();
    if (held.key.owner == owner && held.phase != capture::Phase::idle) {
        state().capture_orphaned = true; (void)state().capture.release(held.key); advance_revision();
    }
    for (auto& entry : state().reservations) if (entry.owner == owner) {
        entry.orphaned = true;
        (void)state().leases.release(owner, entry.handle);
    }
    reclaim_orphans();
}

bool capture_request(std::uint64_t owner, lease::Handle handle, const lease::Snapshot& snapshot,
    capture::Reason& reason) noexcept {
    reason = capture::Reason::authority_lost;
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) { reason = capture::Reason::busy; return false; }
    lease::Reason client_reason;
    if (!client(sampled, client_reason)) return false;
    auto& s = state(); const auto current = guard(sampled);
    s.leases.tick(current); capture_sync(current);
    lease::Receipt receipt;
    if (!owner || !s.leases.state(owner, handle, receipt) || receipt.status != lease::Status::active ||
        !s.leases.issued(owner, handle, snapshot) || receipt.user > 3) return false;
    if (s.failed || s.callbacks_unknown) { reason = capture::Reason::exhausted; return false; }
    if (stream_in_flight(receipt.user)) { reason = capture::Reason::busy; return false; }
    const auto& source = s.sources[receipt.user];
    if (!s.leases.accepted(owner, handle, source.observation) ||
        snapshot.observation_serial != source.observation.observation_serial || snapshot.pad != source.observation.pad) return false;
    const capture::Key key{owner, handle.sequence, receipt.source_epoch, receipt.context_generation, receipt.user};
    auto input = capture_input(source, current, source.observation.pad, source.observation.pad,
        source.observation.observation_serial, true, 0, 0, true);
    input.user = receipt.user; input.safety.source_authorized = true;
    const bool result = s.capture.request(key, receipt.expires_ms, input, false, reason);
    if (result) { s.capture_orphaned = false; advance_revision(); }
    return result;
}
bool capture_state(std::uint64_t owner, lease::Handle handle, capture::Receipt& out) noexcept {
    out = {}; const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto current = guard(sampled);
    s.leases.tick(current); capture_sync(current);
    const auto& receipt = s.capture.state();
    if (!owner || !handle.sequence || receipt.phase == capture::Phase::idle ||
        receipt.key.owner != owner || receipt.key.lease_sequence != handle.sequence) return false;
    out = receipt; out.polls_in_flight = captured_callbacks(receipt.key);
    out.orphaned = s.capture_orphaned; out.callbacks_unknown = s.callbacks_unknown;
    if (out.callbacks_unknown) { out.quarantined = true; out.reason = capture::Reason::exhausted; }
    return true;
}
bool capture_release(std::uint64_t owner, lease::Handle handle) noexcept {
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto& receipt = s.capture.state();
    if (!owner || receipt.key.owner != owner || receipt.key.lease_sequence != handle.sequence) return false;
    const bool result = s.capture.release(receipt.key);
    if (result) { s.leases.invalidate(lease::Reason::cancelled); advance_revision(); }
    return result;
}
bool capture_acknowledge(std::uint64_t owner, lease::Handle handle) noexcept {
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto& receipt = s.capture.state();
    if (!owner || receipt.key.owner != owner || receipt.key.lease_sequence != handle.sequence ||
        s.callbacks_unknown || captured_callbacks(receipt.key) || receipt.key.user >= 4 ||
        s.sources[receipt.key.user].packets.active) return false;
    return s.capture.acknowledge(receipt.key);
}

Poll::~Poll() {
    if (!ticket_) return;
    PublicationScope scope;
    if (!scope.owns()) return;
    drain(ticket_);
}
void begin(bool outer, std::uint32_t user, Poll& poll) noexcept {
    if (!outer || user > 3 || poll.ticket_ || poll.fallback_user_ < 4) return;
    poll.fallback_user_ = user;
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return;
    auto& s = state(); const auto current = guard(sampled);
    s.leases.tick(current); capture_sync(current);
    // Track even cold/unready originals. A later capture cannot ignore a call
    // merely because it began before client readiness or passive admission.
    Callback* vacant{};
    for (auto& callback : s.callbacks) if (!callback.ticket) { vacant = &callback; break; }
    if (!vacant) { callback_failure(user); return; }
    const auto ticket = issue(s.next_ticket), ordinal = issue(s.next_ordinal);
    if (!ticket || !ordinal) { callback_failure(user); return; }
    Callback value; value.ticket = ticket; value.ordinal = ordinal; value.user = user;
    value.revision = s.revision.load(std::memory_order_relaxed); value.context = s.context;
    value.started_ms = sampled.facts.now_ms;
    const auto& captured = s.capture.state();
    if (captured.key.user == user && s.capture.reserved()) {
        value.capture_key = captured.key; value.capture_ticket = s.capture.ticket();
    }
    value.handle = s.leases.active_user() == user ? s.leases.active() :
        value.capture_key.owner ? lease::Handle{value.capture_key.lease_sequence} : lease::Handle{};
    if (auto* entry = reservation(value.handle)) ++entry->callbacks;
    *vacant = value; s.sources[user].latest_start = ordinal; poll.ticket_ = ticket;
}
namespace {
std::uint32_t finalize(std::uint64_t ticket, std::uint32_t fallback_user,
    std::uint32_t original_result, bool raw_known, std::uint32_t raw_packet, const lease::Pad& raw,
    std::uint32_t returned_result, bool returned_known, std::uint32_t returned_packet,
    const lease::Pad& candidate, std::uint64_t staged_revision, void* output, PublishReport publish) noexcept {
    if (!ticket && fallback_user >= 4) return returned_result;
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return returned_result;
    auto& s = state();
    Callback copied;
    for (const auto& callback : s.callbacks) if (callback.ticket == ticket && ticket) { copied = callback; break; }
    const auto& key = s.capture.state().key;
    if (!copied.ticket) {
        // Capacity/counter failure has no accepted callback record. It never
        // supplies fresh facts or completion, and its unknown duty is not ACKable.
        if (fallback_user < 4 && s.callbacks_unknown && key.user == fallback_user &&
            s.sources[fallback_user].connected && s.sources[fallback_user].epoch == key.source_epoch &&
            original_result == 0 && raw_known && output && publish) {
            (void)s.capture.retain_neutralization(key);
            const bool written = publish(output, returned_packet + 1, {});
            s.sources[fallback_user].packets.confirm(key.source_epoch, returned_packet + 1, {}, written);
            s.capture.neutralization_result(key, written);
        }
        return returned_result;
    }
    auto& source = s.sources[copied.user];
    const auto current = guard(sampled); s.leases.tick(current);
    const bool newest = copied.ordinal == source.latest_start && sampled.facts.now_ms >= copied.started_ms;
    const bool current_call = newest && copied.context == s.context &&
        copied.revision == s.revision.load(std::memory_order_relaxed);
    const bool connected = original_result == 0 && raw_known;
    if (newest) {
        if (connected != source.connected || (connected && !source.epoch)) source.epoch = issue(s.next_epoch);
        source.connected = connected;
        if (connected && !source.epoch) callback_failure(copied.user);
    }
    const bool same_packet = returned_packet == raw_packet;
    const bool equal = connected && returned_result == original_result && returned_known &&
        same_packet && raw == candidate;
    bool observation_updated{};
    if (current_call) {
        observation_updated = true;
        const bool eligible = equal && current.ready && current.focused && current.gameplay &&
            current.menu_closed && current.interference_clear &&
            sampled.facts.now_ms - copied.started_ms <= lease::maximum_age_ms;
        source.observation = {copied.user, raw_packet, source.epoch, copied.ordinal,
            sampled.facts.now_ms, s.context, raw, connected, eligible};
        s.leases.observe(copied.handle, source.observation, current);
    }
    capture_sync(current);
    auto final = candidate;
    capture::Plan plan{candidate};
    const bool bound = copied.capture_key.owner && copied.capture_key == s.capture.state().key;
    const auto phase = s.capture.state().phase;
    const bool prior_debt = s.capture.state().debt;
    const bool exact_stream = phase != capture::Phase::idle && s.capture.state().key.user == copied.user &&
        connected && source.connected && source.epoch == s.capture.state().key.source_epoch;
    const bool stage_stale = !staged_revision || staged_revision != s.revision.load(std::memory_order_relaxed);
    const bool delayed = !current_call || copied.capture_ticket.generation != s.capture.ticket().generation;
    const bool has_output = output && publish && original_result == 0 && raw_known && returned_result == 0 && returned_known;
    const bool continuation = !stage_stale && bound && exact_stream && has_output && equal && prior_debt && delayed;
    const bool fallback = bound && exact_stream && has_output && stage_stale;
    if (fallback)
        (void)s.capture.retain_neutralization(copied.capture_key);
    if (!continuation && !fallback && observation_updated && !stage_stale &&
        s.capture.state().key.user == copied.user) {
        auto input = capture_input(source, current, raw, candidate, copied.ordinal,
            has_output, original_result, returned_result, same_packet);
        // The original's sample is no older than its before-call stamp. Using
        // that conservative stamp prevents a slow filter from making old raw
        // input look fresh merely because finalization happened later.
        input.observed_ms = copied.started_ms;
        input.publication_can_end = exact_stream && !s.callbacks_unknown &&
            !stream_in_flight(copied.user, ticket) &&
            source.packets.can_handback(source.epoch, raw_packet, raw);
        plan = s.capture.plan(copied.capture_ticket, input);
        if (plan.suppress) final = plan.pad;
    }
    bool withheld{};
    const auto mask = [&](const capture::Held& held) {
        const auto masked = held.mask(final); withheld = withheld || masked != final; final = masked;
    };
    if (!plan.suppress && s.capture.state().phase == capture::Phase::releasing &&
        s.capture.state().key.user == copied.user && source.connected && source.epoch == s.capture.state().key.source_epoch)
        mask(s.capture.held());
    if (continuation || fallback || (s.callbacks_unknown && bound && exact_stream)) final = {};
    const auto revision = s.revision.load(std::memory_order_relaxed);
    auto packet = returned_packet;
    // A replacement stream is not ours to normalize. Its old capture retains
    // unknown debt, rather than exporting held controls or synthetic numbering.
    if (has_output && (exact_stream || !source.packets.active)) {
        // Inactive pass-through updates history only; it never owns a write.
        packet = plan.release ? raw_packet : source.packets.translate(source.epoch, revision,
            s.capture.state().revision, raw_packet, returned_packet, raw, final,
            exact_stream && (plan.suppress || withheld || continuation || fallback));
    }
    const bool owned_write = exact_stream &&
        (plan.suppress || withheld || continuation || fallback || packet != returned_packet);
    bool written{};
    if (has_output && owned_write) {
        written = publish(output, packet, final);
        source.packets.confirm(source.epoch, packet, final, written);
    }
    if (plan.suppress) {
        const bool confirmed = s.capture.confirm(plan, written);
        if (confirmed && plan.release)
            source.packets.handback(source.epoch, revision, s.capture.state().revision, raw_packet, raw);
    } else if (continuation) s.capture.continuation_result(copied.capture_key, written);
    else if (fallback) s.capture.neutralization_result(copied.capture_key, written);
    else if (owned_write) s.capture.owned_result(s.capture.state().key, written);
    if (s.callbacks_unknown && s.capture.state().phase != capture::Phase::idle)
        (void)s.capture.retain_neutralization(s.capture.state().key);
    drain(ticket);
    return returned_result;
}
}
void finish(Poll& poll, std::uint32_t original_result, bool raw_known,
    std::uint32_t raw_packet, const lease::Pad& raw, std::uint32_t returned_result,
    bool returned_known, std::uint32_t returned_packet, const lease::Pad& returned) noexcept {
    const auto ticket = poll.ticket_; poll.ticket_ = 0;
    (void)finalize(ticket, poll.fallback_user_, original_result, raw_known, raw_packet, raw,
        returned_result, returned_known, returned_packet, returned, publication_revision(), nullptr, nullptr);
    poll.fallback_user_ = 4;
}
std::uint32_t finish_capture(Poll& poll, std::uint32_t original_result, bool raw_known,
    std::uint32_t raw_packet, const lease::Pad& raw, std::uint32_t returned_result,
    bool returned_known, std::uint32_t returned_packet, const lease::Pad& returned,
    std::uint64_t staged_revision, void* output, PublishReport publish) noexcept {
    const auto ticket = poll.ticket_; poll.ticket_ = 0;
    const auto result = finalize(ticket, poll.fallback_user_, original_result, raw_known, raw_packet, raw,
        returned_result, returned_known, returned_packet, returned, staged_revision, output, publish);
    poll.fallback_user_ = 4;
    return result;
}
}
