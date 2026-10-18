#include "Extension/Domination/domination_model.h"
#include "Extension/Domination/domination_parks.h"
#include "Extension/Domination/domination_wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// Domination's rules apart from the game. Argument (optional): a real park file to check too.
using namespace dingosdk;
using namespace dingosdk::domination;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
Json small_park() {
    return Json::parse(R"({"schema":1,"level":"Levels/Test/Test","park":"t","spots":[
        {"id":"box","name":"Box","shape":"polygon","points":[[0,0],[10,0],[10,10],[0,10]],"y":[-1,5],"marker":[5,6,5]},
        {"id":"rail","shape":"capsule","a":[20,0],"b":[30,0],"radius":1,"y":[0,4],"marker":[25,5,0]},
        {"id":"next","shape":"polygon","points":[[10,0],[15,0],[15,10],[10,10]],"y":[-1,5],"marker":[12,6,5]}]})");
}
bool throws(const char *text) {
    try {
        (void)parse_park(Json::parse(text));
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

void park_tests() {
    const auto park = parse_park(small_park());
    check(park.level == "levels/test/test", "the level is kept in lower case");
    check(park.spots.size() == 3 && park.spots[1].capsule, "three spots, the second a capsule");
    check(spot_at(park, {5, 0, 5}) == 0, "inside the box");
    check(spot_at(park, {5, 6, 5}) == -1, "above the box's height range");
    check(spot_at(park, {-0.1f, 0, 5}) == -1, "just outside the box");
    check(spot_at(park, {25, 1, 0.9f}) == 1 && spot_at(park, {25, 1, 1.1f}) == -1, "the rail's radius");
    check(spot_at(park, {30.9f, 1, 0}) == 1 && spot_at(park, {31.1f, 1, 0}) == -1, "the rail's round end");
    check(spot_at(park, {12, 0, 5}) == 2, "the spot sharing the box's edge");
    check(spot_at(park, {10, 0, 5}) >= 0, "a point on a shared edge belongs to one of them");
    check(throws(R"({"schema":1,"level":"l","spots":[
        {"id":"a","shape":"polygon","points":[[0,0],[10,0],[10,10],[0,10]],"y":[0,5],"marker":[0,0,0]},
        {"id":"b","shape":"polygon","points":[[9,0],[20,0],[20,10],[9,10]],"y":[0,5],"marker":[0,0,0]}]})"),
          "overlapping spots are refused");
    check(!throws(R"({"schema":1,"level":"l","spots":[
        {"id":"a","shape":"polygon","points":[[0,0],[10,0],[10,10],[0,10]],"y":[0,5],"marker":[0,0,0]},
        {"id":"b","shape":"polygon","points":[[9,0],[20,0],[20,10],[9,10]],"y":[6,9],"marker":[0,0,0]}]})"),
          "spots at different heights may share ground");
    check(throws(R"({"schema":2,"level":"l","spots":[]})"), "another schema is refused");
    check(throws(R"({"schema":1,"level":"l","spots":[{"id":"a","shape":"polygon","points":[[0,0],[600,0],[600,10]],"y":[0,1],"marker":[0,0,0]}]})"),
          "a spot more than 500 m across is refused");
    check(throws(R"({"schema":1,"level":"l","spots":[{"id":"a","shape":"capsule","a":[1e6,0],"b":[1e6,5],"radius":1,"y":[0,1],"marker":[0,0,0]}]})"),
          "a spot farther than 100 km is refused");
    check(throws(R"({"schema":1,"level":"l","spots":[{"id":"a","shape":"cube","y":[0,1],"marker":[0,0,0]}]})"), "an unknown shape is refused");
    check(throws(R"({"schema":1,"level":"l","spots":[{"id":"a","shape":"polygon","points":[[0,0],[1,0]],"y":[0,1],"marker":[0,0,0]}]})"),
          "a polygon needs three points");
    check(throws(R"({"schema":1,"level":"l","spots":[
        {"id":"a","shape":"capsule","a":[0,0],"b":[1,0],"radius":1,"y":[0,1],"marker":[0,0,0]},
        {"id":"a","shape":"capsule","a":[9,0],"b":[10,0],"radius":1,"y":[0,1],"marker":[0,0,0]}]})"),
          "two spots with one id are refused");
}

