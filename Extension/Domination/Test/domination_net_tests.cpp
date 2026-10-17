#include "Extension/Domination/domination_sync.h"
#include "Extension/Domination/domination_wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <vector>

// Domination's multiplayer rounds (domination_sync.h) between many simulated players over a bad
// network: messages late, reordered, lost and doubled, every player's clock off and running at its
// own speed, players joining late, leaving, the host leaving, rounds started at once. In every case
// the players still in the round must end with the same round, the same board (owners and every
// claim), the same teams and the same results, and their clocks must agree while it runs.
using namespace dingosdk::domination;
using sync::Ms;

namespace {
int failures = 0, checks = 0;
long trace_player = -1; // `trace <seed> <player>`: that player's events, as they come
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

struct Settings {
    int players = 4;
    Ms latency_min = 30, latency_max = 80; // one way
    double loss = 0, duplicate = 0;
    Ms clock_spread = 0;     // each player's clock is off by up to this
    double drift = 0;        // and runs fast or slow by up to this share
    sync::Options options{};
    std::uint16_t duration = 60;
    double tricks_per_second = 0.4; // per player, while the round is on
    std::uint32_t seed = 1;
    std::size_t spots = 15;
};

struct Sim {
    struct Packet {
        Ms at;
        std::size_t to;
        Player from;
        std::vector<std::uint8_t> bytes;
    };
    Settings set;
    std::mt19937 rng;
    std::vector<Player> ids;
    std::vector<sync::Node> nodes;
    std::vector<Ms> offset;
    std::vector<double> rate;
    std::vector<bool> connected;
    std::size_t host = 0;
    bool host_is_player = true;
    std::vector<Packet> wire;
    Ms t = 0; // the true time
    Ms worst_clock_gap = 0;
    std::uint32_t next_round = 1000;
    std::function<void(Sim &)> script; // runs every step
    // A message to drop on its way (from, to, its bytes); none by default.
    std::function<bool(std::size_t, std::size_t, const std::vector<std::uint8_t> &)> drop;
    std::vector<bool> deaf; // in the session, but not on the map: hears and says nothing

    explicit Sim(Settings s) : set(s), rng(s.seed) {
        for (int i = 0; i < set.players; ++i) {
            ids.push_back(0x0110000100000000ull + 1000 + static_cast<Player>(i) * 7919); // host not the lowest id
            nodes.emplace_back(0xC0FFEEu, set.spots);
            std::uniform_int_distribution<Ms> off(-set.clock_spread, set.clock_spread);
            std::uniform_real_distribution<double> dr(-set.drift, set.drift);
            offset.push_back(set.clock_spread ? off(rng) : 0);
            rate.push_back(1.0 + (set.drift > 0 ? dr(rng) : 0));
            connected.push_back(true);
        }
        std::ranges::reverse(ids); // the host (index 0) has the highest id
        rosters();
    }
    Ms local(std::size_t i) const { return offset[i] + static_cast<Ms>(std::llround(static_cast<double>(t) * rate[i])); }
    void rosters() {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            sync::Roster r;
            if (connected[i]) {
                r.self = ids[i];
                r.host = host_is_player && connected[host] ? ids[host] : 0;
                for (std::size_t j = 0; j < nodes.size(); ++j)
                    if (j != i && connected[j]) r.others.push_back(ids[j]);
            }
            nodes[i].set_roster(r);
        }
    }
    void send(std::size_t from, std::vector<std::uint8_t> bytes) {
        std::uniform_real_distribution<double> p(0, 1);
        std::uniform_int_distribution<Ms> lat(set.latency_min, set.latency_max);
        if (!deaf.empty() && deaf[from]) return;
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (j == from || !connected[j] || (!deaf.empty() && deaf[j])) continue;
            if (drop && drop(from, j, bytes)) continue;
            if (p(rng) < set.loss) continue;
            wire.push_back({t + lat(rng), j, ids[from], bytes});
            if (p(rng) < set.duplicate) wire.push_back({t + lat(rng), j, ids[from], bytes});
        }
    }
    void step(Ms dt = 10) {
        t += dt;
        // deliver what is due, in order of arrival
        std::stable_sort(wire.begin(), wire.end(), [](const Packet &a, const Packet &b) { return a.at < b.at; });
        std::size_t done = 0;
        for (; done < wire.size() && wire[done].at <= t; ++done) {
            const auto &p = wire[done];
            if (connected[p.to]) nodes[p.to].receive(p.from, p.bytes, local(p.to));
        }
        wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(done));
        if (script) script(*this);
        std::uniform_real_distribution<double> p(0, 1);
        std::uniform_int_distribution<int> spot(0, static_cast<int>(set.spots) - 1);
        std::lognormal_distribution<float> score(6.0f, 0.8f);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!connected[i]) continue;
            auto &n = nodes[i];
            if (n.running() && p(rng) < set.tricks_per_second * static_cast<double>(dt) / 1000.0) n.claim(spot(rng), std::round(score(rng)), local(i));
            n.tick(local(i));
            for (auto &bytes : n.take_outgoing()) send(i, std::move(bytes));
            for (const auto &e : n.take_events())
                if (static_cast<long>(i) == trace_player)
                    std::printf("  %6.1f s  player %zu: event %d (round %u, value %lld), in %u%s%s\n", static_cast<double>(t) / 1000, i,
                                static_cast<int>(e.kind), e.round, static_cast<long long>(e.value), n.match() ? n.match()->id : 0,
                                n.match() && n.match()->finished ? ", over" : "", n.match() && n.match()->left ? ", left" : "");
        }
        // the clocks of the players in the same running round
        const sync::Match *first = nullptr;
        Ms first_ms = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto &m = nodes[i].match();
            if (!connected[i] || !m || m->finished || !m->multiplayer || local(i) - m->joined < 15000) continue;
            if (!first) first = &*m, first_ms = m->ms(local(i));
            else if (m->id == first->id) worst_clock_gap = std::max(worst_clock_gap, std::abs(static_cast<Ms>(m->ms(local(i))) - first_ms));
        }
    }
    void run_for(Ms ms) {
        for (Ms end = t + ms; t < end;) step();
    }
    std::uint32_t start(std::size_t who, bool solo = false) {
        const auto id = next_round++;
        nodes[who].start(local(who), set.options, set.duration, solo, id);
        for (auto &bytes : nodes[who].take_outgoing()) send(who, std::move(bytes));
        return id;
    }
    void disconnect(std::size_t i) {
        connected[i] = false;
        rosters();
    }
    void reconnect(std::size_t i) {
        connected[i] = true;
        rosters();
    }
};

