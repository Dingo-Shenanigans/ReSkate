// Drives the dedicated server's host with simulated players over a transport that is not
// Steam's, to see what a full server costs and that a change to how it sends leaves what it
// sends alone. Not a pass/fail test of behaviour: it prints how long the passes took and a
// hash of everything the server sent each player.
//
//   dingosdk_server_load_tests [players=120] [seconds=20] [spacing in metres=6] [threads=0: the server's own choice] [hash=1] [stall for this many seconds at 12 s=0]
//
// The players stand in a grid and move a little every pose; each sends 20 poses a second the
// way a game does (pose_batch.h) and says which of the server's pose messages it read.
#include "Server/server_host.h"
#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/pose_codec.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Engine/Game/Build/supported_build.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
// What the fake transport holds: the simulated players and what goes each way.
struct Wire {
    std::mutex mutex; // sends may come from several threads
    TransportStatus status;
    std::vector<TransportMessage> inbound;
    struct Player {
        std::uint64_t hash = 0xCBF29CE484222325ULL; // of the pose and sound messages sent them, in order
        std::uint64_t messages{}, bytes{};
        std::vector<std::uint32_t> batches; // pose messages to say were read
        std::vector<std::vector<std::uint8_t>> acks; // the server's, of this player's own uploads
    };
    std::map<std::uint64_t, Player> players;
    std::map<std::uint64_t, std::string> closed;
    std::uint64_t now{};
    bool hashing = true; // off: a send costs next to nothing, to time the server alone
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
void SteamTransport::disconnect(std::uint64_t id, const char *reason) {
    auto &w = wire();
    std::lock_guard lock(w.mutex);
    w.closed[id] = reason ? reason : "";
    std::erase_if(w.status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
}
void SteamTransport::poll() {}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t> bytes, bool reliable, bool fresh, TrafficLane lane) {
    auto &w = wire();
    std::lock_guard lock(w.mutex);
    const auto found = w.players.find(id);
    if (found == w.players.end()) return false;
    auto &player = found->second;
    ++player.messages;
    player.bytes += bytes.size();
    // Pose and sound messages carry nothing of the session's random numbers: they hash the same
    // from run to run. The rest (rosters, greetings) are only counted.
    if (pose_batch::is_batch(bytes) && bytes.size() >= 8) {
        std::uint32_t number{};
        for (unsigned i = 0; i < 4; ++i) number |= std::uint32_t{bytes[4 + i]} << (8 * i);
        player.batches.push_back(number);
    }
    if (w.hashing && (pose_batch::is_batch(bytes) || lane == TrafficLane::gameplay)) {
        const auto mix = [&](std::uint8_t byte) { player.hash = (player.hash ^ byte) * 0x100000001B3ULL; };
        for (const auto byte : bytes) mix(byte);
        mix(static_cast<std::uint8_t>(reliable));
        mix(static_cast<std::uint8_t>(fresh));
    }
    if (pose_batch::is_ack(bytes)) player.acks.emplace_back(bytes.begin(), bytes.end());
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

// One simulated player's game: where it stands, the poses it has sent and its upload stream.
struct Simulated {
    std::uint64_t id{}, epoch{};
    std::array<float, 3> home{};
    std::uint32_t sequence{};
    std::uint64_t next_pose{};
    bool greeted{};
    struct Kept {
        std::uint32_t sequence{};
        std::uint64_t time_us{};
        pose_codec::QuantPose pose;
    };
    std::deque<Kept> kept;
    pose_batch::Sender upload;
    pose_batch::Ack read; // the server's pose messages this player read
};

Pose pose_of(const Simulated &player, std::uint64_t now) {
    const auto t = static_cast<float>(now % 100000000ULL) / 1000000.f;
    const auto phase = static_cast<float>(player.id % 97);
    Pose pose;
    pose.root.position = {player.home[0] + std::sin(t * 0.7f + phase) * 2.f, player.home[1], player.home[2] + std::cos(t * 0.5f + phase) * 2.f};
    const auto turn = t * 0.3f + phase;
    pose.root.rotation = {0.f, std::sin(turn * 0.5f), 0.f, std::cos(turn * 0.5f)};
    pose.skater.resize(pose_codec::finger_skeleton);
    for (std::size_t bone = 0; bone < pose.skater.size(); ++bone) {
        auto &b = pose.skater[bone];
        // A skater moves a fraction of its bones much from one pose to the next.
        const auto angle = bone % 5 ? 0.1f * static_cast<float>(bone % 13)
                                    : std::sin(t * (1.f + static_cast<float>(bone % 7) * 0.2f) + phase + static_cast<float>(bone)) * 0.4f;
        b.position = {static_cast<float>(bone % 5) * 0.03f, static_cast<float>(bone % 11) * 0.02f, 0.01f};
        b.rotation = {std::sin(angle * 0.5f), 0.f, 0.f, std::cos(angle * 0.5f)};
    }
    if (pose.skater.size() > 1) pose.skater[1].position = pose.root.position;
    pose.board.resize(16);
    for (std::size_t bone = 0; bone < pose.board.size(); ++bone) {
        const auto angle = std::sin(t * 2.f + phase + static_cast<float>(bone)) * 0.2f;
        pose.board[bone].rotation = {0.f, 0.f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)};
    }
    pose.board[0].position = pose.root.position;
    return pose;
}
} // namespace

