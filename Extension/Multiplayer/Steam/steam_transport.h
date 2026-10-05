#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <memory>

namespace dingosdk::multiplayer {
enum class TrafficLane : std::uint16_t { gameplay = 0, control = 1, cosmetics = 2, voice = 3 };
constexpr TrafficLane traffic_lane(PacketKind kind) {
    return kind == PacketKind::voice || kind == PacketKind::radio ? TrafficLane::voice
        : kind == PacketKind::pose || kind == PacketKind::audio ? TrafficLane::gameplay
        : kind == PacketKind::cosmetics ? TrafficLane::cosmetics : TrafficLane::control;
}
struct TransportPeer {
    std::uint64_t id{};
    bool connected{};
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

  private:
    bool bind(void *steam_api, void *sockets, void *networking_utils);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dingosdk::multiplayer
