#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Values crossing the client/physics boundary. No native address is part of this contract.
namespace dingosdk::owned_body {
using Vec = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // Hamilton XYZW
inline constexpr std::size_t maximum_capsules = 8, retained_capacity = 8, receipt_capacity = 4;
// Commands one body's inbox holds before the synchronizer has taken any: the one for the
// measured step N and, behind it, the one for step N+1.
inline constexpr std::size_t pending_capacity = 2;
inline constexpr double native_seconds = 1.0 / 60.0;
inline constexpr std::uint64_t command_lifetime_ms = 100;
inline constexpr float maximum_force = 40000, maximum_torque = 4000, maximum_impulse = 600;

struct Capsule { Vec a{}, b{}; float radius{}; };
struct Definition {
    std::array<Capsule, maximum_capsules> capsules{};
    std::size_t count{};
    float mass{};
    Vec inertia{}, position{};
    Quaternion rotation{};
};
// What a parked body keeps from its definition: the capsules, mass and inertia, compared bit for
// bit, so a re-activated body is exactly the body that was asked for. Position and rotation say
// only where it starts, and may differ.
inline bool same_shape(const Definition& a, const Definition& b) noexcept {
    const auto same = [](float x, float y) { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); };
    const auto same_vec = [&](const Vec& x, const Vec& y) { return same(x[0], y[0]) && same(x[1], y[1]) && same(x[2], y[2]); };
    if (a.count != b.count || a.count > maximum_capsules || !same(a.mass, b.mass) || !same_vec(a.inertia, b.inertia)) return false;
    for (std::size_t i = 0; i < a.count; ++i)
        if (!same_vec(a.capsules[i].a, b.capsules[i].a) || !same_vec(a.capsules[i].b, b.capsules[i].b) ||
            !same(a.capsules[i].radius, b.capsules[i].radius)) return false;
    return true;
}
// How a create asked to reuse a parked body ended: with that body re-activated, or with a new
// allocation and the reason none of the caller's own parked bodies could be used.
enum class Origin { allocated, reused };
inline const char* name(Origin value) noexcept { return value == Origin::reused ? "reused" : "allocated"; }
enum class ReuseMiss { none, none_parked, definition_differs, world_changed, quarantined, parked_body_changed };
inline const char* name(ReuseMiss value) noexcept {
    switch (value) {
    case ReuseMiss::none: return "none";
    case ReuseMiss::none_parked: return "none_parked";
    case ReuseMiss::definition_differs: return "definition_differs";
    case ReuseMiss::world_changed: return "world_changed";
    case ReuseMiss::quarantined: return "quarantined";
    case ReuseMiss::parked_body_changed: return "parked_body_changed";
    }
    return "none";
}
struct CreateReport { Origin origin{}; ReuseMiss miss{}; };
// SDK-generated identifiers. The Lua binding wraps these in locked userdata.
struct Handle { std::uint64_t generation{}; std::uint32_t slot{}; bool operator==(const Handle&) const = default; };
struct Epoch {
    std::uint64_t world{}, body{}, serial{};
    double time{};
    float seconds{};
    bool operator==(const Epoch&) const = default;
};
struct Sample {
    Epoch epoch{};
    Vec position{}, velocity{}, angular_velocity{}, inertia{};
    Quaternion rotation{};
    float mass{};
};
struct Command {
    Epoch epoch{};
    Vec force{}, torque{}, impulse{};
    std::uint32_t impulse_id{};
};
enum class Outcome { none, queued, applied, partial, rejected, expired };
inline const char* name(Outcome value) noexcept {
    switch (value) {
    case Outcome::none: return "none";
    case Outcome::queued: return "queued";
    case Outcome::applied: return "applied";
    case Outcome::partial: return "partial";
    case Outcome::rejected: return "rejected";
    case Outcome::expired: return "expired";
    }
    return "rejected";
}
struct Receipt {
    Outcome outcome{};
    std::uint64_t sequence{};
    Epoch requested{};
    Epoch produced{};
    bool force_attempted{}, torque_attempted{}, retained{}, quarantined{}, successor_available{};
    Sample successor{};
};
// A constructor may have mutated the world before its out handle or return value
// becomes verifiable. Parking the confirmed subset cannot erase that uncertainty.
struct AllocationProgress {
    bool body_confirmed{};
    std::size_t shapes_attempted{}, shapes_confirmed{};
    bool quarantine(bool confirmed_subset_parked) const noexcept {
        return !confirmed_subset_parked || !body_confirmed || shapes_attempted != shapes_confirmed;
    }
};
inline bool finite(const Vec& value, float bound) noexcept {
    for (float v : value) if (!std::isfinite(v) || std::abs(v) > bound) return false;
    return true;
}
inline double length(const Vec& value) noexcept {
    double sum{};
    for (float v : value) sum += static_cast<double>(v) * v;
    return std::sqrt(sum);
}
inline bool calibrated(float seconds) noexcept {
    return std::isfinite(seconds) && std::abs(static_cast<double>(seconds) - native_seconds) <= 1e-7;
}
inline bool valid(const Definition& value) noexcept {
    if (!value.count || value.count > maximum_capsules || !std::isfinite(value.mass) ||
        value.mass < 1 || value.mass > 1000 || !finite(value.inertia, 10000) || !finite(value.position, 100000)) return false;
    for (float v : value.inertia) if (v < .01f) return false;
    double norm{};
    for (float v : value.rotation) { if (!std::isfinite(v)) return false; norm += static_cast<double>(v) * v; }
    if (std::abs(norm - 1) > .0001) return false;
    for (std::size_t i = 0; i < value.count; ++i) {
        const auto& c = value.capsules[i];
        if (!finite(c.a, 10) || !finite(c.b, 10) || !std::isfinite(c.radius) || c.radius < .01f || c.radius > 2) return false;
    }
    return true;
}
inline bool valid(const Command& value) noexcept {
    if (!value.epoch.world || !value.epoch.body || !value.epoch.serial || value.epoch.serial == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(value.epoch.time) || value.epoch.time < 0 || !std::isfinite(value.epoch.time + value.epoch.seconds) ||
        !calibrated(value.epoch.seconds) || !finite(value.force, maximum_force) || !finite(value.torque, maximum_torque) ||
        !finite(value.impulse, maximum_impulse) || length(value.force) > maximum_force || length(value.torque) > maximum_torque ||
        length(value.impulse) > maximum_impulse || (length(value.impulse) > 0 && !value.impulse_id)) return false;
    Vec combined = value.force;
    for (std::size_t i = 0; i < 3; ++i) combined[i] += value.impulse[i] / value.epoch.seconds;
    return finite(combined, maximum_force) && length(combined) <= maximum_force;
}

