// The throwdown relay's state machine without the game: the natives it drives are
// recorded here, and messages are exchanged by hand as other players would send them.
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/grouped_number.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Settings/named_settings.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <map>
#include <stdexcept>

namespace {
struct Call {
    std::string what, series;
    std::uint32_t id{}, mmid{};
    std::int32_t value{};
    std::uint64_t token{};
};
std::vector<Call> calls;
std::map<std::string, std::string> settings_changed;
bool leaderboard_live = true;
bool types_ready = true; // stands in for the event types of the level being found
std::uint64_t last_token{};
}

namespace dingosdk {
std::string change_named_setting(std::string_view name, std::string_view value, bool) {
    settings_changed[std::string(name)] = std::string(value);
    return "ok";
}
} // namespace dingosdk
namespace dingosdk::multiplayer {
std::uint32_t local_native_player_id() noexcept { return 2; }
std::uint64_t native_party_player_info(std::uintptr_t, std::size_t) noexcept { return 0; }
void spectate_party_member(std::uint64_t) noexcept {}
bool prepare_throwdown_injection() noexcept { calls.push_back({"prepare"}); return types_ready; }
bool queue_throwdown_spawn(std::uint64_t token, std::uint32_t host, const std::string &series,
                           const std::vector<std::uint8_t> &, const std::vector<std::uint8_t> &) noexcept {
    last_token = token;
    calls.push_back({"spawn", series, host, 0, 0, token});
    return true;
}
bool queue_throwdown_add(std::uint32_t player, const std::string &series, std::uint32_t mmid) noexcept {
    calls.push_back({"add", series, player, mmid});
    return true;
}
bool queue_throwdown_remove(std::uint32_t player) noexcept { calls.push_back({"remove", {}, player}); return true; }
bool queue_throwdown_start(const std::string &series, std::uint32_t mmid) noexcept {
    calls.push_back({"start", series, 0, mmid});
    return true;
}
bool queue_throwdown_destroy(std::uint32_t mmid) noexcept { calls.push_back({"destroy", {}, 0, mmid}); return true; }
bool queue_throwdown_score(std::uint32_t player, std::int32_t score) noexcept {
    if (!leaderboard_live) return false;
    calls.push_back({"score", {}, player, 0, score});
    return true;
}
bool queue_throwdown_row(std::uint32_t player, std::uint8_t board, bool add, std::int32_t value) noexcept {
    if (!leaderboard_live) return false;
    calls.push_back({add ? "row add" : "row set", {}, player, board, value});
    return true;
}
bool queue_throwdown_attempt(std::uint32_t player, bool landed, const std::array<std::uint8_t, 28> &trick) noexcept {
    if (!leaderboard_live) return false;
    calls.push_back({landed ? "landed" : "missed", {}, player, trick[0]});
    return true;
}
bool queue_throwdown_destroy_event(std::uint32_t player) noexcept {
    calls.push_back({"destroy event", {}, player});
    return true;
}
bool queue_challenge_start(const std::string &series, const std::string &id, bool) noexcept {
    calls.push_back({"challenge start", series + " " + id});
    return true;
}
bool queue_challenge_attempt(std::uint32_t player, std::vector<std::uint8_t> criteria, std::vector<std::uint8_t>) noexcept {
    calls.push_back({"challenge attempt", {}, player, 0, static_cast<std::int32_t>(criteria.size())});
    return true;
}
bool queue_challenge_slam(std::uint32_t player, std::int32_t identifier) noexcept {
    calls.push_back({"challenge slam", {}, player, 0, identifier});
    return true;
}
bool queue_challenge_leave(std::uint32_t player) noexcept { calls.push_back({"challenge leave", {}, player}); return true; }
bool queue_beacon_move(std::uint32_t player, const std::array<float, 16> &location) noexcept {
    calls.push_back({"beacon move", {}, player, 0, static_cast<std::int32_t>(location[12])});
    return true;
}
bool queue_beacon_remove(std::uint32_t player) noexcept { calls.push_back({"beacon remove", {}, player}); return true; }
std::optional<ChallengeCelebration> test_celebration;
std::optional<ChallengeCelebration> take_challenge_celebration() noexcept { return std::exchange(test_celebration, std::nullopt); }
bool queue_throwdown_end_turn(std::uint32_t player) noexcept {
    if (!leaderboard_live) return false; // stands in for "event handle unknown"
    calls.push_back({"end turn", {}, player});
    return true;
}
} // namespace dingosdk::multiplayer

