#include "Server/server_activity.h"
#include "Engine/Game/Multiplayer/chat_rate.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::server;
using multiplayer::ThrowdownMessage;
using Kind = ThrowdownMessage::Kind;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    if (condition) return;
    ++failures;
    std::fprintf(stderr, "FAILED: %s\n", what);
}
constexpr std::uint64_t zee = 76561198000000001ULL, kush = 76561198000000002ULL, tally = 76561198000000003ULL;
std::vector<std::string> lines;
std::string name_of(std::uint64_t id) {
    return id == zee ? "Zee" : id == kush ? "Kush" : id == tally ? "Tally" : std::string{};
}
std::vector<std::uint8_t> wire(Kind kind, std::string series = {}) {
    ThrowdownMessage m;
    m.kind = kind;
    m.leader = zee;
    m.id = 7;
    m.series = std::move(series);
    if (kind == Kind::offer) m.placement = {1, 2, 3};
    return multiplayer::encode_throwdown(m);
}
std::vector<std::uint8_t> wire(ThrowdownMessage m) {
    m.leader = zee;
    m.id = 7;
    return multiplayer::encode_throwdown(m);
}
bool said(std::string_view text) {
    for (const auto &line : lines)
        if (line == text) return true;
    return false;
}
} // namespace

int main() {
    ActivityLog log([](const std::string &line) { lines.push_back(line); }, name_of);
    const std::array<float, 3> at{606.2f, 198.9f, 1075.5f};

    // Spot Battle: a drop, a queue, a start, a turn, and the result once it goes quiet.
    log.throwdown(zee, wire(Kind::offer, "SpotBattle"), &at, 1'000'000);
    log.throwdown(zee, wire(Kind::offer, "SpotBattle"), &at, 11'000'000); // repeated offer: no new line
    check(lines.size() == 1 && lines[0] == "[throwdown] Zee placed a Spot Battle drop near (606, 199, 1076)",
          "a placed drop is logged once with where it is");
    log.throwdown(kush, wire(Kind::join), nullptr, 12'000'000);
    log.throwdown(tally, wire(Kind::join), nullptr, 13'000'000);
    check(said("[throwdown] Tally joined Zee's Spot Battle (3 in the queue)"), "joins are logged with the queue size");
    ThrowdownMessage start;
    start.kind = Kind::start;
    start.order = {zee, kush, tally};
    log.throwdown(zee, wire(start), nullptr, 14'000'000);
    check(said("[throwdown] Zee's Spot Battle started with 3 players: Zee, Kush, Tally"), "the start lists the players");
    for (const int line : {1200, 3300}) {
        ThrowdownMessage row;
        row.kind = Kind::row;
        row.board = 1;
        row.add = true;
        row.value = line;
        log.throwdown(kush, wire(row), nullptr, 20'000'000);
    }
    ThrowdownMessage best;
    best.kind = Kind::row;
    best.value = 4500;
    log.throwdown(kush, wire(best), nullptr, 20'000'000);
    ThrowdownMessage turn;
    turn.kind = Kind::turn_end;
    turn.value = 1;
    log.throwdown(kush, wire(turn), nullptr, 21'000'000);
    check(said("[throwdown] Zee's Spot Battle: Kush scored 4,500 in round 1 (best 4,500)"), "a turn's points are logged");
    log.throwdown(tally, wire(Kind::leave), nullptr, 22'000'000);
    check(said("[throwdown] Tally quit Zee's Spot Battle"), "quitting a running throwdown is logged");
    log.tick(60'000'000);
    check(!said("[throwdown] Zee's Spot Battle has finished: 1. Kush 4,500, 2. Zee 0, 3. Tally 0 (quit)"),
          "a throwdown is not over while turns are still due");
    log.tick(200'000'000);
    check(said("[throwdown] Zee's Spot Battle has finished: 1. Kush 4,500, 2. Zee 0, 3. Tally 0 (quit)"),
          "the result is logged once it goes quiet");

    // S.K.A.T.E.: attempts, and a cancelled queue.
    lines.clear();
    log.throwdown(zee, wire(Kind::offer, "ThrowdownSkate"), nullptr, 300'000'000);
    check(lines.size() == 1 && lines[0] == "[throwdown] Zee placed a S.K.A.T.E. drop", "a drop without a position");
    start.order = {zee, kush};
    log.throwdown(zee, wire(start), nullptr, 300'500'000);
    ThrowdownMessage attempt;
    attempt.kind = Kind::attempt;
    attempt.value = 2;
    attempt.add = true;
    log.throwdown(kush, wire(attempt), nullptr, 301'000'000);
    check(said("[throwdown] Zee's S.K.A.T.E.: Kush landed (turn 2)"), "attempts are logged");
    lines.clear();
    log.throwdown(zee, wire(Kind::close), nullptr, 302'000'000);
    check(lines.empty(), "a running throwdown ignores a close");

    // Objects: a few are listed, many are summed up, moves say nothing.
    lines.clear();
    std::vector<NetworkObject> before, after;
    after.push_back({1, "bk_rail_01", {10.f, 2.f, 30.f}});
    log.objects(kush, before, after);
    check(lines.size() == 1 && lines[0] == "[objects] Kush placed bk_rail_01 at (10, 2, 30)", "a placed object is logged");
    before = after;
    after[0].position[0] = 12.f;
    lines.clear();
    log.objects(kush, before, after);
    check(lines.empty(), "moving an object is not logged");
    for (std::uint64_t id = 2; id < 12; ++id) after.push_back({id, "bk_box", {}});
    log.objects(kush, before, after);
    check(lines.size() == 1 && lines[0] == "[objects] Kush placed 10 objects (11 in total)", "many objects are summed up");

    // Chat pace: four at once, then one per 1.5 s; no repeats within 15 s.
    ChatRate rate;
    using V = ChatRate::Verdict;
    for (int i = 0; i < 4; ++i)
        check(rate.accept(1'000'000, "line " + std::to_string(i)) == V::accepted, "a short burst of chat is allowed");
    check(rate.accept(1'000'000, "line 4") == V::too_fast, "a fifth line at once is refused");
    check(rate.accept(2'600'000, "line 5") == V::accepted, "one more line is allowed after 1.5 s");
    check(rate.accept(2'700'000, "line 5") == V::repeated, "the same line straight after is refused");
    check(rate.accept(30'000'000, "line 5") == V::accepted, "the same line later is allowed");
    // An answer to a vote may repeat the line before it (the same /2 in the next poll), at the same pace.
    ChatRate answers;
    for (int i = 0; i < 4; ++i) check(answers.accept(1'000'000, "/2", 0, true) == V::accepted, "an answer to a vote may repeat the line before it");
    check(answers.accept(1'000'000, "/3", 0, true) == V::too_fast, "an answer is still held to the pace");
    check(answers.accept(2'600'000, "/2") == V::repeated, "the same line, not an answer, is still refused");
    ChatRate relayed;
    for (int i = 0; i < 5; ++i)
        check(relayed.accept(1'000'000, std::to_string(i), 1) == V::accepted, "receivers allow one line of slack");

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("server activity: all checks passed\n");
    return failures ? 1 : 0;
}
