#include "native_input_source.h"
#include "native_input_capture.h"
#include "native_input_write.h"
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
namespace write = input_tracked_write;
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
    write::Key write_key{};
    write::Version write_version{};
};
struct Writer {
    bool occupied{}, attempted{};
    write::Receipt receipt;
    lease::Pad desired{};
    capture::Held held_controls;
    std::uint64_t last_observation{}, release_started_ms{};
};
struct State {
    std::mutex gate;
    lease::Broker leases;
    capture::Capture capture;
    Writer writer;
    std::uint64_t next_writer_generation{1}, last_output_sequence{};
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
void revoke_write(write::Reason reason) noexcept;
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
    const auto why = reason == lease::Reason::context_changed ? capture::Reason::context_changed :
        reason == lease::Reason::stale_epoch ? capture::Reason::clock_regressed :
        reason == lease::Reason::interference ? capture::Reason::interference : capture::Reason::authority_lost;
    state().capture.revoke(why);
    revoke_write(why);
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
std::uint32_t write_callbacks(const write::Key& key) noexcept {
    std::uint32_t count{};
    if (!key.owner) return count;
    for (const auto& callback : state().callbacks)
        if (callback.ticket && callback.write_key == key) ++count;
    return count;
}
bool write_busy(std::uint64_t owner = 0, lease::Handle handle = {}) noexcept {
    const auto& w = state().writer;
    return w.occupied && (!owner || (w.receipt.key.owner == owner &&
        w.receipt.key.lease_sequence == handle.sequence));
}
void revoke_write(write::Reason reason) noexcept {
    auto& s = state(); auto& w = s.writer;
    if (!w.occupied || w.receipt.phase == write::Phase::complete) return;
    if (w.receipt.phase != write::Phase::releasing) {
        w.receipt.phase = write::Phase::releasing; w.receipt.reason = reason;
        w.release_started_ms = 0;
        w.held_controls = w.attempted ? capture::Held::all() : capture::Held{};
    }
}
void write_cleanup() noexcept {
    auto& s = state(); auto& w = s.writer;
    if (!w.occupied) return;
    auto& row = w.receipt;
    // No attempted output means no copied command can survive the closed phase.
    // A partial attempt sets attempted BEFORE touching the destination.
    if (row.phase == write::Phase::releasing && !w.attempted && !s.callbacks_unknown &&
        !write_callbacks(row.key) && row.key.user < 4 && !s.sources[row.key.user].packets.active) {
        row.phase = write::Phase::complete; row.output_released = true;
        row.output_debt = row.quarantined = false;
    }
    if (row.orphaned && row.phase == write::Phase::complete && row.output_released &&
        !row.output_debt && !row.quarantined && !s.callbacks_unknown && !write_callbacks(row.key) &&
        row.key.user < 4 && !s.sources[row.key.user].packets.active) w = {};
}
void write_sync(const lease::Guard& current) noexcept {
    auto& s = state(); auto& w = s.writer;
    if (!w.occupied) return;
    auto& row = w.receipt;
    if (row.phase == write::Phase::pending || row.phase == write::Phase::active) {
        lease::Receipt passive;
        if (!s.leases.state(row.key.owner, {row.key.lease_sequence}, passive) || lease::terminal(passive.status))
            revoke_write(write::Reason::authority_lost);
        else {
            row.source_expires_ms = passive.expires_ms;
            if (!current.ready) revoke_write(write::Reason::authority_lost);
            else if (!current.focused) revoke_write(write::Reason::focus_lost);
            else if (!current.gameplay) revoke_write(write::Reason::gameplay_inactive);
            else if (!current.menu_closed) revoke_write(write::Reason::menu_open);
            else if (!current.interference_clear) revoke_write(write::Reason::interference);
            else if (row.key.context_generation != current.context_generation) revoke_write(write::Reason::context_changed);
            else if (current.now_ms >= row.write_expires_ms) revoke_write(write::Reason::deadline);
        }
    }
    if (row.phase == write::Phase::releasing) {
        if (!w.release_started_ms && current.now_ms) w.release_started_ms = current.now_ms;
        const auto& source = s.sources[row.key.user];
        if (!source.connected || source.epoch != row.key.source_epoch ||
            (w.release_started_ms && current.now_ms >= w.release_started_ms &&
                current.now_ms - w.release_started_ms >= write::cleanup_boundary_ms)) row.quarantined = true;
    }
    if (s.callbacks_unknown && row.phase != write::Phase::complete) {
        revoke_write(write::Reason::exhausted);
        row.reason = write::Reason::exhausted; row.output_debt = row.quarantined = true;
        row.output_released = false; w.held_controls = capture::Held::all();
    }
    write_cleanup();
}
// True records ONE fully copied output. Partial/refused attempts retain debt,
// but are never published counters or a last-confirmed value.
bool write_result(bool published, write::WrittenKind kind) noexcept {
    auto& s = state(); auto& w = s.writer; auto& row = w.receipt;
    if (!published) {
        revoke_write(write::Reason::publication_unconfirmed);
        row.reason = write::Reason::publication_unconfirmed;
        row.output_debt = row.quarantined = true;
        w.held_controls = capture::Held::all();
        return false;
    }
    auto& category = kind == write::WrittenKind::desired ? row.version_publications :
        kind == write::WrittenKind::cleanup ? row.cleanup_publications : row.handback_publications;
    if (row.publications == maximum || category == maximum) {
        s.failed = s.callbacks_unknown = true;
        revoke_write(write::Reason::exhausted);
        row.reason = write::Reason::exhausted; row.output_debt = row.quarantined = true;
        row.output_released = false; return false;
    }
    ++row.publications; ++category; row.last_written_kind = kind;
    // Keep successful-copy evidence coherent even when accounting exhausts.
    // Exhaustion below still forbids active status, handback and ACK.
    if (kind == write::WrittenKind::desired) row.published_revision = row.desired_revision;
    if (row.publications == maximum || category == maximum) {
        s.failed = s.callbacks_unknown = true; revoke_write(write::Reason::exhausted);
        row.reason = write::Reason::exhausted; row.output_debt = row.quarantined = true;
        row.output_released = false; return false;
    }
    if (kind == write::WrittenKind::desired) row.phase = write::Phase::active;
    return true;
}
bool capture_busy(std::uint64_t owner = 0, lease::Handle handle = {}) noexcept {
    const auto& s = state(); const auto& receipt = s.capture.state();
    if (receipt.phase == capture::Phase::idle ||
        (owner && (receipt.key.owner != owner || receipt.key.lease_sequence != handle.sequence))) return false;
    return s.capture.reserved() || s.callbacks_unknown || captured_callbacks(receipt.key) ||
        (receipt.key.user < 4 && s.sources[receipt.key.user].packets.active);
}
void capture_cleanup() noexcept {
    write_cleanup();
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
        if (entry.owner && entry.orphaned && !entry.callbacks && !capture_busy(entry.owner, entry.handle) && !write_busy(entry.owner, entry.handle)) {
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
    write_sync(guard);
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
    if (s.writer.occupied && s.writer.receipt.key.user == user &&
        s.writer.receipt.phase != write::Phase::complete) {
        s.callbacks_unknown = true; revoke_write(write::Reason::exhausted);
        s.writer.receipt.reason = write::Reason::exhausted;
        s.writer.receipt.output_debt = s.writer.receipt.quarantined = true;
        s.writer.held_controls = capture::Held::all();
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
    if (capture_busy() || write_busy()) { reason = lease::Reason::busy; return false; }
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
    if (write_busy(owner, handle)) revoke_write(write::Reason::cancelled);
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
    receipt.write_reserved = write_busy(owner, handle);
    receipt.source_released = lease::terminal(receipt.status) && !entry->callbacks &&
        !receipt.capture_reserved && !receipt.write_reserved;
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
    if (entry->callbacks || capture_busy(owner, handle) || write_busy(owner, handle)) { reason = lease::Reason::busy; return false; }
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
    auto& writer = state().writer;
    if (writer.occupied && writer.receipt.key.owner == owner && !writer.receipt.orphaned) {
        writer.receipt.orphaned = true; revoke_write(write::Reason::cancelled); advance_revision();
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
    if (write_busy()) { reason = capture::Reason::busy; return false; }
    if (s.capture.state().phase == capture::Phase::idle && handle.sequence <= s.last_output_sequence) {
        reason = capture::Reason::invalid_key; return false;
    }
    if (stream_in_flight(receipt.user)) { reason = capture::Reason::busy; return false; }
    const auto& source = s.sources[receipt.user];
    if (!s.leases.accepted(owner, handle, source.observation) ||
        snapshot.observation_serial != source.observation.observation_serial || snapshot.pad != source.observation.pad) return false;
    const capture::Key key{owner, handle.sequence, receipt.source_epoch, receipt.context_generation, receipt.user};
    auto input = capture_input(source, current, source.observation.pad, source.observation.pad,
        source.observation.observation_serial, true, 0, 0, true);
    input.user = receipt.user; input.safety.source_authorized = true;
    const bool result = s.capture.request(key, receipt.expires_ms, input, false, reason);
    if (result) { s.capture_orphaned = false; s.last_output_sequence = handle.sequence; advance_revision(); }
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

bool write_request(std::uint64_t owner, lease::Handle handle, const lease::Snapshot& snapshot,
    const lease::Pad& desired, write::Version& version, write::Reason& reason) noexcept {
    version = {}; reason = write::Reason::authority_lost;
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) { reason = write::Reason::busy; return false; }
    lease::Reason client_reason; if (!client(sampled, client_reason)) return false;
    auto& s = state(); const auto current = guard(sampled);
    s.leases.tick(current); capture_sync(current);
    lease::Receipt passive;
    if (!owner || !s.leases.state(owner, handle, passive) || passive.status != lease::Status::active ||
        !s.leases.issued(owner, handle, snapshot) || passive.user > 3) return false;
    if (s.failed || s.callbacks_unknown || s.revision.load(std::memory_order_relaxed) == maximum) {
        reason = write::Reason::exhausted; return false;
    }
    if (!current.ready || !current.focused || !current.gameplay || !current.menu_closed ||
        !current.interference_clear || passive.context_generation != current.context_generation) return false;
    const auto& source = s.sources[passive.user];
    if (!s.leases.accepted(owner, handle, source.observation) ||
        snapshot.source_epoch != source.epoch || snapshot.context_generation != current.context_generation ||
        snapshot.observation_serial != source.observation.observation_serial || snapshot.pad != source.observation.pad ||
        !snapshot.observed_ms || snapshot.observed_ms > current.now_ms ||
        current.now_ms - snapshot.observed_ms > lease::maximum_age_ms) {
        reason = write::Reason::source_stale; return false;
    }
    if (current.now_ms > maximum - write::write_lifetime_ms) { reason = write::Reason::deadline; return false; }
    if (s.capture.state().phase != capture::Phase::idle) { reason = write::Reason::busy; return false; }
    auto& w = s.writer;
    if (w.occupied) {
        if (!write_busy(owner, handle) || w.receipt.orphaned ||
            (w.receipt.phase != write::Phase::pending && w.receipt.phase != write::Phase::active) ||
            !w.receipt.version_publications) { reason = write::Reason::busy; return false; }
        if (w.receipt.version.revision == maximum) { reason = write::Reason::exhausted; return false; }
        ++w.receipt.version.revision; w.receipt.desired_revision = w.receipt.version.revision;
        w.receipt.version_publications = 0; w.receipt.phase = write::Phase::pending;
    } else {
        if (handle.sequence <= s.last_output_sequence) { reason = write::Reason::invalid_key; return false; }
        if (stream_in_flight(passive.user)) { reason = write::Reason::busy; return false; }
        const auto generation = issue(s.next_writer_generation);
        if (!generation) { reason = write::Reason::exhausted; return false; }
        w = {}; w.occupied = true;
        w.receipt.key = {owner, handle.sequence, passive.source_epoch, passive.context_generation, passive.user};
        w.receipt.version = {generation, 1}; w.receipt.desired_revision = 1;
        w.last_observation = snapshot.observation_serial;
        s.last_output_sequence = handle.sequence;
    }
    w.desired = desired; w.receipt.write_expires_ms = current.now_ms + write::write_lifetime_ms;
    w.receipt.source_expires_ms = passive.expires_ms;
    version = w.receipt.version; reason = write::Reason::none; advance_revision();
    return true;
}
bool write_state(std::uint64_t owner, lease::Handle handle, write::Version version, write::Receipt& out) noexcept {
    out = {}; const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto current = guard(sampled); s.leases.tick(current); capture_sync(current);
    if (!owner || !handle.sequence || !write_busy(owner, handle) || s.writer.receipt.version != version) return false;
    out = s.writer.receipt; out.polls_in_flight = write_callbacks(out.key);
    out.callbacks_unknown = s.callbacks_unknown;
    return true;
}
bool write_release(std::uint64_t owner, lease::Handle handle) noexcept {
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto current = guard(sampled); s.leases.tick(current); capture_sync(current);
    if (!owner || !handle.sequence || !write_busy(owner, handle)) return false;
    if (s.writer.receipt.phase != write::Phase::releasing && s.writer.receipt.phase != write::Phase::complete) {
        revoke_write(write::Reason::cancelled); advance_revision();
    }
    write_cleanup(); return true;
}
bool write_acknowledge(std::uint64_t owner, lease::Handle handle) noexcept {
    const auto sampled = sample(); PublicationScope scope;
    if (!scope.owns()) return false;
    lease::Reason reason; if (!client(sampled, reason)) return false;
    auto& s = state(); const auto current = guard(sampled); s.leases.tick(current); capture_sync(current);
    if (!owner || !handle.sequence || !write_busy(owner, handle)) return false;
    const auto& row = s.writer.receipt;
    if (row.phase != write::Phase::complete || !row.output_released || row.output_debt || row.quarantined ||
        s.callbacks_unknown || write_callbacks(row.key) || row.key.user >= 4 || s.sources[row.key.user].packets.active) return false;
    s.writer = {}; reclaim_orphans(); return true;
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
    if (s.writer.occupied && s.writer.receipt.key.user == user && s.writer.receipt.phase != write::Phase::complete) {
        value.write_key = s.writer.receipt.key; value.write_version = s.writer.receipt.version;
    }
    value.handle = s.leases.active_user() == user ? s.leases.active() :
        value.capture_key.owner ? lease::Handle{value.capture_key.lease_sequence} :
        value.write_key.owner ? lease::Handle{value.write_key.lease_sequence} : lease::Handle{};
    if (auto* entry = reservation(value.handle)) ++entry->callbacks;
    *vacant = value; s.sources[user].latest_start = ordinal; poll.ticket_ = ticket;
}
namespace {
// This runs under the SAME finalizer/publication gate as neutral capture. A
// copied original can supply cleanup raw facts after revocation, but can never
// turn those facts back into admission for the desired Pad.
void finish_write(const Callback& copied, Source& source, const lease::Guard& current,
    bool current_call, bool observation_updated, bool connected, bool equal,
    std::uint32_t raw_packet, const lease::Pad& raw, bool has_output,
    std::uint32_t returned_packet, const lease::Pad& candidate, std::uint64_t staged_revision,
    void* output, PublishReport publish) noexcept {
    auto& s = state(); auto& w = s.writer; auto& row = w.receipt;
    if (!w.occupied || row.phase == write::Phase::complete || copied.write_key != row.key) return;
    const bool exact_stream = connected && source.connected && source.epoch == row.key.source_epoch;
    if (!exact_stream) {
        revoke_write(write::Reason::source_changed);
        if (w.attempted) row.quarantined = true;
        return; // Never normalize a replacement or disconnected stream.
    }
    const bool same_version = copied.write_version == row.version;
    const bool stage_current = staged_revision && staged_revision == s.revision.load(std::memory_order_relaxed);
    const bool fresh = copied.ordinal == source.latest_start && copied.ordinal > w.last_observation &&
        copied.started_ms && current.now_ms >= copied.started_ms &&
        current.now_ms - copied.started_ms <= lease::maximum_age_ms;
    const bool clean = equal && stage_current && current.interference_clear;
    const bool asserting = row.phase == write::Phase::pending || row.phase == write::Phase::active;
    if (asserting && same_version && (!clean || !has_output || !fresh))
        revoke_write(!fresh ? write::Reason::source_stale : write::Reason::interference);
    const bool desired = (row.phase == write::Phase::pending || row.phase == write::Phase::active) &&
        same_version && current_call && observation_updated && fresh && clean && has_output &&
        current.ready && current.focused && current.gameplay && current.menu_closed &&
        current.context_generation == row.key.context_generation && current.now_ms < row.write_expires_ms &&
        s.leases.accepted(row.key.owner, {row.key.lease_sequence}, source.observation);
    if (fresh) {
        w.last_observation = copied.ordinal;
        if (row.phase == write::Phase::releasing) w.held_controls.rest(raw);
    }
    if (!has_output || (!desired && !w.attempted)) return;
    auto final = desired ? w.desired : w.held_controls.mask(candidate);
    // A stale version/ordering/stage can only maintain already-owed neutral
    // output. It cannot prove physical rest or final raw handback.
    const bool delayed = !same_version || !fresh || !stage_current || s.callbacks_unknown;
    if (!desired && delayed) {
        final = {};
        if (row.phase == write::Phase::releasing) w.held_controls = capture::Held::all();
    }
    const bool handback = !desired && row.phase == write::Phase::releasing && same_version && fresh && clean &&
        current.focused && current.gameplay && current.menu_closed && !s.callbacks_unknown &&
        !stream_in_flight(row.key.user, copied.ticket) && !w.held_controls.any() &&
        row.publications < maximum - 1 && source.packets.can_handback(source.epoch, raw_packet, raw);
    if (handback) final = raw;
    const auto revision = s.revision.load(std::memory_order_relaxed);
    const auto packet = handback ? raw_packet : source.packets.translate(source.epoch, revision,
        row.desired_revision, raw_packet, returned_packet, raw, final, true);
    // Record duty before the only destination copy, including refused/partial
    // copies. A successful wrapper return alone never clears that duty.
    w.attempted = true; row.output_debt = true;
    const bool written = publish(output, packet, final);
    source.packets.confirm(source.epoch, packet, final, written);
    const auto kind = handback ? write::WrittenKind::handback :
        desired ? write::WrittenKind::desired : write::WrittenKind::cleanup;
    if (write_result(written, kind) && handback && !s.callbacks_unknown) {
        source.packets.handback(source.epoch, revision, row.desired_revision, raw_packet, raw);
        row.phase = write::Phase::complete; row.output_released = true;
        row.output_debt = row.quarantined = false; w.held_controls = {};
    }
}
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
        if (fallback_user < 4 && s.callbacks_unknown && s.writer.occupied &&
            s.writer.receipt.phase != write::Phase::complete && s.writer.receipt.key.user == fallback_user &&
            s.sources[fallback_user].connected && s.sources[fallback_user].epoch == s.writer.receipt.key.source_epoch &&
            original_result == 0 && raw_known && output && publish) {
            auto& writer = s.writer; auto& fallback_source = s.sources[fallback_user];
            writer.attempted = true; writer.receipt.output_debt = true;
            const auto packet = fallback_source.packets.translate(fallback_source.epoch,
                s.revision.load(std::memory_order_relaxed), writer.receipt.desired_revision,
                raw_packet, returned_packet, raw, {}, true);
            const bool written = publish(output, packet, {});
            fallback_source.packets.confirm(fallback_source.epoch, packet, {}, written);
            (void)write_result(written, write::WrittenKind::cleanup);
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
    if (s.writer.occupied && s.writer.receipt.key.user == copied.user &&
        s.writer.receipt.phase != write::Phase::complete) {
        const bool known_output = output && publish && original_result == 0 && raw_known &&
            returned_result == 0 && returned_known;
        finish_write(copied, source, current, current_call, observation_updated, connected, equal,
            raw_packet, raw, known_output, returned_packet, candidate, staged_revision, output, publish);
        drain(ticket); return returned_result;
    }
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