using namespace dingosdk::multiplayer;
namespace {
using Kind = ThrowdownMessage::Kind;
using Local = ThrowdownLocalAction::Kind;
constexpr std::uint64_t me = 76561198000000010ULL, leader = 76561198000000011ULL, other = 76561198000000012ULL;
void check(bool value, const char *why) {
    if (value) return;
    for (const auto &c : calls)
        std::cerr << "  " << c.what << " id " << std::hex << c.id << " mmid " << c.mmid << std::dec << " value " << c.value << '\n';
    throw std::runtime_error(why);
}
std::vector<ThrowdownMessage> tick(const ThrowdownRelayInput &input) {
    std::vector<ThrowdownMessage> out;
    for (const auto &bytes : tick_throwdown_relay(0, input)) {
        const auto m = decode_throwdown(bytes);
        check(m.has_value(), "The relay produced an undecodable message");
        out.push_back(*m);
    }
    return out;
}
void receive(std::uint64_t sender, const ThrowdownMessage &m) {
    const auto bytes = encode_throwdown(m);
    receive_throwdown_relay(sender, bytes);
}
ThrowdownMessage make(Kind kind, std::uint64_t owner, std::uint32_t id) {
    ThrowdownMessage m; m.kind = kind; m.leader = owner; m.id = id;
    return m;
}
ThrowdownMessage offer(std::uint64_t owner, std::uint32_t id, const char *series = "JamSession") {
    auto m = make(Kind::offer, owner, id);
    m.series = series; m.placement = {1, 2, 3}; m.settings = {4};
    return m;
}
const Call *find(const char *what) {
    for (const auto &c : calls) if (c.what == what) return &c;
    return nullptr;
}
std::size_t count(const char *what) {
    std::size_t n{};
    for (const auto &c : calls) n += c.what == what;
    return n;
}
void local(Local kind, std::uint32_t mmid = 0) {
    ThrowdownLocalAction a{kind};
    a.mmid = mmid; a.series = "JamSession";
    throwdown_relay_local(std::move(a));
}
// The session is over; the world stays loaded.
const ThrowdownRelayInput no_session{0, {}, true, 0};
void end_session() {
    tick(no_session);
    calls.clear();
}

// Another player's drop: shown, joined natively, started by its leader, scored.
void guest_flow() {
    const ThrowdownRelayInput in{me, {{leader, 3}, {other, 5}}, true, 77};
    check(tick(in).empty(), "An idle relay sent something");
    check(settings_changed["DingoThrowdowns.QueueStartTimer"] == "36000", "Queues kept their own start timer with players in the session");
    check(settings_changed["DingoThrowdowns.EnableForceStartThrowdown"] == "1", "Force start stayed off with players in the session");
    receive(leader, offer(leader, 0x1234));
    tick(in);
    const auto *spawn = find("spawn");
    check(spawn && spawn->id == 0x303 && spawn->series == "JamSession", "The leader's drop was not spawned hosted by their virtual id");
    // A second offer (resend) must not spawn again.
    receive(leader, offer(leader, 0x1234));
    tick(in);
    check(count("spawn") == 1, "A resent offer spawned a second copy");
    throwdown_relay_spawned(last_token, 0x5555, 4);
    receive(other, make(Kind::join, leader, 0x1234));
    tick(in);
    const auto *add = find("add");
    check(add && add->id == 0x305 && add->mmid == 0x5555, "Another player's join was not replayed into the local copy");
    local(Local::joined, 0x5555);
    auto out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::join && out[0].leader == leader && out[0].id == 0x1234,
          "Joining the local copy was not reported against the leader's drop");
    auto start = make(Kind::start, leader, 0x1234);
    start.order = {leader, other, me};
    receive(leader, start);
    tick(in);
    const auto *started = find("start");
    check(started && started->mmid == 0x5555, "The leader's start did not start the joined copy");
    ThrowdownLocalAction entered{Local::entered};
    entered.participants = {0x303, 0x305, 2};
    throwdown_relay_local(std::move(entered));
    tick(in);
    auto score = make(Kind::score, leader, 0x1234);
    score.value = 500;
    receive(other, score);
    tick(in);
    const auto *row = find("score");
    check(row && row->id == 0x305 && row->value == 500, "A remote score did not become that player's row");
    ThrowdownLocalAction own{Local::score};
    own.score = 300;
    throwdown_relay_local(std::move(own));
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::score && out[0].leader == leader && out[0].id == 0x1234 && out[0].value == 300,
          "The local score was not sent against the running throwdown");
    local(Local::ended);
    tick(in);
    score.value = 900;
    receive(other, score);
    tick(in);
    check(count("score") == 1, "A score after the end was still written");
    end_session();
}