int main(int argc, char **argv) {
    const auto count = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 120U;
    const auto seconds = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 20U;
    const auto spacing = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 6.f;
    server::ServerConfig config;
    config.max_players = std::max(count, 2U);
    config.activity_log = false;
    const auto stall = argc > 6 ? static_cast<unsigned>(std::atoi(argv[6])) : 0U;
    wire().hashing = argc <= 5 || std::atoi(argv[5]) != 0;
    config.threads = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 0U;
    SteamTransport transport;
    std::size_t lines{};
    server::Host host(config, transport, [&](const std::string &line) {
        if (line.find("joined") == std::string::npos && lines++ < 40) std::printf("log: %s\n", line.c_str());
    });
    std::string error;
    if (!host.start(error)) {
        std::printf("The server did not start: %s\n", error.c_str());
        return 1;
    }
    const auto invite = parse_invite(host.invite());
    if (!invite) {
        std::printf("No invite.\n");
        return 1;
    }
    const auto secret = invite->secret;
    const auto map = map_hash(server::map_destination(config.map));
    auto &w = wire();
    std::vector<Simulated> players(count);
    const auto side = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(count))));
    for (unsigned index = 0; index < count; ++index) {
        auto &player = players[index];
        player.id = 76561198000000001ULL + index;
        player.epoch = 5000 + index;
        player.home = {static_cast<float>(index % side) * spacing, 10.f, static_cast<float>(index / side) * spacing};
        w.players[player.id];
        w.status.peers.push_back({player.id, true});
    }
    const auto header = [&](const Simulated &player, PacketKind kind, std::uint64_t now) {
        Packet p;
        p.kind = kind;
        p.session = secret;
        p.map = map;
        p.world = 1;
        p.epoch = player.epoch;
        p.source = player.id;
        p.time_us = now;
        return p;
    };
    using Clock = std::chrono::steady_clock;
    const std::uint64_t start = 1000000000ULL, step = 5000, warm = 8000000;
    double busy{}, longest{};
    std::uint64_t passes{}, bytes_at_warm{};
    const auto sent_bytes = [&] {
        std::uint64_t total{};
        for (const auto &[id, player] : w.players) total += player.bytes;
        return total;
    };
    for (std::uint64_t now = start; now < start + std::uint64_t{seconds} * 1000000; now += step) {
        w.now = now;
        for (auto &player : players) {
            if (w.closed.contains(player.id)) continue;
            auto &seen = w.players[player.id];
            if (!player.greeted) {
                auto hello = header(player, PacketKind::hello, now);
                hello.build = supported_build::game_sha256_bytes;
                hello.text = "Player " + std::to_string(player.id % 1000);
                w.inbound.push_back({player.id, encode_wire(hello), now});
                player.greeted = true;
                player.next_pose = now + 200000 + (player.id % 10) * 5000;
                continue;
            }
            // What the server said it holds of this player's poses, and which of its messages they read.
            for (const auto &ack : std::exchange(seen.acks, {}))
                if (const auto read = pose_batch::Ack::read(ack)) player.upload.ack(*read);
            if (!seen.batches.empty()) {
                for (const auto number : std::exchange(seen.batches, {})) player.read.note(number);
                const auto ack = player.read.bytes();
                w.inbound.push_back({player.id, {ack.begin(), ack.end()}, now});
            }
            if (now < player.next_pose) continue;
            player.next_pose += 50000;
            auto p = header(player, PacketKind::pose, now);
            p.sequence = ++player.sequence;
            p.pose = pose_of(player, now);
            player.kept.push_back({p.sequence, p.time_us, pose_codec::quantize(p.pose)});
            while (player.kept.size() > 64) player.kept.pop_front();
            const auto find = [&](std::uint32_t sequence) -> std::optional<pose_batch::KeptView> {
                for (auto it = player.kept.rbegin(); it != player.kept.rend(); ++it)
                    if (it->sequence == sequence) return pose_batch::KeptView{it->sequence, it->time_us, &it->pose};
                return {};
            };
            const auto emit = [&](std::span<const std::uint8_t> message, bool) {
                w.inbound.push_back({player.id, {message.begin(), message.end()}, now});
                return true;
            };
            player.upload.begin(1, map);
            player.upload.add(p.source, p.epoch, *find(p.sequence), find, false, false, pose_batch::Rate::full, now, emit);
            player.upload.flush(emit);
        }
        // A stall: for a while the server does not get to run, and everything sent meanwhile waits for it.
        if (stall && now - start >= 12000000 && now - start < 12000000 + std::uint64_t{stall} * 1000000) continue;
        const auto began = Clock::now();
        host.tick(now);
        const auto took = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        if (now - start == warm) bytes_at_warm = sent_bytes();
        if (now - start >= warm) {
            busy += took;
            longest = std::max(longest, took);
            ++passes;
        }
    }
    const auto measured = static_cast<double>(seconds) - static_cast<double>(warm) / 1000000.0;
    std::uint64_t hash = 0xCBF29CE484222325ULL, messages{};
    for (const auto &[id, player] : w.players) {
        hash = (hash ^ player.hash) * 0x100000001B3ULL;
        messages += player.messages;
    }
    std::printf("%u players, %u s (the first %.0f s not timed), %zu left on\n", count, seconds, static_cast<double>(warm) / 1000000.0,
                w.status.peers.size());
    for (const auto &[id, reason] : w.closed) std::printf("  closed %llu: %s\n", static_cast<unsigned long long>(id), reason.c_str());
    if (measured > 0 && passes)
        std::printf("busy %.1f%% of one core; %.2f ms a pass on average, longest %.1f ms; %.0f KB/s out\n",
                    busy / (measured * 10.0), busy / static_cast<double>(passes), longest,
                    static_cast<double>(sent_bytes() - bytes_at_warm) / 1024.0 / measured);
    std::printf("%llu messages sent; pose and sound hash %016llx\n", static_cast<unsigned long long>(messages),
                static_cast<unsigned long long>(hash));
    std::printf("%s\n", host.command("net").c_str());
    return 0;
}