bool same_board(const Board &a, const Board &b) {
    if (a.spots() != b.spots()) return false;
    for (std::size_t s = 0; s < a.spots(); ++s) {
        const int i = static_cast<int>(s);
        if (a.owner(i) != b.owner(i) || a.top(i) != b.top(i) || a.history(i) != b.history(i)) return false;
    }
    return true;
}
bool same_rows(const std::vector<Ranked> &a, const std::vector<Ranked> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].player != b[i].player || a[i].spots != b[i].spots || a[i].total != b[i].total || a[i].hold != b[i].hold || a[i].steals != b[i].steals)
            return false;
    return true;
}
// Everyone still connected is in round `id`, it is over for all of them, and they agree.
void agree(const Sim &sim, std::uint32_t id, const std::string &name) {
    const sync::Match *first = nullptr;
    int in = 0;
    for (std::size_t i = 0; i < sim.nodes.size(); ++i) {
        if (!sim.connected[i]) continue;
        const auto &m = sim.nodes[i].match();
        check(m.has_value() && m->id == id, name + ": player " + std::to_string(i) + " is in the round");
        if (!m || m->id != id) continue;
        check(m->finished, name + ": player " + std::to_string(i) + "'s round is over");
        ++in;
        if (!first) {
            first = &*m;
            continue;
        }
        check(same_board(m->board, first->board), name + ": player " + std::to_string(i) + " has the same board (owners and every claim)");
        check(same_rows(m->results.rows, first->results.rows), name + ": player " + std::to_string(i) + " has the same results");
        check(m->teams == first->teams, name + ": player " + std::to_string(i) + " has the same teams");
        check(m->ended_ms == first->ended_ms, name + ": player " + std::to_string(i) + " ended at the same time");
    }
    check(in >= 2, name + ": at least two players played");
    if (first) {
        int claims = 0;
        for (std::size_t s = 0; s < first->board.spots(); ++s) claims += static_cast<int>(first->board.history(static_cast<int>(s)).size());
        check(claims > 0, name + ": claims were made");
        std::printf("  %s: %d players agree; %d claims kept; clocks within %lld ms\n", name.c_str(), in, claims,
                    static_cast<long long>(sim.worst_clock_gap));
    }
}

void clean_round() {
    Sim sim({});
    const auto id = sim.start(0);
    sim.run_for(2000);
    for (std::size_t i = 1; i < sim.nodes.size(); ++i)
        check(sim.nodes[i].running() && sim.nodes[i].match()->id == id, "clean: player " + std::to_string(i) + " was taken into the round");
    sim.run_for(62000 + 25000);
    agree(sim, id, "clean");
    check(sim.worst_clock_gap <= 300, "clean: clocks agree within 300 ms");
}

