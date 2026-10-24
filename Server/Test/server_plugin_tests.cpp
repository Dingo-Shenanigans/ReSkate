// The dedicated server's plugins (server_plugins.cpp) with the server's host, over a transport
// that is not Steam's and simulated players: what a server plugin sees of the players' skaters
// and roles.
//
//   dingosdk_server_plugin_tests <Server/Test/fixtures/park_mods>
#include "Server/server_host.h"
#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/pose_codec.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/World/world_names.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
// What the fake transport holds: what each player is sent, and what they send.
struct Wire {
    TransportStatus status;
    std::vector<TransportMessage> inbound;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> sent;
};
Wire &wire() {
    static Wire value;
    return value;
}
} // namespace

struct SteamTransport::Impl {};
SteamTransport::SteamTransport() = default;
SteamTransport::~SteamTransport() = default;
bool SteamTransport::open() { return true; }
bool SteamTransport::open_game_server(void *) { return true; }
bool SteamTransport::host(unsigned) {
    wire().status.ready = wire().status.hosting = true;
    wire().status.local_id = (1ULL << 56) | (4ULL << 52) | 1; // a game server's SteamID, never a player's
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
void SteamTransport::disconnect(std::uint64_t id, const char *) {
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
void write(const std::filesystem::path &file, const std::string &text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// One player's game: its greeting, its world being ready, and its poses sent as a game sends
// them (pose_batch.h), standing at `at` and facing `heading` about world up.
struct Player {
    std::uint64_t id{}, epoch{}, secret{}, map{};
    std::string name;
    std::array<float, 3> at{};
    float heading{};
    std::uint32_t sequence{};
    struct Kept {
        std::uint32_t sequence{};
        std::uint64_t time_us{};
        pose_codec::QuantPose pose;
    };
    std::deque<Kept> kept;
    pose_batch::Sender upload;

    Packet header(PacketKind kind, std::uint64_t now) {
        Packet p;
        p.kind = kind;
        p.session = secret;
        p.map = map;
        p.world = 1;
        p.epoch = epoch;
        p.source = id;
        p.time_us = now;
        p.sequence = ++sequence;
        p.build = supported_build::game_sha256_bytes;
        return p;
    }
    void hello(std::uint64_t now) {
        wire().status.peers.push_back({id, true});
        auto p = header(PacketKind::hello, now);
        p.text = name;
        wire().inbound.push_back({id, encode_wire(p), now});
    }
    void ready(std::uint64_t now) {
        auto p = header(PacketKind::world_ready, now);
        p.world_ready = true;
        wire().inbound.push_back({id, encode_wire(p), now});
    }
    void chat(std::uint64_t now, const std::string &text) {
        auto p = header(PacketKind::chat, now);
        p.text = text;
        wire().inbound.push_back({id, encode_wire(p), now});
    }
    void throwdown(std::uint64_t now, const ThrowdownMessage &message) {
        auto p = header(PacketKind::throwdown, now);
        p.throwdown = encode_throwdown(message);
        wire().inbound.push_back({id, encode_wire(p), now});
    }
    // A moment of voice chat, heard everywhere (no hearing distance).
    void speak(std::uint64_t now) {
        auto p = header(PacketKind::voice, now);
        p.voice.distance = 0.f;
        p.voice.bytes = {1, 2, 3, 4};
        wire().inbound.push_back({id, encode_wire(p), now});
    }
    // What the server sent this player since the last look: the latest roster, how many skaters'
    // poses (entries of its pose messages) and how much voice. Acks for their own poses are taken.
    struct Seen {
        std::optional<Packet> roster;
        std::size_t poses{}, voices{};
    };
    Seen seen;
    void read_sent() {
        for (const auto &bytes : std::exchange(wire().sent[id], {})) {
            if (const auto ack = pose_batch::Ack::read(bytes)) upload.ack(*ack);
            else if (const auto batch = pose_batch::read(bytes)) seen.poses += batch->entries.size();
            else if (auto p = decode_wire(bytes); p && p->kind == PacketKind::roster) seen.roster = std::move(p);
            else if (p && p->kind == PacketKind::voice) ++seen.voices;
        }
    }
    Seen look() {
        read_sent();
        return std::exchange(seen, {});
    }
    // The latest roster the server sent this player (and forgets what else it sent).
    std::optional<Packet> roster() { return look().roster; }
    void pose(std::uint64_t now) {
        read_sent();
        auto p = header(PacketKind::pose, now);
        p.pose.root.position = at;
        p.pose.root.rotation = {0.f, std::sin(heading * 0.5f), 0.f, std::cos(heading * 0.5f)};
        p.pose.skater.resize(pose_codec::finger_skeleton);
        p.pose.skater[1].position = at;
        p.pose.board.resize(16);
        p.pose.board[0].position = at;
        kept.push_back({p.sequence, p.time_us, pose_codec::quantize(p.pose)});
        while (kept.size() > 64) kept.pop_front();
        const auto find = [&](std::uint32_t sequence) -> std::optional<pose_batch::KeptView> {
            for (auto it = kept.rbegin(); it != kept.rend(); ++it)
                if (it->sequence == sequence) return pose_batch::KeptView{it->sequence, it->time_us, &it->pose};
            return {};
        };
        const auto emit = [&](std::span<const std::uint8_t> message, bool) {
            wire().inbound.push_back({id, {message.begin(), message.end()}, now});
            return true;
        };
        upload.begin(1, map);
        upload.add(p.source, p.epoch, *find(p.sequence), find, false, false, pose_batch::Rate::full, now, emit);
        upload.flush(emit);
    }
};

int run(const std::filesystem::path &fixtures) {
    const auto root = std::filesystem::temp_directory_path() / "reskate_server_plugin_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Mods");
    for (const auto *folder : {"popular skate 2 street park", "mixed park"})
        std::filesystem::copy(fixtures / folder, root / "Mods" / folder, std::filesystem::copy_options::recursive);
    load_levels(root / "Mods"); // the retail maps
    ServerConfig config;
    config.file = root / "server" / "ReSkateServer.json";
    config.mods = root / "Mods";
    config.auto_update = false;
    config.activity_log = false;
    config.roles = default_roles();
    config.roles.players[76561198000000001ULL] = "moderator"; // the second player has the default role
    config.park_mods = {{"street", "popular skate 2 street park", "San Vansterdam", "bam"}, {"plaza", "mixed park", "Isle of Grom", "grom"}};
    check(config_error(config).empty(), "The test's config was refused: " + config_error(config));

    // A plugin that prints the skaters it sees every 20th tick (a second here), and one without
    // session.skaters. Ticks, not reskate.time: its clock is the real one, and this test's is not.
    write(root / "Plugins" / "watcher" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "session.skaters"]})");
    write(root / "Plugins" / "watcher" / "plugin" / "main.lua", R"(
        local ticks = 0
        reskate.on("tick", function()
            ticks += 1
            if ticks % 20 ~= 0 then return end
            local list = reskate.session.skaters()
            reskate.console.print("skaters " .. #list)
            for _, s in ipairs(list) do
                reskate.console.print(string.format("skater %s at %.1f %.1f %.1f facing %.2f, %s", s.id, s.position.x, s.position.y,
                    s.position.z, s.heading, s.age_ms >= 0 and s.age_ms < 1000 and "fresh" or ("stale " .. s.age_ms)))
            end
        end)
    )");
    write(root / "Plugins" / "blind" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console"]})");
    write(root / "Plugins" / "blind" / "plugin" / "main.lua", R"(
        local ticks = 0
        reskate.on("tick", function()
            ticks += 1
            if ticks % 20 ~= 0 then return end
            local ok, why = pcall(reskate.session.skaters)
            reskate.console.print(ok and "blind saw them" or "blind refused: " .. tostring(why))
        end)
        reskate.console.command("blind parks", "park mods without server.parks", function()
            local ok, why = pcall(reskate.server.parks)
            reskate.console.print(ok and "blind listed parks" or "blind parks refused: " .. tostring(why))
        end)
        reskate.console.command("blind content", "mod content without mods.read", function()
            local ok, why = pcall(reskate.mods.content, "parks")
            reskate.console.print(ok and "blind listed content" or "blind content refused: " .. tostring(why))
        end)
    )");

    // One that says what the level is, and when it changes.
    write(root / "Plugins" / "follower" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console"]})");
    write(root / "Plugins" / "follower" / "plugin" / "main.lua", R"(
        reskate.on("level_unloading", function(name) reskate.console.print("unloading " .. name) end)
        reskate.on("level_loaded", function(name) reskate.console.print("loaded " .. name) end)
        local ticks = 0
        reskate.on("tick", function()
            ticks += 1
            if ticks % 20 == 0 then reskate.console.print("level " .. reskate.level.current().name) end
        end)
    )");

    // One that lists, spawns and removes park mods when the console asks it to.
    write(root / "Plugins" / "parker" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "server.parks"]})");
    write(root / "Plugins" / "parker" / "plugin" / "main.lua", R"(
        reskate.console.command("parker", "park mods, for the test", function(args)
            local verb, name = args[1], args[2]
            if verb == "list" then
                for _, p in ipairs(reskate.server.parks()) do
                    reskate.console.print(string.format("park %s on %s, here %s, spawned %s, %d objects, %d min left", p.name, p.map,
                        tostring(p.here), tostring(p.spawned), p.objects, p.minutes_left))
                end
            elseif verb == "spawn" then
                local ok, why = reskate.server.park_spawn(name, tonumber(args[3]))
                reskate.console.print("spawn " .. name .. ": " .. (ok and "done" or why))
            elseif verb == "remove" then
                local ok, why = reskate.server.park_remove(name)
                reskate.console.print("remove " .. name .. ": " .. (ok and "done" or why))
            elseif verb == "bad" then
                local named = pcall(reskate.server.park_spawn, "Street; map grom")
                local timed = pcall(reskate.server.park_spawn, "street", 5000)
                reskate.console.print("bad " .. (named and "name taken" or "name refused") .. ", " .. (timed and "minutes taken" or "minutes refused"))
            end
        end)
    )");

    // One that says each player's role, and one without server that may not know them.
    write(root / "Plugins" / "ranks" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "session.read", "server"]})");
    write(root / "Plugins" / "ranks" / "plugin" / "main.lua", R"(
        reskate.console.command("ranks", "players' roles, for the test", function()
            for _, p in ipairs(reskate.session.players()) do
                local role = p.role
                if role then
                    reskate.console.print(string.format("role %s %s rank %d staff %s", p.id, role.name, role.rank, tostring(role.staff)))
                    reskate.console.print(pcall(function() role.rank = 1000 end) and "role changed" or "role read-only")
                elseif not p.server then
                    reskate.console.print("role " .. p.id .. " none")
                end
            end
        end)
    )");
    write(root / "Plugins" / "rankless" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "session.read"]})");
    write(root / "Plugins" / "rankless" / "plugin" / "main.lua", R"(
        reskate.console.command("rankless", "roles without server", function()
            local p = reskate.session.players()[1]
            local ok, why = pcall(function() return p.role end)
            reskate.console.print(ok and "rankless saw a role" or "rankless refused: " .. tostring(why))
        end)
    )");

    // One that reads the parks folder of the mods the server has, as a game's plugin reads its
    // enabled mods'.
    write(root / "Plugins" / "reader" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "mods.read"], "content": ["parks"]})");
    write(root / "Plugins" / "reader" / "plugin" / "main.lua", R"(
        reskate.console.command("reader", "mod content, for the test", function()
            for _, f in ipairs(assert(reskate.mods.content("parks"))) do
                reskate.console.print(string.format("content %s (%s) %s %d", f.mod, f.title, f.path, f.size))
            end
            local text = assert(reskate.mods.read_content("mixed park", "parks", "grom.park.json"))
            reskate.console.print("read " .. #text .. " " .. tostring(text:find('"reskate-park"', 1, true) ~= nil))
            local missing, why = reskate.mods.read_content("mixed park", "parks", "nothing.json")
            reskate.console.print("missing " .. tostring(missing) .. " " .. why)
        end)
    )");

    // One that asks players and shows them cards, and says what came back.
    write(root / "Plugins" / "asker" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "server"]})");
    write(root / "Plugins" / "asker" / "plugin" / "main.lua", R"(
        reskate.on("poll_answer", function(e) reskate.console.print(string.format("answer %s %d %s", e.player, e.answer, e.text)) end)
        reskate.on("poll_closed", function(e) reskate.console.print(string.format("closed %s %s", e.player, e.why)) end)
        reskate.console.command("asker", "cards and polls, for the test", function(args)
            local who = args[2] == "all" and "all" or args[2]
            if args[1] == "poll" then
                local id, why = reskate.server.poll(who, "Next track?", {"Docks", "Hill"}, {seconds = tonumber(args[3]) or 30})
                reskate.console.print("poll " .. tostring(id or why))
            elseif args[1] == "card" then
                local ok, why = reskate.server.card(who, "Race in 30 s", 5)
                reskate.console.print("card " .. tostring(ok or why))
            end
        end)
    )");

    // One that hears chat and the /commands the server does not know, reads the map, and changes
    // the map and the time of day for everyone.
    write(root / "Plugins" / "talker" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server",
              "permissions": ["console", "chat.read", "session.read", "travel", "world.environment"]})");
    write(root / "Plugins" / "talker" / "plugin" / "main.lua", R"lua(
        reskate.on("chat", function(line)
            reskate.console.print(string.format("heard %s %s: %s%s", line.sender, line.name, line.text, line.server and " (server)" or ""))
        end)
        reskate.console.command("talker", "chat, the map and the time, for the test", function(args)
            if args[1] == "world" then
                local w = reskate.session.world()
                reskate.console.print("world " .. w.world .. " map " .. w.map)
            elseif args[1] == "levels" then
                local n, named = 0, false
                for _, level in reskate.travel.levels() do
                    n += 1
                    if level.name == "Isle of Grom" and level.can_load then named = true end
                end
                reskate.console.print("levels " .. n .. " " .. tostring(named))
            elseif args[1] == "load" then
                local ok, why = reskate.travel.load(table.concat(args, " ", 2))
                reskate.console.print("load " .. tostring(ok or why))
            elseif args[1] == "tod" then
                local ok, why = reskate.world.set_time(args[2])
                reskate.console.print("tod " .. tostring(ok or why))
            end
        end)
    )lua");

    // One that hides a player from another, wholly or only their voice and sound, and shows them again.
    write(root / "Plugins" / "hider" / "reskate-plugin.json",
          R"({"api": 1, "entry": "plugin/main.lua", "side": "server", "permissions": ["console", "server"]})");
    write(root / "Plugins" / "hider" / "plugin" / "main.lua", R"(
        reskate.console.command("hider", "hiding, for the test", function(args)
            local verb, target, viewer = args[1], args[2], args[3]
            local ok, why
            if verb == "hide" then ok, why = reskate.server.hide(target, viewer)
            elseif verb == "mute" then ok, why = reskate.server.hide(target, viewer, {skater = false})
            elseif verb == "unhide" then ok, why = reskate.server.unhide(target, viewer) end
            reskate.console.print(verb .. " " .. tostring(ok or why))
        end)
    )");

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
    std::vector<Player> players(2);
    for (std::size_t i = 0; i < players.size(); ++i) {
        auto &p = players[i];
        p.id = 76561198000000001ULL + i;
        p.epoch = 5000 + i;
        p.secret = invite->secret;
        p.map = map_hash(map_destination(config.map));
        p.name = "Player " + std::to_string(i + 1);
    }
    players[0].at = {10.f, 2.f, -30.f};
    players[0].heading = 1.f;
    players[1].at = {-4.f, 7.5f, 12.f};

    std::uint64_t now = 1000000000ULL, next_ready = now;
    bool posing{}, speaking{};
    const auto run_for = [&](std::uint64_t microseconds) {
        for (const auto end = now + microseconds; now < end; now += 50000) {
            if (now >= next_ready) {
                next_ready = now + 1000000;
                for (auto &p : players) p.ready(now);
            }
            if (posing)
                for (auto &p : players) p.pose(now);
            if (speaking && (now / 50000) % 4 == 0) players[0].speak(now);
            host.tick(now);
        }
    };
    // Before anyone is in the world, the plugin sees nobody.
    run_for(3000000);
    check(has(log, "skaters 0"), "The plugin did not run, or saw skaters on an empty server");
    check(std::any_of(log.begin(), log.end(),
                     [](const auto &line) { return line.starts_with("blind refused: ") && line.find("session.skaters") != std::string::npos; }),
          "A plugin without session.skaters was not refused");
    check(!has(log, "blind saw them"), "A plugin without session.skaters saw the skaters");

    // Joined and in the world, but no pose yet: still nobody to see.
    for (auto &p : players) p.hello(now);
    run_for(3000000);
    log.clear();
    run_for(1100000);
    check(has(log, "skaters 0"), "A player with no pose yet was a skater");

    // Posing: each where their latest pose put them, facing as it faces.
    posing = true;
    run_for(3000000);
    log.clear();
    run_for(1100000);
    check(has(log, "skaters 2"), "The plugin did not see both skaters");
    check(has(log, "skater 76561198000000001 at 10.0 2.0 -30.0 facing 1.00, fresh"), "The first skater's place, heading or age is wrong");
    check(has(log, "skater 76561198000000002 at -4.0 7.5 12.0 facing 0.00, fresh"), "The second skater's place, heading or age is wrong");
    // A skater who stopped sending stays where they were, older.
    posing = false;
    run_for(2000000);
    log.clear();
    run_for(1100000);
    check(has(log, "skater 76561198000000001 at 10.0 2.0 -30.0 facing 1.00, stale"), "A skater's age did not grow without poses");

    // Roles: the one given (moderator), the server's default (player) for the other, read-only;
    // and nothing for a plugin without server.
    const auto plugin_command = [&](std::string_view line) {
        log.clear();
        check(host.command(line) == "Ran by a plugin.", std::string(line) + " did not reach the plugin");
    };
    plugin_command("ranks");
    check(has(log, "role 76561198000000001 moderator rank 50 staff true"), "The moderator's role is not what the server gave them");
    check(has(log, "role 76561198000000002 player rank 0 staff false"), "A player given no role does not have the default role");
    check(has(log, "role read-only") && !has(log, "role changed"), "A plugin changed a player's role table");
    host.command("role 76561198000000002 admin");
    plugin_command("ranks");
    check(has(log, "role 76561198000000002 admin rank 100 staff true"), "A role given while they are on was not seen");
    plugin_command("rankless");
    check(has(log, "rankless refused: ") && has(log, "server"), "A plugin without server saw a role");

    // Park mods: listed with whether they fit this map; spawned and removed through park-mod's own
    // checks, logged by the plugin's name.
    plugin_command("parker list");
    check(has(log, "park street on San Vansterdam, here true, spawned false, 0 objects, 0 min left"), "street is not listed as it is");
    check(has(log, "park plaza on Isle of Grom, here false, spawned false"), "plaza is not listed as on another map");
    plugin_command("parker spawn street 5");
    check(has(log, "spawn street: done"), "The plugin could not spawn street");
    check(has(log, "[parks] street spawned (") && has(log, "5 min) by plugin parker"), "The spawn was not logged as the plugin's");
    plugin_command("parker list");
    check(has(log, "park street on San Vansterdam, here true, spawned true, ") && has(log, " objects, 5 min left"),
          "street is not listed as spawned for 5 min");
    plugin_command("parker spawn street");
    check(has(log, "spawn street: street is already spawned."), "A spawned park was spawned again");
    plugin_command("parker spawn plaza");
    check(has(log, "spawn plaza: plaza belongs on Isle of Grom"), "A park for another map was spawned");
    plugin_command("parker spawn nothing");
    check(has(log, "spawn nothing: there is no park mod called \"nothing\""), "A park that is not set up was spawned");
    plugin_command("parker bad");
    check(has(log, "bad name refused, minutes refused"), "A bad park name or time was taken");
    plugin_command("parker remove street");
    check(has(log, "remove street: done") && has(log, "[parks] street removed (by plugin parker)"), "The plugin could not remove street");
    plugin_command("parker remove street");
    check(has(log, "remove street: street is not spawned."), "A park not spawned was removed");
    plugin_command("blind parks");
    check(has(log, "blind parks refused: ") && has(log, "server.parks"), "A plugin without server.parks listed the parks");

    // The mods the server has, read through mods.content as a game reads its enabled mods.
    const auto size_of = [&](const char *mod, const char *file) {
        return std::to_string(std::filesystem::file_size(root / "Mods" / mod / "parks" / file));
    };
    plugin_command("reader");
    check(has(log, "content mixed park (mixed park) grom.park.json " + size_of("mixed park", "grom.park.json")) &&
              has(log, "content popular skate 2 street park (popular skate 2 street park) bam.park.json " +
                           size_of("popular skate 2 street park", "bam.park.json")),
          "The server's mods' parks folders were not listed");
    check(has(log, "read " + size_of("mixed park", "grom.park.json") + " true"), "A park mod's file was not read");
    check(has(log, "missing nil not found"), "A missing file was not \"not found\"");
    plugin_command("blind content");
    check(has(log, "blind content refused: ") && has(log, "mods.read"), "A plugin without mods.read listed the mods' content");

    // Cards and polls: each player's own roster carries them, over the server's; a player's answer
    // (as the game sends it, "/2") comes back to the plugin, and so does a poll that ran out.
    const auto first_id = std::to_string(players[0].id), second_id = std::to_string(players[1].id);
    for (auto &p : players) p.roster();
    plugin_command("asker card " + first_id);
    check(has(log, "card true"), "The plugin could not show a card");
    plugin_command("asker poll all 10");
    check(has(log, "poll ") && !has(log, "poll nil"), "The plugin could not ask everyone");
    run_for(200000);
    auto first_roster = players[0].roster(), second_roster = players[1].roster();
    check(first_roster && first_roster->announcement.text == "Race in 30 s" && first_roster->announcement.seconds == 5,
          "The first player's roster has no card");
    check(second_roster && second_roster->announcement.text.empty(), "The second player was shown the first player's card");
    for (auto *r : {&first_roster, &second_roster})
        check(*r && (*r)->vote.label == "Next track?" && (*r)->vote.answers == std::vector<std::string>{"Docks", "Hill"} &&
                  (*r)->vote.kind == server_vote_poll && (*r)->vote.outcome == vote_running && (*r)->vote.seconds == 10,
              "A player's roster has no poll");
    const auto poll_id = first_roster ? first_roster->vote.id : 0;
    check(poll_id && second_roster && second_roster->vote.id == poll_id, "The poll is not one for everyone");
    log.clear();
    players[0].chat(now, "/2");
    run_for(200000);
    check(has(log, "answer " + first_id + " 2 Hill"), "The plugin did not hear the answer");
    first_roster = players[0].roster();
    check(first_roster && first_roster->vote.id != poll_id, "The answered poll stayed on the player's screen");
    players[0].chat(now, "/1");
    run_for(200000);
    check(!has(log, "answer " + first_id + " 1"), "A poll was answered twice");
    log.clear();
    run_for(10000000);
    check(has(log, "closed " + second_id + " timeout") && !has(log, "closed " + first_id), "The unanswered poll did not run out");
    second_roster = players[1].roster();
    check(second_roster && second_roster->vote.id != poll_id && second_roster->announcement.text.empty(), "The poll stayed after it ran out");
    plugin_command("asker poll " + second_id);
    plugin_command("asker poll " + second_id);
    run_for(200000);
    check(has(log, "closed " + second_id + " replaced"), "A new poll did not replace the player's earlier one");
    plugin_command("asker poll 76561198000000099");
    check(has(log, "poll that player is not on the server"), "A poll for nobody here was asked");

    // Chat: a player's line as it is passed on, the server's own, and a /command the server does
    // not know (no "Unknown command" for it: the plugin answers); the server's own commands stay its.
    log.clear();
    players[1].chat(now, "hello everyone");
    run_for(200000);
    check(has(log, "heard 76561198000000002 Player 2: hello everyone"), "The plugin did not hear a player's chat line");
    log.clear();
    host.command("say the server speaks");
    run_for(200000);
    check(has(log, "heard 0 Server: the server speaks (server)"), "The plugin did not hear the server's own line");
    log.clear();
    wire().sent[players[1].id].clear();
    players[1].chat(now, "/race join");
    run_for(200000);
    check(has(log, "heard 76561198000000002 Player 2: /race join"), "A /command the server does not know did not reach the plugin");
    bool unknown{};
    for (const auto &bytes : std::exchange(wire().sent[players[1].id], {}))
        if (auto p = decode_wire(bytes); p && p->kind == PacketKind::chat && p->text.find("Unknown command") != std::string::npos) unknown = true;
    check(!unknown, "The player was told the plugin's /command is unknown");
    log.clear();
    players[1].chat(now, "/help");
    run_for(200000);
    check(!has(log, "heard 76561198000000002 Player 2: /help"), "One of the server's own commands went to the plugin");

    // Hiding: the first player hidden from the second leaves the second's roster and poses, and
    // only theirs; the first still sees the second. Each window is longer than the roster's 2 s.
    const auto in_roster = [](const std::optional<Packet> &roster, std::uint64_t id) {
        return roster && std::any_of(roster->members.begin(), roster->members.end(), [&](const Member &m) { return m.id == id; });
    };
    const auto settle = [&] {
        run_for(300000); // what was on its way before
        for (auto &p : players) p.look();
        run_for(2500000);
    };
    posing = speaking = true;
    settle();
    {
        auto viewer = players[1].look();
        check(viewer.poses > 0 && viewer.voices > 0 && in_roster(viewer.roster, players[0].id),
              "The test needs the second player to see and hear the first");
    }
    plugin_command("hider hide " + first_id + " " + second_id);
    check(has(log, "hide true"), "The plugin could not hide a player");
    settle();
    {
        auto viewer = players[1].look(), target = players[0].look();
        check(viewer.poses == 0 && viewer.voices == 0, "The viewer was still sent the hidden player's poses or voice");
        check(viewer.roster && !in_roster(viewer.roster, players[0].id) && in_roster(viewer.roster, players[1].id),
              "The hidden player was still in the viewer's roster");
        check(target.poses > 0 && in_roster(target.roster, players[1].id), "The hidden player stopped seeing the viewer");
    }
    // Shown again: back in the roster, and their poses follow.
    plugin_command("hider unhide " + first_id + " " + second_id);
    check(has(log, "unhide true"), "The plugin could not show a player again");
    settle();
    {
        auto viewer = players[1].look();
        check(viewer.poses > 0 && viewer.voices > 0 && in_roster(viewer.roster, players[0].id), "A player shown again did not come back");
    }
    // Only their voice and sound: they stay in the roster and their poses go on.
    plugin_command("hider mute " + first_id + " " + second_id);
    check(has(log, "mute true"), "The plugin could not mute a player");
    settle();
    {
        auto viewer = players[1].look();
        check(viewer.poses > 0 && in_roster(viewer.roster, players[0].id), "A muted player was hidden");
        check(viewer.voices == 0, "A muted player's voice was still sent");
    }
    // A plugin that stops shows everyone it hid.
    plugin_command("hider hide " + first_id + " " + second_id);
    check(host.command("plugin off hider").find("hider") != std::string::npos, "The plugin could not be switched off");
    settle();
    {
        auto viewer = players[1].look();
        check(viewer.poses > 0 && viewer.voices > 0 && in_roster(viewer.roster, players[0].id), "A plugin that stopped left a player hidden");
    }
    host.command("plugin on hider");
    run_for(2500000); // it loads again on the next scan
    // Not while they share a party, nor a throwdown.
    players[0].chat(now, "/party invite Player 2");
    run_for(200000);
    players[1].chat(now, "/party accept Player 1");
    run_for(200000);
    plugin_command("hider hide " + first_id + " " + second_id);
    check(has(log, "hide they share a party"), "A player was hidden from someone in their party");
    players[1].chat(now, "/party leave");
    run_for(200000);
    ThrowdownMessage offer;
    offer.kind = ThrowdownMessage::Kind::offer;
    offer.leader = players[0].id;
    offer.id = 1;
    offer.series = "skate";
    offer.placement = {1, 2, 3, 4}; // opaque to the server
    players[0].throwdown(now, offer);
    auto join = offer;
    join.kind = ThrowdownMessage::Kind::join;
    players[1].throwdown(now, join);
    run_for(200000);
    plugin_command("hider hide " + first_id + " " + second_id);
    check(has(log, "hide they are in a throwdown together"), "A player was hidden from someone in their throwdown");
    auto close = offer;
    close.kind = ThrowdownMessage::Kind::close;
    players[0].throwdown(now, close);
    run_for(200000);
    plugin_command("hider hide " + first_id + " " + second_id);
    check(has(log, "hide true"), "A player could not be hidden once the party and the throwdown were over");
    // Hidden, they cannot join the viewer's party either.
    log.clear();
    players[0].chat(now, "/party invite Player 2");
    run_for(200000);
    check(!has(log, "[party] Player 1 invited Player 2"), "A hidden player invited the viewer to their party");
    plugin_command("hider unhide " + first_id);
    check(has(log, "unhide true"), "The plugin could not show a player to everyone");
    posing = speaking = false;

    // The map and the world as the session's, and the maps the server knows.
    plugin_command("talker world");
    check(has(log, "world 1 map " + host.map_name()), "session.world() is not the server's map and world");
    plugin_command("talker levels");
    check(has(log, "levels " + std::to_string(levels().size()) + " true"), "travel.levels() is not the server's maps");
    // The time of day for everyone needs the server's world layers and layer-sync.
    plugin_command("talker tod night");
    check(has(log, "tod the server has no world layers") || has(log, "tod the server lets players keep their own world layers"),
          "A time of day was set without the server's world layers");

    // The level, as players' games name it, and its change heard when the server changes it.
    const auto *first = find_level(config.map);
    const auto other = std::find_if(levels().begin(), levels().end(), [&](const auto &l) { return l.asset != first->asset; });
    check(first && other != levels().end(), "The test needs two retail maps");
    log.clear();
    run_for(1100000);
    check(has(log, "level " + first->asset), "level.current() is not the server's map: " + first->asset);
    host.change_map(other->name);
    log.clear();
    run_for(1100000);
    check(has(log, "unloading " + first->asset) && has(log, "loaded " + other->asset), "The plugin did not hear the map change");
    check(has(log, "level " + other->asset) && !has(log, "level " + first->asset), "level.current() did not follow the map change");
    check(!has(log, "skaters 2"), "Skaters stayed on the old map's places after the map changed");
    // A plugin changes the map back, as the console's map does, with its checks.
    plugin_command("talker load " + first->name);
    {
        std::string said;
        for (const auto &line : log) said += "\n  " + line;
        check(has(log, "load true") && has(log, "[map] A plugin changed the map to "), "The plugin could not change the map:" + said);
    }
    run_for(1100000);
    check(host.map_level() == first->asset, "The plugin's map change did not happen");
    check(host.plugin_map(first->name) == "the server is already on that map", "A change to the map in play was not refused");
    const auto nowhere = host.plugin_map("Nowhere At All");
    check(nowhere.starts_with("no single map is called") || nowhere == "this server does not have that map",
          "A map the server does not have was loaded: " + nowhere);
    plugin_command("talker load " + other->name);
    check(has(log, "load one trip every 5 s"), "travel.load was not held to one trip every 5 s");
    // A player who leaves with a poll open: the plugin hears it closed.
    log.clear();
    host.drop(players[1].id, "testing");
    run_for(200000);
    check(has(log, "closed " + second_id + " left"), "A poll stayed open for a player who left");
    if (failures)
        for (const auto &line : log) std::fprintf(stderr, "log: %s\n", line.c_str());
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: dingosdk_server_plugin_tests <Server/Test/fixtures/park_mods>\n");
        return 2;
    }
    const int result = run(argv[1]);
    if (result == 0) std::printf("server plugin tests passed\n");
    return result;
}
