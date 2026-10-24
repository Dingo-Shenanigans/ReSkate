#include "throwdown_wire.h"
#include <cmath>
#include <bit>
#include <algorithm>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
bool individual(std::uint64_t id) noexcept {
    return (id >> 56) == 1 && ((id >> 52) & 15) == 1 && (id & 0xffffffffULL);
}
bool valid_series(const std::string &series) noexcept {
    return !series.empty() && series.size() <= max_throwdown_series &&
           std::all_of(series.begin(), series.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
           });
}
// A plugin mode's key: the plugin's folder name, a /, and the mode's id.
bool valid_mode_key(const std::string &key) noexcept {
    return key.size() >= 3 && key.size() <= max_challenge_id && key.find('/') != std::string::npos &&
           std::all_of(key.begin(), key.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                      c == '.' || c == '/' || c == ' ';
           });
}
bool valid_challenge_id(const std::string &id) noexcept {
    return !id.empty() && id.size() <= max_challenge_id && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    void integer(std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void blob(const std::vector<std::uint8_t> &value, unsigned size) {
        integer(value.size(), size);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool ok = true;
    std::uint64_t integer(unsigned size) {
        if (!ok || size > bytes.size() - at) { ok = false; return 0; }
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= std::uint64_t{bytes[at + i]} << (8 * i);
        at += size;
        return value;
    }
    std::vector<std::uint8_t> blob(unsigned size, std::size_t limit) {
        const auto length = integer(size);
        if (!ok || length > limit || length > bytes.size() - at) { ok = false; return {}; }
        std::vector<std::uint8_t> value(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += static_cast<std::size_t>(length);
        return value;
    }
};
} // namespace

namespace {
bool printable(const std::string &text, std::size_t limit, bool empty_ok) noexcept {
    return (empty_ok || !text.empty()) && text.size() <= limit &&
           std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; });
}
bool valid_flag_body(const ThrowdownFlagBody &f) noexcept {
    return printable(f.id, max_flag_id, false) && f.stats.size() <= max_flag_stats &&
           std::all_of(f.stats.begin(), f.stats.end(), [](const auto &s) {
               return printable(s.first, max_flag_stat, true) && printable(s.second, max_flag_stat, true);
           }) &&
           std::isfinite(f.yaw) && std::abs(f.yaw) <= 3600.f && f.players <= 32 && f.capacity >= 1 && f.capacity <= 32 &&
           std::all_of(f.at.begin(), f.at.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 100000.f; });
}
} // namespace
std::vector<std::uint8_t> encode_flag_body(const ThrowdownFlagBody &f) {
    if (!valid_flag_body(f)) throw std::invalid_argument("Invalid throwdown flag");
    Writer w;
    const auto text = [&](const std::string &value) {
        w.integer(value.size(), 1);
        w.bytes.insert(w.bytes.end(), value.begin(), value.end());
    };
    text(f.id);
    for (const auto v : f.at) w.integer(std::bit_cast<std::uint32_t>(v), 4);
    w.integer(std::bit_cast<std::uint32_t>(f.yaw), 4);
    w.integer(f.players, 1);
    w.integer(f.capacity, 1);
    w.integer((f.open ? 1U : 0U) | (f.withdrawn ? 2U : 0U), 1);
    w.integer(f.stats.size(), 1);
    for (const auto &[title, value] : f.stats) { text(title); text(value); }
    return std::move(w.bytes);
}
std::optional<ThrowdownFlagBody> decode_flag_body(std::span<const std::uint8_t> bytes) noexcept {
    try {
        Reader r{bytes};
        ThrowdownFlagBody f;
        const auto text = [&](std::size_t limit, std::string &out) {
            const auto length = r.integer(1);
            if (!r.ok || length > limit || length > bytes.size() - r.at) return false;
            out.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at += static_cast<std::size_t>(length);
            return true;
        };
        if (!text(max_flag_id, f.id)) return {};
        for (auto &v : f.at) v = std::bit_cast<float>(static_cast<std::uint32_t>(r.integer(4)));
        f.yaw = std::bit_cast<float>(static_cast<std::uint32_t>(r.integer(4)));
        f.players = static_cast<std::uint8_t>(r.integer(1));
        f.capacity = static_cast<std::uint8_t>(r.integer(1));
        const auto bits = r.integer(1);
        if (bits > 3) return {};
        f.open = (bits & 1) != 0;
        f.withdrawn = (bits & 2) != 0;
        const auto count = r.integer(1);
        if (!r.ok || count > max_flag_stats) return {};
        for (std::uint64_t i = 0; i < count; ++i) {
            std::pair<std::string, std::string> stat;
            if (!text(max_flag_stat, stat.first) || !text(max_flag_stat, stat.second)) return {};
            f.stats.push_back(std::move(stat));
        }
        if (!r.ok || r.at != bytes.size() || !valid_flag_body(f)) return {};
        return f;
    } catch (...) { return {}; }
}