// README.md's park file example loads.
void readme_park_test() {
    const auto park = parse_park(Json::parse(R"park({
  "schema": 1,
  "level": "levels/game/dingolevel_reskate_skate3improved/dingolevel_reskate_skate3improved",
  "park": "Red-bowl park", "map": "Skate 3 Improved", "package": "333-Skate_3_Improved",
  "spots": [
    {"id": "funbox", "name": "Funbox", "kind": "box", "shape": "polygon",
     "points": [[364.97, 389.06], [345.07, 394.39], [342.43, 384.54], [362.33, 379.21]],
     "y": [0.85, 9.35], "marker": [353.7, 6.85, 386.8]},
    {"id": "top-rail", "name": "Long rail", "kind": "rail", "shape": "capsule",
     "a": [370.5, 384.3], "b": [397.5, 379.2], "radius": 1.8,
     "y": [2.25, 9.06], "marker": [384.01, 6.56, 381.75]}
  ],
  "starts": [{"position": [343.5, 2.55, 469.2], "heading": 160}]
})park"));
    check(park.spots.size() == 2 && park.starts.size() == 1 && spot_at(park, {353.7f, 4.3f, 386.8f}) == 0 && spot_at(park, {384.0f, 4.1f, 381.8f}) == 1,
          "the README's park file loads");
    check(park.name == "Red-bowl park" && park.map == "Skate 3 Improved" && park.package == "333-Skate_3_Improved",
          "a park names its map and the map mod's package");
    check(parse_park(small_park()).map.empty() && parse_park(small_park()).package.empty(), "map and package are optional (the game's own map)");
}

void even_out_tests() {
    std::vector<float> spike{0, 0, 0, 3, 0, 0, 0.5f, 0};
    even_out(spike);
    check(spike == std::vector<float>{0, 0, 0, 0, 0, 0, 0.5f, 0}, "a point on top of a wall comes down; a small step stays");
    std::vector<float> two{1, 1, -4, -4, 1, 1};
    even_out(two);
    check(two == std::vector<float>{1, 1, 1, 1, 1, 1}, "two points over a drop come up");
    std::vector<float> lip{5, 0, 5, 0, 5, 0, 5, 5};
    even_out(lip);
    check(std::ranges::all_of(lip, [](float h) { return h == 5; }), "an edge zigzagging over a bowl's lip stays on the deck");
    std::vector<float> step{0, 0, 0, 0, 3, 3, 3, 3};
    even_out(step);
    check(step == std::vector<float>{0, 0, 0, 0, 3, 3, 3, 3}, "a real step (a box's side) stays");
    const float none = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> gaps{none, 2, none, none, 5, none};
    fill_in(gaps, -1);
    check(gaps == std::vector<float>{3, 2, 3, 4, 5, 4}, "heights not found: straight between the found ones, round the loop");
    std::vector<float> lone{none, 7, none};
    fill_in(lone, -1);
    check(lone == std::vector<float>{7, 7, 7}, "one height found: all of them");
    std::vector<float> nothing{none, none};
    fill_in(nothing, -1);
    check(nothing == std::vector<float>{-1, -1}, "none found: the fallback");
    std::vector<float> wrap{4, 0, 0, 0, 0};
    even_out(wrap);
    check(wrap[0] == 0, "round the loop's start too");
}

void line_spot_tests() {
    LineSpots line;
    line.reset(3);
    for (int i = 0; i < 30; ++i) line.add(0, 1.0 / 60);  // 0.5 s on spot 0
    for (int i = 0; i < 60; ++i) line.add(2, 1.0 / 60);  // 1.0 s on spot 2
    for (int i = 0; i < 240; ++i) line.add(-1, 1.0 / 60); // 4 s on no spot
    check(line.winner(0.4) == 2, "the spot with the most time wins, time off spots aside");
    check(line.winner(1.5) == -1, "no spot reaches the minimum");
    line.add(1, 5.0);
    check(line.seconds(1) == 0, "a hitch longer than a second is not counted");
    line.reset(3);
    check(line.winner(0) == -1, "an empty line has no spot");
}

// One tick of score HUD values: trick on, line score, multiplier, spot, slam, wipeout.
TrickSignals tick(bool action, float score, float multiplier, int spot, bool slam = false, bool wipeout = false) {
    TrickSignals in;
    in.action = action;
    in.line_score = score;
    in.multiplier = multiplier;
    in.spot = spot;
    in.slam = slam;
    in.wipeout = wipeout;
    in.dt = 1.0 / 60;
    return in;
}
TrickEvent feed(TrickWatch &watch, const TrickSignals &in, int ticks) {
    TrickEvent last;
    for (int i = 0; i < ticks; ++i) {
        const auto e = watch.update(in);
        if (e.kind != TrickEvent::Kind::none) last = e;
    }
    return last;
}

