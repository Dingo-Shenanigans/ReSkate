#pragma once
#include "pose_codec.h"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Poses from a dedicated server to a player: several players' poses in one message, each as its
// differences (pose_codec.h) from a pose of that player the receiver has said it holds.
//
// A message is a header and entries. A whole pose opens a stream: it names the player and gives
// the stream a number, and every later entry is that number, which earlier pose it builds on, and
// the differences. The receiver answers with which messages it could read in full (an ack: the
// newest and a bit for each of the 64 before it), and the server builds only on poses in messages
// that were acked, or on the whole pose it sent reliably. Messages are kept to one packet, so a
// lost packet loses a handful of poses, each of which the next one makes good.
namespace dingosdk::multiplayer::pose_batch {
inline constexpr std::array<std::uint8_t, 4> batch_magic{'R', 'M', 'Q', '1'}, ack_magic{'R', 'M', 'A', '1'};
inline constexpr std::size_t header_size = 16, ack_size = 16;
// Entries are added to a message while it stays under this, which with Steam's own framing fits a packet.
inline constexpr std::size_t message_room = 1100;
// How many poses a receiver keeps of each stream to build on, and how many a server may send
// past the newest one acked before it sends the stream whole again.
inline constexpr std::size_t receiver_keeps = 32, sender_runs = 24;

inline bool is_batch(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= header_size && std::equal(batch_magic.begin(), batch_magic.end(), bytes.begin());
}
inline bool is_ack(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() == ack_size && std::equal(ack_magic.begin(), ack_magic.end(), bytes.begin());
}

namespace detail {
inline void put(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
inline void varint(std::vector<std::uint8_t> &out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::uint64_t get(unsigned count) {
        if (at > bytes.size() || count > bytes.size() - at) throw std::runtime_error("Truncated pose batch");
        std::uint64_t value{};
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t{bytes[at++]} << (8 * i);
        return value;
    }
    std::uint64_t varint() {
        std::uint64_t value{};
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const auto byte = get(1);
            value |= (byte & 0x7f) << shift;
            if (!(byte & 0x80)) return value;
        }
        throw std::runtime_error("Invalid pose batch number");
    }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (at > bytes.size() || count > bytes.size() - at) throw std::runtime_error("Truncated pose batch");
        const auto part = bytes.subspan(at, count);
        at += count;
        return part;
    }
};
constexpr std::uint64_t zigzag(std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}
constexpr std::int64_t unzigzag(std::uint64_t value) noexcept { return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1); }
} // namespace detail

// How often the pose's player is being sent, which the receiver plays it back by: at the
// session's own rate, or every 100 or 200 ms.
enum class Rate : std::uint8_t { full, half, low };

struct Entry {
    std::uint16_t stream{};
    bool whole{}, collision{};
    Rate rate{Rate::full};
    std::uint64_t source{}, epoch{}; // whole only: who the stream is
    std::uint32_t sequence{};        // whole only: the pose's own number
    std::uint16_t reference{};       // difference only: the low bits of the number of the pose it builds on
    std::uint32_t ahead{};           // difference only: how far past that pose's number its own is (1 or more)
    std::uint64_t time_us{};         // whole: the pose's time. Difference: left 0, see time_step
    std::int64_t time_step{};        // difference only: its time less its reference's
    std::span<const std::uint8_t> pose; // pose_codec bytes
};

// The first 16 bytes: which message this is, and the world and map it belongs to (their low bits:
// enough to tell a message from before a map change).
inline std::vector<std::uint8_t> begin(std::uint32_t number, std::uint64_t world, std::uint64_t map) {
    std::vector<std::uint8_t> out(batch_magic.begin(), batch_magic.end());
    out.reserve(message_room + 64);
    detail::put(out, number, 4);
    detail::put(out, world & 0xffffffff, 4);
    detail::put(out, map & 0xffffffff, 4);
    return out;
}
inline void add(std::vector<std::uint8_t> &out, const Entry &entry) {
    detail::varint(out, entry.stream);
    out.push_back(static_cast<std::uint8_t>((entry.whole ? 1U : 0U) | (entry.collision ? 2U : 0U) | (static_cast<unsigned>(entry.rate) << 2)));
    if (entry.whole) {
        detail::put(out, entry.source, 8);
        detail::put(out, entry.epoch, 8);
        detail::put(out, entry.sequence, 4);
        detail::put(out, entry.time_us, 8);
    } else {
        detail::put(out, entry.reference, 2);
        detail::varint(out, entry.ahead);
        detail::varint(out, detail::zigzag(entry.time_step));
    }
    detail::varint(out, entry.pose.size());
    out.insert(out.end(), entry.pose.begin(), entry.pose.end());
}
// The room an entry takes beyond its pose bytes, at most.
inline constexpr std::size_t entry_room = 3 + 1 + 28 + 3;

