#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>

namespace dingosdk::multiplayer {
using MemberSlots = std::array<Member, max_remote_players>;

// Preserve slot identity for surviving players, including across joins/leaves.
// Callers retire native resources whenever a slot's id or epoch changes.
inline MemberSlots roster_slots(const MemberSlots &previous, std::span<const Member> members,
                                std::uint64_t local) {
    MemberSlots next{};
    for (std::size_t i = 0; i < previous.size(); ++i)
        for (const auto &m : members)
            if (m.id != local && m.id == previous[i].id)
                next[i] = m;
    for (const auto &m : members) {
        if (m.id == local ||
            std::any_of(next.begin(), next.end(), [&](const auto &v) { return v.id == m.id; }))
            continue;
        const auto free = std::find_if(next.begin(), next.end(), [](const auto &v) { return !v.id; });
        if (free == next.end())
            throw std::runtime_error("Session roster exceeds capacity.");
        *free = m;
    }
    return next;
}
inline bool trusted_roster(const Packet &p, std::uint64_t host, std::uint64_t host_epoch, std::uint64_t local,
                           std::uint64_t local_epoch) noexcept {
    if (p.kind != PacketKind::roster || p.source != host || p.epoch != host_epoch ||
        !valid_roster(p.members, p.capacity) || !p.distances.valid() || !valid_multiplayer_tps(p.tps))
        return false;
    bool found_host{}, found_local{};
    for (const auto &m : p.members) {
        if (m.id == host)
            found_host = m.epoch == host_epoch;
        if (m.id == local)
            found_local = m.epoch == local_epoch;
    }
    return found_host && found_local;
}
inline bool routed_source(const Packet &p, const Member &member, std::uint64_t connection, bool hosting,
                          std::uint64_t host, bool direct = false) noexcept {
    // Only the host may forward another Steam identity. A guest never chooses
    // its authority by putting a different sender ID in a packet.
    return member.id && member.epoch && p.source == member.id && p.epoch == member.epoch &&
           (hosting || direct ? connection == member.id : connection == host);
}
// A route report is a short lease: missing reports or a source reconnect restore relay.
inline bool needs_relay(std::span<const Member> routes, std::uint64_t reported, const Member &source,
                        std::uint64_t now) noexcept {
    return !reported || now < reported || now - reported > 1500000 ||
           std::none_of(routes.begin(), routes.end(),
                        [&](const auto &m) { return m.id == source.id && m.epoch == source.epoch; });
}
inline bool dial_peer(std::uint64_t local, std::uint64_t remote, std::uint64_t host) noexcept {
    return local && remote && remote != host && local != host && local < remote;
}
// Recent world positions only; uncertainty returns to full rate. Separate
// enter/exit thresholds avoid oscillating at a distance boundary.
inline std::uint32_t pose_interval(float distance_squared, std::uint32_t previous,
                                   const MultiplayerDistances &settings = {}, unsigned tps = 20) noexcept {
    const auto full = multiplayer_pose_interval(tps);
    if (!std::isfinite(distance_squared) || distance_squared < 0) return full;
    if (!settings.valid()) return full;
    const auto beyond = [&](int metres) { return distance_squared > static_cast<float>(metres * metres); };
    if (!beyond(settings.full_rate_return)) return full;
    if (beyond(settings.low_rate_start)) return 200000;
    if (previous == 200000 && beyond(settings.half_rate_return)) return 200000;
    if (beyond(settings.half_rate_start) || previous == 100000 || previous == 200000) return 100000;
    return full;
}
struct PoseDelivery {
    std::uint64_t source{}, epoch{}, last_sent{};
    std::uint32_t interval_us = 50000;
    std::uint64_t next_source_time{};
    std::uint32_t by_distance{}; // what the distance alone asked for, before any crowd limit
    std::uint8_t precision{};    // how finely the source's rotations are sent to them (a dedicated server's pose_precision)
};
// A crowd in one place: every player there is within full-rate distance of every other, and
// what one player is sent grows with the crowd until their connection cannot carry it. The
// default budget is what was seen to arrive through Steam's relays (about 1 MB/s a player,
// whatever the send rate): nobody near is slowed until about 20 players are in one place at
// 30 TPS; past that the farthest of them are. A
// player is sent at most `budget` poses a second: everyone at the low rate at least, the
// nearest at the full rate with two thirds of what is left, the next nearest at the half
// rate with the rest. These are the squared distances beyond which a player is sent at the
// half and at the low rate; `squared` (the distances to the others) is sorted here.
inline constexpr unsigned crowd_always_full = 8, crowd_always_half = 12;
struct CrowdLimits {
    float half = std::numeric_limits<float>::infinity(), low = std::numeric_limits<float>::infinity();
};
inline CrowdLimits crowd_limits(std::span<float> squared, unsigned tps, unsigned budget = crowd_pose_budget) {
    const unsigned full = 1000000U / multiplayer_pose_interval(tps), half = 10, low = 5;
    const auto count = static_cast<unsigned>(squared.size());
    if (count * full <= budget) return {};
    std::sort(squared.begin(), squared.end());
    const unsigned spare = budget > count * low ? budget - count * low : 0;
    // Whatever the budget, the nearest few are sent at the full rate and the next few at the
    // half rate: with enough players on, the low rate for everyone is the whole budget, and
    // without this nobody at all was left at the full rate (and so nobody was heard).
    const unsigned wanted_full = spare * 2 / 3 / (full - low);
    const unsigned at_full = std::min(count, std::max(wanted_full, crowd_always_full));
    const unsigned left = spare > at_full * (full - low) ? spare - at_full * (full - low) : 0;
    const unsigned at_half = std::min(count - at_full, std::max(left / (half - low), crowd_always_half));
    CrowdLimits limits;
    // Between two players the limit falls halfway, so that neither sits on it.
    const auto after = [&](unsigned sent) {
        return sent >= count ? std::numeric_limits<float>::infinity() : sent ? (squared[sent - 1] + squared[sent]) / 2 : -1.f;
    };
    limits.half = after(at_full);
    limits.low = after(at_full + at_half);
    return limits;
}
inline std::uint32_t crowd_interval(std::uint32_t interval, float distance_squared, const CrowdLimits &limits) noexcept {
    return distance_squared > limits.low ? 200000U : distance_squared > limits.half ? std::max(interval, 100000U) : interval;
}
// Preserve the timer phase across variable client frames/packet arrivals. Only
// the newest state is sent; elapsed slots are skipped without catch-up packets.
inline void advance_pose_deadline(std::uint64_t &next, std::uint64_t now, std::uint64_t interval) {
    next = now + interval - (next && now >= next ? (now - next) % interval : 0);
}
// Reduce optional direct uploads when a guest cannot sustain fan-out. The host
// remains its first destination, and other receivers restore relay through leases.
struct DirectUploadBudget {
    unsigned limit = max_remote_players - 1;
    std::uint64_t next_check{}, recover_at{}, skipped{};
    void update(std::uint64_t now, std::uint64_t queue_us, std::uint64_t skipped_total, unsigned active) {
        if (now < next_check)
            return;
        next_check = now + 1000000;
        const auto new_skips = skipped_total >= skipped ? skipped_total - skipped : 0;
        skipped = skipped_total;
        if (active && (queue_us > 50000 || new_skips > 5)) {
            limit = std::min(limit, active);
            limit /= 2;
            recover_at = now + 10000000;
        } else if (now >= recover_at && queue_us < 10000 && !new_skips) {
            limit = std::min<unsigned>(max_remote_players - 1, limit + 1);
            recover_at = now + 5000000;
        }
    }
};
// What one player's game sends of each relayed stream, with room to spare: a host passes on
// no more than this from any one source, whatever the connection's overall budget allows.
// Outfits: captured twice a second and sent when changed. Counted over five seconds.
inline constexpr unsigned outfit_burst = 12;
struct OutfitBudget {
    std::uint64_t since{};
    unsigned count{};
    bool accept(std::uint64_t now) noexcept {
        if (now < since || now - since >= 5000000) { since = now; count = 0; }
        return ++count <= outfit_burst;
    }
};
// Skater sound: at most one packet per network tick, a few samples each.
struct SoundBudget {
    std::uint64_t since{};
    std::size_t packets{}, samples{};
    bool accept(std::uint64_t now, std::size_t count) noexcept {
        if (now < since || now - since >= 1000000) { since = now; packets = samples = 0; }
        if (packets >= multiplayer_tick_rates.back() + 30U || samples + count > 400) return false;
        ++packets;
        samples += count;
        return true;
    }
};
// A connection that never finished joining (it timed out, or had the wrong password or code)
// may try again at once the first time. After that every failure makes its Steam ID wait
// longer before a host takes its connection again, so one account cannot hold a player slot
// or try passwords over and over.
struct JoinBackoff {
    struct Entry {
        std::uint64_t until{}, last{};
        unsigned failures{};
    };
    static constexpr std::uint64_t forget_us = 30ULL * 60 * 1000000, longest_us = 10ULL * 60 * 1000000;
    std::map<std::uint64_t, Entry> entries;
    // Notes a failed attempt and returns how many this ID has made lately.
    unsigned failed(std::uint64_t id, std::uint64_t now) {
        auto &entry = entries[id];
        if (entry.last && now >= entry.last && now - entry.last > forget_us) entry.failures = 0;
        ++entry.failures;
        entry.last = now;
        const auto wait = entry.failures < 2 ? 0ULL
                        : std::min<std::uint64_t>(longest_us, 5000000ULL << std::min(entry.failures - 2, 8U));
        entry.until = now + wait;
        return entry.failures;
    }
    bool waiting(std::uint64_t id, std::uint64_t now) const {
        const auto found = entries.find(id);
        return found != entries.end() && now < found->second.until;
    }
    void joined(std::uint64_t id) { entries.erase(id); }
    void prune(std::uint64_t now) {
        std::erase_if(entries, [&](const auto &entry) {
            return now >= entry.second.last && now - entry.second.last > forget_us;
        });
    }
};
// How much one peer may send in a second. `when` is when the packet arrived (the transport's
// TransportMessage::arrived), not when it is read: a receiver that could not read for a few
// seconds (a server whose console held it up, a game loading a map) reads everything that
// arrived meanwhile at once, and counted by reading time every peer looked like a flood and
// was dropped. Packets are not always read in arrival order (Steam's lanes are read one after
// another): one from before the second being counted is counted in it.
struct ReceiveBudget {
    std::uint64_t since{}, bytes{}, packets{};
    bool accept(std::uint64_t when, std::size_t size, unsigned sources = 1) {
        if (when > since && when - since >= 1000000) {
            since = when;
            bytes = packets = 0;
        }
        bytes += size;
        ++packets;
        return sources && sources <= max_remote_players && bytes <= 2ULL * 1024 * 1024 * sources &&
               packets <= (2ULL * multiplayer_tick_rates.back() + 80 + 32) * sources;
    }
};
} // namespace dingosdk::multiplayer