void trick_tests() {
    using K = TrickEvent::Kind;
    { // off a ledge into the open: the trick's time is over no spot, it lands on none, so it counts for
      // the spot left just before it began; left too long before, it counts for none
        TrickWatch w(3);
        feed(w, tick(false, 0, 1, 2), 30);  // on spot 2 (half a second at 60 ticks)
        feed(w, tick(false, 0, 1, -1), 20); // off it a third of a second
        feed(w, tick(true, 0, 1, -1), 25);  // the trick, all in the open
        w.update(tick(false, 0, 1, -1));
        const auto e = w.update(tick(false, 300, 1, -1));
        check(e.kind == K::landed && e.spot == 2, "a trick off a spot into the open counts for the spot it left");
        feed(w, tick(false, 300, 1, -1), 90); // 1.5 s in the open
        feed(w, tick(true, 300, 1, -1), 25);
        w.update(tick(false, 300, 1, -1));
        const auto late = w.update(tick(false, 500, 1, -1));
        check(late.kind == K::landed && late.spot == -1, "a trick begun long after leaving a spot counts for none");
    }
    { // the first trick of a line, on spot 1 (as in play: the score rises one tick after the trick ends)
        TrickWatch w(3);
        feed(w, tick(false, 0, 0, -1), 5);
        feed(w, tick(true, 0, 0, 1), 30);
        check(w.in_trick() && w.trick_spot() == 1, "a trick going on, on spot 1");
        check(w.update(tick(false, 0, 0, 1)).kind == K::none, "the trick's points are not in yet");
        const auto e = w.update(tick(false, 11, 1.5f, 1));
        check(e.kind == K::landed && e.spot == 1 && std::abs(e.points - 11) < 0.01f, "landed: 11 points on spot 1");
        // A second trick in the same line: the multiplier's rise scales only its own points.
        feed(w, tick(true, 11, 1.5f, 1), 20);
        w.update(tick(false, 11, 1.5f, 1));
        const auto f = w.update(tick(false, 708, 2.0f, 1));
        const float points = (708 / 2.0f - 11 / 1.5f) * 2.0f;
        check(f.kind == K::landed && f.spot == 1 && std::abs(f.points - points) < 0.01f, "a second trick scores its own points, not the line's");
        // A trick on another spot counts there, with its own points.
        feed(w, tick(true, 708, 2.0f, 2), 20);
        w.update(tick(false, 708, 2.0f, 2));
        const auto g = w.update(tick(false, 958, 2.0f, 2));
        check(g.kind == K::landed && g.spot == 2 && std::abs(g.points - 250) < 0.01f, "a trick on another spot counts there");
    }
    { // where a trick counts: the spot it spent the most time on, not where it landed
        TrickWatch w(3);
        feed(w, tick(false, 100, 1, -1), 2);
        feed(w, tick(true, 100, 1, 0), 40);  // a grind on spot 0
        feed(w, tick(true, 100, 1, -1), 10); // off its end
        const auto e = feed(w, tick(false, 400, 1, -1), 2);
        check(e.kind == K::landed && e.spot == 0 && std::abs(e.points - 300) < 0.01f, "the trick counts for the spot it was done on");
    }
    { // a trick on no spot (begun well after leaving one) counts for no spot
        TrickWatch w(3);
        feed(w, tick(false, 0, 1, 1), 2);
        feed(w, tick(true, 0, 1, 1), 10);
        feed(w, tick(false, 200, 1, 1), 2);
        feed(w, tick(false, 200, 1, -1), 90); // 1.5 s in the open
        feed(w, tick(true, 200, 1, -1), 10);
        const auto e = feed(w, tick(false, 260, 1, -1), 2);
        check(e.kind == K::landed && e.spot == -1, "a trick on no spot counts for none");
    }
    { // a slam: the trick going on scores nothing
        TrickWatch w(3);
        feed(w, tick(false, 0, 1, 1), 2);
        feed(w, tick(true, 0, 1, 1), 10);
        feed(w, tick(false, 300, 1, 1), 2);
        feed(w, tick(true, 300, 1, 1), 10);
        const auto e = w.update(tick(true, 300, 1, 1, true));
        check(e.kind == K::broken && !w.in_trick(), "a slam ends the trick going on");
        const auto after = feed(w, tick(false, 0, 0, 1), 30);
        check(after.kind == K::none, "the slammed trick scores nothing");
        const auto wipe = feed(w, tick(false, 0, 0, 1, false, true), 2);
        check(wipe.kind == K::none, "a wipeout with nothing going on says nothing");
    }
    { // a trick whose points never come scores nothing, and the next trick is still read
        TrickWatch w(3, 5);
        feed(w, tick(false, 50, 1, 0), 2);
        feed(w, tick(true, 50, 1, 0), 10);
        const auto none = feed(w, tick(false, 50, 1, 0), 10);
        feed(w, tick(true, 50, 1, 0), 10);
        const auto e = feed(w, tick(false, 90, 1, 0), 2);
        check(none.kind == K::none && e.kind == K::landed && std::abs(e.points - 40) < 0.01f, "a trick worth nothing is skipped");
    }
}

