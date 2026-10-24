// Park mods on the dedicated server (server_parks.cpp): reading a park mod's file, and spawning,
// timing out and removing parks as the server's own objects, read the way a player's game reads
// them. The server runs over a transport that is not Steam's, with simulated players.
//
//   dingosdk_server_park_tests <Server/Test/fixtures/park_mods>
#include "Server/server_host.h"
#include "Engine/Game/World/world_names.h"
#include "Server/server_parks.h"
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

// One player's game, as far as the parks go: what the server sent it, read the way its session
// reads it (session_receive.cpp): an object update counts only from a source the roster names,
// with that source's epoch, through the server.
struct Game {
    std::uint64_t id{}, epoch{}, secret{}, map{}, world = 1;
    std::uint32_t sequence{};
    std::string name;
    DeltaReceiver receiver;
    std::optional<Member> server;
    ObjectState park; // the server's objects as this game holds them
    std::vector<std::string> chat;
    std::vector<std::string> votes; // the roster's: the votes of the server's own that are on
    bool refused{}; // an object update the game would not have taken
    bool joined{};

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
    void say(std::string_view text, std::uint64_t now) {
        auto p = header(PacketKind::chat, now);
        p.text = std::string(text);
        send(p);
    }
    // Once a second: the map is loaded here (and the server hears from the player).
    void ready(std::uint64_t now) {
        auto p = header(PacketKind::world_ready, now);
        p.world_ready = true;
        send(p);
    }
    void read() {
        for (const auto &bytes : std::exchange(wire().sent[id], {})) {
            if (pose_batch::is_batch(bytes) || pose_batch::Ack::read(bytes) || sound_codec::is_sound(bytes)) continue;
            bool missing{};
            const auto p = receiver.receive(bytes, missing, 0);
            if (!p) continue;
            if (p->kind == PacketKind::welcome) joined = true;
            if (p->kind == PacketKind::world_state) {
                if (p->world > world) park = {}; // clear_world: a new map starts every layout over
                world = p->world;
                if (p->map) map = p->map;
            }
            if (p->kind == PacketKind::roster) {
                for (const auto &member : p->members)
                    if (member.id == p->source) server = member;
                votes.clear();
                for (const auto &v : p->server_custom_votes) votes.push_back(v.name);
            }
            if (p->kind == PacketKind::chat) chat.push_back(p->text);
            if (p->kind == PacketKind::objects) {
                if (!server || p->source != server->id || !routed_source(*p, *server, server->id, false, server->id) ||
                    park.receive(p->objects) == ObjectState::Result::invalid)
                    refused = true;
            }
        }
    }
};