bool valid_throwdown(const ThrowdownMessage &m) noexcept {
    if (!individual(m.leader) || !m.id) return false;
    switch (m.kind) {
    case ThrowdownMessage::Kind::offer:
        return valid_series(m.series) && !m.placement.empty() && m.placement.size() <= max_throwdown_params &&
               m.settings.size() <= max_throwdown_params && m.order.size() <= max_throwdown_order &&
               std::all_of(m.order.begin(), m.order.end(), individual);
    case ThrowdownMessage::Kind::start:
    case ThrowdownMessage::Kind::invite:
        return !m.order.empty() && m.order.size() <= max_throwdown_order &&
               std::all_of(m.order.begin(), m.order.end(), individual);
    case ThrowdownMessage::Kind::row: return m.board < max_throwdown_boards;
    case ThrowdownMessage::Kind::attempt:
    case ThrowdownMessage::Kind::turn_end: return m.value > 0;
    case ThrowdownMessage::Kind::close:
    case ThrowdownMessage::Kind::join:
    case ThrowdownMessage::Kind::leave:
    case ThrowdownMessage::Kind::score: return true;
    case ThrowdownMessage::Kind::challenge_start:
        return valid_series(m.series) && valid_challenge_id(m.challenge) && m.order.size() >= 2 &&
               m.order.size() <= max_challenge_players && m.order.front() == m.leader &&
               std::all_of(m.order.begin(), m.order.end(), individual);
    case ThrowdownMessage::Kind::challenge_attempt:
        return !m.criteria.empty() && m.criteria.size() % challenge_criteria_size == 0 &&
               m.criteria.size() <= max_challenge_criteria * challenge_criteria_size && m.indexes.size() % 4 == 0 &&
               m.indexes.size() <= max_challenge_criteria * 4;
    case ThrowdownMessage::Kind::challenge_optout:
    case ThrowdownMessage::Kind::challenge_slam:
    case ThrowdownMessage::Kind::challenge_leave: return true;
    case ThrowdownMessage::Kind::beacon:
        return m.leader && std::all_of(m.location.begin(), m.location.end(),
                                       [](float v) { return std::isfinite(v) && std::abs(v) < 1e6f; });
    case ThrowdownMessage::Kind::mode:
        return valid_mode_key(m.challenge) && (m.series.empty() || valid_series(m.series)) && m.value >= 1 && m.value <= 10;
    case ThrowdownMessage::Kind::flag:
        return valid_mode_key(m.challenge) && decode_flag_body(m.placement).has_value();
    }
    return false;
}

std::vector<std::uint8_t> encode_throwdown(const ThrowdownMessage &m) {
    if (!valid_throwdown(m)) throw std::invalid_argument("Invalid throwdown message");
    Writer w;
    w.integer(static_cast<std::uint8_t>(m.kind), 1);
    w.integer(m.leader, 8);
    w.integer(m.id, 4);
    switch (m.kind) {
    case ThrowdownMessage::Kind::offer:
        w.integer(m.series.size(), 1);
        w.bytes.insert(w.bytes.end(), m.series.begin(), m.series.end());
        w.blob(m.placement, 2);
        w.blob(m.settings, 2);
        [[fallthrough]];
    case ThrowdownMessage::Kind::start:
    case ThrowdownMessage::Kind::invite:
        w.integer(m.order.size(), 1);
        for (const auto id : m.order) w.integer(id, 8);
        break;
    case ThrowdownMessage::Kind::score:
    case ThrowdownMessage::Kind::turn_end: w.integer(static_cast<std::uint32_t>(m.value), 4); break;
    case ThrowdownMessage::Kind::row:
        w.integer(m.board, 1);
        w.integer(m.add ? 1 : 0, 1);
        w.integer(static_cast<std::uint32_t>(m.value), 4);
        break;
    case ThrowdownMessage::Kind::attempt:
        w.integer(static_cast<std::uint32_t>(m.value), 4);
        w.integer(m.add ? 1 : 0, 1);
        w.bytes.insert(w.bytes.end(), m.trick.begin(), m.trick.end());
        break;
    case ThrowdownMessage::Kind::challenge_start:
        w.integer(m.series.size(), 1);
        w.bytes.insert(w.bytes.end(), m.series.begin(), m.series.end());
        w.integer(m.challenge.size(), 1);
        w.bytes.insert(w.bytes.end(), m.challenge.begin(), m.challenge.end());
        w.integer(m.add ? 1 : 0, 1);
        w.integer(m.order.size(), 1);
        for (const auto id : m.order) w.integer(id, 8);
        break;
    case ThrowdownMessage::Kind::challenge_attempt:
        w.blob(m.criteria, 2);
        w.blob(m.indexes, 2);
        break;
    case ThrowdownMessage::Kind::challenge_slam: w.integer(static_cast<std::uint32_t>(m.value), 4); break;
    case ThrowdownMessage::Kind::beacon:
        w.integer(m.add ? 1 : 0, 1);
        if (m.add)
            for (const auto v : m.location) w.integer(std::bit_cast<std::uint32_t>(v), 4);
        break;
    case ThrowdownMessage::Kind::mode:
        w.integer(m.challenge.size(), 1);
        w.bytes.insert(w.bytes.end(), m.challenge.begin(), m.challenge.end());
        w.integer(m.series.size(), 1);
        w.bytes.insert(w.bytes.end(), m.series.begin(), m.series.end());
        w.integer(static_cast<std::uint32_t>(m.value), 1);
        break;
    case ThrowdownMessage::Kind::flag:
        w.integer(m.challenge.size(), 1);
        w.bytes.insert(w.bytes.end(), m.challenge.begin(), m.challenge.end());
        w.blob(m.placement, 2);
        break;
    default: break;
    }
    return std::move(w.bytes);
}

