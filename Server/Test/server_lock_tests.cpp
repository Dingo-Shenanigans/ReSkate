// Tools locked for single players (lock, unlock, locks, throwdown_locks): each player's roster
// and permissions as their game reads them, with the server over a transport that is not
// Steam's and simulated players. Players without locks get what everyone got before.
//
//   dingosdk_server_lock_tests
#include "Server/server_host.h"
#include "Engine/Game/World/world_names.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/sound_codec.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include "Extension/Multiplayer/Session/room.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Engine/Game/Build/supported_build.h"

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
// What the fake transport holds: what each player is sent, and what they send.
struct Wire {
    TransportStatus status;
    std::vector<TransportMessage> inbound;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> sent;
    std::map<std::uint64_t, std::string> closed;
};
Wire &wire() {
    static Wire value;
    return value;
}
} // namespace

// A game server's SteamID, as Steam gives the server one: never a player's.
constexpr std::uint64_t server_steam_id = (1ULL << 56) | (4ULL << 52) | 1;

struct SteamTransport::Impl {};
SteamTransport::SteamTransport() = default;
SteamTransport::~SteamTransport() = default;
bool SteamTransport::open() { return true; }
bool SteamTransport::open_game_server(void *) { return true; }
bool SteamTransport::host(unsigned) {
    wire().status.ready = wire().status.hosting = true;
    wire().status.local_id = server_steam_id;
    return true;
}
bool SteamTransport::join(std::uint64_t, std::uint32_t, std::uint16_t) { return false; }
bool SteamTransport::listen_direct(std::uint16_t) { return true; }
void SteamTransport::set_packing(unsigned) {}
std::vector<std::string> SteamTransport::take_direct_notes() { return {}; }
bool SteamTransport::set_steam_debug(bool) { return false; }
bool SteamTransport::connect_peer(std::uint64_t) { return false; }
void SteamTransport::allow_peers(std::span<const Member>) {}
bool SteamTransport::socket_test() { return true; }
void SteamTransport::stop() {}
void SteamTransport::disconnect(std::uint64_t id, const char *reason) {
    wire().closed[id] = reason ? reason : "";
    std::erase_if(wire().status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
}
void SteamTransport::poll() {}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t> bytes, bool, bool, TrafficLane) {
    if (std::none_of(wire().status.peers.begin(), wire().status.peers.end(), [&](const auto &peer) { return peer.id == id; })) return false;
    wire().sent[id].emplace_back(bytes.begin(), bytes.end());
    return true;
}
void SteamTransport::send_batch(std::span<TransportSend> messages) {
    for (auto &message : messages) message.sent = send(message.id, message.bytes, message.reliable, message.fresh, message.lane);
}
std::vector<TransportMessage> SteamTransport::receive() { return std::exchange(wire().inbound, {}); }
std::string SteamTransport::name(std::uint64_t) { return {}; }
const TransportStatus &SteamTransport::status() const { return wire().status; }
std::string SteamTransport::take_closed(std::uint64_t) { return {}; }
std::string SteamTransport::link_report(std::uint64_t) { return {}; }
std::int64_t SteamTransport::pending(std::uint64_t) const { return 0; }
std::string SteamTransport::relay_status() const { return "OK"; }
bool SteamTransport::set_send_rate(int) { return true; }
int SteamTransport::send_rate() const { return 900 * 1024; }
std::vector<TransportLink> SteamTransport::links() { return {}; }
bool SteamTransport::bind(void *, void *, void *) { return true; }
} // namespace dingosdk::multiplayer

namespace {
using namespace dingosdk;
using namespace dingosdk::multiplayer;
using namespace dingosdk::server;

int failures{};
void check(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}
bool has(const std::vector<std::string> &lines, std::string_view text) {
    return std::any_of(lines.begin(), lines.end(), [&](const auto &line) { return line.find(text) != std::string::npos; });
}

// One player's game, as far as the tools go: the roster's tool flags and the permissions its
// role is sent with, as its session reads them (session_receive.cpp).
struct Game {
    std::uint64_t id{}, epoch{}, secret{}, map{}, world = 1;
    std::uint32_t sequence{};
    std::string name;
    DeltaReceiver receiver;
    bool joined{};
    unsigned rosters{};
    bool noclip{}, no_bail{}, boosts{}, enforce_tuning{};
    std::uint32_t announcement{};
    std::optional<std::uint32_t> permissions;
    std::vector<std::string> chat;