void only_the_controller() {
    Sim sim({});
    const auto refused = sim.nodes[2].start(sim.local(2), {}, 60, false, 5);
    check(refused == sync::StartResult::not_controller, "only the host starts a round for everyone");
    check(sim.nodes[2].start(sim.local(2), {}, 60, true, 6) == sync::StartResult::started, "anyone may play alone outside the session's round");
    const auto id = sim.start(0);
    sim.run_for(3000);
    check(sim.nodes[2].running() && sim.nodes[2].match()->id == id && sim.nodes[2].match()->multiplayer,
          "a round of one's own gives way to the session's");
    // A guest's stop only takes them out; they are not pulled back in.
    sim.nodes[3].stop(sim.local(3));
    sim.run_for(12000);
    check(sim.nodes[3].match()->finished && sim.nodes[0].running(), "a guest who leaves is out, the round goes on");
    // Nor by starting a round of their own.
    check(sim.nodes[3].start(sim.local(3), {}, 30, true, 77) == sync::StartResult::started, "a guest who left plays alone");
    sim.run_for(12000);
    check(sim.nodes[3].match()->id == 77 && !sim.nodes[3].match()->multiplayer, "a round of their own isn't taken over by the round they left");
    sim.run_for(40000 + 25000);
    check(sim.nodes[0].match()->id == id, "the session's round went on");
    // The controller's stop ends it for everyone.
    const auto second = sim.start(0);
    sim.run_for(20000);
    sim.nodes[0].stop(sim.local(0));
    sim.run_for(3000);
    for (std::size_t i = 0; i < sim.nodes.size(); ++i)
        check(sim.nodes[i].match() && sim.nodes[i].match()->id == second && sim.nodes[i].match()->finished,
              "the host's stop ends round 2 for player " + std::to_string(i));
    sim.run_for(25000);
    agree(sim, second, "stopped");
}

void bad_network() {
    Settings s;
    s.players = 6;
    s.latency_min = 20;
    s.latency_max = 400; // messages overtake each other
    s.loss = 0.10;
    s.duplicate = 0.05;
    s.clock_spread = 10000;
    s.drift = 0.001;
    s.options.teams = true;
    s.duration = 120;
    s.seed = 7;
    Sim sim(s);
    sim.disconnect(5); // joins late
    const auto id = sim.start(0);
    bool joined = false, left = false;
    sim.script = [&](Sim &x) {
        if (!joined && x.t > 30000) joined = true, x.reconnect(5);
        if (!left && x.t > 70000) left = true, x.disconnect(4);
    };
    sim.run_for(130000 + 30000);
    agree(sim, id, "bad network, teams, late joiner, one gone");
    check(sim.worst_clock_gap <= 600, "bad network: clocks agree within 600 ms (" + std::to_string(sim.worst_clock_gap) + ")");
    const auto &m = *sim.nodes[0].match();
    int red = 0, blue = 0;
    for (const auto &[who, team] : m.teams) (team ? blue : red)++;
    check(std::abs(red - blue) <= 1 && red + blue == 6, "bad network: six players in two even teams");
    check(m.team_of(sim.ids[5]) >= 0, "bad network: the late joiner got a team");
}

void host_leaves() {
    Settings s;
    s.players = 5;
    s.seed = 3;
    s.loss = 0.05;
    Sim sim(s);
    const auto id = sim.start(0);
    sim.script = [&](Sim &x) {
        static bool gone = false;
        if (!gone && x.t > 25000) gone = true, x.host_is_player = false, x.disconnect(0);
    };
    sim.run_for(40000);
    // The next in line (the lowest id) now runs rounds; its stop ends this one.
    std::size_t next = 1;
    for (std::size_t i = 1; i < sim.nodes.size(); ++i)
        if (sim.ids[i] < sim.ids[next]) next = i;
    check(sim.nodes[next].controls(), "with the host gone, the lowest id runs rounds");
    sim.nodes[next].stop(sim.local(next));
    sim.run_for(25000);
    agree(sim, id, "host left");
}

void two_at_once() {
    Settings s;
    s.players = 4;
    s.seed = 11;
    Sim sim(s);
    // For a moment two players each think they run the session (one has not seen the host yet).
    sim.nodes[3].set_roster({sim.ids[3], 0, {sim.ids[1], sim.ids[2]}});
    const auto a = sim.start(0);
    const auto b = sim.nodes[3].start(sim.local(3), s.options, s.duration, false, 999); // a lower number
    for (auto &bytes : sim.nodes[3].take_outgoing()) sim.send(3, std::move(bytes));
    check(b == sync::StartResult::started, "two at once: both started");
    sim.rosters();
    sim.run_for(64000 + 25000);
    check(a > 999, "two at once: the host's round has the higher number");
    agree(sim, 999, "two rounds at once, the lower stands");
}

void big_board() {
    Settings s;
    s.players = 16;
    s.tricks_per_second = 2.0;
    s.duration = 180;
    s.loss = 0.02;
    s.latency_max = 200;
    s.seed = 5;
    Sim sim(s);
    const auto id = sim.start(0);
    sim.run_for(184000 + 25000);
    agree(sim, id, "16 players, a board in parts");
}

