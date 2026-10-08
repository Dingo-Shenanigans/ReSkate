#pragma once
#include "wire_codec.h"
#include <map>
#include <tuple>

namespace dingosdk::multiplayer {
using StreamKey = std::pair<std::uint64_t, PacketKind>;
struct WireUpdate {
    std::vector<std::uint8_t> bytes, baseline;
    bool establishes_baseline() const { return !baseline.empty(); }
};
// Reference-encoded updates one broadcast already built for one packet encoding. A delta
// depends only on that encoding and on the reference snapshot (its sequence and exact
// bytes), so a later recipient holding the same reference gets the same bytes without
// another patch and compression. References point into the senders: keep a cache only
// while no sender records a send (DeltaSender::sent).
struct DeltaCache {
    struct Entry {
        std::uint32_t sequence{};
        const std::vector<std::uint8_t> *reference{};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Entry> entries;
};
// Every delta references a reliable full snapshot, never the previous delta.
// Failed/skipped sends do not advance the sender's reference state.
class DeltaSender {
    struct Base {
        std::vector<std::uint8_t> raw;
        std::uint64_t epoch{}, world{}, time{}, touched{};
        std::uint32_t sequence{};
    };
    std::map<StreamKey, Base> bases_;
    std::uint64_t clock_{};
    std::uint64_t refresh_us_ = 2000000;
    bool aligned_{};
    WireUpdate build(const Packet &, std::span<const std::uint8_t> raw, std::span<const std::uint8_t> wire,
                     DeltaCache *cache) const;

  public:
    WireUpdate prepare(const Packet &) const;
    WireUpdate prepare(const Packet &, std::span<const std::uint8_t> raw,
                       std::span<const std::uint8_t> wire) const;
    // The same result, reusing (and adding to) deltas built for other recipients of
    // exactly this raw/wire encoding.
    WireUpdate prepare(const Packet &, std::span<const std::uint8_t> raw, std::span<const std::uint8_t> wire,
                       DeltaCache &cache) const;
    void sent(const Packet &, WireUpdate &&);
    // Drops a stream's reference: the next packet of it goes as a whole state.
    void forget(std::uint64_t source, PacketKind kind) { bases_.erase(StreamKey{source, kind}); }
    // How old a pose or sound reference may get before a whole state replaces it. Each one
    // is sent reliably, so Steam resends it until it arrives: a dedicated server, with a
    // stream for every pair of players, keeps them rarer than the two seconds a game does.
    // `aligned`: on the source's own clock, at each multiple of the time, instead of that
    // long after this recipient's last one. Every recipient of a source then takes the same
    // packet as its reference, and one patch serves them all (DeltaCache).
    void set_refresh(std::uint64_t microseconds, bool aligned = false) {
        refresh_us_ = microseconds;
        aligned_ = aligned;
    }
};
class DeltaReceiver {
    struct Base {
        std::vector<std::uint8_t> raw;
        std::uint64_t epoch{}, world{}, session{}, map{};
        std::uint32_t sequence{};
    };
    struct Stream {
        std::deque<Base> bases;
        std::uint64_t touched{};
    };
    std::map<StreamKey, Stream> streams_;
    std::uint64_t clock_{};

  public:
    // Missing references are a recoverable dropped frame, not protocol failure.
    std::optional<Packet> receive(std::span<const std::uint8_t>, bool &missing_reference,
                                  std::uint64_t accepted_world = 0) noexcept;
    // Whether a delta of this stream against baseline `sequence` of `epoch` could be decoded now.
    bool holds(std::uint64_t source, PacketKind kind, std::uint64_t epoch, std::uint32_t sequence) const noexcept;
};
} // namespace dingosdk::multiplayer
