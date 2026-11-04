#include "synchros_wire.h"
#include "sharing_rules.h"
#include <cstring>
#include <stdexcept>

namespace dingosdk::multiplayer::synchros {
namespace {
struct Writer {
    std::vector<std::uint8_t> out;
    void u8(std::uint8_t v) { out.push_back(v); }
    void u16(std::uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    template<std::size_t N> void raw(const std::array<std::uint8_t, N>& v) { out.insert(out.end(), v.begin(), v.end()); }
    void bytes(std::span<const std::uint8_t> v) { u32(static_cast<std::uint32_t>(v.size())); out.insert(out.end(), v.begin(), v.end()); }
    void text(const std::string& v) {
        if (v.size() > 0xffff) throw std::length_error("text too long");
        u16(static_cast<std::uint16_t>(v.size()));
        out.insert(out.end(), v.begin(), v.end());
    }
};
struct Reader {
    std::span<const std::uint8_t> in;
    std::size_t at{};
    bool ok{true};
    bool need(std::size_t n) { if (!ok || in.size() - at < n) ok = false; return ok; }
    std::uint8_t u8() { if (!need(1)) return 0; return in[at++]; }
    std::uint16_t u16() { if (!need(2)) return 0; std::uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= static_cast<std::uint16_t>(in[at++] << (8 * i)); return v; }
    std::uint32_t u32() { if (!need(4)) return 0; std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[at++]) << (8 * i); return v; }
    std::uint64_t u64() { if (!need(8)) return 0; std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[at++]) << (8 * i); return v; }
    template<std::size_t N> void raw(std::array<std::uint8_t, N>& v) { if (!need(N)) return; std::memcpy(v.data(), in.data() + at, N); at += N; }
    std::vector<std::uint8_t> bytes(std::size_t limit) {
        const auto n = u32();
        if (n > limit || !need(n)) { ok = false; return {}; }
        std::vector<std::uint8_t> v(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return v;
    }
    std::string text(std::size_t limit) {
        const auto n = u16();
        if (n > limit || !need(n)) { ok = false; return {}; }
        std::string v(reinterpret_cast<const char*>(in.data()) + at, n);
        at += n;
        // Names are printable ASCII paths; anything else is not a name the game has.
        for (const unsigned char c : v) if (c < 0x20 || c > 0x7e) ok = false;
        return v;
    }
    std::size_t count(std::size_t limit) {
        const auto n = u32();
        if (n > limit) ok = false;
        return ok ? n : 0;
    }
};

void write_file(Writer& w, const File& f) {
    w.raw(f.id);
    w.u32(f.size);
    w.u8(static_cast<std::uint8_t>(f.kind));
    w.text(f.name);
    w.text(f.bundle);
    w.text(f.superbundle);
    w.u64(f.original_size);
    w.u32(f.resource_type);
    w.u64(f.resource_id);
    w.bytes(f.resource_meta);
    w.raw(f.chunk_guid);
    w.u32(f.logical_offset);
    w.u32(f.logical_size);
}
File read_file(Reader& r) {
    File f;
    r.raw(f.id);
    f.size = r.u32();
    const auto kind = r.u8();
    if (kind > static_cast<std::uint8_t>(FileKind::toc_chunk)) r.ok = false;
    f.kind = static_cast<FileKind>(kind);
    f.name = r.text(max_name);
    f.bundle = r.text(max_name);
    f.superbundle = r.text(max_name);
    f.original_size = r.u64();
    f.resource_type = r.u32();
    f.resource_id = r.u64();
    f.resource_meta = r.bytes(max_resource_meta);
    r.raw(f.chunk_guid);
    f.logical_offset = r.u32();
    f.logical_size = r.u32();
    if (!f.size || f.size > max_file) r.ok = false;
    // What a kind needs: assets their names, bundled ones their bundle.
    if ((f.kind == FileKind::ebx || f.kind == FileKind::resource) && f.name.empty()) r.ok = false;
    if (f.kind != FileKind::toc_chunk && f.bundle.empty()) r.ok = false;
    if (f.superbundle.empty()) r.ok = false;
    return f;
}
} // namespace

std::vector<std::uint8_t> encode(const Message& message) {
    Writer w;
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, Hello>) {
            w.u8(static_cast<std::uint8_t>(Kind::hello)); w.u8(wire_version);
            w.u64(m.session);
            w.u32(static_cast<std::uint32_t>(m.proofs.size()));
            for (const auto& p : m.proofs) w.raw(p);
        } else if constexpr (std::is_same_v<T, Welcome>) {
            w.u8(static_cast<std::uint8_t>(Kind::welcome)); w.u8(wire_version);
            w.u8(m.allowed ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Want>) {
            w.u8(static_cast<std::uint8_t>(Kind::want)); w.u8(wire_version);
            w.u32(static_cast<std::uint32_t>(m.items.size()));
            for (const auto& item : m.items) w.text(item);
        } else if constexpr (std::is_same_v<T, Contents>) {
            w.u8(static_cast<std::uint8_t>(Kind::contents)); w.u8(wire_version);
            w.u32(static_cast<std::uint32_t>(m.items.size()));
            for (const auto& item : m.items) {
                w.text(item.item);
                w.u8(static_cast<std::uint8_t>(item.withheld));
                w.u32(static_cast<std::uint32_t>(item.files.size()));
                for (const auto& f : item.files) write_file(w, f);
            }
        } else if constexpr (std::is_same_v<T, Fetch>) {
            w.u8(static_cast<std::uint8_t>(Kind::fetch)); w.u8(wire_version);
            w.u32(static_cast<std::uint32_t>(m.files.size()));
            for (const auto& id : m.files) w.raw(id);
        } else if constexpr (std::is_same_v<T, Piece>) {
            w.u8(static_cast<std::uint8_t>(Kind::piece)); w.u8(wire_version);
            w.raw(m.id);
            w.u32(m.offset);
            w.u32(m.total);
            w.bytes(m.bytes);
        } else if constexpr (std::is_same_v<T, Refuse>) {
            w.u8(static_cast<std::uint8_t>(Kind::refuse)); w.u8(wire_version);
            w.text(m.reason.size() > max_reason ? m.reason.substr(0, max_reason) : m.reason);
        }
    }, message);
    if (w.out.size() > max_message) throw std::length_error("Skate Synchros message over Steam's limit");
    return w.out;
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_message) return std::nullopt;
    Reader r{bytes};
    const auto kind = r.u8();
    if (r.u8() != wire_version || !r.ok) return std::nullopt;
    Message out;
    switch (static_cast<Kind>(kind)) {
    case Kind::hello: {
        Hello m;
        m.session = r.u64();
        const auto n = r.count(max_entered_codes);
        for (std::size_t i = 0; i < n && r.ok; ++i) { Proof p{}; r.raw(p); m.proofs.push_back(p); }
        if (!m.session) r.ok = false;
        out = std::move(m);
        break;
    }
    case Kind::welcome: {
        const auto v = r.u8();
        if (v > 1) r.ok = false;
        out = Welcome{v == 1};
        break;
    }
    case Kind::want: {
        Want m;
        const auto n = r.count(max_items);
        for (std::size_t i = 0; i < n && r.ok; ++i) {
            auto item = r.text(max_name);
            if (item.empty()) r.ok = false;
            m.items.push_back(std::move(item));
        }
        if (m.items.empty()) r.ok = false;
        out = std::move(m);
        break;
    }
    case Kind::contents: {
        Contents m;
        const auto n = r.count(max_items);
        for (std::size_t i = 0; i < n && r.ok; ++i) {
            ItemContents item;
            item.item = r.text(max_name);
            const auto withheld = r.u8();
            if (withheld > static_cast<std::uint8_t>(Withheld::unreadable) || item.item.empty()) r.ok = false;
            item.withheld = static_cast<Withheld>(withheld);
            const auto files = r.count(max_files_per_item);
            if (item.withheld != Withheld::none && files) r.ok = false;  // a withheld item carries nothing
            for (std::size_t k = 0; k < files && r.ok; ++k) item.files.push_back(read_file(r));
            m.items.push_back(std::move(item));
        }
        if (r.ok && contents_bytes(m) > max_outfit) r.ok = false;
        out = std::move(m);
        break;
    }
    case Kind::fetch: {
        Fetch m;
        const auto n = r.count(max_fetch);
        for (std::size_t i = 0; i < n && r.ok; ++i) { ContentId id{}; r.raw(id); m.files.push_back(id); }
        if (m.files.empty()) r.ok = false;
        out = std::move(m);
        break;
    }
    case Kind::piece: {
        Piece m;
        r.raw(m.id);
        m.offset = r.u32();
        m.total = r.u32();
        m.bytes = r.bytes(max_piece);
        if (!m.total || m.total > max_file || m.bytes.empty() || m.offset > m.total || m.bytes.size() > m.total - m.offset) r.ok = false;
        out = std::move(m);
        break;
    }
    case Kind::refuse: {
        out = Refuse{r.text(max_reason)};
        break;
    }
    default: return std::nullopt;
    }
    if (!r.ok || r.at != bytes.size()) return std::nullopt;
    return out;
}

std::uint64_t contents_bytes(const Contents& contents) noexcept {
    std::uint64_t total = 0;
    for (const auto& item : contents.items)
        for (const auto& f : item.files) total += f.size;
    return total;
}
} // namespace dingosdk::multiplayer::synchros