struct Batch {
    std::uint32_t number{}, world{}, map{};
    std::vector<Entry> entries; // their pose bytes point into the message read
};
// A message's header and entries, or nothing when it is not one or is cut short.
inline std::optional<Batch> read(std::span<const std::uint8_t> bytes) noexcept {
    try {
        if (!is_batch(bytes)) return {};
        detail::Reader r{bytes, 4};
        Batch batch;
        batch.number = static_cast<std::uint32_t>(r.get(4));
        batch.world = static_cast<std::uint32_t>(r.get(4));
        batch.map = static_cast<std::uint32_t>(r.get(4));
        while (r.at < bytes.size()) {
            Entry entry;
            const auto stream = r.varint();
            if (stream > 0xffff) return {};
            entry.stream = static_cast<std::uint16_t>(stream);
            const auto flags = r.get(1);
            if (flags & ~0xfULL || ((flags >> 2) & 3) > 2) return {};
            entry.whole = flags & 1;
            entry.collision = flags & 2;
            entry.rate = static_cast<Rate>((flags >> 2) & 3);
            if (entry.whole) {
                entry.source = r.get(8);
                entry.epoch = r.get(8);
                entry.sequence = static_cast<std::uint32_t>(r.get(4));
                entry.time_us = r.get(8);
            } else {
                entry.reference = static_cast<std::uint16_t>(r.get(2));
                const auto ahead = r.varint();
                if (!ahead || ahead > 0xffffff) return {};
                entry.ahead = static_cast<std::uint32_t>(ahead);
                entry.time_step = detail::unzigzag(r.varint());
            }
            const auto length = r.varint();
            if (length > bytes.size()) return {};
            entry.pose = r.take(static_cast<std::size_t>(length));
            batch.entries.push_back(entry);
            if (batch.entries.size() > 512) return {};
        }
        return batch;
    } catch (...) {
        return {};
    }
}

