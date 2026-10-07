#include "delta_codec.h"
#include "pose_delta.h"
#include "block_codec.h"
#include <algorithm>

namespace dingosdk::multiplayer {
namespace {
bool state_kind(PacketKind kind) {
    return kind == PacketKind::pose || kind == PacketKind::audio || kind == PacketKind::cosmetics;
}
bool magic(std::span<const std::uint8_t> b, const char *m) {
    return b.size() >= 4 && std::equal(b.begin(), b.begin() + 4, m);
}
void put(std::vector<std::uint8_t> &b, std::uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint64_t get(std::span<const std::uint8_t> b, std::size_t at, unsigned n) {
    std::uint64_t value{};
    for (unsigned i = 0; i < n; ++i)
        value |= std::uint64_t{b[at + i]} << (8 * i);
    return value;
}
} // namespace
WireUpdate DeltaSender::prepare(const Packet &p) const {
    const auto raw = encode(p, true);
    return prepare(p, raw, encode_wire_bytes(raw));
}
WireUpdate DeltaSender::prepare(const Packet &p, std::span<const std::uint8_t> raw,
                                std::span<const std::uint8_t> wire) const {
    return build(p, raw, wire, nullptr);
}
WireUpdate DeltaSender::prepare(const Packet &p, std::span<const std::uint8_t> raw, std::span<const std::uint8_t> wire,
                                DeltaCache &cache) const {
    return build(p, raw, wire, &cache);
}
WireUpdate DeltaSender::build(const Packet &p, std::span<const std::uint8_t> raw, std::span<const std::uint8_t> wire,
                              DeltaCache *cache) const {
    WireUpdate out;
    if (!state_kind(p.kind)) {
        out.bytes.assign(wire.begin(), wire.end());
        return out;
    }
    const auto found = bases_.find({p.source, p.kind});
    if (found == bases_.end() || found->second.epoch != p.epoch || found->second.world != p.world || p.time_us < found->second.time ||
        (p.kind != PacketKind::cosmetics &&
         (aligned_ ? p.time_us / refresh_us_ != found->second.time / refresh_us_ : p.time_us - found->second.time >= refresh_us_))) {
        if (wire.size() + 4 > max_packet)
            return out;
        out.bytes.assign(wire.begin(), wire.end());
        out.bytes.insert(out.bytes.begin(), {'R', 'M', 'B', '1'});
        out.baseline.assign(raw.begin(), raw.end());
        return out;
    }
    const auto &base = found->second;
    if (cache)
        for (const auto &entry : cache->entries)
            if (entry.sequence == base.sequence && *entry.reference == base.raw) {
                out.bytes = entry.bytes;
                return out;
            }
    std::size_t best = wire.size();
    if (p.kind == PacketKind::pose) {
        // Pose patches encode changed fields directly. Avoid also building and
        // compressing a full XOR candidate for every destination peer.
        const auto patch = pose_delta::encode(raw, base.raw);
        if (!patch.empty()) {
            std::vector<std::uint8_t> sparse{'R', 'M', 'S', '1'};
            put(sparse, p.source, 8);
            put(sparse, p.epoch, 8);
            put(sparse, static_cast<unsigned>(p.kind), 2);
            put(sparse, base.sequence, 4);
            put(sparse, raw.size(), 4);
            put(sparse, patch.size(), 4);
            auto compressed = compress_block(patch);
            if (34 + compressed.bytes.size() < best) {
                if (compressed.codec == BlockCodec::zstd) sparse[3] = '2';
                sparse.insert(sparse.end(), compressed.bytes.begin(), compressed.bytes.end());
                out.bytes = std::move(sparse);
            }
        }
    } else {
        std::vector<std::uint8_t> difference(raw.begin(), raw.end());
        for (std::size_t i = 0; i < std::min(raw.size(), base.raw.size()); ++i)
            difference[i] ^= base.raw[i];
        std::vector<std::uint8_t> delta{'R', 'M', 'D', '1'};
        put(delta, p.source, 8);
        put(delta, p.epoch, 8);
        put(delta, static_cast<unsigned>(p.kind), 2);
        put(delta, base.sequence, 4);
        put(delta, raw.size(), 4);
        auto block = compress_block(difference);
        if (30 + block.bytes.size() < best) {
            if (block.codec == BlockCodec::zstd) delta[3] = '2';
            delta.insert(delta.end(), block.bytes.begin(), block.bytes.end());
            out.bytes = std::move(delta);
        }
    }
    if (out.bytes.empty())
        out.bytes.assign(wire.begin(), wire.end());
    if (cache)
        cache->entries.push_back({base.sequence, &base.raw, out.bytes});
    return out;
}
void DeltaSender::sent(const Packet &p, WireUpdate &&update) {
    const StreamKey key{p.source, p.kind};
    if (!update.establishes_baseline()) {
        if (auto it = bases_.find(key); it != bases_.end())
            it->second.touched = ++clock_;
        return;
    }
    // A connection has at most seven active remote sources plus its sender.
    // Evict old departed streams without allowing unbounded retained cosmetics.
    if (!bases_.contains(key) && bases_.size() >= max_players * 3) {
        const auto oldest = std::min_element(bases_.begin(), bases_.end(), [](const auto &a, const auto &b) {
            return a.second.touched < b.second.touched;
        });
        bases_.erase(oldest);
    }
    bases_[key] = {std::move(update.baseline), p.epoch, p.world, p.time_us, ++clock_, p.sequence};
}
bool DeltaReceiver::holds(std::uint64_t source, PacketKind kind, std::uint64_t epoch,
                          std::uint32_t sequence) const noexcept {
    const auto stream = streams_.find({source, kind});
    return stream != streams_.end() &&
           std::any_of(stream->second.bases.begin(), stream->second.bases.end(),
                       [&](const auto &b) { return b.epoch == epoch && b.sequence == sequence; });
}
std::optional<Packet> DeltaReceiver::receive(std::span<const std::uint8_t> bytes, bool &missing,
                                           std::uint64_t accepted_world) noexcept {
    missing = false;
    try {
        if (bytes.size() > max_packet)
            return {};
        if (magic(bytes, "RMB1")) {
            const auto raw = decode_wire_bytes(bytes.subspan(4));
            auto p = raw ? decode(*raw) : std::nullopt;
            if (!p || !state_kind(p->kind))
                return {};
            // Late snapshots must not evict references for the current world.
            if (accepted_world && p->world != accepted_world)
                return p;
            const StreamKey key{p->source, p->kind};
            if (!streams_.contains(key) && streams_.size() >= max_players * 3) {
                const auto oldest =
                    std::min_element(streams_.begin(), streams_.end(), [](const auto &a, const auto &b) {
                        return a.second.touched < b.second.touched;
                    });
                streams_.erase(oldest);
            }
            auto &stream = streams_[key];
            stream.touched = ++clock_;
            // Re-encoding a decoded quaternion can change its last quantized bit.
            // Cache the exact packed bytes, never regenerated native transforms.
            if (stream.bases.empty() || stream.bases.back().epoch != p->epoch)
                stream.bases.clear();
            if (std::none_of(stream.bases.begin(), stream.bases.end(),
                             [&](const auto &b) { return b.sequence == p->sequence; }))
                stream.bases.push_back({*raw, p->epoch, p->world, p->session, p->map, p->sequence});
            while (stream.bases.size() > 3)
                stream.bases.pop_front();
            return p;
        }
        const bool sparse = magic(bytes, "RMS1") || magic(bytes, "RMS2");
        if (!sparse && !magic(bytes, "RMD1") && !magic(bytes, "RMD2"))
            return decode_wire(bytes);
        if (bytes.size() <= (sparse ? 34U : 30U))
            return {};
        const StreamKey key{get(bytes, 4, 8), static_cast<PacketKind>(get(bytes, 20, 2))};
        const auto epoch = get(bytes, 12, 8), sequence = get(bytes, 22, 4), size = get(bytes, 26, 4);
        if (!state_kind(key.second) || !epoch || size < packet_header_size || size > max_packet || bytes.size() >= size)
            return {};
        const auto stream = streams_.find(key);
        if (stream == streams_.end()) {
            missing = true;
            return {};
        }
        const auto base =
            std::find_if(stream->second.bases.begin(), stream->second.bases.end(),
                         [&](const auto &b) { return b.epoch == epoch && b.sequence == sequence; });
        if (base == stream->second.bases.end()) {
            missing = true;
            return {};
        }
        const auto header = sparse ? 34U : 30U;
        const auto unpacked = sparse ? get(bytes, 30, 4) : size;
        if (sparse && (key.second != PacketKind::pose || unpacked < packet_header_size + 6 ||
                       unpacked > max_packet + 1024)) return {};
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(unpacked));
        if (!decompress_block(bytes.subspan(header), raw,
            bytes[3] == '2' ? BlockCodec::zstd : BlockCodec::lz4)) return {};
        if (sparse)
            raw = pose_delta::decode(raw, base->raw, static_cast<std::size_t>(size));
        else
            for (std::size_t i = 0; i < std::min(raw.size(), base->raw.size()); ++i)
                raw[i] ^= base->raw[i];
        auto p = decode(raw);
        if (!p || p->source != key.first || p->kind != key.second || p->epoch != epoch ||
            p->world != base->world || p->session != base->session || p->map != base->map)
            return {};
        stream->second.touched = ++clock_;
        return p;
    } catch (...) {
        return {};
    }
}
} // namespace dingosdk::multiplayer