// The local player's own drop: offered, joined by others, force-started, scored.
void leader_flow() {
    const ThrowdownRelayInput in{me, {{other, 4}}, true, 78};
    tick(in);
    ThrowdownLocalAction exited{Local::exited};
    exited.series = "JamSession"; exited.mmid = 0x42; exited.placement = {9}; exited.settings = {8};
    throwdown_relay_local(std::move(exited));
    auto out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::offer && out[0].leader == me && out[0].id == 0x42 &&
              out[0].placement == std::vector<std::uint8_t>{9} && out[0].settings == std::vector<std::uint8_t>{8},
          "A configured drop was not offered to the other players");
    receive(other, make(Kind::join, me, 0x42));
    tick(in);
    const auto *add = find("add");
    check(add && add->id == 0x304 && add->mmid == 0x42, "A remote join did not enter the local player's queue");
    receive(other, make(Kind::join, me, 0x42));
    tick(in);
    check(count("add") == 1, "A repeated join was added twice");
    local(Local::force_started, 0x42);
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::start && out[0].order == std::vector<std::uint64_t>{me, other},
          "Force start was not relayed with the queue's players");
    ThrowdownLocalAction entered{Local::entered};
    entered.participants = {2, 0x304};
    throwdown_relay_local(std::move(entered));
    out = tick(in);
    check(out.empty(), "Entering the event sent a second start");
    auto score = make(Kind::score, me, 0x42);
    score.value = 700;
    receive(other, score);
    tick(in);
    const auto *row = find("score");
    check(row && row->id == 0x304 && row->value == 700, "A guest's score did not reach the leader's leaderboard");
    end_session();

    // Cancelling on the details page closes the drop for everyone.
    tick(in);
    exited = {Local::exited};
    exited.series = "JamSession"; exited.mmid = 0x43; exited.placement = {9};
    throwdown_relay_local(std::move(exited));
    tick(in);
    ThrowdownLocalAction cancel{Local::exited};
    cancel.cancelling = true;
    throwdown_relay_local(std::move(cancel));
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::close && out[0].id == 0x43, "A cancelled drop was not closed");
    // Modes with no relay stay local.
    exited = {Local::exited};
    exited.series = "Race"; exited.mmid = 0x44; exited.placement = {9};
    throwdown_relay_local(std::move(exited));
    check(tick(in).empty(), "A mode with no relay (Race) was offered");
    end_session();
}