// A spot's claims: who owns it, a steal, a new best, a lower score.
void claim_tests() {
    const auto park = parse_park(small_park());
    constexpr Player me = 2, other = 3;
    Board board(park.spots.size());
    board.claim(0, other, 1000, 4000);
    check(board.owner(0) == other && board.owner(1) == 0, "a claim owns its spot; the others are open");
    auto o = board.claim(0, me, 1000, 5000);
    check(!o.taken() && o.before == other && o.before_score == 1000, "an equal score claimed later does not take the spot");
    o = board.claim(0, me, 1001, 6000);
    check(o.taken() && board.owned(me) == 1, "a higher score takes the spot");
    o = board.claim(0, me, 900, 7000);
    check(!o.taken() && !o.improved() && !o.best && board.best(0, me) == 1001, "a lower score changes nothing");
    o = board.claim(0, me, 1500, 8000);
    check(o.improved() && o.before_score == 1001, "a better trick on your own spot is a new best");
    o = board.claim(1, me, 1, 9000);
    check(o.taken() && o.before == 0, "an open spot is taken by any score");
    check(!board.claim(-1, me, 500, 0).best && !board.claim(2, me, 0, 0).best && !board.claim(5, me, 10, 0).best,
          "no spot, or no score, claims nothing");
    check(board.owned(me) == 2 && board.owned_total(me) == 1501, "spots owned and their total");
}

void board_tests() {
    const auto park = parse_park(small_park());
    constexpr Player a = 0x0110000100000010, b = 0x0110000100000020, c = 0x0110000100000030;
    Board board(park.spots.size());
    {
        // A higher claim heard first, then an earlier lower one: not a best, but kept (the replay
        // changes: hold times, steals); heard again, nothing new.
        Board late(park.spots.size());
        late.claim(0, a, 900, 5000);
        const auto earlier = late.claim(0, a, 400, 2000);
        check(!earlier.best && earlier.kept && late.history_size(0) == 2, "an earlier, lower claim heard late is kept, not a best");
        check(!late.claim(0, a, 400, 2000).kept, "the same claim again keeps nothing new");
        check(!late.claim(0, a, 300, 6000).kept, "a later, lower claim is not kept");
        // The same claims in any order: the same history (a later, lower claim heard first stops
        // being a best when an earlier, higher one comes).
        const std::vector<std::tuple<Player, float, std::uint32_t>> claims{{a, 600.0f, 9000u}, {b, 500.0f, 3000u}, {a, 800.0f, 4000u}, {a, 700.0f, 1000u}, {b, 650.0f, 7000u}};
        std::vector<std::size_t> order{0, 1, 2, 3, 4};
        Board first(park.spots.size());
        for (const auto i : order) first.claim(0, std::get<0>(claims[i]), std::get<1>(claims[i]), std::get<2>(claims[i]));
        bool alike = true;
        while (std::ranges::next_permutation(order).found) {
            Board other(park.spots.size());
            for (const auto i : order) other.claim(0, std::get<0>(claims[i]), std::get<1>(claims[i]), std::get<2>(claims[i]));
            alike = alike && other.history(0) == first.history(0) && other.owner(0) == first.owner(0);
        }
        check(alike && first.history_size(0) == 4, "the same claims in any of 120 orders: the same history (a's 700 then 800, b's 500 then 650)");
    }
    auto o = board.claim(0, a, 500, 1000);
    check(o.taken() && o.kept && board.owner(0) == a, "the first claim owns an open spot");
    o = board.claim(0, b, 700, 2000);
    check(o.taken() && o.before == a && board.owner(0) == b, "a higher score steals the spot");
    o = board.claim(0, a, 700, 1500);
    check(o.taken() && board.owner(0) == a, "an equal score claimed earlier wins (both sides agree on it)");
    o = board.claim(0, c, 700, 1500);
    check(!o.taken() && board.owner(0) == a, "equal score and time: the lower id keeps it");
    o = board.claim(0, b, 650, 900);
    check(!o.best && board.best(0, b) == 700, "only a player's best counts");
    board.claim(1, b, 100, 3000);
    board.claim(2, c, 50, 3000);
    const auto st = board.standings();
    check(st.size() == 3 && st[0].player == a && st[0].spots == 1 && st[1].player == b && st[2].player == c,
          "standings: most spots, then the higher total");
}

