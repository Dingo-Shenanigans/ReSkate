// The dedicated server's Lua scripts (server_scripts.cpp) on a real Host, over a transport that is
// not Steam's: a simulated player joins and chats, a script kicks them from its chat handler, and
// the scripts' events, timers and argument checks show in the server's log.
#include "Server/server_host.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Engine/Game/Build/supported_build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
struct Wire {
    TransportStatus status;
    std::vector<TransportMessage> inbound;
    std::set<std::uint64_t> closed;
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
    wire().status.local_id = 90071992547409920ULL + 1;
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
    wire().closed.insert(id);
    std::erase_if(wire().status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
}
void SteamTransport::poll() {}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t>, bool, bool, TrafficLane) { return !wire().closed.contains(id); }
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

constexpr const char *script = R"lua(
server.on("join", function(p)
  print("join", p.name, p.id, p.admin, p.objects)
  print("announce to", server.announce("hi " .. p.name, p), server.announce("nobody", "zzz"))
end)
server.on("chat", function(p, text)
  print("chat", p.name, text)
  if text == "kick me" then print("kicked:", server.kick(p)) end
end)
server.on("leave", function(p) print("leave", p.name, p.id, server.player(p.id)) end)
server.after(0, function() print("after ran") end)
local runs, id = 0
id = server.every(0.1, function()
  runs = runs + 1
  if runs == 3 then print("every ran 3, cancelled", server.cancel(id)) end
end)
server.every(0.1, function() error("boom") end)
print("bad event", pcall(server.on, "bogus", print))
print("too often", pcall(server.every, 0.01, print))
print("cancel unknown", server.cancel(12345))
print("store empty", server.store("visits"), pcall(server.store, "", 1), pcall(server.store, "k", {}))
server.store("visits", 3); server.store("name", "Zoë"); server.store("gone", true); server.store("gone", nil)
print("stored", server.store("visits"), server.store("name"), server.store("gone"))
print("not utf8", pcall(server.store, "x", "\255"))
print("run", server.run("status") ~= "")
server.command("mute", function(p) print("mute ran by", p.role) end, {role = "Moderator"})
print("bad role", pcall(server.command, "x", print, {role = "a mod"}))
server.on("chat", function(p, text)
  if text == "make me mod" then
    print("role before", server.role(p), p.role)
    print("given", server.role(p, "moderator"))
    print("role after", server.role(p), server.player(p.id).role)
  end
end)
)lua";

int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
} // namespace

int main() {
    const auto folder = std::filesystem::temp_directory_path() / "reskate_server_scripts_tests";
    std::filesystem::remove_all(folder);
    // scripts\ in the folder, as next to the server: server.store saves to data\ beside it.
    const auto scripts = folder / "scripts";
    std::filesystem::create_directories(scripts);
    std::ofstream(scripts / "test.lua", std::ios::binary | std::ios::trunc) << script;
    // The server saves its settings and bans (a kick does) into the folder it runs in.
    const auto was = std::filesystem::current_path();
    std::filesystem::current_path(folder);

    server::ServerConfig config;
    config.roles = server::default_roles(); // as a new server's data/roles.json has them
    config.activity_log = false;
    SteamTransport transport;
    std::vector<std::string> log;
    server::Host host(config, transport, [&](const std::string &line) { log.push_back(line); });
    const auto errors = host.load_scripts(scripts);
    check(errors.empty(), "the script loads");
    if (!errors.empty()) std::printf("%s", errors.c_str());
    std::string error;
    if (!host.start(error)) {
        std::printf("The server did not start: %s\n", error.c_str());
        return 1;
    }
    const auto invite = parse_invite(host.invite());
    const auto map = map_hash(server::map_destination(config.map));
    const std::uint64_t player = 76561198000000001ULL;
    auto &w = wire();
    w.status.peers.push_back({player, true});
    const auto send = [&](PacketKind kind, std::uint64_t now, std::string text) {
        Packet p;
        p.kind = kind;
        p.session = invite->secret;
        p.map = map;
        p.world = 1;
        p.epoch = 5000;
        p.source = player;
        p.time_us = now;
        if (kind == PacketKind::hello) p.build = supported_build::game_sha256_bytes;
        p.text = std::move(text);
        w.inbound.push_back({player, encode_wire(p), now});
    };

    // Real time, for the timers (a steady clock of their own); the Host's clock moves with it.
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t now = 1000000000ULL;
    send(PacketKind::hello, now, "Tester");
    for (int pass = 0; std::chrono::steady_clock::now() - started < std::chrono::milliseconds(800); ++pass) {
        now += 5000;
        if (pass == 10) send(PacketKind::chat, now, "/mute"); // no role yet: refused
        if (pass == 15) send(PacketKind::chat, now, "make me mod");
        if (pass == 18) send(PacketKind::chat, now, "/mute");
        if (pass == 20) send(PacketKind::chat, now, "hello there");
        if (pass == 40) send(PacketKind::chat, now, "kick me");
        host.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto has = [&](std::string_view text) {
        return std::count_if(log.begin(), log.end(), [&](const std::string &line) { return line.find(text) != std::string::npos; });
    };
    for (const auto &line : log)
        if (line.starts_with("[script]")) std::printf("  log: %s\n", line.c_str());
    check(has("[script] join Tester 76561198000000001 false 0") == 1, "join: the player's table");
    check(has("[announcement -> Tester] hi Tester") == 1 && has("[script] announce to true false") == 1,
          "announce: a card for one player; false for nobody");
    check(has("[script] chat Tester hello there") == 1, "chat: the player and their text");
    check(has("[script] kicked: Tester was kicked") == 1, "a chat handler kicks the player who spoke");
    check(has("[script] leave Tester 76561198000000001 nil") == 1, "leave: who they were, and that they are gone");
    check(w.closed.contains(player), "the kicked player was disconnected");
    check(has("[script] after ran") == 1, "after: runs once");
    check(has("[script] every ran 3, cancelled true") == 1, "every: repeats, and cancels itself");
    check(has("[script] timer failed:") == 1 && has("boom") >= 1, "a failing repeating timer is logged once and stopped");
    check(has("[script] bad event false") == 1, "server.on refuses an unknown event");
    check(has("[script] too often false") == 1, "server.every refuses under 0.1 s");
    check(has("[script] cancel unknown false") == 1, "server.cancel of no timer is false");
    check(has("[script] store empty nil false") == 1 && has("[script] stored 3 Zoë nil") == 1, "server.store keeps, reads and deletes values");
    check(has("[script] not utf8 false") == 1, "server.store refuses text that is not UTF-8");
    check(has("[script] run true") == 1, "server.run answers with the server's reply");
    check(has("[script] bad role false") == 1, "server.command refuses a role that is not a name");
    check(has("[script] role before player player") == 1 && has("[script] role after moderator moderator") == 1,
          "server.role reads a player's role from the roles, and gives one");
    check(has("[script] mute ran by moderator") == 1, "a role's command runs only once the player has the role");
    {
        // A new server (a restart) reads what the last one stored.
        const auto later = folder / "later";
        std::filesystem::create_directories(later);
        std::ofstream(later / "read.lua", std::ios::binary | std::ios::trunc) << R"(print("restored", server.store("visits"), server.store("name")))";
        server::Host again(config, transport, [&](const std::string &line) { log.push_back(line); });
        check(again.load_scripts(later).empty() && has("[script] restored 3 Zoë") == 1, "server.store survives a restart");
    }
    std::filesystem::current_path(was);
    std::filesystem::remove_all(folder);
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
