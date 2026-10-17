#include "domination_sync.h"
#include "domination_wire.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dingosdk::domination::sync {
std::uint8_t Options::bits() const { return static_cast<std::uint8_t>(teams ? wire::teams : 0); }
Options Options::from(std::uint8_t b) { return {(b & wire::teams) != 0}; }

Player controller(const Roster &roster) {
    if (!roster.self) return 0;
    const auto present = [&](Player who) { return who == roster.self || std::ranges::find(roster.others, who) != roster.others.end(); };
    if (roster.host && present(roster.host)) return roster.host;
    Player lowest = roster.self;
    for (const auto who : roster.others)
        if (who && who < lowest) lowest = who;
    return lowest;
}

// ---- a round ----------------------------------------------------------------------------
std::uint32_t Match::ms(Ms now) const { return static_cast<std::uint32_t>(std::clamp<Ms>(now - origin, 0, 4'000'000'000LL)); }
double Match::left_s(Ms now) const {
    if (finished) return 0;
    const double played = std::max<double>(0, static_cast<double>(ms(now)) - countdown_ms) / 1000.0;
    return std::max(0.0, duration_s - played);
}
int Match::team_of(Player who) const {
    const auto it = teams.find(who);
    return it == teams.end() ? -1 : it->second;
}
bool Match::rivals(Player a, Player b) const {
    if (!a || !b || a == b) return false;
    if (!with_teams()) return true;
    const int ta = team_of(a), tb = team_of(b);
    return ta < 0 || tb < 0 || ta != tb;
}
Results Match::standings(Ms now) const {
    if (finished) return results;
    Results r;
    const auto t = with_teams() ? teams : Teams{};
    r.stats = round_stats(board, std::min(ms(now), end_ms()), t);
    r.rows = ranked(board, r.stats, t);
    return r;
}

// ---- starting and stopping ----------------------------------------------------------------
StartResult Node::start(Ms now, Options options, std::uint16_t duration_s, bool solo, std::uint32_t id) {
    if (!spots_) return StartResult::no_spots;
    if (running() && match_->multiplayer) return StartResult::running;
    const bool together = !solo && roster_.session();
    if (together && !controls()) return StartResult::not_controller;
    if (running()) finish(now, match_->ms(now)); // a solo round gives way
    Match m;
    m.id = id ? id : 1;
    m.self = roster_.self ? roster_.self : solo_self;
    m.starter = m.self;
    m.options = options;
    m.duration_s = duration_s;
    m.multiplayer = together;
    m.origin = now;
    m.board = Board(spots_);
    m.teams_heard = true;
    m.joined = now;
    m.next_resend = now;
    if (together) m.seen[m.self] = now;
    match_ = std::move(m);
    if (together) {
        left_round_ = 0;
        send_round(true, 0);
    }
    say({Event::Kind::started, match_->self});
    return StartResult::started;
}

bool Node::stop(Ms now) {
    if (!running()) return false;
    auto &m = *match_;
    // The controller (or whoever started it) ends it for everyone; anyone else leaves it.
    const bool ends = !m.multiplayer || m.starter == m.self || controls();
    const auto at = std::min(m.ms(now), m.end_ms());
    // Ending it, or (from anyone else) saying this player left it.
    if (m.multiplayer) send_round(false, at, !ends);
    if (m.multiplayer && !ends) left_round_ = m.id;
    finish(now, at, ends);
    m.left = m.multiplayer && !ends;
    m.stopper = m.multiplayer && ends;
    say({ends ? Event::Kind::stopped : Event::Kind::left, m.self});
    return true;
}

// The board without the claims made after `end` (a round stopped early: claims a player made
// before the stop reached them are dropped on every copy alike).
Board until(const Board &board, std::uint32_t end) {
    Board kept(board.spots());
    std::vector<std::pair<std::uint32_t, std::pair<int, domination::Event>>> all;
    for (std::size_t s = 0; s < board.spots(); ++s)
        for (const auto &e : board.history(static_cast<int>(s)))
            if (e.ms <= end) all.push_back({e.ms, {static_cast<int>(s), e}});
    std::ranges::stable_sort(all, [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[ms, item] : all) kept.claim(item.first, item.second.player, item.second.score, ms);
    return kept;
}

void Node::finish(Ms now, std::uint32_t ended_ms, bool after) {
    auto &m = *match_;
    // Ended for this copy (not just left: `rejoin` may still bring it back): never joined again.
    if (after && std::ranges::find(over_, m.id) == over_.end()) {
        over_.push_back(m.id);
        if (over_.size() > 16) over_.pop_front();
    }
    m.finished = true;
    m.finished_at = now;
    m.ended_ms = std::min(ended_ms, m.end_ms());
    if (m.ended_ms < m.end_ms()) m.board = until(m.board, m.ended_ms);
    recompute();
    say({Event::Kind::finished, m.self, {}, false, m.ended_ms, m.id});
    if (m.multiplayer && after) send_board(); // the last word, so that everyone's results agree
    if (m.multiplayer) m.next_after = now + after_resend_ms;
}
void Node::recompute() {
    auto &m = *match_;
    const auto t = m.with_teams() ? m.teams : Teams{};
    m.results.stats = round_stats(m.board, m.ended_ms, t);
    m.results.rows = ranked(m.board, m.results.stats, t);
}

// ---- claims -------------------------------------------------------------------------------
std::optional<ClaimOutcome> Node::take(int spot, Player who, float score, std::uint32_t at) {
    auto &m = *match_;
    if (spot < 0 || static_cast<std::size_t>(spot) >= spots_ || !who) return std::nullopt;
    if (!std::isfinite(score) || score <= 0) return std::nullopt;
    if (!m.players.contains(who) && m.players.size() >= max_players) return std::nullopt;
    if (m.board.history_size(spot) >= max_events_per_spot) return std::nullopt;
    m.players.insert(who);
    return m.board.claim(spot, who, score, at);
}

std::optional<Node::Claimed> Node::claim(int spot, float score, Ms now) {
    if (!running() || match_->counting_down(now)) return std::nullopt;
    auto &m = *match_;
    const auto at = m.ms(now);
    if (at >= m.end_ms()) return std::nullopt;
    Claimed c;
    c.score = score;
    const auto out = take(spot, m.self, c.score, at);
    if (!out) return std::nullopt;
    c.outcome = *out;
    c.ms = at;
    if (m.multiplayer && out->best) {
        wire::BoardMessage b{park_, m.id, {{static_cast<std::uint8_t>(spot), m.self, c.score, at}}};
        outgoing_.push_back(wire::encode(b));
    }
    if (out->after != out->before) say({Event::Kind::claim, m.self, *out});
    return c;
}

// ---- messages out -------------------------------------------------------------------------
// `elapsed`: running, this player's time in the round; stopped, the moment it was stopped (or left).
void Node::send_round(bool running, std::uint32_t elapsed, bool left) {
    const auto &m = *match_;
    wire::RoundMessage r{park_, m.id, m.starter, running, m.options.bits(), m.duration_s, elapsed, left};
    outgoing_.push_back(wire::encode(r));
}
// The board as this player knows it: every claim kept, in messages of max_board_entries at most.
// A board longer than two messages goes out a part at a time, a different part each time. An
// empty one goes too: it tells the others this player is in the round.
void Node::send_board() {
    auto &m = *match_;
    std::vector<wire::Entry> all;
    for (std::size_t spot = 0; spot < m.board.spots(); ++spot)
        for (const auto &e : m.board.history(static_cast<int>(spot)))
            all.push_back({static_cast<std::uint8_t>(spot), e.player, e.score, e.ms});
    constexpr std::size_t most = 2 * wire::max_board_entries;
    std::vector<wire::Entry> part;
    if (all.size() <= most) part = std::move(all);
    else {
        for (std::size_t n = 0; n < most; ++n) part.push_back(all[(m.board_cursor + n) % all.size()]);
        m.board_cursor = (m.board_cursor + most) % all.size();
    }
    std::size_t at = 0;
    do {
        const auto count = std::min(wire::max_board_entries, part.size() - at);
        const auto from = part.begin() + static_cast<std::ptrdiff_t>(at);
        outgoing_.push_back(wire::encode(wire::BoardMessage{park_, m.id, {from, from + static_cast<std::ptrdiff_t>(count)}}));
        at += count;
    } while (at < part.size());
}
void Node::send_teams() {
    const auto &m = *match_;
    wire::TeamsMessage t{park_, m.id, {}};
    for (const auto &[who, team] : m.teams)
        if (t.members.size() < wire::max_team_entries) t.members.emplace_back(who, static_cast<std::uint8_t>(team));
    outgoing_.push_back(wire::encode(t));
}

Player Node::authority(Ms now) const {
    if (!match_) return 0;
    const auto &m = *match_;
    const auto c = controller();
    const auto recent = [&](Player who) {
        const auto it = m.seen.find(who);
        return who == m.self || (it != m.seen.end() && now - it->second < gone_ms);
    };
    if (c && recent(c)) return c;
    Player lowest = m.self;
    for (const auto &[who, when] : m.seen)
        if (now - when < gone_ms && who < lowest) lowest = who;
    return lowest;
}

void Node::tick(Ms now) {
    if (!match_) return;
    auto &m = *match_;
    if (!m.finished) {
        if (m.ms(now) >= m.end_ms()) {
            finish(now, m.end_ms());
            return;
        }
        if (!m.multiplayer || now < m.next_resend) return;
        m.next_resend = now + resend_ms;
        m.seen[m.self] = now;
        // Everyone in it says the round runs (late joiners and players coming back hear it from
        // anyone, even while its authority changes hands); clocks follow only the authority's.
        send_round(true, m.ms(now));
        if (authority(now) == m.self) {
            if (m.with_teams() && (m.teams_heard || now - m.joined >= teams_patience_ms)) {
                std::vector<Player> playing{m.self};
                for (const auto &[who, when] : m.seen)
                    if (who != m.self && now - when < gone_ms) playing.push_back(who);
                const int before = m.team_of(m.self);
                m.teams = balance_teams(m.teams, playing);
                m.teams_heard = true;
                if (m.team_of(m.self) != before) say({Event::Kind::teams, m.self, {}, false, m.team_of(m.self)});
                send_teams();
            }
        }
        send_board();
        return;
    }
    // Over: the stop (from whoever stopped it), the teams (from the authority) and the board again,
    // for whoever missed the last of them: often at first, then now and then for a while (a player
    // whose connection dropped across the end hears it all when it comes back).
    // A player who left says so again for a while instead (once may be lost: the others would go on
    // counting them in it, as its authority even, and nobody would say the round runs).
    if (m.multiplayer && m.next_after && now >= m.next_after) {
        const Ms since = now - m.finished_at;
        if (since > (m.left ? after_window_ms : after_all_ms)) m.next_after = 0;
        else {
            m.next_after = now + (since <= after_window_ms ? after_resend_ms : after_slow_ms);
            if (m.left) {
                send_round(false, m.ended_ms, true);
                return;
            }
            if (m.stopper) send_round(false, m.ended_ms);
            if (m.with_teams() && !m.teams.empty() && authority(now) == m.self) send_teams();
            send_board();
        }
    }
}

// ---- messages in --------------------------------------------------------------------------
void Node::merge(Player, const wire::BoardMessage &b, Ms now) {
    auto &m = *match_;
    const auto latest = m.finished ? static_cast<Ms>(m.ended_ms) : std::min<Ms>(m.end_ms(), static_cast<Ms>(m.ms(now)) + claim_ahead_ms);
    std::vector<ClaimOutcome> changed;
    bool any{};
    for (const auto &e : b.entries) {
        if (e.ms < countdown_ms || e.ms > latest) continue;
        const auto out = take(e.spot, e.player, e.score, e.ms);
        if (!out) continue;
        // An earlier claim heard late changes the replay (hold times, steals) without raising a best.
        any = any || out->kept;
        if (!out->best) continue;
        if (out->after != out->before) changed.push_back(*out);
    }
    // Many owners changed by one message is a catch-up (a player who just joined): quiet. Once the
    // round is over, claims heard late (up to minutes after) change its results without a word.
    const bool quiet = changed.size() > 3;
    if (!m.finished)
        for (const auto &out : changed) say({Event::Kind::claim, out.player, out, quiet});
    if (m.finished && any) recompute();
}

void Node::receive(Player from, std::span<const std::uint8_t> bytes, Ms now) {
    if (!from) return;
    const auto message = wire::decode(bytes);
    if (!message) {
        // Another build's Domination: said once per player, so they know why rounds don't meet.
        const auto version = wire::version_of(bytes);
        if (version && *version != wire::version && told_version_.size() < 256 && told_version_.insert(from).second)
            say({Event::Kind::version, from, {}, false, *version});
        return;
    }
    const auto seen = [&](Match &m) {
        if (m.seen.size() < max_players || m.seen.contains(from)) m.seen[from] = now;
    };
    const auto ours = [&](std::uint32_t park, std::uint32_t round) {
        return park == park_ && match_ && match_->multiplayer && match_->id == round;
    };
    if (const auto *b = std::get_if<wire::BoardMessage>(&*message)) {
        if (!ours(b->park, b->round) || from == match_->self) return;
        auto &m = *match_;
        // A round over still takes claims made before its end (a trick landed on another player's
        // last second, or by a player whose connection dropped for a while): all copies end with
        // all of them.
        seen(m);
        merge(from, *b, now);
        return;
    }
    if (const auto *t = std::get_if<wire::TeamsMessage>(&*message)) {
        if (!ours(t->park, t->round) || from == match_->self) return;
        auto &m = *match_;
        // Teams handed out near the end still count for its results.
        if (m.finished && m.left) return;
        seen(m);
        // The authority's word; a player not told yet takes anyone's.
        if (m.teams_heard && from != authority(now)) return;
        Teams teams;
        for (const auto &[who, team] : t->members) teams[who] = team;
        const int before = m.team_of(m.self);
        m.teams = std::move(teams);
        m.teams_heard = true;
        if (m.finished) recompute();
        if (m.team_of(m.self) >= 0 && m.team_of(m.self) != before) say({Event::Kind::teams, m.self, {}, false, m.team_of(m.self)});
        return;
    }
    const auto &r = std::get<wire::RoundMessage>(*message);
    if (r.park != park_) {
        if (r.running && told_park_.size() < 256 && told_park_.insert(from).second) say({Event::Kind::park, from});
        return;
    }
    if (ours(r.park, r.round)) {
        auto &m = *match_;
        seen(m);
        if (r.left) {
            m.seen.erase(from); // that player left it: no longer counted in it
            return;
        }
        if (!r.running) {
            // Stopped, at a moment (from the controller, or relayed by a player whose copy it ended):
            // every copy ends at it, and a stop that comes after this copy's own end (missed, late,
            // or near the end) still moves the end back to it. This copy then relays it too.
            if (m.left) return;
            if (!m.finished) {
                finish(now, r.elapsed);
                m.stopper = true;
                say({Event::Kind::stopped, from});
            } else if (r.elapsed < m.ended_ms) {
                finish(now, r.elapsed);
                m.stopper = true;
            }
            return;
        }
        // A player who left and asked to be back in: their copy reopens, and the others' boards
        // fill in what they missed. (A round that was stopped or ran out never reopens.)
        if (m.finished && m.left && left_round_ != m.id && static_cast<Ms>(r.elapsed) < static_cast<Ms>(m.end_ms())) {
            m.finished = m.left = false;
            m.finished_at = 0;
            m.ended_ms = 0;
            m.results = {};
            m.next_resend = now;
            m.next_after = 0;
            m.seen[m.self] = now;
            say({Event::Kind::joined, m.starter});
        }
        // Every clock follows the authority's.
        if (!m.finished && from != m.self && from == authority(now)) {
            const auto gap = static_cast<Ms>(r.elapsed) - static_cast<Ms>(m.ms(now));
            if (gap > clock_slack_ms || gap < -clock_slack_ms) {
                m.origin -= gap / 2;
                say({Event::Kind::clock, from, {}, false, gap / 2});
            }
        }
        return;
    }
    if (!r.running || r.round == left_round_ || !roster_.self || std::ranges::find(over_, r.round) != over_.end()) return;
    if (static_cast<Ms>(r.elapsed) >= static_cast<Ms>(r.duration) * 1000 + countdown_ms) return; // over already
    std::uint32_t moved_from{};
    if (running()) {
        auto &m = *match_;
        if (m.multiplayer) {
            // Two rounds at once: the lower number stands, while nothing is claimed in the other.
            if (r.round > m.id || !m.players.empty() || static_cast<Ms>(m.ms(now)) > countdown_ms + 10000) return;
            moved_from = m.id;
        } else
            finish(now, m.ms(now)); // a round of one's own gives way to the session's
    }
    Match m;
    m.id = r.round;
    m.self = roster_.self;
    m.starter = r.starter;
    m.options = Options::from(r.options);
    m.duration_s = r.duration;
    m.multiplayer = true;
    m.origin = now - static_cast<Ms>(r.elapsed);
    m.board = Board(spots_);
    m.joined = now;
    m.next_resend = now; // the first board goes out at once: it tells the others this player is in
    m.seen[m.self] = now;
    m.seen[from] = now;
    match_ = std::move(m);
    say(moved_from ? Event{Event::Kind::moved, r.starter, {}, false, moved_from} : Event{Event::Kind::joined, r.starter});
}

std::vector<std::vector<std::uint8_t>> Node::take_outgoing() { return std::exchange(outgoing_, {}); }
std::vector<Event> Node::take_events() { return std::exchange(events_, {}); }
} // namespace dingosdk::domination::sync