// Copies that must go: full, started without us, closed, leader gone, session over.
void removal_flow() {
    ThrowdownRelayInput in{me, {{leader, 3}, {other, 5}}, true, 79};
    tick(in);
    receive(leader, offer(leader, 7));
    tick(in);
    throwdown_relay_spawned(last_token, 0x70, 2);
    tick(in);
    receive(other, make(Kind::join, leader, 7));
    tick(in);
    check(count("destroy") == 1 && find("destroy")->mmid == 0x70, "A full copy the local player had not joined stayed");
    calls.clear();

    receive(leader, offer(leader, 8));
    tick(in);
    throwdown_relay_spawned(last_token, 0x80, 4);
    tick(in);
    auto start = make(Kind::start, leader, 8);
    start.order = {leader, other};
    receive(leader, start);
    tick(in);
    check(count("destroy") == 1 && !find("start"), "A copy the local player had not joined was started");
    calls.clear();

    receive(leader, offer(leader, 9));
    tick(in);
    // Closed while its spawn is outstanding: destroyed once the MMID is known.
    receive(leader, make(Kind::close, leader, 9));
    tick(in);
    check(!find("destroy"), "A copy was destroyed before it existed");
    throwdown_relay_spawned(last_token, 0x90, 4);
    tick(in);
    check(count("destroy") == 1 && find("destroy")->mmid == 0x90, "A copy closed while spawning stayed");
    calls.clear();

    receive(leader, offer(leader, 10));
    tick(in);
    throwdown_relay_spawned(last_token, 0xa0, 4);
    tick(in);
    in.peers = {{other, 5}}; // the leader left the session
    tick(in);
    check(count("destroy") == 1 && find("destroy")->mmid == 0xa0, "A departed leader's copy stayed");
    calls.clear();

    in.peers = {{leader, 3}, {other, 5}};
    receive(leader, offer(leader, 11));
    tick(in);
    throwdown_relay_spawned(last_token, 0xb0, 4);
    tick(in);
    // Offers from someone who is not the leader, or from outside the session, are ignored.
    receive(other, offer(leader, 12));
    receive(76561198000000099ULL, offer(76561198000000099ULL, 13));
    tick(in);
    check(count("spawn") == 1, "A forged or foreign offer spawned a copy");
    tick(no_session);
    check(count("destroy") == 1 && find("destroy")->mmid == 0xb0, "Ending the session left a copy in the world");
    check(settings_changed["DingoThrowdowns.QueueStartTimer"] == "1", "The solo queue timer was not restored");
    calls.clear();
}
// Spot Battle: one turn order everywhere, both leaderboards and turn ends relayed in order.
void spot_battle_flow() {
    const ThrowdownRelayInput in{me, {{leader, 3}, {other, 5}}, true, 81};
    tick(in);
    std::vector<std::uint32_t> ids{2, 0x305, 0x303};
    check(!throwdown_relay_order(ids), "Participants were reordered outside a linked queue");
    receive(leader, offer(leader, 0x20, "SpotBattle"));
    tick(in);
    check(find("spawn") && find("spawn")->series == "SpotBattle", "A Spot Battle was not shown to the other players");
    throwdown_relay_spawned(last_token, 0x600, 4);
    receive(other, make(Kind::join, leader, 0x20));
    tick(in);
    local(Local::joined, 0x600);
    tick(in);
    check(throwdown_relay_order(ids) && ids == std::vector<std::uint32_t>{0x303, 2, 0x305},
          "The turn order is not leader first, then by Steam ID");
    std::vector<std::uint32_t> stranger{2, 0x399, 0x303};
    check(throwdown_relay_order(stranger) && stranger == std::vector<std::uint32_t>{0x303, 2, 0x399},
          "An unknown participant did not keep its place after the known players");
    auto start = make(Kind::start, leader, 0x20);
    start.order = {leader, me, other};
    receive(leader, start);
    tick(in);
    ThrowdownLocalAction entered{Local::entered};
    entered.participants = {0x303, 2, 0x305};
    throwdown_relay_local(std::move(entered));
    tick(in);

    ThrowdownLocalAction mine{Local::turn_started};
    mine.player = 2;
    throwdown_relay_local(std::move(mine));
    tick(in);
    ThrowdownLocalAction row{Local::row};
    row.board = 1; row.add = true; row.score = 150;
    throwdown_relay_local(std::move(row));
    ThrowdownLocalAction someone_else{Local::turn_ended};
    someone_else.player = 0x305; // another player's turn ending here is not ours to send
    throwdown_relay_local(std::move(someone_else));
    ThrowdownLocalAction ended_turn{Local::turn_ended};
    ended_turn.player = 2;
    throwdown_relay_local(std::move(ended_turn));
    ThrowdownLocalAction jam_score{Local::score};
    jam_score.score = 150;
    throwdown_relay_local(std::move(jam_score));
    auto out = tick(in);
    check(out.size() == 2 && out[0].kind == Kind::row && out[0].board == 1 && out[0].add && out[0].value == 150 &&
              out[0].leader == leader && out[0].id == 0x20 && out[1].kind == Kind::turn_end && out[1].value == 1,
          "Own Spot Battle rows and turn end were not relayed (or a Jam score was)");

    // Held while the leaderboard (and event handle) is not there, then replayed in order.
    leaderboard_live = false;
    calls.clear();
    auto remote_row = make(Kind::row, leader, 0x20);
    remote_row.board = 0; remote_row.add = false; remote_row.value = 400;
    receive(other, remote_row);
    auto their_end = make(Kind::turn_end, leader, 0x20);
    their_end.value = 1;
    receive(other, their_end);
    tick(in);
    check(calls.empty(), "A remote row went out before the leaderboard existed");
    leaderboard_live = true;
    tick(in);
    check(calls.size() == 1 && calls[0].what == "row set", "A remote turn end went out before that turn started here");
    ThrowdownLocalAction theirs{Local::turn_started};
    theirs.player = 0x305;
    throwdown_relay_local(std::move(theirs));
    tick(in);
    check(calls.size() == 2 && calls[0].what == "row set" && calls[0].id == 0x305 && calls[0].mmid == 0 &&
              calls[0].value == 400 && calls[1].what == "end turn" && calls[1].id == 0x305,
          "Remote rows and turn ends were not replayed in order as that player");
    local(Local::ended);
    tick(in);
    check(!throwdown_relay_order(ids), "The shared order outlived the throwdown");
    end_session();
}
// S.K.A.T.E.: each attempt replayed once, in the owner's same turn.
void skate_flow() {
    const ThrowdownRelayInput in{me, {{leader, 3}, {other, 5}}, true, 82};
    tick(in);
    receive(leader, offer(leader, 0x30, "ThrowdownSkate"));
    tick(in);
    check(find("spawn") && find("spawn")->series == "ThrowdownSkate", "A S.K.A.T.E. throwdown was not shown to the other players");
    throwdown_relay_spawned(last_token, 0x700, 4);
    receive(other, make(Kind::join, leader, 0x30));
    tick(in);
    local(Local::joined, 0x700);
    tick(in);
    auto start = make(Kind::start, leader, 0x30);
    start.order = {leader, me, other};
    receive(leader, start);
    tick(in);
    ThrowdownLocalAction entered{Local::entered};
    entered.participants = {0x303, 2, 0x305};
    throwdown_relay_local(std::move(entered));
    tick(in);
    const auto turn_started = [](std::uint32_t id) {
        ThrowdownLocalAction a{Local::turn_started};
        a.player = id;
        throwdown_relay_local(std::move(a));
    };
    const auto remote_attempt = [](std::uint64_t from, int turn, bool landed, std::uint8_t mark) {
        auto m = make(Kind::attempt, leader, 0x30);
        m.value = turn; m.add = landed; m.trick[0] = mark;
        receive(from, m);
    };
    calls.clear();

    // The leader sets a trick on their first turn.
    turn_started(0x303);
    remote_attempt(leader, 1, true, 0xa1);
    tick(in);
    check(calls.size() == 1 && calls[0].what == "landed" && calls[0].id == 0x303 && calls[0].mmid == 0xa1,
          "The leader's attempt was not replayed as theirs in their turn");
    check(throwdown_relay_hides(other) && !throwdown_relay_hides(leader) && !throwdown_relay_hides(76561198000000099ULL),
          "Only the player who is up should be shown (and nobody outside the throwdown hidden)");
    // Another player's attempt that arrives before their turn starts here waits for it.
    remote_attempt(other, 1, false, 0xb1);
    tick(in);
    check(calls.size() == 1, "An attempt was replayed before its turn started here");
    turn_started(2);
    ThrowdownLocalAction mine{Local::attempt};
    mine.player = 2; mine.add = true; mine.trick[0] = 0xc1;
    throwdown_relay_local(std::move(mine));
    auto out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::attempt && out[0].value == 1 && out[0].add && out[0].trick[0] == 0xc1,
          "The local attempt was not relayed with its turn number");
    ThrowdownLocalAction skate_end{Local::turn_ended};
    skate_end.player = 2;
    throwdown_relay_local(std::move(skate_end));
    check(tick(in).empty(), "A S.K.A.T.E. turn end was relayed (turns end by themselves after attempts)");
    ThrowdownLocalAction watching{Local::attempt};
    watching.player = 0x305; // a wipeout while someone else is up: the local server rejects it
    throwdown_relay_local(std::move(watching));
    check(tick(in).empty(), "An attempt made outside the local player's turn was relayed");
    turn_started(0x305);
    tick(in);
    check(calls.size() == 2 && calls[1].what == "missed" && calls[1].id == 0x305 && calls[1].mmid == 0xb1,
          "A held attempt did not go once its turn started");

    // Round 2: out of time on the local turn goes out as a miss.
    turn_started(2);
    ThrowdownLocalAction timeout{Local::timer_failed};
    timeout.player = 2;
    throwdown_relay_local(std::move(timeout));
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::attempt && out[0].value == 2 && !out[0].add,
          "A local timeout was not relayed as a miss");
    // This machine's timer failed the other player's turn first: their late attempt is dropped.
    turn_started(0x305);
    ThrowdownLocalAction local_timer{Local::timer_failed};
    local_timer.player = 0x305;
    throwdown_relay_local(std::move(local_timer));
    tick(in);
    remote_attempt(other, 2, true, 0xb2);
    remote_attempt(leader, 1, true, 0xa9); // for a turn that is over here
    tick(in);
    check(calls.size() == 2, "An attempt was replayed after this machine had already decided that turn");
    // The other player quits: their turns resolve as misses here, without them.
    receive(other, make(Kind::leave, leader, 0x30));
    tick(in);
    turn_started(0x305);
    tick(in);
    check(calls.size() == 3 && calls[2].what == "missed" && calls[2].id == 0x305,
          "A player who quit was not given a miss on their turn");
    throwdown_relay_local({Local::quit});
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::leave, "Quitting was not relayed");
    check(calls.size() == 4 && calls[3].what == "destroy event" && (calls[3].id == 0x303 || calls[3].id == 0x305),
          "The event left behind on the local server was not ended");
    calls.pop_back();
    check(!throwdown_relay_hides(other), "Players stayed hidden after the local player quit");
    turn_started(0x305);
    tick(in);
    check(calls.size() == 3, "Turns were still resolved after the local player quit");
    local(Local::ended);
    tick(in);
    check(!throwdown_relay_hides(other), "Players stayed hidden after the throwdown");
    end_session();
}
// A running S.K.A.T.E. throwdown with the leader (and `other`, if `third`), joined by the local player.
void join_running_skate(const ThrowdownRelayInput &in, bool third) {
    tick(in);
    receive(leader, offer(leader, 0x50, "ThrowdownSkate"));
    tick(in);
    throwdown_relay_spawned(last_token, 0x900, 4);
    if (third) receive(other, make(Kind::join, leader, 0x50));
    tick(in);
    local(Local::joined, 0x900);
    tick(in);
    auto start = make(Kind::start, leader, 0x50);
    start.order = third ? std::vector<std::uint64_t>{leader, me, other} : std::vector<std::uint64_t>{leader, me};
    receive(leader, start);
    tick(in);
    ThrowdownLocalAction entered{Local::entered};
    entered.participants = third ? std::vector<std::uint32_t>{0x303, 2, 0x305} : std::vector<std::uint32_t>{0x303, 2};
    throwdown_relay_local(std::move(entered));
    tick(in);
    take_throwdown_relay_notices();
    calls.clear();
}
// Everyone else quits or leaves: the last player's throwdown ends instead of running on alone.
void alone_flow() {
    const ThrowdownRelayInput two{me, {{leader, 3}}, true, 91};
    join_running_skate(two, false);
    receive(leader, make(Kind::leave, leader, 0x50));
    tick(two);
    check(count("destroy event") == 1 && find("destroy event")->id == 2,
          "The last player's throwdown was not ended when the other player quit");
    const auto notices = take_throwdown_relay_notices();
    check(notices.size() == 1 && notices[0].find("S.K.A.T.E.") != std::string::npos,
          "The last player was not told why the throwdown ended");
    check(!throwdown_relay_hides(leader), "Players stayed hidden after the throwdown ended");
    tick(two);
    check(count("destroy event") == 1, "The throwdown was ended twice");
    end_session();

    // The other player drops out of the session mid-throwdown.
    join_running_skate(two, false);
    const ThrowdownRelayInput gone{me, {}, true, 91};
    tick(gone);
    check(count("destroy event") == 1 && find("destroy event")->id == 2,
          "The throwdown went on after the only other player left the session");
    end_session();

    // With three, one leaving the session is a quit: their turn resolves, the others play on.
    const ThrowdownRelayInput three{me, {{leader, 3}, {other, 5}}, true, 91};
    join_running_skate(three, true);
    ThrowdownLocalAction up{Local::turn_started};
    up.player = 0x305;
    throwdown_relay_local(std::move(up));
    const ThrowdownRelayInput left{me, {{leader, 3}}, true, 91};
    tick(left);
    check(count("destroy event") == 0, "A throwdown with two players left was ended");
    check(count("missed") == 1 && find("missed")->id == 0x305,
          "A player who left the session was not given a miss on their turn");
    receive(leader, make(Kind::leave, leader, 0x50));
    tick(left);
    check(count("destroy event") == 1 && find("destroy event")->id == 2,
          "The throwdown was not ended once the last other player quit too");
    end_session();
}
// Coop challenges: a start near other players invites them; attempts and leaves are replayed.
void challenge_flow() {
    // The local player leads: another player 20 m away, a third far off.
    ThrowdownRelayInput in{me, {{leader, 3, "Lead", true, true}, {other, 5, "Far", false, true}}, true, 77};
    tick(in);
    take_throwdown_relay_notices();
    const auto players = throwdown_relay_challenge_players("OTS", "Plot-014-OTS-03");
    check(players && *players == std::vector<std::uint32_t>{0x303}, "A challenge start did not invite the nearby player only");
    ThrowdownLocalAction started{Local::challenge_started};
    started.series = "OTS"; started.challenge = "Plot-014-OTS-03"; started.participants = {2, 0x303};
    throwdown_relay_local(std::move(started));
    auto out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::challenge_start && out[0].order == std::vector<std::uint64_t>{me, leader} &&
              out[0].challenge == "Plot-014-OTS-03",
          "The coop challenge start was not sent with the shared order");
    const auto key = out[0].id;
    check(take_throwdown_relay_notices().size() == 1, "The leader was not told who joined");
    ThrowdownLocalAction attempt{Local::challenge_attempt};
    attempt.criteria.assign(0x14 * 2, 1); attempt.indexes.assign(8, 0);
    throwdown_relay_local(std::move(attempt));
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::challenge_attempt && out[0].criteria.size() == 0x28,
          "The local attempt was not relayed");
    auto remote = make(Kind::challenge_attempt, me, key);
    remote.criteria.assign(0x14, 2); remote.indexes.assign(4, 0);
    receive(leader, remote);
    auto hit = make(Kind::challenge_slam, me, key); hit.value = 9;
    receive(leader, hit);
    tick(in);
    check(count("challenge attempt") == 1 && find("challenge attempt")->id == 0x303 &&
              count("challenge slam") == 1 && find("challenge slam")->value == 9,
          "A remote attempt or slam hit was not replayed as that player");
    receive(other, remote); // not a participant
    tick(in);
    check(count("challenge attempt") == 1, "A non-participant's attempt was replayed");
    // The celebration: the other player stands at Player2 with the local skater's lift over its spot
    // (a root is not at the feet), and a pose already at spot 0 on their own machine moves over whole.
    {
        ChallengeCelebration party{};
        party.count = 2;
        party.spots[0] = {1, 1, 1, 0, 0, 0, 0, 1, 4.f, -870.f, 77.f, 0};
        party.spots[1] = {1, 1, 1, 0, 0, 0, 0, 1, 5.5f, -870.f, 77.f, 0};
        test_celebration = party;
        auto posed = in;
        posed.position = std::array<float, 3>{4.f, -869.f, 77.f};
        tick(posed);
        std::this_thread::sleep_for(std::chrono::milliseconds(850)); // the local skater gets there first
        tick(posed);
        take_throwdown_relay_notices();
        const auto same = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        const auto there = throwdown_relay_celebration_offset(leader, {4.2f, -869.f, 77.f});
        check(there && same((*there)[0], 1.5f) && same((*there)[1], 0.f) && same((*there)[2], 0.f),
              "A player in their own celebration was not moved over to their spot whole");
        const auto away = throwdown_relay_celebration_offset(leader, {100.f, 20.f, 30.f});
        check(away && same(100.f + (*away)[0], 5.5f) && same(20.f + (*away)[1], -869.f) && same(30.f + (*away)[2], 77.f),
              "A player elsewhere was not stood at their spot at the local skater's height");
        check(!throwdown_relay_celebration_offset(other, {0.f, 0.f, 0.f}), "A non-participant was placed in the celebration");
    }
    receive(leader, make(Kind::challenge_leave, me, key));
    tick(in);
    check(count("challenge leave") == 1 && find("challenge leave")->id == 0x303, "A remote quit did not let that player go");
    throwdown_relay_local({Local::challenge_ended});
    tick(in);
    calls.clear();

    // Invites off: a start stays the local player's own.
    set_challenge_invites(false);
    tick(in);
    check(!throwdown_relay_challenge_players("OTS", "Plot-2"), "A start invited players with invites off");
    set_challenge_invites(true);

    // Outside the party: a nearby player is not invited, and their challenge doesn't pull us in.
    const ThrowdownRelayInput strangers{me, {{leader, 3, "Lead", true, false}, {other, 5, "Far", false, true}}, true, 77};
    tick(strangers);
    check(!throwdown_relay_challenge_players("OTS", "Plot-2"), "A start invited a nearby player outside the party");
    auto foreign = make(Kind::challenge_start, leader, 20);
    foreign.series = "Session"; foreign.challenge = "Plot-020-Session-01"; foreign.order = {leader, me};
    receive(leader, foreign);
    out = tick(strangers);
    check(out.size() == 1 && out[0].kind == Kind::challenge_optout && count("challenge start") == 0,
          "A challenge from outside the party pulled the player in");
    tick(in);

    // A guest: the leader's start runs the same challenge here with the leader as a virtual player.
    auto start = make(Kind::challenge_start, leader, 11);
    start.series = "Session"; start.challenge = "Plot-020-Session-01"; start.order = {leader, me};
    receive(leader, start);
    tick(in);
    check(count("challenge start") == 1 && find("challenge start")->series == "Session Plot-020-Session-01",
          "A guest did not start its copy of the leader's challenge");
    const auto guest_players = throwdown_relay_challenge_players("Session", "Plot-020-Session-01");
    check(guest_players && *guest_players == std::vector<std::uint32_t>{0x303}, "The guest copy did not get the leader");
    auto early = make(Kind::challenge_attempt, leader, 11);
    early.criteria.assign(0x14, 3); early.indexes.assign(4, 0);
    receive(leader, early);
    tick(in);
    check(count("challenge attempt") == 0, "An attempt was replayed before the local copy started");
    ThrowdownLocalAction guest_started{Local::challenge_started};
    guest_started.series = "Session"; guest_started.challenge = "Plot-020-Session-01"; guest_started.participants = {2, 0x303};
    throwdown_relay_local(std::move(guest_started));
    tick(in);
    check(count("challenge attempt") == 1 && find("challenge attempt")->id == 0x303,
          "An attempt that arrived before the copy started was not replayed once it did");
    // The leader drops out of the session: the copy lets them go.
    const ThrowdownRelayInput alone{me, {{other, 5, "Far", false, true}}, true, 77};
    tick(alone);
    check(count("challenge leave") == 1 && find("challenge leave")->id == 0x303, "A departed player stayed in the copy");
    throwdown_relay_local({Local::challenge_ended});
    tick(alone);
    take_throwdown_relay_notices();

    // Flagged for game speed mid-challenge: the local player leaves their copy.
    {
        auto again = make(Kind::challenge_start, leader, 13);
        again.series = "Session"; again.challenge = "Plot-020-Session-01"; again.order = {leader, me};
        receive(leader, again);
        tick(in);
        ThrowdownLocalAction flagged_start{Local::challenge_started};
        flagged_start.series = "Session"; flagged_start.challenge = "Plot-020-Session-01"; flagged_start.participants = {2, 0x303};
        throwdown_relay_local(std::move(flagged_start));
        tick(in);
        calls.clear();
        ThrowdownRelayInput barred{me, {}, true, 77};
        barred.barred = true;
        tick(barred);
        check(count("challenge leave") == 1 && find("challenge leave")->id == 2,
              "A player flagged for game speed stayed in the coop challenge");
        take_throwdown_relay_notices();
        tick(in);
    }

    // Busy (another challenge running here): the invite is declined.
    receive(other, start); // not the leader
    auto busy = start; busy.id = 12;
    set_challenge_invites(false);
    receive(leader, busy);
    out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::challenge_optout, "A declined invite did not opt out");
    set_challenge_invites(true);
    end_session();
}
// Party beacons: the local one is relayed; another player's is shown here as theirs and goes
// when they remove it or leave.
void beacon_flow() {
    const ThrowdownRelayInput in{me, {{leader, 3, "Lead", true, true}, {other, 5, "Far", false, false}}, true, 90};
    tick(in);
    calls.clear();
    ThrowdownLocalAction placed{Local::beacon_placed};
    placed.location[12] = 10; placed.location[14] = 5;
    throwdown_relay_local(placed);
    auto out = tick(in);
    check(out.size() == 1 && out[0].kind == Kind::beacon && out[0].add && out[0].location[12] == 10,
          "The local beacon was not relayed");
    const auto revision = out[0].id;
    // A second first placement while one stands is refused natively: nothing moves.
    placed.location[12] = 50;
    throwdown_relay_local(placed);
    check(tick(in).empty(), "A refused second placement was relayed");
    placed.add = true; // the move the player agreed to
    throwdown_relay_local(placed);
    out = tick(in);
    check(out.size() == 1 && out[0].location[12] == 50 && out[0].id != revision, "A moved beacon was not relayed");

    auto theirs = make(Kind::beacon, leader, 1);
    theirs.add = true; theirs.location[12] = 7;
    receive(leader, theirs);
    tick(in);
    check(count("beacon move") == 1 && find("beacon move")->id == 0x303 && find("beacon move")->value == 7,
          "Another player's beacon was not shown as theirs");
    receive(leader, theirs); // the periodic resend
    tick(in);
    check(count("beacon move") == 1, "A resent beacon was shown again");
    theirs.id = 2; theirs.location[12] = 8;
    receive(leader, theirs);
    tick(in);
    check(count("beacon move") == 2 && calls.back().value == 8, "A moved beacon was not moved here");
    auto spoof = theirs; spoof.leader = other;
    receive(leader, spoof);
    tick(in);
    check(count("beacon move") == 2, "A beacon was shown for someone other than its sender");
    auto removed = make(Kind::beacon, leader, 3);
    receive(leader, removed);
    tick(in);
    check(count("beacon remove") == 1 && find("beacon remove")->id == 0x303, "A removed beacon stayed");
    // Leaving the session takes it too.
    theirs.id = 4;
    receive(leader, theirs);
    tick(in);
    const ThrowdownRelayInput gone{me, {{other, 5, "Far", false, false}}, true, 90};
    tick(gone);
    check(count("beacon remove") == 2, "A departed player's beacon stayed");
    // The local beacon removed.
    throwdown_relay_local({Local::beacon_removed});
    out = tick(gone);
    check(out.size() == 1 && out[0].kind == Kind::beacon && !out[0].add, "The local beacon's removal was not relayed");
    end_session();
}
// A level change takes the event types with it. The relay asks for them again instead of
// remembering that it once had them, and shows a drop only when they are back.
void level_change_flow() {
    using namespace std::chrono_literals;
    const ThrowdownRelayInput in{me, {{leader, 3}}, true, 77};
    tick(in);
    types_ready = false;
    std::this_thread::sleep_for(300ms); // past the relay's quarter-second re-check
    calls.clear();
    receive(leader, offer(leader, 0x4321));
    tick(in);
    check(count("prepare") == 1 && !find("spawn"), "A drop was shown while the level's event types were not found");
    types_ready = true;
    std::this_thread::sleep_for(300ms);
    tick(in);
    check(count("spawn") == 1, "The drop was not shown once the event types were found again");
    end_session();
}
} // namespace

