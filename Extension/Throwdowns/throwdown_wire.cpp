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

bool valid_throwdown(const ThrowdownMessage &m) noexcept {
    if (!individual(m.leader) || !m.id) return false;
    switch (m.kind) {
    case ThrowdownMessage::Kind::offer:
        return valid_series(m.series) && !m.placement.empty() && m.placement.size() <= max_throwdown_params &&
               m.settings.size() <= max_throwdown_params && m.order.size() <= max_throwdown_order &&
               std::all_of(m.order.begin(), m.order.end(), individual);
    case ThrowdownMessage::Kind::start:
        return !m.order.empty() && m.order.size() <= max_throwdown_order && m.settings.size() <= max_throwdown_params &&
               std::all_of(m.order.begin(), m.order.end(), individual);
    case ThrowdownMessage::Kind::row: return m.board < max_throwdown_boards;
    case ThrowdownMessage::Kind::attempt: return m.value > 0 && m.detected.size() <= max_detected_attempt;
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
        w.integer(m.order.size(), 1);
        for (const auto id : m.order) w.integer(id, 8);
        if (m.kind == ThrowdownMessage::Kind::start) w.blob(m.settings, 2);
        w.integer(m.rules_mask, 1);
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
        w.integer(m.rules_mask, 1);
        w.blob(m.detected, 2);
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
    default: break;
    }
    if(w.bytes.size()>4096)throw std::invalid_argument("Throwdown exceeds packet budget");
    return std::move(w.bytes);
}

std::optional<ThrowdownMessage> decode_throwdown(std::span<const std::uint8_t> bytes) noexcept {
    try {
        Reader r{bytes};
        ThrowdownMessage m;
        const auto kind = r.integer(1);
        if (kind < 1 || kind > 15) return {};
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
        case ThrowdownMessage::Kind::start: {
            const auto count = r.integer(1);
            if (!r.ok || count > max_throwdown_order) return {};
            for (std::uint64_t i = 0; i < count; ++i) m.order.push_back(r.integer(8));
            if (m.kind == ThrowdownMessage::Kind::start) m.settings = r.blob(2, max_throwdown_params);
            m.rules_mask = static_cast<std::uint8_t>(r.integer(1));
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
            m.rules_mask = static_cast<std::uint8_t>(r.integer(1));
            m.detected = r.blob(2, max_detected_attempt);
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
        default: break;
        }
        if (!r.ok || r.at != bytes.size() || !valid_throwdown(m)) return {};
        return m;
    } catch (...) { return {}; }
}
} // namespace dingosdk::multiplayer
