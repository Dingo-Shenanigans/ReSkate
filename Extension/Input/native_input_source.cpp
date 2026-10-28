#include "native_input_source.h"
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
constexpr std::size_t callback_capacity = 128;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
struct Reservation {
    std::uint64_t owner{};
    lease::Handle handle{};
    std::uint32_t user{4}, callbacks{};
    bool orphaned{};
};
struct Source {
    lease::Source observation{};
    std::uint64_t latest_start{};
};
struct Callback {
    std::uint64_t ticket{}, revision{}, context{}, ordinal{}, started_ms{};
    lease::Handle handle{};
    std::uint32_t user{4};
};
struct State {
    std::mutex gate;
    lease::Broker leases;
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
thread_local bool held{};
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
void reclaim_orphans() noexcept {
    auto& s = state();
    for (auto& entry : s.reservations) if (entry.owner && entry.orphaned && !entry.callbacks) {
        (void)s.leases.acknowledge(entry.owner, entry.handle);
        entry = {};
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
    if (!enabled || held) return;
    state().gate.lock();
    held = owns_ = true;
}
PublicationScope::~PublicationScope() {
    if (!owns_) return;
    held = false;
    state().gate.unlock();
}
bool publication_held() noexcept { return held; }
std::uint64_t publication_revision() noexcept { return state().revision.load(std::memory_order_acquire); }
void publication_changed() noexcept {
    if (!held) return;
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
    s.leases.tick(guard(current));
    reclaim_orphans();
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
    return state().leases.read(owner, handle, guard(sampled), snapshot, reason);
}
bool lease_renew(std::uint64_t owner, lease::Handle handle, const lease::Snapshot& snapshot,
    lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    return state().leases.renew(owner, handle, snapshot, guard(sampled), reason);
}
bool lease_release(std::uint64_t owner, lease::Handle handle, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    state().leases.tick(guard(sampled));
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (!state().leases.release(owner, handle)) { reason = lease::Reason::unavailable; return false; }
    reason = lease::Reason::none;
    return true;
}
bool lease_state(std::uint64_t owner, lease::Handle handle,
    lease::Receipt& receipt, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    state().leases.tick(guard(sampled));
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (!state().leases.state(owner, handle, receipt)) { reason = lease::Reason::unavailable; return false; }
    receipt.polls_in_flight = entry->callbacks;
    receipt.source_released = lease::terminal(receipt.status) && !entry->callbacks;
    receipt.orphaned = entry->orphaned;
    reason = lease::Reason::none;
    return true;
}
bool lease_acknowledge(std::uint64_t owner, lease::Handle handle, lease::Reason& reason) noexcept {
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) { reason = lease::Reason::busy; return false; }
    if (!client(sampled, reason)) return false;
    state().leases.tick(guard(sampled));
    Reservation* entry{};
    if (!owned(owner, handle, entry, reason)) return false;
    if (entry->callbacks) { reason = lease::Reason::busy; return false; }
    if (!state().leases.acknowledge(owner, handle)) { reason = lease::Reason::unavailable; return false; }
    *entry = {};
    reason = lease::Reason::none;
    return true;
}
void lease_release_all(std::uint64_t owner) noexcept {
    if (!owner) return;
    PublicationScope scope;
    if (!scope.owns()) return;
    for (auto& entry : state().reservations) if (entry.owner == owner) {
        entry.orphaned = true;
        (void)state().leases.release(owner, entry.handle);
    }
    reclaim_orphans();
}

Poll::~Poll() {
    if (!ticket_) return;
    PublicationScope scope;
    if (!scope.owns()) return;
    drain(ticket_);
}
void begin(bool outer, std::uint32_t user, Poll& poll) noexcept {
    if (!outer || user > 3 || poll.ticket_) return;
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) return;
    auto& s = state();
    const auto current = guard(sampled);
    s.leases.tick(current);
    if (!current.ready || !current.focused || !current.gameplay || !current.menu_closed || !current.interference_clear) return;
    Callback* vacant{};
    for (auto& callback : s.callbacks) if (!callback.ticket) { vacant = &callback; break; }
    if (!vacant) { invalidate(lease::Reason::capacity); return; }
    const auto ticket = issue(s.next_ticket), ordinal = issue(s.next_ordinal);
    if (!ticket || !ordinal) { s.failed = true; invalidate(lease::Reason::sequence_exhausted); return; }
    const auto handle = s.leases.active_user() == user ? s.leases.active() : lease::Handle{};
    if (auto* entry = reservation(handle)) ++entry->callbacks;
    *vacant = {ticket, s.revision.load(std::memory_order_relaxed), s.context,
        ordinal, sampled.facts.now_ms, handle, user};
    s.sources[user].latest_start = ordinal;
    poll.ticket_ = ticket;
}
void finish(Poll& poll, std::uint32_t original_result, bool raw_known,
    std::uint32_t raw_packet, const lease::Pad& raw, std::uint32_t returned_result,
    bool returned_known, std::uint32_t returned_packet, const lease::Pad& returned) noexcept {
    if (!poll.ticket_) return;
    const auto sampled = sample();
    PublicationScope scope;
    if (!scope.owns()) return;
    auto& s = state();
    Callback copied{};
    for (const auto& callback : s.callbacks) if (callback.ticket == poll.ticket_) { copied = callback; break; }
    const auto ticket = poll.ticket_;
    poll.ticket_ = 0;
    if (!copied.ticket) return;
    const auto current = guard(sampled);
    s.leases.tick(current);
    auto& source = s.sources[copied.user];
    // A concurrent newer original wins. Older completions cannot roll back
    // source contents, disconnect state, or the next owner's continuity boundary.
    const bool current_call = copied.ordinal == source.latest_start && copied.context == s.context &&
        copied.revision == s.revision.load(std::memory_order_relaxed) &&
        sampled.facts.now_ms >= copied.started_ms;
    if (current_call) {
        const bool connected = original_result == 0 && raw_known;
        const bool same = connected && returned_result == original_result && returned_known &&
            raw_packet == returned_packet && raw == returned;
        const bool eligible = same && current.ready && current.focused && current.gameplay &&
            current.menu_closed && current.interference_clear &&
            sampled.facts.now_ms - copied.started_ms <= lease::maximum_age_ms;
        auto& observed = source.observation;
        if (!connected || !eligible) {
            observed = {};
            observed.user = copied.user;
            s.leases.observe(copied.handle, observed, current);
        } else {
            const auto epoch = observed.connected ? observed.epoch : issue(s.next_epoch);
            if (!epoch) { s.failed = true; invalidate(lease::Reason::sequence_exhausted); }
            else {
                observed = {copied.user, raw_packet, epoch, copied.ordinal,
                    sampled.facts.now_ms, s.context, raw, true, true};
                s.leases.observe(copied.handle, observed, current);
            }
        }
    }
    drain(ticket);
}
}