// External synchronization owns this fixed inbox and receipt ring. A receipt is reserved
// before publication, and consumption precedes the first setter. Older unacknowledged
// results cannot overwrite a new packet or prevent consecutive native-step commands.
//
// The inbox holds pending_capacity commands. In some rides the client's tick falls between the
// physics clock edge and the synchronizer's calls on the body (counted in the game on
// 2026-11-14: no listed synchronizer call between the edge and the tick, three after it, in
// every frame of such a ride), so the tick that reads sample N+1 still finds its command for N
// pending. The command for N+1 then waits behind it: command, deadline and inbox are the first
// pending command's, the next_ members the second's. Each is taken only by a call that reads
// the step after its own epoch, keeps its own deadline and receipt, and gets its own verdict.
struct Mailbox {
    Command command{};
    std::array<Receipt, receipt_capacity> receipts{};
    std::size_t head{}, count{}, inbox{};
    std::uint64_t deadline{}, last_serial{}, last_submitted{}, sequence{};
    std::uint32_t last_impulse{};
    Epoch last_requested{};
    bool pending{}, producing{}, failed{};
    Command next_command{};
    std::size_t next_inbox{};
    std::uint64_t next_deadline{};
    bool next_pending{};

    // The one clock reading at which a command measured at this epoch is produced.
    static Epoch target_of(Epoch epoch) noexcept { ++epoch.serial; epoch.time += epoch.seconds; return epoch; }
    // The second pending command becomes the first, with its own deadline and receipt row.
    void advance() noexcept {
        if (!next_pending) return;
        command = next_command;
        deadline = next_deadline;
        inbox = next_inbox;
        pending = true;
        next_pending = false;
    }

