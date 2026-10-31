#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Values crossing the client/physics boundary. No native address is part of this contract.
namespace dingosdk::owned_body {
using Vec = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // Hamilton XYZW
inline constexpr std::size_t maximum_capsules = 8, retained_capacity = 8, receipt_capacity = 4;
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
struct Mailbox {
    Command command{};
    std::array<Receipt, receipt_capacity> receipts{};
    std::size_t head{}, count{}, inbox{};
    std::uint64_t deadline{}, last_serial{}, last_submitted{}, sequence{};
    std::uint32_t last_impulse{};
    Epoch last_requested{};
    bool pending{}, producing{}, failed{};

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
    bool submit(const Command& value, const Epoch& observed, std::uint64_t now, std::uint64_t assigned = 0) noexcept {
        if (failed || pending || producing || count == receipts.size() || !valid(value) || value.epoch != observed ||
            value.epoch.serial <= last_submitted || (value.impulse_id && value.impulse_id <= last_impulse) ||
            sequence == std::numeric_limits<std::uint64_t>::max() || now > std::numeric_limits<std::uint64_t>::max() - command_lifetime_ms ||
            (last_submitted && (value.epoch.world != last_requested.world || value.epoch.body != last_requested.body ||
                value.epoch.time <= last_requested.time || value.epoch.seconds != last_requested.seconds))) return false;
        if (!assigned) assigned = sequence + 1;
        if (assigned <= sequence) return false;
        command = value;
        deadline = now + command_lifetime_ms;
        pending = true;
        last_submitted = value.epoch.serial;
        last_requested = value.epoch;
        sequence = assigned;
        inbox = (head + count) % receipts.size();
        ++count;
        auto& record = receipts[inbox];
        record = {};
        record.outcome = Outcome::queued;
        record.sequence = sequence;
        record.requested = value.epoch;
        return true;
    }
    bool consume(const Epoch& current, std::uint64_t now, Command& out) noexcept {
        if (!pending || failed) return false;
        auto& record = receipts[inbox];
        Epoch target = command.epoch;
        ++target.serial;
        target.time += target.seconds;
        if (now >= deadline || current.serial > target.serial) {
            pending = false;
            record.outcome = Outcome::expired;
            return false;
        }
        if (current == command.epoch) return false;
        if (current != target) { pending = false; record.outcome = Outcome::rejected; return false; }
        out = command;
        pending = false;
        producing = true;
        last_serial = command.epoch.serial;
        record.produced = current;
        if (command.impulse_id) last_impulse = command.impulse_id;
        return true;
    }
    bool finish(bool force_attempted, bool torque_attempted, bool complete) noexcept {
        if (!producing) return false;
        producing = false;
        auto& record = receipts[inbox];
        record.force_attempted = force_attempted;
        record.torque_attempted = torque_attempted;
        record.outcome = complete ? Outcome::applied : force_attempted || torque_attempted ? Outcome::partial : Outcome::rejected;
        if (!complete) { failed = true; cancel(); }
        return true;
    }
    void cancel() noexcept {
        failed = true;
        if (pending) { pending = false; receipts[inbox].outcome = Outcome::rejected; }
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