std::optional<ThrowdownMessage> decode_throwdown(std::span<const std::uint8_t> bytes) noexcept {
    try {
        Reader r{bytes};
        ThrowdownMessage m;
        const auto kind = r.integer(1);
        if (kind < 1 || kind > 18) return {};
        m.kind = static_cast<ThrowdownMessage::Kind>(kind);
        m.leader = r.integer(8);
        m.id = static_cast<std::uint32_t>(r.integer(4));
        switch (m.kind) {
        case ThrowdownMessage::Kind::offer: {
            const auto length = r.integer(1);
            if (!r.ok || length > max_throwdown_series || length > bytes.size() - r.at) return {};
            m.series.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at += static_cast<std::size_t>(length);
            m.placement = r.blob(2, max_throwdown_params);
            m.settings = r.blob(2, max_throwdown_params);
            [[fallthrough]];
        }
        case ThrowdownMessage::Kind::start:
        case ThrowdownMessage::Kind::invite: {
            const auto count = r.integer(1);
            if (!r.ok || count > max_throwdown_order) return {};
            for (std::uint64_t i = 0; i < count; ++i) m.order.push_back(r.integer(8));
            break;
        }
        case ThrowdownMessage::Kind::score:
        case ThrowdownMessage::Kind::turn_end: m.value = static_cast<std::int32_t>(r.integer(4)); break;
        case ThrowdownMessage::Kind::row: {
            m.board = static_cast<std::uint8_t>(r.integer(1));
            const auto add = r.integer(1);
            if (add > 1) return {};
            m.add = add != 0;
            m.value = static_cast<std::int32_t>(r.integer(4));
            break;
        }
        case ThrowdownMessage::Kind::attempt: {
            m.value = static_cast<std::int32_t>(r.integer(4));
            const auto landed = r.integer(1);
            if (landed > 1) return {};
            m.add = landed != 0;
            for (auto &byte : m.trick) byte = static_cast<std::uint8_t>(r.integer(1));
            break;
        }
        case ThrowdownMessage::Kind::challenge_start: {
            const auto text = [&](std::size_t limit, std::string &out) {
                const auto length = r.integer(1);
                if (!r.ok || length > limit || length > bytes.size() - r.at) return false;
                out.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                return true;
            };
            if (!text(max_throwdown_series, m.series) || !text(max_challenge_id, m.challenge)) return {};
            const auto contest = r.integer(1);
            if (contest > 1) return {};
            m.add = contest != 0;
            const auto count = r.integer(1);
            if (!r.ok || count > max_challenge_players) return {};
            for (std::uint64_t i = 0; i < count; ++i) m.order.push_back(r.integer(8));
            break;
        }
        case ThrowdownMessage::Kind::challenge_attempt:
            m.criteria = r.blob(2, max_challenge_criteria * challenge_criteria_size);
            m.indexes = r.blob(2, max_challenge_criteria * 4);
            break;
        case ThrowdownMessage::Kind::challenge_slam: m.value = static_cast<std::int32_t>(r.integer(4)); break;
        case ThrowdownMessage::Kind::beacon: {
            const auto placed = r.integer(1);
            if (placed > 1) return {};
            m.add = placed != 0;
            if (m.add)
                for (auto &v : m.location) v = std::bit_cast<float>(static_cast<std::uint32_t>(r.integer(4)));
            break;
        }
        case ThrowdownMessage::Kind::mode: {
            const auto text = [&](std::size_t limit, std::string &out) {
                const auto length = r.integer(1);
                if (!r.ok || length > limit || length > bytes.size() - r.at) return false;
                out.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                return true;
            };
            if (!text(max_challenge_id, m.challenge) || !text(max_throwdown_series, m.series)) return {};
            m.value = static_cast<std::int32_t>(r.integer(1));
            break;
        }
        case ThrowdownMessage::Kind::flag: {
            const auto length = r.integer(1);
            if (!r.ok || length > max_challenge_id || length > bytes.size() - r.at) return {};
            m.challenge.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at += static_cast<std::size_t>(length);
            m.placement = r.blob(2, max_throwdown_params);
            break;
        }
        default: break;
        }
        if (!r.ok || r.at != bytes.size() || !valid_throwdown(m)) return {};
        return m;
    } catch (...) { return {}; }
}
} // namespace dingosdk::multiplayer