void rejoin_and_options() {
    Settings s;
    s.players = 3;
    s.options.teams = true;
    s.seed = 31;
    Sim sim(s);
    const auto id = sim.start(0);
    sim.run_for(3000);
    for (std::size_t i = 0; i < sim.nodes.size(); ++i)
        check(sim.nodes[i].match() && sim.nodes[i].match()->options == s.options, "every player has the round's options (" + std::to_string(i) + ")");
    sim.nodes[2].stop(sim.local(2));
    sim.run_for(12000);
    check(sim.nodes[2].match()->finished, "the guest left");
    sim.nodes[2].rejoin();
    sim.run_for(7000);
    check(sim.nodes[2].running() && sim.nodes[2].match()->id == id, "domination join puts a guest who left back into the round");
    sim.run_for(64000 + 25000);
    agree(sim, id, "rejoined");
    const auto &rows = sim.nodes[0].match()->results.rows;
    for (std::size_t i = 1; i < rows.size(); ++i)
        check(rows[i - 1].spots > rows[i].spots || (rows[i - 1].spots == rows[i].spots && rows[i - 1].total >= rows[i].total),
              "the results go by spots, then the total");
}

void bad_claims() {
    // Messages a misbehaving or broken client could send: claims outside the play, on spots the
    // park doesn't have, scores that aren't scores. None of them may change anyone's board.
    Settings s;
    s.players = 2;
    s.seed = 41;
    s.tricks_per_second = 0;
    Sim sim(s);
    const auto id = sim.start(0);
    sim.run_for(10000);
    const auto bad = [&](std::uint8_t spot, float score, std::uint32_t ms) {
        wire::BoardMessage b{0xC0FFEEu, id, {{spot, sim.ids[1], score, ms}}};
        sim.nodes[0].receive(sim.ids[1], wire::encode(b), sim.local(0));
    };
    bad(0, 500, 1000);                  // in the countdown
    bad(0, 500, 70000);                 // after the end
    bad(0, 500, 9000 + 60000);          // far in the future
    bad(static_cast<std::uint8_t>(s.spots), 500, 6000); // a spot the park doesn't have
    std::vector<std::uint8_t> nan_score = wire::encode(wire::BoardMessage{0xC0FFEEu, id, {{0, sim.ids[1], 1, 6000}}});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nan_score.data() + 13 + 1 + 8, &nan, 4);
    sim.nodes[0].receive(sim.ids[1], nan_score, sim.local(0));
    int claims = 0;
    for (std::size_t spot = 0; spot < s.spots; ++spot) claims += static_cast<int>(sim.nodes[0].match()->board.history(static_cast<int>(spot)).size());
    check(claims == 0, "claims outside the play, on unknown spots or with bad scores change nothing");
    bad(3, 500, 6000);
    check(sim.nodes[0].match()->board.owner(3) == sim.ids[1], "a good claim still counts");
    // Another version, another park: said once per player.
    auto other = wire::encode(wire::RoundMessage{0xC0FFEEu, 9, sim.ids[1], true, 0, 60, 0});
    other[1] = 1;
    sim.nodes[0].receive(sim.ids[1], other, sim.local(0));
    sim.nodes[0].receive(sim.ids[1], other, sim.local(0));
    sim.nodes[0].receive(sim.ids[1], wire::encode(wire::RoundMessage{0xBADu, 9, sim.ids[1], true, 0, 60, 0}), sim.local(0));
    int version = 0, park = 0;
    for (const auto &e : sim.nodes[0].take_events())
        version += e.kind == sync::Event::Kind::version, park += e.kind == sync::Event::Kind::park;
    check(version == 1 && park == 1, "another version or another park is said once per player");
    check(sim.nodes[0].match()->id == id, "and leaves the round alone");
}

// ARCHITECTURE.md's "two players, in a few lines", kept compiling and true.
void architecture_example() {
    const std::uint32_t park_hash = 0xC0FFEEu;
    const std::size_t spots = 20;
    const Player id_a = 0x0110000100000020, id_b = 0x0110000100000010;
    const std::uint32_t round_id = 7;
    Ms now_a = 0, now_b = 50000; // each side's own clock
    sync::Node a(park_hash, spots), b(park_hash, spots);
    a.set_roster({id_a, id_a, {id_b}});
    b.set_roster({id_b, id_a, {id_a}});
    a.start(now_a, {}, 300, false, round_id);
    const auto exchange = [&] {
        a.tick(now_a);
        for (auto &bytes : a.take_outgoing()) b.receive(id_a, bytes, now_b);
        b.tick(now_b);
        for (auto &bytes : b.take_outgoing()) a.receive(id_b, bytes, now_a);
    };
    exchange();
    check(b.running() && b.match()->id == round_id, "the example: b joins a's round at once");
    for (int i = 0; i < 600; ++i, now_a += 10, now_b += 10) exchange(); // 6 s: past the countdown
    const auto claimed = b.claim(4, 250, now_b);
    for (int i = 0; i < 100; ++i, now_a += 10, now_b += 10) exchange();
    check(claimed && a.match()->board.owner(4) == id_b, "the example: b's trick owns the spot on a's side too");
    check(!a.match()->standings(now_a).rows.empty(), "the example: standings to show");
}