// Which messages a receiver read in full: the newest, and a bit for each of the 64 before it
// (bit 0 the one just before).
struct Ack {
    std::uint32_t newest{};
    std::uint64_t earlier{};
    bool any{};
    void note(std::uint32_t number) noexcept {
        if (!any) {
            any = true;
            newest = number;
            earlier = 0;
        } else if (number > newest) {
            const auto step = number - newest;
            earlier = step >= 64 ? 0 : earlier << step;
            if (step <= 64) earlier |= std::uint64_t{1} << (step - 1); // the one that was newest
            newest = number;
        } else if (number < newest && newest - number <= 64) {
            earlier |= std::uint64_t{1} << (newest - number - 1);
        }
    }
    bool holds(std::uint32_t number) const noexcept {
        return any && (number == newest || (number < newest && newest - number <= 64 && (earlier >> (newest - number - 1)) & 1));
    }
    std::array<std::uint8_t, ack_size> bytes() const noexcept {
        std::array<std::uint8_t, ack_size> out{};
        std::copy(ack_magic.begin(), ack_magic.end(), out.begin());
        for (unsigned i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(newest >> (8 * i));
        for (unsigned i = 0; i < 8; ++i) out[8 + i] = static_cast<std::uint8_t>(earlier >> (8 * i));
        return out;
    }
    static std::optional<Ack> read(std::span<const std::uint8_t> bytes) noexcept {
        if (!is_ack(bytes)) return {};
        Ack ack;
        ack.any = true;
        for (unsigned i = 0; i < 4; ++i) ack.newest |= std::uint32_t{bytes[4 + i]} << (8 * i);
        for (unsigned i = 0; i < 8; ++i) ack.earlier |= std::uint64_t{bytes[8 + i]} << (8 * i);
        return ack;
    }
};

// A receiver's side of one stream: who it is, and the last poses of theirs it rebuilt.
struct Stream {
    std::uint64_t source{}, epoch{};
    struct Held {
        std::uint32_t sequence{};
        std::uint64_t time_us{};
        pose_codec::QuantPose pose;
    };
    std::deque<Held> held;
    const Held *find_low(std::uint16_t low) const noexcept {
        for (auto it = held.rbegin(); it != held.rend(); ++it)
            if (static_cast<std::uint16_t>(it->sequence) == low) return &*it;
        return nullptr;
    }
    bool holds(std::uint32_t sequence) const noexcept {
        for (const auto &pose : held)
            if (pose.sequence == sequence) return true;
        return false;
    }
    void keep(Held pose) {
        held.push_back(std::move(pose));
        while (held.size() > receiver_keeps) held.pop_front();
    }
};
// What one entry rebuilds to on the receiver, or nothing when it cannot be (a stream it was never
// told of, a reference it does not hold, bytes that are not a pose). `streams` is updated: a whole
// pose opens or reopens its stream, and every rebuilt pose is kept to build on.
struct Rebuilt {
    std::uint64_t source{}, epoch{}, time_us{};
    std::uint32_t sequence{};
    bool collision{};
    Rate rate{Rate::full};
    pose_codec::QuantPose pose;
    bool repeat{}; // this stream's pose of that number was already held: nothing new
};
template <typename Streams> std::optional<Rebuilt> rebuild(Streams &streams, const Entry &entry) {
    Rebuilt out;
    out.collision = entry.collision;
    out.rate = entry.rate;
    if (entry.whole) {
        if (!pose_codec::whole(entry.pose)) return {};
        auto pose = pose_codec::decode(entry.pose, nullptr);
        if (!pose) return {};
        auto &stream = streams[entry.stream];
        if (stream.source != entry.source || stream.epoch != entry.epoch) stream = {entry.source, entry.epoch, {}};
        out.source = entry.source;
        out.epoch = entry.epoch;
        out.sequence = entry.sequence;
        out.time_us = entry.time_us;
        out.repeat = stream.holds(entry.sequence);
        out.pose = std::move(*pose);
        if (!out.repeat) stream.keep({out.sequence, out.time_us, out.pose});
        return out;
    }
    const auto found = streams.find(entry.stream);
    if (found == streams.end() || pose_codec::whole(entry.pose)) return {};
    auto &stream = found->second;
    const auto *reference = stream.find_low(entry.reference);
    if (!reference) return {};
    auto pose = pose_codec::decode(entry.pose, &reference->pose);
    if (!pose) return {};
    out.source = stream.source;
    out.epoch = stream.epoch;
    out.sequence = reference->sequence + entry.ahead;
    out.time_us = static_cast<std::uint64_t>(static_cast<std::int64_t>(reference->time_us) + entry.time_step);
    out.repeat = stream.holds(out.sequence);
    out.pose = std::move(*pose);
    if (!out.repeat) stream.keep({out.sequence, out.time_us, out.pose});
    return out;
}

// A server's side of what one player is sent: for each other player a stream, the newest pose of
// it the receiver acked, and the messages sent and not yet acked. Poses are handed to add() as
// they fall due; it packs them into messages and gives each to `emit(bytes, reliable)`, which
// answers whether it went out. `find(sequence)` gives a kept pose of the player being added, or
// nothing when it is no longer kept.
struct KeptView {
    std::uint32_t sequence{};
    std::uint64_t time_us{};
    const pose_codec::QuantPose *pose{};
};
class Sender {
  public:
    // What add() did with a pose, for counting.
    enum class Did : std::uint8_t { nothing, difference, whole };
    struct Added {
        Did did{Did::nothing};
        std::size_t bytes{};
    };
    void begin(std::uint64_t world, std::uint64_t map) {
        world_ = world;
        map_ = map;
        message_.clear();
        batch_ = {};
    }
    template <typename Find, typename Emit>
    Added add(std::uint64_t source, std::uint64_t epoch, const KeptView &pose, Find &&find, bool hold_fingers, bool collision, Rate rate,
              std::uint64_t now, Emit &&emit) {
        using pose_codec::Fingers;
        auto &out = streams_[source];
        if (!out.stream || out.epoch != epoch) {
            out = {};
            out.epoch = epoch;
            out.stream = next_stream_++;
            if (!next_stream_) next_stream_ = 1;
        }
        // The pose to build on: the newest they acked, else the whole one sent reliably (it will
        // arrive; a difference that overtakes it is only dropped). Neither when too many have
        // gone since for the receiver to still hold it, or it is no longer kept here.
        std::optional<KeptView> reference;
        if (out.acked && out.sends - out.acked_send <= sender_runs) reference = find(out.acked_sequence);
        if (!reference && out.whole_sent && out.sends - out.whole_send <= sender_runs) reference = find(out.whole_sequence);
        if (reference && pose.sequence <= reference->sequence) return {}; // nothing newer than they hold
        Entry entry;
        entry.stream = out.stream;
        entry.collision = collision;
        entry.rate = rate;
        std::vector<std::uint8_t> bytes;
        bool fingers{};
        if (reference) {
            // Fingers: not sent to a player too far to see them; and after that, sent whole until
            // an ack shows the receiver has them again, since its copy of the reference has old
            // ones in it.
            const auto mode = hold_fingers ? Fingers::held : out.fingers_stale ? Fingers::whole : Fingers::sent;
            bytes = pose_codec::encode_delta(*pose.pose, *reference->pose, mode);
            fingers = mode == Fingers::whole;
            if (bytes.empty()) reference.reset(); // another skeleton (a changed rig): whole
        }
        if (!reference) {
            // The whole pose, in a message of its own, reliably. Not again while the last is still
            // on its way: at most one a second for a stream.
            if (out.whole_sent && now - out.whole_at < 1000000) return {};
            flush(emit);
            open();
            bytes = pose_codec::encode_whole(*pose.pose);
            entry.whole = true;
            entry.source = source;
            entry.epoch = epoch;
            entry.sequence = pose.sequence;
            entry.time_us = pose.time_us;
            entry.pose = bytes;
            pose_batch::add(message_, entry);
            out.acked = false;
            out.whole_sent = true;
            out.whole_sequence = pose.sequence;
            out.whole_send = ++out.sends;
            out.whole_at = now;
            out.fingers_stale = false;
            batch_.poses.push_back({source, epoch, pose.sequence, out.sends, true});
            send(emit, true);
            return {Did::whole, bytes.size()};
        }
        entry.reference = static_cast<std::uint16_t>(reference->sequence);
        entry.ahead = pose.sequence - reference->sequence;
        entry.time_step = static_cast<std::int64_t>(pose.time_us) - static_cast<std::int64_t>(reference->time_us);
        entry.pose = bytes;
        if (message_.empty()) open();
        if (!batch_.poses.empty() && message_.size() + bytes.size() + entry_room > message_room) {
            send(emit, false);
            open();
        }
        pose_batch::add(message_, entry);
        if (hold_fingers) {
            out.fingers_stale = true;
            out.last_held_batch = batch_.number;
        }
        batch_.poses.push_back({source, epoch, pose.sequence, ++out.sends, fingers});
        return {Did::difference, bytes.size()};
    }
    // Sends what add() has packed so far.
    template <typename Emit> void flush(Emit &&emit) { send(emit, false); }
    // The receiver said which messages it read in full: the poses in those are ones it holds.
    void ack(const Ack &ack) {
        for (auto it = sent_.begin(); it != sent_.end();) {
            if (!ack.holds(it->number)) {
                ++it;
                continue;
            }
            for (const auto &pose : it->poses) {
                const auto found = streams_.find(pose.source);
                if (found == streams_.end() || found->second.epoch != pose.epoch) continue;
                auto &out = found->second;
                if (!out.acked || pose.sequence > out.acked_sequence) {
                    out.acked = true;
                    out.acked_sequence = pose.sequence;
                    out.acked_send = pose.send;
                }
                // (Not when a pose without fingers went out after this one.)
                if (pose.fingers && it->number > out.last_held_batch) out.fingers_stale = false;
            }
            it = sent_.erase(it);
        }
        while (!sent_.empty() && sent_.front().number + 128 < ack.newest) sent_.pop_front();
    }
    // A new world: every stream starts over. The messages' numbers go on, so the receiver's
    // acks need no starting over with them.
    void restart() {
        streams_.clear();
        sent_.clear();
        message_.clear();
        batch_ = {};
    }
    // The player left: their stream is nobody's.
    void forget(std::uint64_t source) { streams_.erase(source); }
    std::size_t unacked() const noexcept { return sent_.size(); }

