#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <memory>

namespace dingosdk::multiplayer {
enum class TrafficLane : std::uint16_t { gameplay = 0, control = 1, cosmetics = 2, voice = 3 };
constexpr TrafficLane traffic_lane(PacketKind kind) {
    return kind == PacketKind::voice ? TrafficLane::voice
        : kind == PacketKind::pose || kind == PacketKind::audio ? TrafficLane::gameplay
        : kind == PacketKind::cosmetics ? TrafficLane::cosmetics : TrafficLane::control;
}
struct TransportPeer {
    std::uint64_t id{};
    bool connected{};
};
// What Steam measures of one connection right now (SteamTransport::links).
struct TransportLink {
    std::uint64_t id{};
    bool connected{}, measured{};
    int ping_ms{}, pending_bytes{}, send_rate{}; // send_rate: bytes a second Steam will let out to them
    float quality_local = -1, quality_remote = -1; // 0-1 of packets delivered each way; -1 unknown
    float out_bps{}, in_bps{};
    std::uint64_t queue_us{}; // how long a message sent now would wait
    // Which of Steam's relay locations the connection goes through, by their short names
    // ("ord", "fra"...): the one this side uses and the one the other side uses. A ping far
    // above the direct one is the route, and these say which way it went.
    std::string relay, remote_relay;
};
struct TransportMessage {
    std::uint64_t peer{};
    std::vector<std::uint8_t> bytes;
    // When Steam received it, in microseconds on a clock of its own (only differences mean
    // anything); 0 when the transport does not say. A receiver that was kept from reading for a
    // while gets everything that arrived meanwhile in one go: this is what tells that from a flood.
    std::uint64_t arrived{};
};
// One message of SteamTransport::send_batch, with send()'s arguments; `sent` receives what
// send() would have returned for it.
struct TransportSend {
    std::uint64_t id{};
    std::span<const std::uint8_t> bytes;
    bool reliable{}, fresh{};
    TrafficLane lane = TrafficLane::control;
    bool sent{};
};
struct TransportStatus {
    bool ready{}, hosting{}, connected{};
    std::uint64_t local_id{}, peer_id{}, sent{}, received{}, dropped{};
    std::string detail;
    std::string peer_name;
    std::vector<TransportPeer> peers;
    bool telemetry{};
    unsigned prioritized_connections{};
    std::uint64_t cosmetic_queue_us{};
    int ping_ms{}, send_rate{}, pending_bytes{};
    float outgoing_bps{}, incoming_bps{}, delivery_local = -1, delivery_remote = -1;
    std::uint64_t queue_us{}, skipped{}, send_failures{}, invalid_messages{}, sent_bytes{}, received_bytes{},
        raw_sent_bytes{};
};
// What each connection may send a second (bytes): a crowded server's players, mostly.
inline constexpr int connection_send_rate = 900 * 1024, min_send_rate = 128 * 1024, max_send_rate = 16 * 1024 * 1024;
class SteamTransport {
  public:
    SteamTransport();
    ~SteamTransport();
    SteamTransport(const SteamTransport &) = delete;
    SteamTransport &operator=(const SteamTransport &) = delete;
    bool open();
    // Dedicated server: the logged-on Steam game server's networking, from the
    // steam_api64.dll module handle it was started with.
    bool open_game_server(void *steam_api);
    bool host(unsigned capacity);
    bool join(std::uint64_t steam_id);
    bool connect_peer(std::uint64_t steam_id);
    void allow_peers(std::span<const Member>);
    bool socket_test();
    void stop();
    void disconnect(std::uint64_t id, const char *reason);
    void poll();
    bool send(std::uint64_t id, std::span<const std::uint8_t>, bool reliable, bool fresh = false,
              TrafficLane lane = TrafficLane::control);
    // Several sends in one Steam call (one networking lock) where lanes are available.
    void send_batch(std::span<TransportSend> messages);
    std::vector<TransportMessage> receive();
    std::string name(std::uint64_t id);
    const TransportStatus &status() const;
    // For a host's log. How Steam says a connection it closed ended, once (empty when this
    // side closed it, or it is still open): who closed it, Steam's reason code and words, and
    // the last ping and quality measured. And a connected player's link right now.
    std::string take_closed(std::uint64_t id);
    std::string link_report(std::uint64_t id);
    // Bytes waiting to go out to a player, as last measured (a few times a second).
    std::int64_t pending(std::uint64_t id) const;
    // What each connection may send a second from now on, the open ones included. False when
    // out of range, or when an open connection could not be changed (new ones still get it).
    bool set_send_rate(int bytes_per_second);
    int send_rate() const;
    // Every connection, measured now: one Steam call each, so for a command, not every tick.
    std::vector<TransportLink> links();

  private:
    bool bind(void *steam_api, void *sockets, void *networking_utils);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dingosdk::multiplayer
