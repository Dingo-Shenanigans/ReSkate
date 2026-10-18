#include "domination_wire.h"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace dingosdk::domination::wire {
namespace {
enum : std::uint8_t { round_type = 1, board_type = 2, teams_type = 3 };
enum : std::uint8_t { running_action = 1, stopped_action = 2, left_action = 3 };
constexpr std::uint8_t all_options = teams;

void put(std::vector<std::uint8_t> &out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool ok = true;
    std::uint64_t take(int count) {
        if (at + count > bytes.size()) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < count; ++i) v |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
        at += count;
        return v;
    }
    bool done() const { return ok && at == bytes.size(); }
};
bool sane_score(float score) { return std::isfinite(score) && score > 0 && score <= 1e9f; }
} // namespace

bool is_domination(std::span<const std::uint8_t> bytes) noexcept { return !bytes.empty() && bytes[0] == tag; }
std::optional<std::uint8_t> version_of(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 2 || bytes[0] != tag) return std::nullopt;
    return bytes[1];
}

std::vector<std::uint8_t> encode(const Message &message) {
    std::vector<std::uint8_t> out;
    put(out, tag, 1);
    put(out, version, 1);
    if (const auto *r = std::get_if<RoundMessage>(&message)) {
        put(out, round_type, 1);
        put(out, r->park, 4);
        put(out, r->round, 4);
        put(out, r->starter, 8);
        put(out, r->running ? running_action : r->left ? left_action : stopped_action, 1);
        put(out, r->options & all_options, 1);
        put(out, r->duration, 2);
        put(out, r->elapsed, 4);
    } else if (const auto *b = std::get_if<BoardMessage>(&message)) {
        if (b->entries.size() > max_board_entries) throw std::invalid_argument("a Domination board message has too many claims");
        put(out, board_type, 1);
        put(out, b->park, 4);
        put(out, b->round, 4);
        put(out, b->entries.size(), 2);
        for (const auto &e : b->entries) {
            put(out, e.spot, 1);
            put(out, e.player, 8);
            put(out, std::bit_cast<std::uint32_t>(e.score), 4);
            put(out, e.ms, 4);
        }
    } else {
        const auto &t = std::get<TeamsMessage>(message);
        if (t.members.size() > max_team_entries) throw std::invalid_argument("a Domination team list is too long");
        put(out, teams_type, 1);
        put(out, t.park, 4);
        put(out, t.round, 4);
        put(out, t.members.size(), 1);
        for (const auto &[player, team] : t.members) {
            put(out, player, 8);
            put(out, team, 1);
        }
    }
    return out;
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) noexcept {
    Reader in{bytes};
    if (in.take(1) != tag || in.take(1) != version) return std::nullopt;
    const auto type = in.take(1);
    const auto park = static_cast<std::uint32_t>(in.take(4)), round = static_cast<std::uint32_t>(in.take(4));
    if (!in.ok || !round) return std::nullopt;
    if (type == round_type) {
        RoundMessage r{park, round};
        r.starter = in.take(8);
        const auto action = in.take(1);
        r.options = static_cast<std::uint8_t>(in.take(1));
        r.duration = static_cast<std::uint16_t>(in.take(2));
        r.elapsed = static_cast<std::uint32_t>(in.take(4));
        if (!in.done() || !r.starter || action < running_action || action > left_action || (r.options & ~all_options) || !r.duration)
            return std::nullopt;
        r.running = action == running_action;
        r.left = action == left_action;
        return r;
    }
    if (type == board_type) {
        BoardMessage b{park, round};
        const auto count = in.take(2);
        if (!in.ok || count > max_board_entries) return std::nullopt;
        for (std::uint64_t i = 0; i < count; ++i) {
            Entry e;
            e.spot = static_cast<std::uint8_t>(in.take(1));
            e.player = in.take(8);
            e.score = std::bit_cast<float>(static_cast<std::uint32_t>(in.take(4)));
            e.ms = static_cast<std::uint32_t>(in.take(4));
            if (!in.ok || !e.player || !sane_score(e.score)) return std::nullopt;
            b.entries.push_back(e);
        }
        if (!in.done()) return std::nullopt;
        return b;
    }
    if (type == teams_type) {
        TeamsMessage t{park, round};
        const auto count = in.take(1);
        if (!in.ok || count > max_team_entries) return std::nullopt;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto player = in.take(8);
            const auto team = in.take(1);
            if (!in.ok || !player || team > 1) return std::nullopt;
            t.members.emplace_back(player, static_cast<std::uint8_t>(team));
        }
        if (!in.done()) return std::nullopt;
        return t;
    }
    return std::nullopt;
}
}