// A stop that one player never hears the first time (lost on the way): it goes out again with the
// boards after the end, and that player's copy ends where everyone's did.
void lost_stop() {
    Settings s;
    s.players = 4;
    s.seed = 51;
    s.tricks_per_second = 1.0;
    Sim sim(s);
    int dropped = 0;
    sim.drop = [&](std::size_t from, std::size_t to, const std::vector<std::uint8_t> &bytes) {
        const auto m = wire::decode(bytes);
        const auto *r = m ? std::get_if<wire::RoundMessage>(&*m) : nullptr;
        if (r && !r->running && from == 0 && to == 2 && dropped == 0) return ++dropped, true;
        return false;
    };
    const auto id = sim.start(0);
    sim.run_for(30000);
    sim.nodes[0].stop(sim.local(0));
    sim.run_for(1000);
    check(dropped == 1 && sim.nodes[2].running(), "the first stop never reached player 2");
    sim.run_for(3000);
    check(!sim.nodes[2].running(), "the stop went out again and reached player 2");
    sim.run_for(30000 + 25000);
    agree(sim, id, "a lost stop");
}

// The host stops 0.2 s before the end, while other clocks are up to 0.3 s ahead: copies that
// already ended move their end back to the stop's moment.
void stop_at_the_end() {
    Settings s;
    s.players = 4;
    s.seed = 61;
    s.tricks_per_second = 2.0;
    s.duration = 40;
    Sim sim(s);
    sim.offset[1] += 300, sim.offset[2] += 250, sim.offset[3] -= 200; // within the clocks' slack
    const auto id = sim.start(0);
    // The first stop to player 1 is lost: it ends on its own, and the stop comes again after.
    int dropped = 0;
    sim.drop = [&](std::size_t from, std::size_t to, const std::vector<std::uint8_t> &bytes) {
        const auto m = wire::decode(bytes);
        const auto *r = m ? std::get_if<wire::RoundMessage>(&*m) : nullptr;
        if (r && !r->running && !r->left && from == 0 && to == 1 && dropped == 0) return ++dropped, true;
        return false;
    };
    while (sim.nodes[0].match()->ms(sim.local(0)) < 44000 - 200) sim.step();
    sim.nodes[0].stop(sim.local(0));
    sim.run_for(1000);
    check(dropped == 1 && sim.nodes[1].match()->finished && sim.nodes[1].match()->ended_ms == 44000, "player 1 ended on its own (the stop lost)");
    sim.run_for(30000);
    check(sim.nodes[1].match()->ended_ms < 44000, "the stop that came after moved player 1's end back");
    agree(sim, id, "a stop at the very end");
}

// Teams that reach a player only after their copy ended (every teams message to them lost while
// it ran): the teams said again after the end still count, and the results agree.
void teams_at_the_end() {
    Settings s;
    s.players = 5;
    s.seed = 71;
    s.options.teams = true;
    s.duration = 40;
    Sim sim(s);
    bool lost_some = false;
    sim.drop = [&](std::size_t, std::size_t to, const std::vector<std::uint8_t> &bytes) {
        const auto m = wire::decode(bytes);
        const bool teams = m && std::holds_alternative<wire::TeamsMessage>(*m);
        if (teams && to == 2 && sim.nodes[2].running()) return lost_some = true;
        return false;
    };
    const auto id = sim.start(0);
    sim.run_for(44000 + 100);
    check(lost_some && sim.nodes[2].match()->finished && sim.nodes[2].match()->team_of(sim.ids[2]) < 0, "player 2's copy ended without the teams");
    sim.run_for(30000);
    check(sim.nodes[2].match()->team_of(sim.ids[2]) >= 0, "the teams said after the end reached player 2");
    agree(sim, id, "teams after the end");
}