    Receipt oldest() const noexcept { return count ? receipts[head] : Receipt{}; }
    bool find(std::uint64_t selected, Receipt& out) const noexcept {
        if (!selected) { out = oldest(); return true; }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& record = receipts[(head + i) % receipts.size()];
            if (record.sequence == selected) { out = record; return true; }
        }
        out = {};
        return false;
    }
    bool needs_successor() const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& record = receipts[(head + i) % receipts.size()];
            if (record.outcome == Outcome::applied && !record.successor_available) return true;
        }
        return false;
    }
    // The service supplies globally unique IDs across its bodies. Zero keeps a local
    // sequence for standalone users of this contract, including the portable fixture.
    // Behind a pending command only the command for exactly the next step is taken: the epoch
    // the pending one will be produced at, so the clock already reads the pending one's step.
    // Its impulse event is compared with the pending one's too, because last_impulse moves only
    // when a command is produced.
    bool submit(const Command& value, const Epoch& observed, std::uint64_t now, std::uint64_t assigned = 0) noexcept {
        if (failed || producing || next_pending || count == receipts.size() || !valid(value) || value.epoch != observed ||
            value.epoch.serial <= last_submitted || (value.impulse_id && value.impulse_id <= last_impulse) ||
            sequence == std::numeric_limits<std::uint64_t>::max() || now > std::numeric_limits<std::uint64_t>::max() - command_lifetime_ms ||
            (last_submitted && (value.epoch.world != last_requested.world || value.epoch.body != last_requested.body ||
                value.epoch.time <= last_requested.time || value.epoch.seconds != last_requested.seconds))) return false;
        if (pending && (value.epoch != target_of(command.epoch) || (value.impulse_id && value.impulse_id <= command.impulse_id))) return false;
        if (!assigned) assigned = sequence + 1;
        if (assigned <= sequence) return false;
        const auto row = (head + count) % receipts.size();
        if (pending) {
            next_command = value;
            next_deadline = now + command_lifetime_ms;
            next_inbox = row;
            next_pending = true;
        } else {
            command = value;
            deadline = now + command_lifetime_ms;
            inbox = row;
            pending = true;
        }
        last_submitted = value.epoch.serial;
        last_requested = value.epoch;
        sequence = assigned;
        ++count;
        auto& record = receipts[row];
        record = {};
        record.outcome = Outcome::queued;
        record.sequence = sequence;
        record.requested = value.epoch;
        return true;
    }
    // A command is produced only by a call that reads the step after its own epoch. When the
    // first pending command ends here without being produced, the second is judged at once by
    // the same rule against the same clock: it is never expired for the first's missed step, and
    // never produced at any reading but its own target.
    // The reading is taken by value. advance() rewrites command, and a caller may have read the
    // epoch it passes from command itself; by reference the reading would then change under the
    // loop to the second command's own epoch, and the second would be judged against that.
    bool consume(Epoch current, std::uint64_t now, Command& out) noexcept {
        while (pending && !failed) {
            auto& record = receipts[inbox];
            const Epoch target = target_of(command.epoch);
            if (now >= deadline || current.serial > target.serial) record.outcome = Outcome::expired;
            else if (current == command.epoch) return false;
            else if (current != target) record.outcome = Outcome::rejected;
            else {
                out = command;
                pending = false;
                producing = true;
                last_serial = command.epoch.serial;
                record.produced = current;
                if (command.impulse_id) last_impulse = command.impulse_id;
                return true;
            }
            pending = false;
            advance();
        }
        return false;
    }
    // Deadlines alone, for a caller with no clock reading to offer: each pending command
    // expires by its own. Such a caller must not hand consume the pending command's own epoch
    // in place of a reading: consume would take it for the clock, and past the first command's
    // deadline it would reject the second for not being at its epoch or its step.
    void expire(std::uint64_t now) noexcept {
        while (pending && !failed && now >= deadline) {
            pending = false;
            receipts[inbox].outcome = Outcome::expired;
            advance();
        }
    }
    bool finish(bool force_attempted, bool torque_attempted, bool complete) noexcept {
        if (!producing) return false;
        producing = false;
        auto& record = receipts[inbox];
        record.force_attempted = force_attempted;
        record.torque_attempted = torque_attempted;
        record.outcome = complete ? Outcome::applied : force_attempted || torque_attempted ? Outcome::partial : Outcome::rejected;
        if (!complete) { failed = true; cancel(); }
        else advance();
        return true;
    }
    void cancel() noexcept {
        failed = true;
        if (pending) { pending = false; receipts[inbox].outcome = Outcome::rejected; }
        if (next_pending) { next_pending = false; receipts[next_inbox].outcome = Outcome::rejected; }
    }
    bool observe_successor(const Sample& value) noexcept {
        bool attached{};
        for (std::size_t i = 0; i < count; ++i) {
            auto& record = receipts[(head + i) % receipts.size()];
            const auto& produced = record.produced;
            if (record.outcome != Outcome::applied || record.successor_available || value.epoch.world != produced.world ||
                value.epoch.body != produced.body || value.epoch.serial <= produced.serial || !std::isfinite(value.epoch.time) ||
                value.epoch.time <= produced.time || !calibrated(value.epoch.seconds)) continue;
            record.successor = value;
            record.successor_available = true;
            attached = true;
        }
        return attached;
    }
    bool acknowledge(std::uint64_t value) noexcept {
        if (!value || !count) return false;
        const auto& record = receipts[head];
        if (record.sequence != value || record.outcome == Outcome::none || record.outcome == Outcome::queued ||
            (record.outcome == Outcome::applied && !record.successor_available)) return false;
        receipts[head] = {};
        head = (head + 1) % receipts.size();
        --count;
        return true;
    }
};
} // namespace dingosdk::owned_body