int run(const std::filesystem::path &fixtures) {
    // ---- Reading park mods -------------------------------------------------------------------
    {
        const auto street = load_park_mod(fixtures, {"street", "popular skate 2 street park", "San Vansterdam", "bam"});
        check(street.key == "bam" && street.objects.size() == 8 && street.skipped.empty(), "The street park did not load whole");
        const auto &first = street.objects.front();
        check(first.item == "own_bkpads_generic_padlow_00001" && first.position == std::array<float, 3>{-583.f, 1948.35f, 1651.f} &&
                  first.rotation == std::array<float, 4>{0.f, .3827f, 0.f, .9239f} && first.scale == 1.f,
              "The street park's first object is not the file's");
        // No key: the mod's only park.
        check(load_park_mod(fixtures, {"street", "popular skate 2 street park", "San Vansterdam", ""}).key == "bam",
              "A mod's only park was not found without a key");
        const auto mixed = load_park_mod(fixtures, {"plaza", "mixed park", "Isle of Grom", "grom"});
        check(mixed.objects.size() == 2 && mixed.skipped.size() == 3, "Objects players' games would not show were not skipped");
        check(mixed.skipped.size() == 3 && mixed.skipped[0] == "#2 vehicle_car_00001" && mixed.skipped[2].starts_with("#4 "),
              "Skipped objects are not named by their place and item");
        check(mixed.objects.size() == 2 && mixed.objects[1].scale == 2.5f, "A park object's size was lost");
        const auto refused = [&](ParkMod park, std::string_view why) {
            try {
                load_park_mod(fixtures, park);
            } catch (const std::exception &e) {
                if (std::string(e.what()).find(why) != std::string::npos) return true;
                std::fprintf(stderr, "  (said: %s)\n", e.what());
            }
            return false;
        };
        // Said without the server's own paths: the reason can reach chat (an admin, a vote).
        check(refused({"x", "not installed", "San Vansterdam", "bam"}, "The mod folder \"not installed\" is not in the Mods folder."),
              "A missing mod folder was not reported, or named the server's path");
        check(refused({"x", "popular skate 2 street park", "Isle of Grom", "grom"}, "has no park for grom"), "A missing park file was not reported");
        check(refused({"x", "two maps", "San Vansterdam", ""}, "set \"key\""), "A mod with parks for two maps did not ask for a key");
        check(load_park_mod(fixtures, {"x", "two maps", "Super Ultra Mega Resort", "mpr"}).objects.size() == 3, "A park chosen by its key did not load");
        check(refused({"x", "broken", "San Vansterdam", "bam"}, "cannot be read"), "A park file that is not JSON was not reported");
        check(refused({"x", "not a park", "San Vansterdam", "bam"}, "not a ReSkate park"), "A file without the park format was accepted");
        check(refused({"x", "../park_mods/broken", "San Vansterdam", "bam"}, "not a mod folder name"), "A path was taken for a folder");
    }

    // ---- The server --------------------------------------------------------------------------
    const auto empty = std::filesystem::temp_directory_path() / "reskate_server_park_tests_mods";
    std::filesystem::create_directories(empty);
    load_levels(empty); // the retail maps
    ServerConfig config;
    config.file = std::filesystem::temp_directory_path() / "reskate_server_park_tests" / "ReSkateServer.json";
    // A copy of the fixtures the test may change, as an owner updates a mod while the server runs.
    const auto mods = std::filesystem::temp_directory_path() / "reskate_server_park_tests_live";
    std::filesystem::remove_all(mods);
    std::filesystem::copy(fixtures, mods, std::filesystem::copy_options::recursive);
    config.mods = mods;
    config.auto_update = false;
    config.park_mods = {{"street", "popular skate 2 street park", "San Vansterdam", "bam"},
                        {"street2", "popular skate 2 street park", "San Vansterdam", "bam"},
                        {"street3", "popular skate 2 street park", "San Vansterdam", ""},
                        {"plaza", "mixed park", "Isle of Grom", "grom"},
                        {"gone", "not installed", "San Vansterdam", "bam"}};
    config.park_limits = {2, 20};
    config.votes.cooldown = 0;
    CustomVote park_vote{"park", "Spawn a park for 30 min", "park-mod add {arg} 30", {"street", "plaza"}};
    CustomVote unpark_vote{"unpark", "Remove a park", "park-mod remove {arg}", {"street", "plaza"}};
    config.votes.custom = {park_vote, unpark_vote};
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
        game.id = id;
        game.epoch = 5000 + id % 1000;
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
        if (!admin) log.push_back(reply); // as the console writes its replies to the log
        return reply;
    };
    const auto logged = [&](std::string_view text) { return has(log, text); };

    // Roles (Server/roles.h): the player is an admin, whose "*" gives them park-mod.
    const std::uint64_t admin_id = 76561198000000001ULL;
    config.roles = default_roles();
    config.roles.players[admin_id] = "admin";
    auto &a = add_game(admin_id, "Robin");
    a.hello(now);
    run_for(2000000);
    check(a.joined && a.server && a.server->id == server_steam_id, "The player did not join, or the roster does not name the server");

    // Spawned: the server's own objects, which the player's game takes from the server.
    auto reply = command("park-mod add street 2");
    check(reply == "[parks] street spawned (8 objects, 2 min) by console", "Spawn reply: " + reply);
    const auto spawned_at = now;
    run_for(1000000);
    check(!a.refused && a.park.objects().size() == 8, "The player's game did not receive the park as the server's objects");
    check(has(a.chat, "The street park is here for 2 min."), "Players were not told of the park");

    // What cannot be spawned, and nothing is spawned of it.
    check(command("park-mod add street").find("already spawned") != std::string::npos, "A park was spawned twice");
    reply = command("park-mod add plaza");
    check(reply.find("[parks] plaza could not be spawned: plaza belongs on Isle of Grom") == 0, "A park was spawned on another map: " + reply);
    check(command("park-mod add nothing").find("no park mod called") != std::string::npos, "An unknown park was not refused");
    reply = command("park-mod add gone");
    check(reply.find("[parks] gone could not be spawned: The mod folder \"not installed\"") == 0, "A missing mod was not an error line: " + reply);
    check(command("park-mod add street2 street3").find("at most 2 parks") != std::string::npos, "The parks limit was not kept");
    check(command("park-mod add street2 0").find("1 to 1440") != std::string::npos, "0 minutes was accepted");

    // Several at once, each with IDs of its own.
    reply = command("park-mod add street2");
    check(reply == "[parks] street2 spawned (8 objects, until removed) by console", "Second park: " + reply);
    run_for(1000000);
    check(!a.refused && a.park.objects().size() == 16, "Two parks did not reach the player as one layout");
    reply = command("park-mod list");
    check(reply.find("2 of 2 spawned, 16 of 20 objects") != std::string::npos && reply.find("street  \"popular skate 2 street park\" (bam)") != std::string::npos &&
              reply.find("2 min left") != std::string::npos && reply.find("until removed") != std::string::npos,
          "list: " + reply);

    // Roles: a moderator's role does not list park-mod; a role that lists it gives it alone.
    {
        const std::uint64_t mod_id = 76561198000000003ULL;
        config.roles.players[mod_id] = "moderator";
        auto &m = add_game(mod_id, "Moddy");
        m.hello(now);
        run_for(2000000);
        m.say("/park-mod list", now);
        run_for(500000);
        check(has(m.chat, "Your role (moderator) cannot use park-mod."), "A moderator could use park-mod");
        Role builder;
        builder.name = "builder";
        builder.rank = 20;
        builder.commands = {"park-mod"};
        config.roles.list.push_back(builder);
        config.roles.settle();
        config.roles.players[mod_id] = "builder";
        m.say("/park-mod", now); // not the same line again: chat turns repeats away
        run_for(500000);
        check(has(m.chat, "Park mods (2 of 2 spawned"), "A role listing park-mod could not use it");
        check(!has(m.chat, "Your role (builder) cannot use"), "A role listing park-mod was refused it");
        wire().status.peers.erase(std::remove_if(wire().status.peers.begin(), wire().status.peers.end(),
                                                 [&](const auto &peer) { return peer.id == mod_id; }),
                                  wire().status.peers.end());
        games.erase(mod_id);
        run_for(1000000);
    }

    // An admin in game: by their name.
    a.say("/park-mod remove street2", now);
    run_for(500000);
    check(logged("[parks] street2 removed (by Robin)"), "An admin's removal was not logged by name");
    check(has(a.chat, "[parks] street2 removed (by Robin)"), "The admin was not told");
    run_for(500000);
    check(a.park.objects().size() == 8, "A removed park stayed on the player's map");
    // The objects limit: all or none.
    config.park_limits.parks = 3;
    reply = command("park-mod add street2 street3");
    check(reply.find("that makes 24 park objects, and at most 20") != std::string::npos, "The objects limit was not kept: " + reply);
    check(command("park-mod list").find("1 of 3 spawned, 8 of 20 objects") != std::string::npos, "Half of a refused add was spawned");
    config.park_limits = {3, 512};

    // Expiry: a minute's warning, then gone.
    run_for(spawned_at + 61000000 - now);
    check(has(a.chat, "The street park goes in a minute."), "No warning a minute before a park went");
    check(!logged("removed (expired)"), "A park went early");
    run_for(spawned_at + 121000000 - now);
    check(logged("[parks] street removed (expired)") && has(a.chat, "The street park is gone."), "An expired park was not removed");
    check(a.park.revision() && a.park.objects().empty(), "An expired park stayed on the player's map");

    // Players vote for one (the owner's custom votes run park-mod as the console).
    a.say("/vote park street", now);
    run_for(1000000);
    check(logged("[parks] street spawned (8 objects, 30 min) by a vote"), "A vote did not spawn the park");
    a.say("/vote unpark street", now);
    run_for(1000000);
    check(logged("[parks] street removed (by a vote)"), "A vote did not remove the park");
    a.say("/vote park plaza", now);
    run_for(1000000);
    check(has(a.chat, "The plaza park could not be spawned: plaza belongs on Isle of Grom"), "A vote's failure did not reach chat");

    // clear, and a player who joins late is sent the park, and it stays when they leave.
    command("park-mod add street3");
    auto &b = add_game(76561198000000002ULL, "Late");
    b.hello(now);
    run_for(2000000);
    check(!b.refused && b.park.objects().size() == 8, "A player who joined later was not sent the park");
    wire().status.peers.erase(std::remove_if(wire().status.peers.begin(), wire().status.peers.end(),
                                             [&](const auto &peer) { return peer.id == b.id; }),
                              wire().status.peers.end());
    games.erase(b.id);
    run_for(2000000);
    check(command("park-mod list").find("1 of 3 spawned") != std::string::npos, "A park went with a player");
    reply = command("park-mod clear");
    check(reply == "[parks] street3 removed (by console)", "clear: " + reply);
    check(command("park-mod clear") == "No parks are spawned.", "clear with nothing spawned");

    // Live: a spawned park's file read again, in place, keeping its timer.
    const auto street_file = mods / "popular skate 2 street park" / "parks" / "bam.park.json";
    command("park-mod add street 10");
    run_for(1000000);
    std::ofstream(street_file, std::ios::binary | std::ios::trunc) << R"({"format": "reskate-park", "schema_version": 1, "revision": 1,
        "maps": {"bam": [{"id": 1, "item": "own_bkpads_generic_padlow_00001", "position": [0, 10, 0], "rotation": [0, 0, 0, 1]},
                         {"id": 2, "item": "own_bkpads_generic_padlow_00001", "position": [4, 10, 0], "rotation": [0, 0, 0, 1]},
                         {"id": 3, "item": "own_bkpads_generic_padlow_00001", "position": [8, 10, 0], "rotation": [0, 0, 0, 1]}]}})";
    reply = command("park-mod reload street");
    check(reply == "[parks] street reloaded (3 objects) by console", "reload: " + reply);
    run_for(1000000);
    check(!a.refused && a.park.objects().size() == 3, "A reloaded park did not reach the player");
    check(has(a.chat, "The street park was updated."), "Players were not told of the update");
    check(command("park-mod list").find("3 objects, 10 min left") != std::string::npos, "A reload lost the park's timer");
    check(command("park-mod reload street2").find("street2 is not spawned") != std::string::npos, "A park that is not up was reloaded");
    check(command("park-mod reload nothing").find("no park mod called") != std::string::npos, "An unknown park was reloaded");
    std::ofstream(street_file, std::ios::binary | std::ios::trunc) << "{ not json";
    check(command("park-mod reload street").find("[parks] street could not be reloaded: ") == 0, "A broken file was reloaded");
    run_for(1000000);
    check(a.park.objects().size() == 3, "A failed reload changed the park");
    std::filesystem::copy_file(fixtures / "popular skate 2 street park" / "parks" / "bam.park.json", street_file,
                               std::filesystem::copy_options::overwrite_existing);
    command("park-mod clear");

    // Live: parks set up from the console or an admin, saved to the config, no restart.
    reply = command("park-mod config add newpark | popular skate 2 street park | san vansterdam | bam");
    check(reply == "[parks] newpark set up (\"popular skate 2 street park\", bam on San Vansterdam) by console", "config add: " + reply);
    {
        const auto saved = load_config(config.file);
        check(!saved.park_mods.empty() && saved.park_mods.back().name == "newpark" && saved.park_mods.back().map == "San Vansterdam" &&
                  saved.park_mods.back().folder == "popular skate 2 street park" && saved.park_mods.back().key == "bam",
              "A park set up live was not saved to the config");
    }
    check(command("park-mod add newpark") == "[parks] newpark spawned (8 objects, until removed) by console", "A park set up live did not spawn");
    check(command("park-mod config add newpark | popular skate 2 street park | San Vansterdam").find("already set up") != std::string::npos,
          "A park was set up twice");
    check(command("park-mod config add New Park! | x | San Vansterdam").find("could not be set up: park_mods") != std::string::npos,
          "A bad park name was set up");
    check(command("park-mod config add elsewhere | x | Nowhere").find("is not a single known map") != std::string::npos, "A park on an unknown map was set up");
    check(command("park-mod config add later | ../x | San Vansterdam").find("folder must be") != std::string::npos, "A path was set up as a folder");
    check(command("park-mod config add half | x").find("park-mod config add <name>") == 0, "A park without a map was set up");
    reply = command("park-mod config add later | copied later | Isle of Grom");
    check(reply.find("its folder is not in the Mods folder yet") != std::string::npos, "A missing folder was not mentioned: " + reply);
    a.say("/park-mod config remove later", now);
    run_for(500000);
    check(logged("[parks] later no longer set up (by Robin)"), "An admin could not remove a park from the setup");
    reply = command("park-mod config remove newpark");
    check(reply == "[parks] newpark removed (by console)\n[parks] newpark no longer set up (by console)", "config remove: " + reply);
    run_for(1000000);
    check(a.park.objects().empty(), "A park removed from the setup stayed on the map");
    {
        const auto saved = load_config(config.file);
        check(std::none_of(saved.park_mods.begin(), saved.park_mods.end(),
                           [](const auto &park) { return park.name == "newpark" || park.name == "later"; }),
              "A park removed from the setup is still in the config");
    }
    check(command("park-mod config remove newpark").find("no park mod called") != std::string::npos, "A park was removed from the setup twice");

    // A map change takes the parks with it.
    command("park-mod add street");
    run_for(1000000);
    command("map Isle of Grom");
    check(logged("[parks] street removed (map change)"), "A map change did not remove the park");
    run_for(3000000);
    check(a.park.objects().empty(), "A park stayed after the map changed");
    reply = command("park-mod add plaza 5");
    check(reply.starts_with("[parks] plaza spawned (2 objects, 5 min) by console\n[parks] plaza: skipped 3 object(s)"), "plaza: " + reply);
    run_for(1000000);
    check(!a.refused && a.park.objects().size() == 2, "The park on the new map did not reach the player");
    const auto &objects = a.park.objects();
    check(std::any_of(objects.begin(), objects.end(), [](const auto &row) { return row.second.scale == 2.5f; }), "A resized object lost its size");
    check(command("park-mod add street").find("belongs on San Vansterdam") != std::string::npos, "A park for the old map was spawned");
    // A map mod copied in while the server runs is known after "maps reload".
    {
        check(command("maps").find("Brand New") == std::string::npos, "A map that is not installed is listed");
        std::filesystem::create_directories(mods / "someone-Brand_New");
        std::ofstream(mods / "someone-Brand_New" / "reskate-levels.json", std::ios::binary)
            << R"({"schema":1,"levels":[{"asset":"levels/game/dingolevel_reskate_brandnew/dingolevel_reskate_brandnew","displayName":"Brand New"}]})";
        reply = command("maps reload");
        check(reply.find("new: Brand New") != std::string::npos, "maps reload: " + reply);
        check(command("maps").find("Brand New") != std::string::npos && find_level("Brand New"), "A map copied in was not known after a reload");
        check(command("maps reload").find("none new") != std::string::npos, "A second reload found the same map new again");
        // A custom map goes by its own name: its level is dingolevel_reskate_<map>, and "reskate"
        // is no part of what is shown or typed (typed the old way, it still means the map).
        check(dingosdk::world_level_name("levels/game/dingolevel_reskate_brandnew/dingolevel_reskate_brandnew") == "brandnew" &&
                  dingosdk::world_level_name("Levels/Game/BAM_LevelRoot/BAM_LevelRoot") == "San Vansterdam",
              "A custom map's name still starts with reskate, or a game map's name changed");
        const auto *brand_new = find_level("Brand New");
        check(brand_new && find_level("brandnew") == brand_new && find_level("reskate brandnew") == brand_new &&
                  find_level("RESKATE brand") == brand_new && !find_level("reskate"),
              "A custom map was not found by its own name, or by the old one");
    }
    // Votes are decided by those who vote, at the end of their time: with three players on, one
    // yes and nobody else voting passes; a yes and a no fails (60% of the votes cast is needed);
    // and two yes passes at once, since the third player could not change it.
    {
        auto &c = add_game(76561198000000003ULL, "Cee");
        auto &d = add_game(76561198000000004ULL, "Dee");
        c.map = d.map = a.map; // the map and world the server is on now
        c.world = d.world = a.world;
        c.hello(now);
        d.hello(now);
        run_for(2000000);
        check(c.joined && d.joined, "The players for the votes did not join");
        const auto said = [&](std::string_view text) {
            return std::count_if(a.chat.begin(), a.chat.end(), [&](const std::string &line) { return line.find(text) != std::string::npos; });
        };
        const auto passed = said("passed ("), failed = said("failed ("), alone = said("passed (1 yes, 0 no, 1 needed of 1 vote)");
        a.say("/vote unpark street", now);
        run_for(5000000);
        check(said("passed (") == passed && said("failed (") == failed, "A vote two players had not voted in was decided before its time");
        run_for(27000000);
        check(said("passed (1 yes, 0 no, 1 needed of 1 vote)") == alone + 1, "A vote did not pass on the votes cast");
        a.say("/vote unpark street", now);
        run_for(1000000);
        c.say("/no", now);
        run_for(5000000);
        check(said("failed (") == failed && said("passed (") == passed + 1, "A vote a player could still pass was decided before its time");
        run_for(27000000);
        check(said("failed (1 yes, 1 no, 2 needed of 2 votes)") == 1, "A vote did not fail on the votes cast");
        a.say("/vote unpark street", now);
        run_for(1000000);
        d.say("/yes", now);
        run_for(1000000);
        check(said("passed (2 yes, 0 no, 2 needed of 2 votes)") == 1, "A vote nobody could change any more was not decided at once");
    }
    // Poll answers: the same /2 in two polls back to back is an answer in each (chat turns
    // repeats away, answers not), twice in one poll it is still one; a chat line said twice is
    // still turned away. The card shows the question and the result, so chat names neither; the
    // log has the result.
    {
        const auto said = [&](std::string_view text) {
            return std::count_if(a.chat.begin(), a.chat.end(), [&](const std::string &line) { return line.find(text) != std::string::npos; });
        };
        const auto logged = [&](std::string_view text) {
            return std::count_if(log.begin(), log.end(), [&](const std::string &line) { return line.find(text) != std::string::npos; });
        };
        auto &c = games.at(76561198000000003ULL);
        command("poll First? | yes | no");
        a.say("/2", now);
        c.say("/2", now);
        run_for(1000000);
        a.say("/2", now);
        run_for(1000000);
        check(said("You already answered no.") == 1, "A second answer in the same poll was not answered as one");
        command("poll end");
        run_for(1000000);
        check(logged("Poll \"First?\" ended: yes 0, no 2 (no wins).") == 1, "A poll counted an answer given twice twice");
        check(!said("First?"), "A poll's question or result was repeated in chat");
        command("poll Second? | yes | no");
        a.say("/2", now);
        run_for(1000000);
        command("poll end");
        run_for(1000000);
        check(logged("Poll \"Second?\" ended: yes 0, no 1 (no wins).") == 1, "The same answer in the next poll was turned away");
        c.say("hello again", now);
        run_for(1000000);
        c.say("hello again", now);
        run_for(1000000);
        check(said("hello again") == 1, "The same chat line twice in a row was not turned away");
    }
    // The rotation: the minutes left are announced, the vote's maps are drawn a minute ahead,
    // and the vote decides: staying adds time, a map changes to it.
    {
        const auto said = [&](std::string_view text) {
            return std::count_if(a.chat.begin(), a.chat.end(), [&](const std::string &line) { return line.find(text) != std::string::npos; });
        };
        auto &c = games.at(76561198000000003ULL);
        auto &d = games.at(76561198000000004ULL);
        command("rotation vote on");
        command("rotation stay 2");
        check(command("rotation 3").find("A vote for the next map") != std::string::npos, "rotation with a vote: " + command("rotation"));
        run_for(61000000); // two minutes left: nothing of the vote yet
        check(!said("Vote for the next map"), "The vote for the next map started early");
        run_for(120000000); // the time is up: the vote runs, its answers three maps, any, and staying
        check(said("Vote for the next map") == 1 && said("/4 Random map  /5 Stay here") == 1, "The vote for the next map did not start as it should");
        a.say("/5", now);
        run_for(1000000);
        check(!said("The vote keeps"), "The vote ended before everyone had voted");
        run_for(31000000);
        check(said("The vote keeps") == 1 && said("for 2 more minutes") == 1, "Staying did not win the vote");
        check(command("rotation").find("in about 2 min") != std::string::npos, "Staying did not add its time: " + command("rotation"));
        // "/vote extend": the server's own vote for the same, at any time. It is in the votes games list.
        command("rotation stay 4");
        run_for(1000000);
        check(std::find(a.votes.begin(), a.votes.end(), "extend") != a.votes.end(), "The extend vote is not among the votes games are told of");
        a.say("/vote extend", now);
        run_for(1000000);
        c.say("/yes", now);
        d.say("/yes", now);
        run_for(1000000);
        check(said("more minutes passed (") == 1, "The vote to extend the map did not pass");
        reply = command("rotation");
        check(reply.find("in about 5 min") != std::string::npos, "/vote extend did not add its time: " + reply); // a minute was left, and four more
        command("votes extend off");
        check(command("vote extend") == "Extend votes are off on this server.", "An extend vote started while it was off: " + command("vote extend"));
        command("votes extend on");
        command("rotation stay 2");
        run_for(180000000);
        // The next time everyone votes for the first map: the vote ends at once and the map changes.
        run_for(121000000);
        check(said("Vote for the next map") == 2, "The vote did not come round again after staying");
        const auto before = said("Next map: ");
        a.say("/1", now);
        c.say("/1", now);
        d.say("/1", now);
        run_for(1000000);
        check(said("Next map: ") == before + 1 || logged("[rotation] Next map: "), "The vote did not change the map once everyone had voted");
        // "rotation extend" gives the map in play more time and says so.
        run_for(3000000);
        reply = command("rotation extend 5");
        check(reply.find("in about 7 min") != std::string::npos || reply.find("in about 8 min") != std::string::npos, "rotation extend: " + reply);
        command("rotation off");
        command("rotation vote off");
    }

    // ---- Away --------------------------------------------------------------------------------
    // A player whose skater stands still while their game says they are at it (flying a plugin's
    // drone) stays; one whose game says nothing is removed when the away limit runs out.
    {
        command("afk-kick 2");
        auto &pilot = add_game(76561198000000071ULL, "Pilot");
        auto &asleep = add_game(76561198000000072ULL, "Asleep");
        pilot.map = asleep.map = a.map; // the map and world the server is on now
        pilot.world = asleep.world = a.world;
        pilot.hello(now);
        asleep.hello(now);
        run_for(2000000);
        check(pilot.joined && asleep.joined, "The away test's players did not join");
        // Their games load the map, as a game does once let in, and are in: the time away starts.
        for (auto *game : {&pilot, &asleep}) {
            auto loading = game->header(PacketKind::world_ready, now);
            loading.world_ready = false;
            game->send(loading);
        }
        run_for(2000000);
        for (int i = 0; i < 9; ++i) {
            pilot.send(pilot.header(PacketKind::present, now));
            // A flood of them counts no more than one.
            pilot.send(pilot.header(PacketKind::present, now));
            run_for(present_interval_us);
        }
        check(wire().closed.contains(asleep.id) && logged("[afk] Asleep was removed after 2 min away."),
              "A player who did nothing was not removed for being away");
        check(!wire().closed.contains(pilot.id), "A player whose game said they were at it was removed for being away: " +
                                                      (wire().closed.contains(pilot.id) ? wire().closed[pilot.id] : std::string{}));
        command("afk-kick off");
    }
    check(!wire().closed.contains(admin_id), "The player was disconnected: " + (wire().closed.contains(admin_id) ? wire().closed[admin_id] : std::string{}));
    return failures;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dingosdk_server_park_tests <park mod fixtures folder>\n");
        return 2;
    }
    try {
        const auto result = run(argv[1]);
        if (!result) std::printf("server park tests passed\n");
        return result ? 1 : 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