// The controller stops the round once and changes maps at once (it says nothing more), and that
// one stop is lost on the way to a player: the players who heard it say it again, and that player
// ends where everyone did.
void stop_then_away() {
    Settings s;
    s.players = 4;
    s.seed = 91;
    s.tricks_per_second = 1.0;
    Sim sim(s);
    sim.deaf.assign(sim.nodes.size(), false);
    bool lost = false;
    sim.drop = [&](std::size_t from, std::size_t to, const std::vector<std::uint8_t> &bytes) {
        const auto m = wire::decode(bytes);
        const auto *r = m ? std::get_if<wire::RoundMessage>(&*m) : nullptr;
        if (r && !r->running && from == 0 && to == 3) return lost = true;
        return false;
    };
    const auto id = sim.start(0);
    sim.run_for(25000);
    sim.nodes[0].stop(sim.local(0));
    sim.step();
    sim.deaf[0] = true; // gone to another map: hears and says nothing more
    sim.run_for(30000);
    check(lost, "the controller's one stop never reached player 3");
    const sync::Match &m0 = *sim.nodes[0].match();
    for (std::size_t i = 1; i < sim.nodes.size(); ++i) {
        const auto &m = *sim.nodes[i].match();
        check(m.finished && m.ended_ms == m0.ended_ms && same_board(m.board, m0.board) && same_rows(m.results.rows, m0.results.rows),
              "player " + std::to_string(i) + " ended where the controller stopped it (relayed)");
    }
    (void)id;
}

// The host starts a round, then goes to another map (still in the session, deaf to the round):
// once it is gone from the round, its authority is the lowest id in it, a guest. When that guest
// leaves, the next lowest takes over at once, and a player who arrives then is still taken in.
void authority_leaves() {
    Settings s;
    s.players = 5;
    s.seed = 81;
    Sim sim(s);
    sim.deaf.assign(sim.nodes.size(), false);
    std::size_t lowest = 1, second = 0;
    for (std::size_t i = 1; i < sim.nodes.size(); ++i)
        if (sim.ids[i] < sim.ids[lowest]) lowest = i;
    for (std::size_t i = 1; i < sim.nodes.size(); ++i)
        if (i != lowest && (second == 0 || sim.ids[i] < sim.ids[second])) second = i;
    std::size_t late = 1;
    while (late == lowest || late == second) ++late;
    sim.deaf[late] = true; // arrives later
    const auto id = sim.start(0);
    sim.run_for(10000);
    sim.deaf[0] = true; // the host goes to another map
    sim.run_for(30000);
    check(sim.nodes[second].authority(sim.local(second)) == sim.ids[lowest], "with the host away, the lowest id in the round is its authority");
    sim.nodes[lowest].stop(sim.local(lowest));
    sim.run_for(1000);
    check(sim.nodes[second].running() && sim.nodes[second].authority(sim.local(second)) == sim.ids[second],
          "the next lowest id takes over the moment the authority leaves");
    sim.deaf[late] = false;
    sim.run_for(7000);
    check(sim.nodes[late].running() && sim.nodes[late].match()->id == id, "a player who arrives after the authority left is taken in");
}

void many_seeds() {
    // The same checks over many random networks.
    for (std::uint32_t seed = 100; seed < 120; ++seed) {
        Settings s;
        s.players = 3 + static_cast<int>(seed % 6);
        s.latency_min = 10;
        s.latency_max = 50 + (seed % 5) * 120;
        s.loss = (seed % 4) * 0.05;
        s.duplicate = (seed % 3) * 0.05;
        s.clock_spread = (seed % 2) ? 30000 : 0;
        s.drift = (seed % 3) * 0.0005;
        s.options.teams = seed % 2 == 0;
        s.duration = 45;
        s.seed = seed;
        Sim sim(s);
        const auto id = sim.start(0);
        sim.run_for(49000 + 30000);
        agree(sim, id, "seed " + std::to_string(seed));
    }
}
// A player cut off across a stop comes back, still running the old round on their own clock (and
// saying it, being the only one they heard), just as the next round starts: nobody is pulled back
// into the old round, and they end up in the new one with everyone else.
void stale_round_comes_back() {
    Settings s;
    s.players = 4;
    s.seed = 77;
    s.duration = 60;
    s.tricks_per_second = 0.0; // nothing claimed in the new round early on: the merge rule could apply
    Sim sim(s);
    sim.deaf.assign(sim.nodes.size(), false);
    const auto old = sim.start(0);
    sim.run_for(5000);
    sim.deaf[3] = true;
    sim.run_for(30000); // player 3 hears nobody for 25 s: their own authority, still running
    sim.nodes[0].stop(sim.local(0));
    sim.run_for(15000);
    const auto next = sim.start(0);
    sim.run_for(1000);
    check(sim.nodes[3].running() && sim.nodes[3].match()->id == old, "stale: player 3 still runs the old round");
    sim.deaf[3] = false;
    sim.set.tricks_per_second = 0.4;
    sim.run_for(64000 + 65000);
    for (std::size_t i = 0; i < sim.nodes.size(); ++i)
        check(sim.nodes[i].match() && sim.nodes[i].match()->id == next, "stale: player " + std::to_string(i) + " ends in the new round");
    agree(sim, next, "stale round comes back");
    (void)old;
}