// A score read from text: what atoi reads, and digit groups read whole.
void grouped_number_checks() {
    using dingosdk::throwdowns_text::parse_grouped_int;
    const auto same = [](const char *text, int value, bool grouped) {
        const auto parsed = parse_grouped_int(text);
        return parsed.value == value && parsed.grouped == grouped;
    };
    if (!(same("5432", 5432, false) && same("0", 0, false) && same("  -17", -17, false) && same("+9", 9, false) &&
          same("", 0, false) && same("abc", 0, false) && same("12abc", 12, false) && same(nullptr, 0, false)))
        throw std::runtime_error("A plain number was not read as atoi reads it");
    if (!(same("5,432", 5432, true) && same("1,250", 1250, true) && same("12,345,678", 12345678, true) &&
          same("5.432", 5432, true) && same("5 432", 5432, true) && same("5'432", 5432, true) &&
          same("5\xC2\xA0" "432", 5432, true) && same("5\xE2\x80\xAF" "432", 5432, true) && same("-1,000 pts", -1000, true)))
        throw std::runtime_error("A grouped score was cut short");
    if (!(same("12.5", 12, false) && same("5,43", 5, false) && same("5,4321", 5, false) && same("1234,567", 1234, false) &&
          same("5, 432", 5, false) && same("5,,432", 5, false) && same("1,234.5", 1234, true)))
        throw std::runtime_error("Text that is not a grouped number was read as one");
    if (!(same("99999999999", INT_MAX, false) && same("-99999999999", INT_MIN, false) && same("999,999,999,999", INT_MAX, true)))
        throw std::runtime_error("A number out of range did not saturate");
}
int main() {
    try {
        guest_flow();
        leader_flow();
        removal_flow();
        spot_battle_flow();
        grouped_number_checks();
        skate_flow();
        alone_flow();
        challenge_flow();
        beacon_flow();
        level_change_flow();
        std::cout << "Throwdown relay: guest, leader, removal, Spot Battle, S.K.A.T.E., last-player, coop challenge, party beacon and level change flows passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