  private:
    struct Out {
        std::uint64_t epoch{};
        std::uint16_t stream{};
        bool acked{}, whole_sent{}, fingers_stale{};
        std::uint32_t acked_sequence{}, whole_sequence{}, sends{}, acked_send{}, whole_send{}, last_held_batch{};
        std::uint64_t whole_at{};
    };
    struct SentPose {
        std::uint64_t source{}, epoch{};
        std::uint32_t sequence{}, send{};
        bool fingers{}; // it carried the fingers whole
    };
    struct SentBatch {
        std::uint32_t number{};
        std::vector<SentPose> poses;
    };
    void open() {
        batch_ = {next_batch_++, {}};
        message_ = pose_batch::begin(batch_.number, world_, map_);
    }
    // Unreliable messages are let go when the connection is behind (the next poses replace
    // them); nothing is then recorded as sent, so nothing is built on them either.
    template <typename Emit> void send(Emit &&emit, bool reliable) {
        if (batch_.poses.empty()) return;
        if (emit(std::span<const std::uint8_t>(message_), reliable)) {
            sent_.push_back(std::move(batch_));
            while (sent_.size() > 256) sent_.pop_front();
        }
        batch_ = {};
        message_.clear();
    }
    std::map<std::uint64_t, Out> streams_;
    std::deque<SentBatch> sent_;
    std::vector<std::uint8_t> message_;
    SentBatch batch_;
    std::uint64_t world_{}, map_{};
    std::uint16_t next_stream_ = 1;
    std::uint32_t next_batch_ = 1;
};
} // namespace dingosdk::multiplayer::pose_batch