// One random round with random trouble: players dropping out of the session and coming back,
// leaving the round and (mostly) rejoining it, off on another map a while (hearing and saying
// nothing), the round stopped early; odd seeds start a second round soon after the first ends,
// while some of that may still be going on. Every player still in the (last) round at the end
// must agree.
// Returns whether it did (checks counted as usual).
bool fuzz_round(std::uint32_t seed, bool loud) {
    std::mt19937 r(seed);
    const auto pick = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(r); };
    Settings s;
    s.players = pick(2, 10);
    s.latency_min = pick(5, 60);
    s.latency_max = s.latency_min + pick(0, 600);
    s.loss = pick(0, 30) / 100.0;
    s.duplicate = pick(0, 10) / 100.0;
    s.clock_spread = pick(0, 1) ? pick(0, 60000) : 0;
    s.drift = pick(0, 10) / 10000.0;
    s.options.teams = pick(0, 1) == 1;
    s.duration = static_cast<std::uint16_t>(pick(30, 90));
    s.tricks_per_second = pick(1, 15) / 10.0;
    s.seed = seed;
    Sim sim(s);
    sim.deaf.assign(sim.nodes.size(), false);
    const int before = failures;
    const Ms length = (s.duration + 4) * 1000;
    struct Trouble {
        Ms from, to;
        int kind; // 0 out of the session, 1 out of the round, 2 another map (3: the host out of the session)
        std::size_t who;
        bool back;  // rejoins the round (kind 1)
        bool solo = false; // plays a round of their own before rejoining (kind 1)
        bool ended = false; // kind 1: leaving ended the round for everyone (they ran rounds then)
        int state = 0; // 0 to come, 1 going on, 2 over
    };
    std::vector<Trouble> trouble;
    for (int n = pick(0, 6); n-- > 0;) {
        const Ms from = pick(500, static_cast<int>(length) - 500);
        trouble.push_back({from, from + pick(1000, 20000), pick(0, 2), static_cast<std::size_t>(pick(1, s.players - 1)), pick(0, 3) > 0});
        trouble.back().solo = pick(0, 1) == 1;
    }
    const bool stopped = pick(0, 3) == 0;
    const Ms stop_at = pick(5000, static_cast<int>(length) + 1000);
    bool stop_done = false;
    if (seed % 3 == 0) { // the host drops out a while (the rounds' control moves, and back)
        const Ms from = pick(1000, static_cast<int>(length));
        trouble.push_back({from, from + pick(2000, 30000), 3, 0, true});
    }
    const bool second = seed % 2 == 1;
    const Ms second_at = length + pick(2000, 30000);
    std::uint32_t id2 = 0;
    std::string story = std::format("seed {}: {} players, {}-{} ms, loss {:.2f}, dup {:.2f}, spread {}, drift {:.4f}, {}{} s",
                                    seed, s.players, s.latency_min, s.latency_max, s.loss, s.duplicate, s.clock_spread, s.drift,
                                    s.options.teams ? "teams " : "", s.duration);
    const auto id = sim.start(0);
    const Ms began = sim.t;
    sim.script = [&](Sim &sm) {
        const Ms at = sm.t - began;
        if (second && !id2 && at >= second_at && sm.connected[0]) { // (the host back, if it was out)
            id2 = sm.start(0);
            story += std::format("; round 2 at {}", at);
        }
        if (stopped && !stop_done && at >= stop_at) {
            stop_done = true;
            sm.nodes[0].stop(sm.local(0));
            story += std::format("; stopped at {}", at);
        }
        for (auto &e : trouble) {
            if (e.state == 0 && at >= e.from) {
                e.state = 1;
                if ((e.kind == 0 || e.kind == 3) && sm.connected[e.who]) sm.disconnect(e.who);
                else if (e.kind == 1) {
                    sm.nodes[e.who].stop(sm.local(e.who));
                    e.ended = sm.nodes[e.who].match() && !sm.nodes[e.who].match()->left;
                }
                else if (e.kind == 2) sm.deaf[e.who] = true;
                story += std::format("; {} {} at {}", e.kind == 0 || e.kind == 3 ? "out" : e.kind == 1 ? "leaves" : "away", e.who, at);
            } else if (e.state == 1 && at >= e.to) {
                e.state = 2;
                if (e.kind == 0 || e.kind == 3) sm.reconnect(e.who);
                else if (e.kind == 1 && e.back) {
                    if (e.solo) sm.start(e.who, true);
                    sm.nodes[e.who].rejoin();
                    story += std::format("; {} back{} at {}", e.who, e.solo ? " from a solo round" : "", at);
                }
                else if (e.kind == 2) sm.deaf[e.who] = false;
            }
        }
    };
    sim.run_for(length + 20000 + 45000);
    if (second) {
        while (!id2) sim.step();
        sim.run_for(length + 65000);
    }
    const auto want = second ? id2 : id;
    // When the round really ended (a stop, or a stand-in controller's leave, may have ended it early).
    std::uint32_t really_ended = static_cast<std::uint32_t>(length);
    for (std::size_t i = 0; i < sim.nodes.size(); ++i)
        if (const auto &m = sim.nodes[i].match(); sim.connected[i] && m && m->id == want && !m->left && m->finished)
            really_ended = std::min(really_ended, m->ended_ms);
    // Everyone still in it agrees.
    const sync::Match *first = nullptr;
    std::size_t first_i = 0;
    int in = 0;
    for (std::size_t i = 0; i < sim.nodes.size(); ++i) {
        const auto &m = sim.nodes[i].match();
        // In a second round, every player in the session is in it (nobody left that one), unless
        // the host started it alone (a round of their own: nobody else is taken into it).
        const auto &host_round = sim.nodes[0].match();
        if (second && sim.connected[i] && host_round && host_round->multiplayer)
            check(m && m->id == want && !m->left,
                  story + std::format(": player {} is in round 2 (in {}{}{})", i, m ? m->id : 0, m && m->finished ? ", over" : "", m && m->left ? ", left" : ""));
        // A player who left and asked back in well before the end is in it again.
        if (!second && sim.connected[i]) {
            const Trouble *last = nullptr;
            for (const auto &e : trouble)
                if (e.who == i && e.kind == 1 && (!last || e.from > last->from)) last = &e;
            // (Leaving as a player, not stopping it as the one running rounds; nothing else after.)
            const bool alone = last && std::ranges::none_of(trouble, [&](const Trouble &e) { return e.who == i && &e != last && e.to > last->from; });
            // (And someone stayed in it, with no trouble, from then to the end, to hear it from.)
            const auto steady = [&](std::size_t j) {
                const auto &mj = sim.nodes[j].match();
                return j != i && sim.connected[j] && mj && mj->id == want && !mj->left &&
                       std::ranges::none_of(trouble, [&](const Trouble &e) { return e.who == j && e.to > last->to && e.from < static_cast<Ms>(really_ended); });
            };
            bool heard = false;
            for (std::size_t j = 0; last && j < sim.nodes.size(); ++j) heard = heard || steady(j);
            if (last && alone && heard && last->back && !last->ended && last->to + 8000 < static_cast<Ms>(really_ended))
                check(m && m->id == want && !m->left, story + std::format(": player {} is back in the round", i));
        }
        if (!sim.connected[i] || !m || m->id != want || m->left) continue;
        check(m->finished, story + std::format(": player {}'s round is over", i));
        ++in;
        if (!first) {
            first = &*m, first_i = i;
            continue;
        }
        const auto who = std::format(": player {} and player {}", i, first_i);
        check(same_board(m->board, first->board), story + who + " have the same board");
        check(same_rows(m->results.rows, first->results.rows), story + who + " have the same results");
        check(m->teams == first->teams, story + who + " have the same teams");
        check(m->ended_ms == first->ended_ms, story + who + std::format(" ended at the same time ({} and {})", m->ended_ms, first->ended_ms));
    }
    check(in >= 1 && sim.nodes[0].match() && sim.nodes[0].match()->id == want, story + ": the host is in its round");
    if (loud) std::printf("  %s: %d agree\n", story.c_str(), in);
    return failures == before;
}
} // namespace