    Packet header(PacketKind kind, std::uint64_t now) {
        Packet p;
        p.kind = kind;
        p.session = secret;
        p.map = map;
        p.world = world;
        p.epoch = epoch;
        p.source = id;
        p.time_us = now;
        p.sequence = ++sequence;
        p.build = supported_build::game_sha256_bytes;
        return p;
    }
    void send(const Packet &p) { wire().inbound.push_back({id, encode_wire(p), p.time_us}); }
    void hello(std::uint64_t now) {
        wire().status.peers.push_back({id, true});
        auto p = header(PacketKind::hello, now);
        p.text = name;
        send(p);
    }
    void leave() {
        std::erase_if(wire().status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
    }
    void say(std::string_view text, std::uint64_t now) {
        auto p = header(PacketKind::chat, now);
        p.text = std::string(text);
        send(p);
    }
    void ready(std::uint64_t now) {
        auto p = header(PacketKind::world_ready, now);
        p.world_ready = true;
        send(p);
    }
    void throwdown(const ThrowdownMessage &message, std::uint64_t now) {
        auto p = header(PacketKind::throwdown, now);
        p.throwdown = encode_throwdown(message);
        send(p);
    }
    // What their game lets them use: the roster's flag, or their role's "movement".
    bool may_noclip() const { return noclip || (permissions && (*permissions & role_movement)); }
    bool may_no_bail() const { return no_bail || (permissions && (*permissions & role_movement)); }
    bool may_boost() const { return boosts || (permissions && (*permissions & role_movement)); }
    void read() {
        for (const auto &bytes : std::exchange(wire().sent[id], {})) {
            if (pose_batch::is_batch(bytes) || pose_batch::Ack::read(bytes) || sound_codec::is_sound(bytes)) continue;
            bool missing{};
            const auto p = receiver.receive(bytes, missing, 0);
            if (!p) continue;
            if (p->kind == PacketKind::welcome) joined = true;
            if (p->kind == PacketKind::world_state) {
                world = p->world;
                if (p->map) map = p->map;
            }
            if (p->kind == PacketKind::roster) {
                ++rosters;
                noclip = p->guest_noclip;
                no_bail = p->guest_no_bail;
                boosts = p->guest_boosts;
                enforce_tuning = p->enforce_tuning;
                announcement = p->announcement.id;
            }
            if (p->kind == PacketKind::permissions) permissions = p->role_permissions;
            if (p->kind == PacketKind::chat) chat.push_back(p->text);
        }
    }
};

int run() {
    const auto empty = std::filesystem::temp_directory_path() / "reskate_server_lock_tests_mods";
    std::filesystem::create_directories(empty);
    load_levels(empty); // the retail maps
    ServerConfig config;
    config.file = std::filesystem::temp_directory_path() / "reskate_server_lock_tests" / "ReSkateServer.json";
    config.mods = empty;
    config.auto_update = false;
    // A server for free skating: every tool allowed, everyone's own physics.
    config.noclip = config.no_bail = config.boosts = true;
    config.enforce_tuning = false;
    config.roles = default_roles();
    Role trainer; // "movement" without commands: noclip and the rest even where players may not
    trainer.name = "trainer";
    trainer.rank = 5;
    trainer.permissions = role_movement;
    config.roles.list.push_back(trainer);
    config.roles.settle();
    const std::uint64_t admin_id = 76561198000000001ULL, b_id = 76561198000000002ULL, c_id = 76561198000000003ULL,
                        d_id = 76561198000000004ULL;
    config.roles.players[admin_id] = "admin";
    config.roles.players[d_id] = "trainer";
    check(config_error(config).empty(), "The test's config was refused: " + config_error(config));

    std::vector<std::string> log;
    SteamTransport transport;
    Host host(config, transport, [&](const std::string &line) { log.push_back(line); });
    std::string error;
    if (!host.start(error)) {
        std::fprintf(stderr, "The server did not start: %s\n", error.c_str());
        return 1;
    }
    const auto invite = parse_invite(host.invite());
    if (!invite) {
        std::fprintf(stderr, "No invite.\n");
        return 1;
    }
    std::map<std::uint64_t, Game> games;
    const auto add_game = [&](std::uint64_t id, std::string name) -> Game & {
        auto &game = games[id];
        game = {};
        game.id = id;
        game.epoch = 5000 + id % 1000 + games.size();
        game.secret = invite->secret;
        game.map = map_hash(map_destination(config.map));
        game.name = std::move(name);
        return game;
    };
    std::uint64_t now = 1000000000ULL, next_ready = now;
    const auto run_for = [&](std::uint64_t microseconds) {
        for (const auto end = now + microseconds; now < end; now += 50000) {
            if (now >= next_ready) {
                next_ready = now + 1000000;
                for (auto &[id, game] : games)
                    if (game.joined) game.ready(now);
            }
            host.tick(now);
            for (auto &[id, game] : games) game.read();
        }
    };
    const auto command = [&](std::string_view line, std::uint64_t admin = 0) {
        const auto reply = host.command(line, admin);
        if (!admin) log.push_back(reply);
        run_for(500000); // the next roster and permissions go out
        return reply;
    };
    const auto logged = [&](std::string_view text) { return has(log, text); };
    // Every tool the server allows, and its own physics off: what a player had before locks.
    const auto free = [](const Game &g) { return g.noclip && g.no_bail && g.boosts && !g.enforce_tuning; };

    auto &a = add_game(admin_id, "Robin");
    auto &b = add_game(b_id, "Bailey Two");
    auto &c = add_game(c_id, "Casey");
    auto &d = add_game(d_id, "Dana");
    for (auto *g : {&a, &b, &c, &d}) g->hello(now);
    run_for(3000000);
    for (auto *g : {&a, &b, &c, &d}) check(g->joined && g->rosters && g->permissions, g->name + " did not join");

    // ---- Without locks: everyone has what the server allows ----------------------------------
    for (auto *g : {&a, &b, &c, &d}) check(free(*g), g->name + "'s roster is not the server's own without locks");
    check(a.permissions && (*a.permissions & role_movement) && d.permissions && (*d.permissions & role_movement),
          "A role's movement was not sent without locks");
    auto reply = command("locks");
    check(reply == "0 players have locked tools", "locks with none: " + reply);

    // ---- lock: one player's roster says the tools are off; nobody else's changes --------------
    reply = command("lock Bailey Two nobail,boosts"); // a name with a space, the tools last
    check(reply == "Locked for Bailey Two: nobail, boosts.", "lock reply: " + reply);
    check(logged("[lock] Bailey Two (76561198000000002): nobail, boosts locked by console"), "The lock was not logged");
    check(b.noclip && !b.no_bail && !b.boosts && !b.enforce_tuning, "Bailey's roster does not have nobail and boosts locked");
    check(!b.may_no_bail() && !b.may_boost() && b.may_noclip(), "Bailey's game would still allow the locked tools");
    check(has(b.chat, "The server locked for you: nobail, boosts."), "Bailey was not told");
    for (auto *g : {&a, &c, &d}) check(free(*g), g->name + "'s roster changed with someone else's lock");
    check(command("lock 76561198000000002 nobail") == "Bailey Two already has nobail locked.", "A second lock of the same tool changed something");

    // ---- tuning: their own physics stand down; their role's movement is untouched -------------
    reply = command("lock Casey tuning");
    check(reply == "Locked for Casey: tuning.", "lock tuning: " + reply);
    check(c.enforce_tuning && c.noclip && c.no_bail && c.boosts, "Casey's roster does not enforce the tuning, or lost a tool");
    check(command("lock Dana tuning") == "Locked for Dana: tuning.", "lock Dana tuning");
    check(d.permissions && (*d.permissions & role_movement), "A tuning lock took a role's movement away");

    // ---- A role with movement cannot lift a lock -----------------------------------------------
    command("noclip off");
    command("nobail off");
    check(!c.noclip && !c.no_bail && c.boosts && d.may_noclip() && d.may_no_bail(), "noclip/nobail off did not reach the players");
    reply = command("lock Dana nobail");
    check(reply == "Locked for Dana: nobail.", "lock Dana nobail: " + reply);
    check(d.permissions && !(*d.permissions & role_movement), "A locked player's permissions still have movement");
    check(!d.may_no_bail(), "Dana's role lifted the No Bail lock");
    check(d.may_noclip() && d.may_boost() && d.enforce_tuning, "Dana lost what their role gives beside the locked tool");
    check(!c.may_noclip() && b.permissions && !(*b.permissions & role_movement), "Players without movement got it");
    command("noclip on");
    command("nobail on");

    // ---- Players with the same locks share a roster; a one-player announcement still works ----
    check(command("lock Dana nobail,boosts") == "Locked for Dana: boosts.", "lock Dana boosts");
    check(command("unlock Dana tuning") == "Unlocked for Dana: tuning.", "unlock Dana tuning");
    check(logged("[lock] Dana (76561198000000004): tuning unlocked by console"), "The unlock was not logged");
    // Bailey and Dana: nobail, boosts. Dana's role would give them back; the roster folds it in.
    check(!d.no_bail && !d.boosts && d.noclip && !d.enforce_tuning, "Dana's roster is not Bailey's");
    command("announce-to Casey Your heat is next");
    check(c.announcement && !a.announcement && !b.announcement && !d.announcement, "A one-player announcement reached others");
    check(c.enforce_tuning && free(a), "The announcement changed the locks");

    // ---- locks lists them ----------------------------------------------------------------------
    reply = command("locks");
    check(reply.starts_with("3 players have locked tools") && reply.find("76561198000000002  Bailey Two: nobail, boosts") != std::string::npos &&
              reply.find("Casey: tuning") != std::string::npos && reply.find("Dana: nobail, boosts") != std::string::npos,
          "locks: " + reply);

    // ---- Wrong use ------------------------------------------------------------------------------
    check(command("lock Casey jetpack").starts_with("lock <player|SteamID64>"), "An unknown tool was accepted");
    check(command("lock Nobody nobail").starts_with("No single connected player"), "An unknown player was accepted");
    check(command("lock Casey").starts_with("lock <player|SteamID64>"), "lock without tools was accepted");
    // An admin in game cannot lock a player of their rank or above; they can lock others, from chat too.
    check(command("lock Robin all", admin_id).starts_with("You cannot lock"), "An admin locked their own rank");
    a.say("/lock Casey noclip", now);
    run_for(1000000);
    check(logged("[lock] Casey (76561198000000003): noclip locked by Robin"), "An admin's lock from chat was not logged by their name");
    check(!c.noclip && has(a.chat, "Locked for Casey: noclip."), "An admin's lock from chat did not apply");

    // ---- unlock gives back exactly what everyone has -----------------------------------------
    reply = command("unlock Bailey Two");
    check(reply == "Unlocked for Bailey Two: nobail, boosts.", "unlock: " + reply);
    check(free(b) && b.permissions && !(*b.permissions & role_movement), "Bailey's roster is not the server's own after unlock");
    check(has(b.chat, "The server unlocked for you: nobail, boosts."), "Bailey was not told of the unlock");
    command("unlock Dana all");
    check(free(d) && d.permissions && (*d.permissions & role_movement), "Dana did not get their roster and movement back");
    check(command("unlock Dana") == "Dana has none of noclip, nobail, boosts, tuning, ollies, objects locked.", "unlock with nothing locked");
    check(command("lock Dana objects") == "Locked for Dana: objects.", "objects did not lock by name");
    check(command("unlock Dana") == "Unlocked for Dana: objects.", "a bare unlock did not take back objects");

    // ---- An admin's locks stay with the player when they leave and join again -----------------
    c.leave();
    run_for(2000000);
    check(logged("[lock] Casey (76561198000000003): noclip, tuning stay locked if they join again"), "Leaving did not keep the admin's locks");
    reply = command("locks");
    check(reply.find("76561198000000003  Casey (not here): noclip, tuning") != std::string::npos, "locks does not list a player who left: " + reply);
    games.erase(c_id);
    {
        auto &back = add_game(c_id, "Casey");
        back.hello(now);
        run_for(3000000);
        check(back.joined && !back.noclip && back.enforce_tuning && back.no_bail && back.boosts, "Casey's locks did not come back when they joined again");
        check(has(back.chat, "The server locked for you: noclip, tuning.") &&
                  logged("[lock] Casey (76561198000000003): noclip, tuning locked again (joined)"),
              "Casey was not told their locks are back");
        back.leave();
        run_for(2000000);
    }
    // Unlocked while away, by the name they had or their SteamID64: they come back free.
    reply = command("unlock Casey tuning");
    check(reply == "Unlocked for Casey: tuning.", "unlock of a player who left, by name: " + reply);
    check(logged("[lock] Casey (76561198000000003): tuning unlocked by console (not here)"), "The unlock of a player who left was not logged");
    reply = command("unlock 76561198000000003");
    check(reply == "Unlocked for Casey: noclip.", "unlock of a player who left, by SteamID64: " + reply);
    // A player who is not here is locked by their SteamID64.
    reply = command("lock 76561198000000009 nobail");
    check(reply == "Locked for 76561198000000009: nobail.", "lock of a player who is not here: " + reply);
    check(command("lock Somebody nobail").find("their SteamID64") != std::string::npos, "lock of an unknown name did not say how");
    check(command("unlock 76561198000000009") == "Unlocked for 76561198000000009: nobail.", "unlock of a player never seen");
    games.erase(c_id);
    auto &c2 = add_game(c_id, "Casey");
    c2.hello(now);
    run_for(3000000);
    check(c2.joined && free(c2), "Locks came back after they were unlocked");
    check(command("locks") == "0 players have locked tools", "Locks remain after everyone was unlocked");

    // ---- throwdown_locks: the players of a running throwdown --------------------------------------
    check(command("throwdown-locks").find("(now off)") != std::string::npos, "throwdown-locks is not off by default");
    config.activity_log = false; // locks follow throwdowns without the activity log
    config.announce_throwdowns = false;
    reply = command("throwdown-locks nobail,tuning");
    check(reply == "The players of a throwdown have nobail, tuning locked while it runs.", "throwdown-locks: " + reply);
    ThrowdownMessage offer;
    offer.kind = ThrowdownMessage::Kind::offer;
    offer.leader = b_id;
    offer.id = 7;
    offer.series = "ThrowdownSkate";
    offer.placement = {1, 2, 3, 4};
    b.throwdown(offer, now);
    run_for(500000);
    auto join = offer;
    join.kind = ThrowdownMessage::Kind::join;
    join.series.clear();
    join.placement.clear();
    d.throwdown(join, now);
    run_for(500000);
    check(free(b) && free(d), "A queue locked its players before it started");
    auto start = join;
    start.kind = ThrowdownMessage::Kind::start;
    start.order = {b_id, d_id};
    b.throwdown(start, now);
    run_for(500000);
    check(logged("[lock] Bailey Two (76561198000000002): nobail, tuning locked by throwdown") &&
              logged("[lock] Dana (76561198000000004): nobail, tuning locked by throwdown"),
          "The throwdown's players were not locked");
    check(!b.no_bail && b.enforce_tuning && b.boosts && !d.may_no_bail() && d.enforce_tuning && d.may_boost(),
          "The throwdown's players' rosters do not have its locks");
    check(has(b.chat, "Locked for you while your throwdown runs: nobail, tuning."), "Bailey was not told of the throwdown's locks");
    check(free(a) && free(c2), "Players outside the throwdown were locked");
    reply = command("locks");
    check(reply.find("Bailey Two: (throwdown: nobail, tuning)") != std::string::npos, "locks does not show a throwdown's: " + reply);
    // Changing the setting while it runs changes only what differs, and says so.
    command("throwdown-locks nobail,boosts");
    check(logged("[lock] Bailey Two (76561198000000002): tuning unlocked by throwdown-locks") &&
              logged("[lock] Bailey Two (76561198000000002): boosts locked by throwdown-locks"),
          "Changing throwdown-locks during a throwdown did not change just the difference");
    check(!b.no_bail && !b.boosts && !b.enforce_tuning, "Bailey's roster does not have the new throwdown locks");
    check(has(b.chat, "The server's throwdown locks changed, unlocked for you: tuning.") &&
              has(b.chat, "The server's throwdown locks changed, locked for you while it runs: boosts.") &&
              !has(b.chat, "Your throwdown is over"),
          "Bailey was told their throwdown was over when only the setting changed");
    command("throwdown-locks nobail,tuning");
    check(!b.no_bail && b.boosts && b.enforce_tuning, "Bailey's roster did not go back with the setting");
    // An admin's lock outlasts the throwdown; quitting ends the throwdown's.
    command("lock Dana boosts");
    auto quit = join;
    quit.kind = ThrowdownMessage::Kind::leave;
    d.throwdown(quit, now);
    run_for(500000);
    check(logged("[lock] Dana (76561198000000004): nobail, tuning unlocked by throwdown"), "Quitting the throwdown did not unlock it");
    check(d.no_bail && !d.enforce_tuning && !d.boosts && has(d.chat, "Your throwdown is over, unlocked: nobail, tuning."),
          "Dana's roster after quitting: the throwdown's gone, the admin's kept");
    check(!b.no_bail && b.enforce_tuning, "Another player's quitting ended Bailey's throwdown");
    // Gone quiet: a S.K.A.T.E. sends nothing more for 90 s once it has finished.
    run_for(95000000);
    check(logged("[lock] Bailey Two (76561198000000002): nobail, tuning unlocked by throwdown") && free(b),
          "The throwdown's end did not unlock its players");
    command("unlock Dana");
    // Off: nothing is locked for a throwdown.
    command("throwdown-locks off");
    offer.id = 8;
    b.throwdown(offer, now);
    start.id = 8;
    b.throwdown(start, now);
    run_for(500000);
    check(free(b) && free(d), "throwdown-locks off still locked a throwdown's players");

    for (auto *g : {&a, &b, &d})
        check(!wire().closed.contains(g->id), g->name + " was disconnected: " + (wire().closed.contains(g->id) ? wire().closed[g->id] : std::string{}));
    return failures;
}
} // namespace

int main() {
    try {
        const auto result = run();
        if (!result) std::printf("server lock tests passed\n");
        return result ? 1 : 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