// Three players' copies of the board, each with its own claims sent to the others as they happen
// (a board message of one claim): in any order, every copy ends up with the same owners. Then a
// fourth player who joined late gets one side's whole board and ends up with the same owners and
// the same round replay.
void sync_tests() {
    const auto park = parse_park(small_park());
    constexpr Player a = 0x0110000100000010, b = 0x0110000100000020, c = 0x0110000100000030, d = 0x0110000100000040;
    struct Sent {
        Player from;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Sent> wire_log;
    struct Side {
        Player self;
        Board board;
    };
    std::vector<Side> sides{{a, Board(park.spots.size())}, {b, Board(park.spots.size())}, {c, Board(park.spots.size())}};
    const std::uint32_t park_id = 0x1234, round = 77;
    // Each side's own tricks: claimed on its board at once, sent when they raise its best.
    const std::vector<std::tuple<Player, int, float, std::uint32_t>> tricks{
        {a, 0, 500.0f, 1000u}, {b, 0, 700.0f, 2000u}, {c, 1, 300.0f, 2500u}, {a, 0, 700.0f, 1500u}, {b, 1, 300.0f, 2400u},
        {c, 2, 90.0f, 4000u},  {a, 2, 80.0f, 3000u},  {b, 2, 95.0f, 5000u},  {c, 0, 650.0f, 6000u}, {a, 1, 280.0f, 7000u}};
    for (const auto &[who, spot, score, ms] : tricks) {
        auto &side = *std::ranges::find_if(sides, [&](const Side &s) { return s.self == who; });
        if (side.board.claim(spot, who, score, ms).best)
            wire_log.push_back({who, wire::encode(wire::BoardMessage{park_id, round, {{static_cast<std::uint8_t>(spot), who, score, ms}}})});
    }
    const auto take = [](Board &board, const std::vector<std::uint8_t> &bytes) {
        const auto message = wire::decode(bytes);
        check(message.has_value() && std::holds_alternative<wire::BoardMessage>(*message), "a board message decodes");
        if (!message) return;
        for (const auto &e : std::get<wire::BoardMessage>(*message).entries) board.claim(e.spot, e.player, e.score, e.ms);
    };
    // Delivery: every side receives the others' messages, each in its own order.
    for (std::size_t n = 0; n < sides.size(); ++n) {
        auto order = wire_log;
        if (n == 1) std::ranges::reverse(order);
        if (n == 2) std::rotate(order.begin(), order.begin() + order.size() / 2, order.end());
        for (const auto &sent : order)
            if (sent.from != sides[n].self) take(sides[n].board, sent.bytes);
    }
    const auto same_owners = [&](const Board &x, const Board &y) {
        for (int spot = 0; spot < 3; ++spot)
            if (x.owner(spot) != y.owner(spot) || x.top(spot) != y.top(spot)) return false;
        return true;
    };
    check(same_owners(sides[0].board, sides[1].board) && same_owners(sides[0].board, sides[2].board),
          "every player's board has the same owners, whatever order the claims came in");
    check(sides[0].board.owner(0) == a && sides[0].board.owner(1) == b && sides[0].board.owner(2) == b,
          "owners: 700 earlier beats 700 later; 300 at 2.4 s beats 300 at 2.5 s; the top score");
    // The late joiner: one whole board, as a participant sends it every few seconds.
    wire::BoardMessage whole{park_id, round, {}};
    for (int spot = 0; spot < 3; ++spot)
        for (const auto &e : sides[1].board.history(spot)) whole.entries.push_back({static_cast<std::uint8_t>(spot), e.player, e.score, e.ms});
    Board late(park.spots.size());
    take(late, wire::encode(whole));
    take(late, wire::encode(whole)); // twice changes nothing
    check(same_owners(late, sides[0].board), "a player who joins late gets the same owners from one whole board");
    const auto s0 = round_stats(sides[0].board, 10000), s1 = round_stats(sides[1].board, 10000), s2 = round_stats(sides[2].board, 10000),
               sl = round_stats(late, 10000);
    check(s0.hold == s1.hold && s0.hold == s2.hold && s0.hold == sl.hold && s0.steals == s1.steals && s0.steals == sl.steals,
          "every copy replays the round the same: hold times and steals agree");
    for (int spot = 0; spot < 3; ++spot) check(late.history_size(spot) == sides[1].board.history_size(spot), "a board sent twice keeps one copy of each claim");
    (void)d;
}

// The round replayed: who held what for how long, steals, the best trick, and the standings.
void stats_tests() {
    const auto park = parse_park(small_park());
    constexpr Player a = 0x10, b = 0x20, c = 0x30;
    Board board(park.spots.size());
    board.claim(0, a, 500, 1000);
    board.claim(0, b, 700, 2000); // b steals the box
    board.claim(0, a, 800, 3000); // a takes it back
    board.claim(0, b, 600, 4000); // not b's best: nothing
    board.claim(1, c, 300, 5000);
    const auto st = round_stats(board, 10000);
    const auto near = [](double x, double y) { return std::abs(x - y) < 1e-6; };
    check(near(st.hold.at(a), 8) && near(st.hold.at(b), 1) && near(st.hold.at(c), 5), "hold times: a 1 s + 7 s, b 1 s, c 5 s");
    check(st.steals.at(a) == 1 && st.steals.at(b) == 1 && !st.steals.contains(c), "a steal is a spot taken from another player");
    check(st.best.player == a && st.best.score == 800 && st.best_spot == 0, "the best trick: a's 800 on the box");
    check(st.spot_hold[0].first == a && near(st.spot_hold[0].second, 8) && st.spot_hold[2].first == 0, "each spot's longest holder");
    const auto early = round_stats(board, 2500);
    check(near(early.hold.at(a), 1) && near(early.hold.at(b), 0.5) && !early.hold.contains(c), "replayed to a moment: claims after it do not count");
    const auto by_spots = ranked(board, st);
    check(by_spots.size() == 3 && by_spots[0].player == a && by_spots[1].player == c && by_spots[2].player == b,
          "most spots wins; a tie on spots goes to the higher total");
    check(near(by_spots[0].hold, 8) && by_spots[0].steals == 1 && by_spots[0].total == 800, "a row's time held, steals and total");
    board.claim(2, b, 10, 9500);
    const auto by_spots2 = ranked(board, round_stats(board, 10000));
    check(by_spots2[0].player == a && by_spots2[1].player == c && by_spots2[2].player == b && near(by_spots2[2].hold, 1.5),
          "three players with a spot each: by their totals");
    Board total(park.spots.size());
    total.claim(0, a, 200, 9000); // held 1 s
    total.claim(1, b, 100, 1000); // held 9 s
    const auto by_total = ranked(total, round_stats(total, 10000));
    check(by_total[0].player == a && by_total[1].player == b && by_total[1].hold > by_total[0].hold, "a tie on spots goes to the total, not the time held");
    Board tie(park.spots.size());
    tie.claim(0, a, 100, 5000);
    tie.claim(1, b, 100, 5000);
    const auto even = ranked(tie, round_stats(tie, 10000));
    check(even[0].player == a && even[0].spots == even[1].spots && even[0].total == even[1].total, "a dead heat lists the lower id first");
}

// Teams: steals only from the other team, the standings per team, and teams handed out evenly.
void teams_tests() {
    const auto park = parse_park(small_park());
    constexpr Player a = 0x10, b = 0x20, c = 0x30;
    const Teams teams{{a, 0}, {b, 1}, {c, 1}};
    Board board(park.spots.size());
    board.claim(0, a, 500, 1000);
    board.claim(0, b, 700, 2000); // red to blue: a steal
    board.claim(1, c, 300, 3000);
    board.claim(1, b, 400, 4000); // blue to blue: no steal
    board.claim(2, a, 50, 5000);
    const auto st = round_stats(board, 10000, teams);
    check(st.steals.at(b) == 1 && !st.steals.contains(a), "taking a spot from a teammate is no steal");
    const auto rows = ranked(board, st, teams);
    check(rows.size() == 2 && rows[0].player == 1 && rows[0].spots == 2 && rows[1].player == 0 && rows[1].spots == 1,
          "one row per team: blue 2 spots, red 1");
    check(std::abs(rows[0].hold - (8 + 7)) < 1e-6 && rows[0].steals == 1, "a team's hold time and steals add up its players'");
    auto placed = balance_teams({}, {0x30, 0x10, 0x20});
    check(placed.at(0x10) == 0 && placed.at(0x20) == 1 && placed.at(0x30) == 0, "teams in id order, taking turns");
    placed = balance_teams(placed, {0x10, 0x20, 0x30, 0x40});
    check(placed.at(0x40) == 1 && placed.at(0x10) == 0 && placed.at(0x30) == 0, "a newcomer joins the smaller team; nobody else moves");
    const auto kept = balance_teams({{0x5, 1}}, {0x6, 0x7, 0x5});
    check(kept.at(0x5) == 1 && kept.at(0x6) == 0 && kept.at(0x7) == 0, "a placed player keeps their team; the others even it out");
}

void wire_tests() {
    const wire::RoundMessage round{0xdeadbeef, 42, 0x0110000100000010, true, wire::teams, 300, 12345};
    const wire::BoardMessage board{0xdeadbeef, 42, {{14, 0x0110000100000010, 1234.5f, 99000}, {0, 0x0110000100000020, 10, 4000}}};
    const wire::TeamsMessage teams{0xdeadbeef, 42, {{0x0110000100000010, std::uint8_t{0}}, {0x0110000100000020, std::uint8_t{1}}}};
    const auto r = wire::encode(round), b = wire::encode(board), t = wire::encode(teams);
    check(wire::is_domination(r) && wire::is_domination(b) && wire::is_domination(t) && r[0] == 0xD0, "Domination's messages start with their tag");
    check(r.size() == 27 && b.size() == 13 + 2 * 17 && t.size() == 12 + 2 * 9, "message sizes");
    check(wire::encode(wire::BoardMessage{1, 1, {}}).size() == 13 && wire::decode(wire::encode(wire::BoardMessage{1, 1, {}})).has_value(),
          "an empty board (a player saying they are in) is fine");
    const auto r2 = wire::decode(r), b2 = wire::decode(b), t2 = wire::decode(t);
    check(r2 && std::get<wire::RoundMessage>(*r2) == round, "a round message round-trips");
    const wire::RoundMessage left{0xdeadbeef, 42, 0x0110000100000010, false, 0, 300, 9000, true};
    const auto l2 = wire::decode(wire::encode(left));
    check(l2 && std::get<wire::RoundMessage>(*l2).left && !std::get<wire::RoundMessage>(*l2).running, "a player leaving round-trips, apart from a stop");
    check(b2 && std::get<wire::BoardMessage>(*b2) == board, "a board round-trips");
    check(t2 && std::get<wire::TeamsMessage>(*t2) == teams, "teams round-trip");
    auto bad = b;
    bad.pop_back();
    check(!wire::decode(bad), "a short message is refused");
    bad = b;
    bad.push_back(0);
    check(!wire::decode(bad), "a long message is refused");
    bad = b;
    bad[1] = 1;
    check(!wire::decode(bad) && wire::version_of(bad) == std::optional<std::uint8_t>(std::uint8_t{1}), "another version is refused, and told apart");
    check(!wire::decode(wire::encode(wire::BoardMessage{1, 0, {{0, 1, 10, 0}}})), "round 0 is refused");
    check(!wire::decode(wire::encode(wire::BoardMessage{1, 1, {{0, 1, -5, 0}}})), "a negative score is refused");
    check(!wire::decode(wire::encode(wire::BoardMessage{1, 1, {{0, 0, 5, 0}}})), "a claim by nobody is refused");
    bad = r;
    bad[3 + 4 + 4 + 8 + 1] = 0x80; // the options byte
    check(!wire::decode(bad), "unknown options are refused");
    bad = t;
    bad.back() = 2;
    check(!wire::decode(bad), "a third team is refused");
    bool threw = false;
    try {
        wire::encode(wire::BoardMessage{1, 1, std::vector<wire::Entry>(wire::max_board_entries + 1, wire::Entry{0, 1, 1, 0})});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "a board too long for one message is not sent");
    check(wire::encode(wire::BoardMessage{1, 1, std::vector<wire::Entry>(wire::max_board_entries, wire::Entry{0, 1, 1, 0})}).size() <= 4096,
          "a full board fits a throwdown packet (4096 bytes)");
    const std::vector<std::uint8_t> relay{6, 1, 2, 3};
    check(!wire::is_domination(relay) && !wire::decode(relay), "the relay's own messages are not Domination's");
}

// The parks built into ReSkate read, and match the park files they came from.
void builtin_tests(const char *path) {
    for (const auto &builtin : builtin_parks) {
        Park park;
        try {
            park = parse_park(Json::parse(builtin.json));
        } catch (const std::exception &error) {
            std::printf("FAIL: built-in park %s: %s\n", std::string(builtin.id).c_str(), error.what());
            ++failures;
            continue;
        }
        check(!park.spots.empty() && !park.starts.empty(), "a built-in park has spots and starts");
        if (path && builtin.id == "skate3improved-redbowl") {
            std::ifstream file(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto from_file = parse_park(Json::parse(text));
            bool same = from_file.spots.size() == park.spots.size() && from_file.starts.size() == park.starts.size() && from_file.level == park.level;
            for (std::size_t i = 0; same && i < park.spots.size(); ++i)
                same = park.spots[i].id == from_file.spots[i].id && park.spots[i].points == from_file.spots[i].points &&
                       park.spots[i].y_min == from_file.spots[i].y_min && park.spots[i].marker == from_file.spots[i].marker;
            check(same, "the built-in red-bowl park matches its park file");
        }
    }
}

void real_park(const char *path) {
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check(!text.empty(), "the real park file reads");
    Park park;
    try {
        park = parse_park(Json::parse(text));
    } catch (const std::exception &error) {
        std::printf("FAIL: the real park file: %s\n", error.what());
        ++failures;
        return;
    }
    check(park.spots.size() == 20, "the red-bowl park has 20 spots");
    check(park.starts.size() == 10 && start_for(park, 0) == 0 && start_for(park, 12) == 2, "10 starts, shared out in order");
    for (const auto &start : park.starts) check(spot_at(park, {start.position[0], start.position[1] + 1, start.position[2]}) == -1, "no start stands on a spot");
    const auto at = [&](Vec3 p) { const int i = spot_at(park, p); return i < 0 ? std::string("-") : park.spots[i].id; };
    check(at({327, -0.5f, 445}) == "deep-end", "the deep end's floor");
    check(at({345, 0.4f, 425}) == "main-bowl", "the main bowl's floor");
    check(at({380, 0.5f, 410}) == "east-arm", "the snake arm's floor");
    check(at({384, 4.3f, 381.8f}) == "top-rail", "on the long rail");
    check(at({353.7f, 4.5f, 386.8f}) == "funbox", "on the funbox");
    // Where the user ran `trainer where` (the skater's root, about 1 m above the bowl floor at 0.35).
    check(at({364.46f, 1.37f, 416.31f}) == "main-bowl", "the user's position in the park is in the main bowl");
    check(at({420, 3.5f, 420}) == "-", "the open plaza by the fence is no spot");
    // The mounds and the south curb (in no spot until 2026-10-15: a trick on them counted for nothing).
    check(at({303.2f, 4.5f, 419.0f}) == "west-mound" && at({311.2f, 4.8f, 430.8f}) == "bowl-mound", "the west mounds are spots");
    check(at({391.8f, 4.6f, 420.6f}) == "east-mound" && at({377.7f, 4.6f, 450.0f}) == "south-mound", "the east and south mounds are spots");
    check(at({337.0f, 4.5f, 473.4f}) == "south-curb", "the south curb is a spot");
    check(at({327, 20, 445}) == "-", "high above the deep end is no spot");
}
} // namespace

int main(int argc, char **argv) {
    park_tests();
    readme_park_test();
    even_out_tests();
    line_spot_tests();
    trick_tests();
    claim_tests();
    board_tests();
    sync_tests();
    stats_tests();
    teams_tests();
    wire_tests();
    // (ctest passes an empty argument when no park file is configured.)
    const char *park = argc > 1 && *argv[1] ? argv[1] : nullptr;
    builtin_tests(park);
    if (park) real_park(park);
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