int main(int argc, char **argv) {
    // `trace <seed> <player>`: one fuzz round, telling one player's events (Event::Kind numbers).
    if (argc > 3 && std::strcmp(argv[1], "trace") == 0) {
        trace_player = std::strtol(argv[3], nullptr, 10);
        return fuzz_round(static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)), true) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    // `fuzz <n> [first seed]`: n random rounds with random trouble, the failing seeds told.
    if (argc > 2 && std::strcmp(argv[1], "fuzz") == 0) {
        const auto n = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
        const auto from = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 1000u;
        int bad = 0;
        for (std::uint32_t seed = from; seed < from + n; ++seed)
            if (!fuzz_round(seed, false)) ++bad;
        std::printf("fuzz: %d of %u rounds disagreed (%d of %d checks failed)\n", bad, n, failures, checks);
        return bad ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    clean_round();
    only_the_controller();
    bad_network();
    host_leaves();
    two_at_once();
    big_board();
    rejoin_and_options();
    bad_claims();
    architecture_example();
    lost_stop();
    stop_at_the_end();
    teams_at_the_end();
    stop_then_away();
    authority_leaves();
    stale_round_comes_back();
    many_seeds();
    for (std::uint32_t seed = 1000; seed < 1012; ++seed) fuzz_round(seed, true);
    std::printf(failures ? "%d of %d checks failed\n" : "all %d checks passed\n", failures ? failures : checks, checks);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
