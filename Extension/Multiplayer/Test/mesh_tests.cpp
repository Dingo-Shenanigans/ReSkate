// Exercises the real session admission/routing implementation with an in-memory
// Steam transport. Native actors/UI are stubbed; no game process is opened.
#include "Extension/Multiplayer/Session/session.cpp"
#include "Extension/Multiplayer/Session/session_view.cpp"
#include "Extension/Multiplayer/Session/session_send.cpp"
#include "Extension/Multiplayer/Session/session_receive.cpp"
#include "Extension/Multiplayer/Session/session_commands.cpp"
#include "Extension/Multiplayer/Session/session_party.cpp"
#include <iostream>
#include <set>

namespace dingosdk {
bool read_local_camera_transform(std::uintptr_t, std::uintptr_t, std::array<float, 16>&) noexcept { return false; }
ParksModel local_profile_parks() { return {}; }
void set_lobby_park_mode(bool, bool) {}
bool simulated_object_guest{};
void set_lobby_object_guest(bool guest) { simulated_object_guest = guest; }
WorldLayersModel simulated_host_layers;
WorldLayersModel local_profile_world_layers() { return simulated_host_layers; }
void apply_host_world_layers(bool, const WorldLayerChoices &) {}
void apply_host_park_choices(const ParkChoices &) {}
std::optional<NetworkObjectSnapshot> capture_local_network_objects() { return {}; }
void set_remote_network_objects(std::string_view, std::span<const NetworkObjectOwner>) {}
void clear_remote_network_objects() {}
std::vector<std::pair<std::uint64_t, std::uint64_t>> removed_object_owners;
void remove_remote_network_objects(std::uint64_t owner, std::uint64_t epoch) {
    if (owner && epoch) removed_object_owners.emplace_back(owner, epoch);
}
std::string network_object_status() { return {}; }
bool simulated_placement_allowed = true;
void set_lobby_object_placement_allowed(bool allowed) { simulated_placement_allowed = allowed; }
unsigned simulated_guest_wipes{};
bool clear_lobby_guest_objects() { ++simulated_guest_wipes; return true; }
}
namespace dingosdk {
bool teleport_local_skater(const std::array<float, 3>&) { return true; }
void update_board_lock(std::uintptr_t, std::uintptr_t, bool) noexcept {}
void update_developer_hoodie(std::uintptr_t, std::uintptr_t, std::uint64_t, std::uint64_t, DeveloperHoodieState &,
                             const multiplayer::MarkStyles &) noexcept {}
void update_developer_board(std::uintptr_t, std::uintptr_t, std::uint64_t, std::uint64_t, DeveloperBoardState &,
                            const multiplayer::MarkStyles &) noexcept {}
}
namespace dingosdk::multiplayer {
// The backend's lists are not read here: a check lists the players it means.
std::set<std::pair<std::uint64_t, IdentityList>> simulated_identities;
bool identity_listed(std::uint64_t id, IdentityList list) noexcept { return simulated_identities.contains({id, list}); }
bool local_allows_player_collision(std::uintptr_t, std::uintptr_t) noexcept { return false; }
void update_remote_collision(std::uintptr_t, std::uintptr_t, const Pose &, bool, std::uint64_t) noexcept {}
void expire_remote_collision(std::uintptr_t, std::uint64_t) noexcept {}
}
namespace dingosdk::mods {
// Every simulated player runs the same process, so they share one scoring state.
ScoringState simulated_scoring{true, 0, {}};
ScoringState scoring_state() noexcept { return simulated_scoring; }
}
namespace dingosdk::physics_tuning {
void prepare() noexcept {}
std::optional<Encoded> local_differences(std::uintptr_t, std::size_t) { return {}; }
void enforce(std::uintptr_t, std::uintptr_t, std::span<const std::uint8_t>) noexcept {}
void release(std::uintptr_t) noexcept {}
std::string status() { return {}; }
}
namespace dingosdk::profile_runtime {
void install_script_error_log(std::uintptr_t) noexcept {}
void install_board_wear_hold(std::uintptr_t) noexcept {}
std::optional<bool> local_preference(std::string_view) noexcept { return {}; }
void set_local_preference(std::string_view, bool) noexcept {}
std::optional<Json> local_value(std::string_view) noexcept { return {}; }
void set_local_values(const std::vector<std::pair<std::string, Json>>&) noexcept {}
}

namespace dingosdk::multiplayer {
// Voice capture and playback need audio devices; the mesh carries no voice here.
struct VoiceChat::Impl {};
VoiceChat::VoiceChat() = default;
VoiceChat::~VoiceChat() = default;
void VoiceChat::configure(VoiceSettings) {}
VoiceModel VoiceChat::model() const { return {}; }
void VoiceChat::mute(std::uint64_t, bool) {}
void VoiceChat::volume(std::uint64_t, float) {}
void VoiceChat::update(VoiceScene) {}
void VoiceChat::reset() {}
void VoiceChat::process_native(std::uintptr_t) noexcept {}
void VoiceChat::receive(const Packet&) {}
std::vector<VoiceData> VoiceChat::take_capture() { return {}; }
}

namespace dingosdk::multiplayer {
struct SimulatedNetwork {
    static inline std::set<std::pair<std::uint64_t, std::uint64_t>> blocked;
    static inline bool lose_unreliable{}, lose_reports{};
    static inline bool slow_join{};
    static inline bool cosmetics_first{}, audit_lanes{};
    static inline std::uint64_t hold_control_to{};
    static inline std::array<unsigned, 3> lane_sends{};
    static inline std::map<std::uint64_t, std::uint64_t> queues;
    // What a message's arrival time is (TransportMessage::arrived); 0: the transport does not say.
    static inline std::uint64_t clock{};
    static std::pair<std::uint64_t, std::uint64_t> pair(std::uint64_t a, std::uint64_t b) {
        return {std::min(a, b), std::max(a, b)};
    }
};
struct SteamTransport::Impl {
    static inline std::uint64_t next_id = 76561198000000000ULL;
    static inline std::map<std::uint64_t, Impl *> bus;
    TransportStatus state;
    unsigned capacity = max_players;
    std::set<std::uint64_t> allowed;
    struct Queued { TransportMessage message; TrafficLane lane{}; };
    std::vector<Queued> inbox;
    std::uint64_t host{}, unreliable{};
    void link(std::uint64_t id) {
        if (std::none_of(state.peers.begin(), state.peers.end(), [&](const auto &p) { return p.id == id; }))
            state.peers.push_back({id, true});
    }
};
SteamTransport::SteamTransport() : impl_(std::make_unique<Impl>()) {
    impl_->state.local_id = ++Impl::next_id;
    Impl::bus[impl_->state.local_id] = impl_.get();
}
SteamTransport::~SteamTransport() {
    stop();
    Impl::bus.erase(impl_->state.local_id);
}
bool SteamTransport::open() { return impl_->state.ready = true; }
bool SteamTransport::host(unsigned capacity) {
    stop();
    open();
    if (capacity < 2 || capacity > max_players) return false;
    impl_->capacity = capacity;
    impl_->state.hosting = true;
    return true;
}
bool SteamTransport::join(std::uint64_t id) {
    if (SimulatedNetwork::slow_join) Sleep(2);
    stop();
    open();
    impl_->host = id;
    impl_->allowed.insert(id);
    return connect_peer(id);
}
bool SteamTransport::connect_peer(std::uint64_t id) {
    auto &p = *impl_;
    const auto other = Impl::bus.find(id);
    if (!p.allowed.contains(id) || other == Impl::bus.end() ||
        SimulatedNetwork::blocked.contains(SimulatedNetwork::pair(id, p.state.local_id)))
        return false;
    auto &q = *other->second;
    if (q.state.hosting && q.state.peers.size() >= q.capacity - 1 &&
        std::none_of(q.state.peers.begin(), q.state.peers.end(), [&](const auto &peer) { return peer.id == p.state.local_id; }))
        return false;
    if (!q.state.hosting && !q.allowed.contains(p.state.local_id))
        return false;
    p.link(id);
    q.link(p.state.local_id);
    return true;
}
void SteamTransport::allow_peers(std::span<const Member> members) {
    impl_->allowed.clear();
    impl_->allowed.insert(impl_->host);
    for (const auto &m : members)
        if (m.id != impl_->state.local_id)
            impl_->allowed.insert(m.id);
    auto peers = impl_->state.peers;
    for (const auto &p : peers)
        if (!impl_->allowed.contains(p.id))
            disconnect(p.id, "Removed");
}
void SteamTransport::disconnect(std::uint64_t id, const char *reason) {
    impl_->state.detail = reason;
    if (auto it = Impl::bus.find(id); it != Impl::bus.end())
        it->second->state.detail = reason;
    std::erase_if(impl_->state.peers, [&](const auto &p) { return p.id == id; });
    if (auto it = Impl::bus.find(id); it != Impl::bus.end())
        std::erase_if(it->second->state.peers, [&](const auto &p) { return p.id == impl_->state.local_id; });
}
void SteamTransport::stop() {
    auto peers = impl_->state.peers;
    for (const auto &p : peers)
        disconnect(p.id, "");
    impl_->allowed.clear();
    impl_->host = 0;
    impl_->state.hosting = false;
    impl_->inbox.clear();
}
void SteamTransport::poll() {
    impl_->state.telemetry = true;
    impl_->state.queue_us = SimulatedNetwork::queues[impl_->state.local_id];
}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t> bytes, bool reliable, bool, TrafficLane lane) {
    auto &p = *impl_;
    if (std::none_of(p.state.peers.begin(), p.state.peers.end(), [&](const auto &v) { return v.id == id; }))
        return false;
    ++p.state.sent;
    p.state.sent_bytes += bytes.size();
    const auto decoded = decode_wire(bytes);
    if (SimulatedNetwork::audit_lanes) {
        auto kind = decoded ? std::optional(decoded->kind) : std::nullopt;
        if (bytes.size() > 4 && std::equal(bytes.begin(), bytes.begin() + 4, "RMB1")) {
            if (const auto baseline = decode_wire(bytes.subspan(4))) kind = baseline->kind;
        } else if (bytes.size() >= 30 && bytes[0] == 'R' && bytes[1] == 'M' && (bytes[2] == 'D' || bytes[2] == 'S'))
            kind = static_cast<PacketKind>(bytes[20] | (unsigned{bytes[21]} << 8));
        if (!kind || lane != traffic_lane(*kind)) throw std::runtime_error("Stream/reference sent on the wrong Steam lane");
        ++SimulatedNetwork::lane_sends[static_cast<std::size_t>(lane)];
    }
    if (SimulatedNetwork::lose_reports && decoded && decoded->kind == PacketKind::routes)
        return true;
    if (!reliable && SimulatedNetwork::lose_unreliable && ++p.unreliable % 5 == 0)
        return true;
    Impl::bus.at(id)->inbox.push_back({{p.state.local_id, {bytes.begin(), bytes.end()}, SimulatedNetwork::clock}, lane});
    return true;
}
void SteamTransport::send_batch(std::span<TransportSend> messages) {
    for (auto &message : messages)
        message.sent = send(message.id, message.bytes, message.reliable, message.fresh, message.lane);
}
std::vector<TransportMessage> SteamTransport::receive() {
    auto queued = std::exchange(impl_->inbox, {});
    if (SimulatedNetwork::cosmetics_first)
        std::stable_sort(queued.begin(), queued.end(), [](const auto &a, const auto &b) {
            return a.lane == TrafficLane::cosmetics && b.lane != TrafficLane::cosmetics;
        });
    std::vector<TransportMessage> messages;
    for (auto &item : queued) {
        if (SimulatedNetwork::hold_control_to == impl_->state.local_id && item.lane == TrafficLane::control)
            impl_->inbox.push_back(std::move(item));
        else messages.push_back(std::move(item.message));
    }
    return messages;
}
const TransportStatus &SteamTransport::status() const { return impl_->state; }
std::string SteamTransport::name(std::uint64_t id) { return "Player " + std::to_string(id); }
bool SteamTransport::socket_test() { return true; }

// No native/game/UI calls in this harness.
NativeFrame tick_frame;
unsigned capture_delay_ms{};
bool captured_full_pose{};
float shown_root{};
std::uint64_t shown_at{};
NativeFrame capture_local(std::uintptr_t, std::uintptr_t) {
    if (capture_delay_ms) Sleep(capture_delay_ms);
    return tick_frame;
}
NativeFrame capture_local(std::uintptr_t base, std::uintptr_t client, bool full_pose) {
    captured_full_pose = full_pose;
    return capture_local(base, client);
}
bool show_remote(std::uintptr_t, std::uintptr_t, const NativeFrame &, const Pose &pose, std::string &) {
    shown_root = pose.root.position[0];
    shown_at = now_us();
    return true;
}
void remove_remote(std::uintptr_t) noexcept {}
void update_remote_cosmetics(std::uintptr_t, const NativeFrame &, const Appearance &, std::string &) {}
std::uint64_t remote_pose_updates() noexcept { return 0; }
std::uint64_t remote_board_pose_updates() noexcept { return 0; }
NativeAnimationStats remote_animation_stats() noexcept { return {}; }
std::uintptr_t remote_skater_entity() noexcept { return 0; }
std::uintptr_t remote_board_entity() noexcept { return 0; }
std::uint64_t remote_skater_generation() noexcept { return 0; }
void note_remote_distance(float) noexcept {}
void update_native_party(std::uintptr_t, const PartyRoster &, unsigned, bool) noexcept {}
void set_native_party_changes(bool) noexcept {}
bool post_native_party_invite(std::uint64_t) noexcept { return false; }
void set_native_nametags_enabled(bool) noexcept {}
void set_native_compass_enabled(bool) noexcept {}
void prepare_native_indicators(std::uintptr_t) noexcept {}
void prepare_player_ui(std::uintptr_t) noexcept {}
void prepare_remote_audio(std::uintptr_t) noexcept {}
bool install_entity_hooks(std::uintptr_t, std::string &) noexcept { return true; }
void publish_custom_nametags(std::uintptr_t, std::vector<NametagPlayer>, std::optional<std::array<float, 3>>, bool, bool, float) noexcept {}
void set_custom_nametags_enabled(bool) noexcept {}
GameUiState sample_game_ui_state(std::uintptr_t) noexcept { return {}; }
void note_local_skater(const Transform &) noexcept {}
void initialize_native_throwdowns(std::uintptr_t) noexcept {}
// The relay itself needs the game; routing is checked through each node's inbox.
std::vector<std::vector<std::uint8_t>> tick_throwdown_relay(std::uintptr_t, const ThrowdownRelayInput &) { return {}; }
std::vector<std::string> take_throwdown_relay_notices() { return {}; }
void receive_throwdown_relay(std::uint64_t, std::span<const std::uint8_t>) {}
bool throwdown_relay_hides(std::uint64_t) noexcept { return false; }
std::optional<std::array<float, 3>> throwdown_relay_celebration_offset(std::uint64_t, const std::array<float, 3> &) noexcept { return std::nullopt; }
bool throwdown_relay_waits_offboard() noexcept { return false; }
std::shared_ptr<const SteamSocialSnapshot> steam_social_snapshot() {
    static const auto snapshot = std::make_shared<SteamSocialSnapshot>();
    return snapshot;
}
std::optional<Appearance> capture_cosmetics(std::uintptr_t, const NativeFrame &, std::string &) { return {}; }
void prepare_audio_capture(std::uintptr_t, const NativeFrame &) noexcept {}
std::vector<AudioSample> drain_audio_capture(std::uint64_t) { return {}; }
void update_remote_audio(std::uintptr_t, const NativeFrame &, const Pose &, const AudioState &) noexcept {}
void stop_remote_audio() noexcept {}
void reset_audio() noexcept {}
std::string native_audio_status() { return {}; }
std::uint64_t captured_audio_frames() noexcept { return 0; }
std::uint64_t played_audio_frames() noexcept { return 0; }
void update_player_ui(std::uintptr_t, const Pose *, std::string) noexcept {}
void update_party_position(const Pose *) noexcept {}
std::string player_ui_status() { return {}; }
std::uint64_t player_map_updates() noexcept { return 0; }
std::unique_ptr<LobbyApi> make_steam_lobby_api() { return {}; }

namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
// Exercise the public UI command queue and Steam lobby completion through tick(),
// rather than starting a simulated Session after admission has already begun.
struct JoinLobby final : LobbyApi {
    std::uint64_t host_id{};
    bool completed{};
    std::map<std::string, std::string> metadata;
    void open() override { Sleep(2); }
    std::uint64_t create(unsigned) override { return 0; }
    std::uint64_t search() override { return 1; }
    std::uint64_t join(std::uint64_t) override { return 2; }
    std::optional<LobbyResult> poll(std::uint64_t, LobbyCall kind) override {
        if (kind == LobbyCall::list) return LobbyResult{true, 0, 1, 1};
        if (kind == LobbyCall::join && completed) return LobbyResult{true, 9001, 0, 1};
        return {};
    }
    void leave(std::uint64_t) override {}
    std::uint64_t at(int) override { return 9001; }
    std::uint64_t owner(std::uint64_t) override { return host_id; }
    std::string data(std::uint64_t, const char *key) override { return metadata[key]; }
    bool data(std::uint64_t, const char *, const std::string &) override { return true; }
    bool joinable(std::uint64_t, bool) override { return true; }
    bool visibility(std::uint64_t, bool) override { return true; }
    std::string name() override { return "Host"; }
};
void join_tick_checks() {
    auto &guest = session();
    SimulatedNetwork::slow_join = true;
    tick_frame.ready = true;
    tick_frame.pose.skater.resize(1);
    const std::string map = "Levels/Root/Root|Levels/Beach/Beach";
    auto host_storage = std::make_unique<Session>();
    auto &host = *host_storage;
    host.mode = Mode::host;
    host.secret = 73;
    host.epoch = 100;
    host.map = map_hash(map);
    host.map_name = map;
    host.started_map = true;
    host.transport.host(8);
    host.host_id = host.transport.status().local_id;
    const auto code = format_invite({host.host_id, host.secret});
    for (unsigned route = 0; route < 3; ++route) for (bool protected_lobby : {false, true}) {
        const bool browser = route != 0, friend_join = route == 2;
        if (friend_join && protected_lobby) continue; // Password rejection is covered by the lobby adapter tests.
        stop(guest, "Next join test");
        check(!simulated_object_guest, "Leaving retained the guest object layout");
        check(!guest.joined_public_lobby, "Stopping must clear the public friend-join target");
        const auto password = protected_lobby ? "test password" : "";
        host.password = password_key(password, host.secret);
        networking(host, tick_frame, now_us());
        JoinLobby *api{};
        if (browser) {
            auto backend = std::make_unique<JoinLobby>();
            api = backend.get();
            api->host_id = host.host_id;
            api->metadata = {{"rs_kind", "reskate-co-skate"},
                {"rs_protocol", std::to_string(protocol_version)},
                {"rs_build", std::string(supported_build::game_sha256)},
                {"rs_name", "Host"}, {"rs_map", map}, {"rs_code", code},
                {"rs_players", "1"}, {"rs_capacity", "8"}, {"rs_open", "1"},
                {"rs_password", protected_lobby ? "1" : "0"}};
            guest.lobbies = SteamLobbies(std::move(backend));
            if (!friend_join) {
                guest.lobbies.refresh(1000000);
                guest.lobbies.tick(1100000);
            }
        }
        check(queue_command(friend_join ? "join-friend-lobby" : browser ? "join-lobby" : "join", browser ? "9001" : code, password),
              "UI join request was not queued");
        tick(0, 0, true, map, nullptr);
        if (browser) {
            check(guest.lobbies.status().joining, "Fresh browser request timed out in its starting tick");
            api->completed = true;
            Sleep(110); // The real lobby adapter polls at 100 ms intervals.
            tick(0, 0, true, map, nullptr);
        }
        check(guest.mode == Mode::join && guest.awaiting_map,
              "Fresh join timed out in its starting tick");
        check(simulated_object_guest, "Joining did not isolate the guest's personal object layout");
        for (unsigned frame = 0; frame < 50 && !guest.peers[0].handshaken; ++frame) {
            networking(host, tick_frame, now_us());
            tick(0, 0, true, map, nullptr);
            Sleep(1);
        }
        check(guest.mode == Mode::join && player_count(guest) == 2 && !guest.awaiting_map,
              "Queued join did not complete map/password/admission handshake");
        check(guest.joined_public_lobby == (browser ? 9001ULL : 0ULL), "Only verified lobby joins retain a public presence target");
        // Party requests (menus and the game's own party buttons) go through the same queue.
        check(queue_command("party", "status", {}), "A party request was not queued");
        tick(0, 0, true, map, nullptr);
        check(guest.requests.empty(), "A queued party request was not run");
        // So do the host's switches in both menus: each has to be let into the queue. The
        // physics tuning one was left out, and its switch did nothing in either menu.
        for (const char *setting : {"object-placement", "noclip-allow", "nobail-allow", "boosts-allow", "tuning-enforce",
                                    "world-layer-sync", "clear-objects"}) {
            const bool queued = queue_command(setting, "toggle", {});
            if (!queued) std::cerr << "Refused: " << setting << "\n";
            check(queued, "A menu's host setting was refused before it reached the session");
            tick(0, 0, true, map, nullptr);
            check(guest.requests.empty(), "A queued host setting was not run");
        }
        if (friend_join) {
            check(queue_command("join-friend-lobby", "9002", {}), "Steam friend join request should enter the normal queue");
            tick(0, 0, true, map, nullptr);
            check(guest.mode == Mode::join && guest.host_id == host.host_id && guest.joined_public_lobby == 9001,
                  "A Steam join click must not replace an active session");
        }
    }
    stop(guest, "Timeout checks");
    networking(host, tick_frame, now_us());
    check(queue_command("join", code, {}), "Timeout join was not queued");
    tick(0, 0, true, map, nullptr);
    check(guest.mode == Mode::join, "Fresh timeout-check join failed");
    guest.join_started = now_us() - 180000001;
    tick(0, 0, true, map, nullptr);
    check(guest.mode == Mode::off && guest.status.find("timed out") != std::string::npos,
          "Real three-minute join timeout was disabled");
    check(queue_command("join", code, {}), "Stale join was not queued");
    guest.requests.back()->queued = now_us() - 10000001;
    tick(0, 0, true, map, nullptr);
    check(guest.mode == Mode::off, "Expired UI command was executed");
    stop(guest, "Join tick checks complete");
    guest.lobbies = SteamLobbies(nullptr);
    tick_frame = {};
    SimulatedNetwork::slow_join = false;
    std::cout << "Join tick checks: queued code/browser joins, password admission, pending Steam calls, "
                 "real timeout and stale commands passed.\n";
}
void client_tick_checks() {
    auto &s = session();
    const std::string map = "Levels/Root/Root|Levels/Beach/Beach";
    tick_frame = {};
    tick_frame.ready = true;
    tick_frame.pose.skater.resize(395);
    tick_frame.pose.board.resize(17);
    command("echo", {}, {});
    tick(0, 0, true, map, nullptr);
    auto &peer = s.peers[0];
    peer.poses.clear();
    const auto base = now_us();
    auto pose = packet(s, PacketKind::pose, base - 200000);
    pose.pose = tick_frame.pose;
    check(peer.poses.push(pose, base - 200000), "Client timing reference rejected");
    ++pose.sequence; pose.time_us = base; pose.pose.root.position[0] = 2;
    check(peer.poses.push(pose, base), "Client timing latest pose rejected");
    auto outfit = packet(s, PacketKind::cosmetics, base);
    outfit.appearance = {{skater_recipe_key, 2, {}, {{1, "Outfit", {}}}},
                         {board_recipe_key, 1, {}, {{2, "Deck", {}}}}};
    check(peer.appearance.push(outfit), "Client timing appearance rejected");
    s.next_send = base + 1000000;
    capture_delay_ms = 40;
    shown_at = 0;
    tick(0, 0, true, map, nullptr);
    capture_delay_ms = 0;
    check(!captured_full_pose, "Client captured a complete skeleton before the send deadline");
    const auto expected = 1.f + static_cast<float>(shown_at - base) / 100000.f;
    check(shown_at >= base && shown_root > 1.35f && std::abs(shown_root - std::min(expected, 2.f)) < .05f,
          "Client capture work left playback on the old frame timestamp");
    const auto timing = s.client_timing.snapshot(now_us());
    check(timing.peak_ms[ClientTiming::capture] >= 35 && timing.work_max_ms >= 35,
          "Actual client capture stall was absent from diagnostics");
    stop(s, "Client timing checks complete");
    tick_frame = {};
    std::cout << "Client tick: injected capture stall measured; playback uses fresh wall time while Local Echo remains buffered.\n";
}
struct Simulation {
    std::vector<std::unique_ptr<Session>> nodes;
    std::uint64_t now = 10000000;
    NativeFrame local;
    std::vector<float> positions;
    unsigned capacity, tps;
    std::size_t held = static_cast<std::size_t>(-1); // a node that is not run: held up, reading nothing
    bool stamp{};                                     // messages carry when they arrived
    explicit Simulation(unsigned limit = max_players, unsigned rate = 20) : capacity(limit), tps(rate) {
        local.ready = true;
        local.pose.skater.resize(395);
        local.pose.board.resize(17);
    }
    Session &add(bool correct_password = true) {
        auto s = std::make_unique<Session>();
        s->capacity = capacity;
        s->secret = 73;
        s->epoch = nodes.size() + 100;
        s->map = 42;
        s->started_map = true;
        s->password = password_key(correct_password ? "test room" : "wrong password", s->secret);
        if (nodes.empty()) {
            s->tps = tps;
            s->mode = Mode::host;
            s->transport.host(capacity);
            s->host_id = s->transport.status().local_id;
        } else {
            s->mode = Mode::join;
            s->host_id = nodes.front()->host_id;
            s->transport.join(s->host_id);
        }
        auto cosmetics = packet(*s, PacketKind::cosmetics, now);
        cosmetics.appearance = {
            {skater_recipe_key, 2, {}, {{1, "Outfit" + std::to_string(nodes.size()), {}}}},
            {board_recipe_key, 1, {}, {{2, "Deck", {}}}}};
        s->cosmetic_packet = encode_wire(cosmetics);
        nodes.push_back(std::move(s));
        return *nodes.back();
    }
    void run(unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) {
            now += network_tick_us;
            SimulatedNetwork::clock = stamp ? now : 0;
            for (std::size_t n = 0; n < nodes.size(); ++n) {
                auto &s = *nodes[n];
                if (s.mode == Mode::off || n == held)
                    continue;
                local.pose.root.position[0] = static_cast<float>(now - 10000000) / 1000000;
                local.pose.root.position[2] = n < positions.size() ? positions[n] : static_cast<float>(n);
                refresh_host_choices(s, now);
                networking(s, local, now);
                if (s.mode != Mode::off) {
                    sync_objects(s, local, now);
                    send_local(s, local, now);
                    auto sound = packet(s, PacketKind::audio, now);
                    sound.audio = {AudioSample{}};
                    sound.audio[0].state.values[1] = static_cast<float>(n) + .5f;
                    broadcast(s, sound, true, true, now);
                }
            }
        }
    }
    void fresh(unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            for (unsigned j = 0; j < count; ++j)
                if (i != j) {
                    auto *p = find_peer(*nodes[i], nodes[j]->transport.status().local_id);
                    if (!p || !p->handshaken) {
                        std::cerr << "Missing source " << j << " at node " << i << " count=" << count
                                  << " time=" << now << " status=" << nodes[i]->status << "\n";
                        for (std::size_t k = 0; k < nodes.size(); ++k)
                            std::cerr << k << ": " << nodes[k]->status << "\n";
                        throw std::runtime_error("Admitted player missing from roster");
                    }
                    check(p->appearance.value().has_value(),
                          "Player cosmetic recipe missing after route transition");
                    const auto sound = p->audio.sample(now);
                    check(sound && sound->values[1] == static_cast<float>(j) + .5f,
                          "Player sound missing or attributed to wrong source");
                    const auto pose = p->poses.sample(now);
                    const auto lag = pose ? local.pose.root.position[0] - pose->root.position[0] : 999.f;
                    const auto bound = .25f + 3.f * static_cast<float>(p->received_pose_interval) / 1000000.f;
                    if (lag >= bound) std::cerr << "Lag: receiver=" << i << " sender=" << j << " seconds=" << lag
                        << " cadence=" << p->received_pose_interval << " arrival_age=" << (now - p->pose_arrival) << "\n";
                    check(pose && lag < bound, "Remote pose fell behind sender time");
                }
    }
};
// Linked throwdowns travel through the host like chat: each message reaches every
// other player exactly once, never its sender, and never another world.
void throwdown_routing_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(60); sim.fresh(3);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2];
    const auto received = [](const Session &s, std::uint64_t from, const std::vector<std::uint8_t> &message) {
        return std::count_if(s.throwdown_inbox.begin(), s.throwdown_inbox.end(),
                             [&](const auto &entry) { return entry.first == from && entry.second == message; });
    };
    const auto clear = [&] { for (auto &node : sim.nodes) node->throwdown_inbox.clear(); };
    clear();
    const std::vector<std::uint8_t> from_guest{3, 1, 2, 3}, from_host{5, 9, 9};
    send_throwdown(first, from_guest);
    sim.run(10);
    const auto guest_id = first.transport.status().local_id;
    check(received(host, guest_id, from_guest) == 1 && received(second, guest_id, from_guest) == 1 &&
              first.throwdown_inbox.empty(),
          "A guest's throwdown message did not reach every other player exactly once");
    clear();
    send_throwdown(host, from_host);
    sim.run(10);
    const auto host_id = host.transport.status().local_id;
    check(received(first, host_id, from_host) == 1 && received(second, host_id, from_host) == 1 && host.throwdown_inbox.empty(),
          "The host's throwdown message did not reach every guest exactly once");
    clear();
    auto stale = packet(first, PacketKind::throwdown, sim.now);
    stale.throwdown = from_guest;
    stale.world = first.world + 1;
    check(first.transport.send(first.host_id, encode_wire(stale), true), "Could not queue a wrong-world throwdown fixture");
    sim.run(10);
    check(host.throwdown_inbox.empty() && second.throwdown_inbox.empty(), "A throwdown message from another world was relayed");
}
// A lobby's parties are the ones its players form, kept by the host the way a dedicated
// server keeps them: nobody is in a party for being in the lobby.
void party_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 4; ++i) sim.add();
    sim.run(60); sim.fresh(4);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2], &third = *sim.nodes[3];
    const auto id = [](const Session &s) { return s.transport.status().local_id; };
    for (const auto &node : sim.nodes)
        check(!node->local_party && node->party_invites.empty(), "A lobby put a player in a party nobody formed");

    check(send_party_request(first, PartyAction::invite, id(second)).empty(), "A guest's party invite was not sent");
    sim.run(20);
    check(second.party_invites.size() == 1 && second.party_invites.front().from == id(first),
          "The host did not pass a guest's invite on to the invited guest");
    check(!first.local_party && !second.local_party, "An unanswered invite made a party");
    check(send_party_request(second, PartyAction::accept, id(first)).empty(), "Accepting an invite was not sent");
    sim.run(40);
    check(first.local_party && first.local_party == second.local_party && first.local_party_leader && !second.local_party_leader,
          "An accepted invite did not put both guests in one party led by the inviter");
    check(party_member(first, id(second)) && party_member(second, id(first)), "Party members do not see each other as members");
    check(!host.local_party && !third.local_party && !party_member(third, id(first)) && party_of(third, id(first)) == first.local_party,
          "Players outside a party were put in it, or cannot see who is in it");

    // The host's own requests are answered on the spot, and make a second party.
    check(send_party_request(host, PartyAction::invite, id(third)).empty(), "The host's own party invite was refused");
    sim.run(20);
    check(third.party_invites.size() == 1 && third.party_invites.front().from == id(host), "The host's invite did not reach the guest");
    check(send_party_request(third, PartyAction::accept, id(host)).empty(), "Accepting the host's invite was not sent");
    sim.run(40);
    check(host.local_party && host.local_party == third.local_party && host.local_party_leader && host.local_party != first.local_party,
          "The host and its guest are not in a party of their own");
    check(party_of(first, id(third)) == host.local_party && !party_member(first, id(third)), "A guest does not see the other party");

    // Party chat reaches the party and nobody else.
    const auto heard = [](const Session &s, std::string_view words) {
        return std::any_of(s.chat.begin(), s.chat.end(), [&](const auto &line) { return line.text.find(words) != std::string::npos; });
    };
    check(send_party_chat(first, "meet at the bowl").empty(), "A guest's party chat was refused");
    sim.run(20);
    check(heard(first, "meet at the bowl") && heard(second, "meet at the bowl"), "Party chat did not reach the party");
    check(!heard(host, "meet at the bowl") && !heard(third, "meet at the bowl"), "Party chat reached players outside the party");
    check(send_party_chat(host, "host party only").empty(), "The host's party chat was refused");
    sim.run(20);
    check(heard(third, "host party only") && !heard(first, "host party only") && !heard(second, "host party only"),
          "The host's party chat did not stay in its party");

    // Leaving: a party left with one member is no party.
    check(send_party_request(second, PartyAction::leave, 0).empty(), "Leaving a party was not sent");
    sim.run(40);
    check(!first.local_party && !second.local_party, "A party of one was kept");
    check(send_party_chat(first, "anyone").starts_with("You're not in a party"), "Party chat was sent without a party");
    // A guest leaving the lobby is out of their party as well.
    stop(third, "Left"); sim.run(40);
    check(!host.local_party && host.parties.parties().empty(), "A guest who left the lobby stayed in the host's party");
    std::cout << "Lobby parties: nobody by default, invites, two parties, party chat, leaving and departures passed.\n";
}
// A player's badge and colour, in chat and on their nametag: who the backend lists them as
// comes before what they are in the lobby, and a developer before the other lists.
void role_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 4; ++i) sim.add();
    sim.run(60); sim.fresh(4);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2], &third = *sim.nodes[3];
    const auto id = [](const Session &s) { return s.transport.status().local_id; };
    using Role = std::pair<std::uint32_t, std::string>;
    using L = IdentityList;
    check(player_role(first, id(host), false) == Role{nametag_host, "Host"} &&
              player_role(host, id(host), true) == Role{nametag_host, "Host"} &&
              player_role(host, id(first), false) == Role{nametag_white, {}},
          "Players the backend does not list did not get their lobby roles");

    simulated_identities = {{id(host), L::homie}, {id(first), L::content_creator},
                            {id(second), L::developer}, {id(second), L::content_creator}, {id(second), L::homie},
                            {id(third), L::content_creator}, {id(third), L::homie}};
    check(player_role(first, id(host), false) == Role{nametag_homie, "Homie"} &&
              player_role(host, id(host), true) == Role{nametag_homie, "Homie"},
          "A homie who hosts is not shown as a homie");
    check(player_role(host, id(first), false) == Role{nametag_creator, "Creator"} &&
              player_role(first, id(first), true) == Role{nametag_creator, "Creator"},
          "A content creator is not shown as one");
    check(player_role(host, id(second), false) == Role{nametag_developer, "Dev"},
          "A developer on every list is not shown as a developer");
    check(player_role(host, id(third), false) == Role{nametag_creator, "Creator"},
          "A content creator who is also a homie is not shown as a creator");
    // A special tag comes before every lobby role: a Centrix player who hosts is Centrix, not Host.
    simulated_identities.insert({id(host), L::centrix});
    check(player_role(first, id(host), false) == Role{nametag_centrix, "Centrix"} &&
              player_role(host, id(host), true) == Role{nametag_centrix, "Centrix"},
          "A Centrix player who hosts is not shown as Centrix");
    simulated_identities.erase({id(host), L::centrix});
    // A player who has turned their tag off (the Special page) is whatever they are in the lobby,
    // to everyone: their appearance carries the choice through the host. Their items are a
    // separate choice, which leaves the tag alone.
    auto plain = packet(host, PacketKind::cosmetics, sim.now);
    plain.appearance = {{skater_recipe_key, 2, {}, {{1, "Outfit0", {}}}}, {board_recipe_key, 1, {}, {{2, "Deck", {}}}}};
    plain.appearance.hide_items = true;
    host.cosmetic_packet = encode_wire(plain);
    broadcast(host, plain, true, false, sim.now);
    sim.run(20);
    const auto told = [&](const Session &s) {
        const auto *peer = find_peer(const_cast<Session &>(s), id(host));
        return peer && !shows_items(*peer) && shows_tag(*peer);
    };
    check(told(first) && told(second) && player_role(first, id(host), false) == Role{nametag_homie, "Homie"},
          "A homie who turned their items off was not seen to, or lost their tag");
    // A later packet, as the host's game would send after the change.
    const auto look = plain.appearance;
    plain = packet(host, PacketKind::cosmetics, sim.now);
    plain.appearance = look;
    plain.appearance.hide_items = false, plain.appearance.hide_tag = true;
    host.cosmetic_packet = encode_wire(plain);
    broadcast(host, plain, true, false, sim.now);
    sim.run(20);
    check(player_role(first, id(host), false) == Role{nametag_host, "Host"} &&
              player_role(second, id(host), false) == Role{nametag_host, "Host"},
          "A homie who turned their tag off still shows as a homie");
    const auto *shown = find_peer(first, id(host));
    check(shown && shows_items(*shown), "Turning their tag off turned a player's items off");
    check(player_role(host, id(first), false) == Role{nametag_creator, "Creator"}, "One player's choice hid another's tag");
    show_own_items(false);
    check(player_role(first, id(first), true) == Role{nametag_creator, "Creator"}, "Turning their items off hid a player's own tag");
    show_own_items(true);
    show_own_tag(false);
    check(player_role(first, id(first), true) == Role{nametag_white, {}} &&
              player_role(host, id(first), false) == Role{nametag_creator, "Creator"},
          "A player's own choice did not hide their tag from themselves, or hid it from others before they were told");
    show_own_tag(true);
    // A chat line carries its sender's role.
    check(send_chat(first, "new video is up").empty(), "A guest's chat was refused");
    sim.run(20);
    const auto said = std::find_if(host.chat.begin(), host.chat.end(), [](const auto &line) { return line.text == "new video is up"; });
    check(said != host.chat.end() && said->color == nametag_creator && said->tag == "Creator",
          "A content creator's chat line does not carry their role");

    // None of it can be claimed. A badge goes to a Steam identity this PC is itself connected to:
    // a guest knows another guest only from the host's roster until Steam connects the two, and
    // a roster alone, which a host fills as it likes, gives nobody a badge.
    const Role developer{nametag_developer, "Dev"};
    auto *seen = find_peer(first, id(second));
    check(seen && seen->direct_ready && player_role(first, id(second), false) == developer &&
              player_role(third, id(second), false) == developer,
          "Guests connected to a developer do not see them as one");
    seen->direct_ready = false;
    check(!steam_vouched(first, *seen) && player_role(first, id(second), false) == Role{nametag_white, {}},
          "A guest took a developer's identity from the host's roster alone");
    seen->direct_ready = true;
    // In chat a guest's badge rests on their own copy of the line, sent straight to the players
    // Steam connects them to: the host passes lines on, and could pass on anything under
    // anyone's name.
    const auto line_of = [](const Session &s, std::string_view text) {
        const auto line = std::find_if(s.chat.begin(), s.chat.end(), [&](const auto &v) { return v.text == text; });
        return line == s.chat.end() ? nullptr : &*line;
    };
    check(send_chat(second, "patch notes are out").empty(), "A developer's chat was refused");
    sim.run(6);
    const auto *real = line_of(third, "patch notes are out"), *at_host = line_of(host, "patch notes are out");
    check(real && real->tag == "Dev" && real->color == nametag_developer && at_host && at_host->tag == "Dev",
          "A developer's own line lost its badge on the way, or waited");
    check(std::count_if(third.chat.begin(), third.chat.end(), [](const auto &v) { return v.text == "patch notes are out"; }) == 1,
          "A line and its sender's own copy of it were both shown");
    // A line the host makes up under a developer's name waits for their copy, then shows as
    // any other player's would.
    auto lie = packet(host, PacketKind::chat, sim.now);
    lie.source = id(second), lie.epoch = second.epoch;
    lie.text = "send me your password";
    check(send_packet(host, id(third), lie, true, false), "The made-up line could not be sent");
    sim.run(4);
    check(!line_of(third, "send me your password"), "A line without its sender's copy did not wait for it");
    sim.run(20);
    const auto *made_up = line_of(third, "send me your password");
    check(made_up && made_up->tag.empty() && made_up->color == nametag_white, "A line the host made up carries a developer's badge");
    // So does one from a build that sends no copy; the host, who has it from the developer
    // themselves, shows the badge.
    auto old = packet(second, PacketKind::chat, sim.now);
    old.text = "from an older build";
    check(send_packet(second, id(host), old, true, false), "The older build's line could not be sent");
    sim.run(30);
    const auto *bare = line_of(third, "from an older build"), *first_hand = line_of(host, "from an older build");
    check(bare && bare->tag.empty() && first_hand && first_hand->tag == "Dev",
          "A line with no copy from its sender shows a badge to a guest, or lost it at the host");
    // The other lists' badges travel the same way, guest to guest.
    check(send_chat(third, "clip is on my channel").empty(), "A content creator's chat was refused");
    sim.run(6);
    const auto *clip = line_of(first, "clip is on my channel");
    check(clip && clip->color == nametag_creator && clip->tag == "Creator", "A content creator's own line lost its badge");
    // A player on no list stays plain whatever their own packets ask for: the styles only shape
    // what a list already gives.
    simulated_identities.erase({id(first), L::content_creator});
    auto wish = packet(first, PacketKind::cosmetics, sim.now);
    wish.appearance = look;
    wish.appearance.marks.fill({MarkMode::gradient, {255, 0, 0}, {0, 0, 255}, 2});
    first.cosmetic_packet = encode_wire(wish);
    broadcast(first, wish, true, false, sim.now);
    sim.run(20);
    const auto *wisher = find_peer(host, id(first));
    check(wisher && wisher->appearance.value() && wisher->appearance.value()->marks[0].mode == MarkMode::gradient &&
              !identity_mark(id(first)) && player_role(host, id(first), false) == Role{nametag_white, {}} &&
              player_role(third, id(first), false) == Role{nametag_white, {}},
          "A player on no list got a badge by sending styles");
    // And their chat is as it always was: shown as it arrives, with no copy sent or waited for.
    check(send_chat(first, "anyone at the plaza").empty(), "A guest's chat was refused");
    sim.run(3);
    const auto *ordinary = line_of(third, "anyone at the plaza");
    const auto *said_to = find_peer(third, id(first));
    check(ordinary && ordinary->tag.empty() && ordinary->color == nametag_white && said_to && said_to->chat_proofs.empty() &&
              said_to->chat_waiting.empty(),
          "An ordinary player's line was held up, or a copy of it was kept");
    // Nor can anyone name someone else as the sender. What does not come over that player's own
    // Steam connection is refused, and whoever sent it is out of the session.
    const auto heard = [](const Session &s) {
        return std::any_of(s.chat.begin(), s.chat.end(), [](const auto &line) { return line.text == "free decks at my link"; });
    };
    auto forged = packet(first, PacketKind::chat, sim.now);
    forged.source = id(second), forged.epoch = second.epoch;
    forged.text = "free decks at my link";
    check(send_packet(first, id(host), forged, true, false) && send_packet(first, id(third), forged, true, false),
          "The forged chat line could not be sent");
    sim.run(20);
    check(!heard(host) && !heard(second) && !heard(third), "A guest spoke as a developer");
    check(!find_peer(host, id(first)) && !find_peer(third, id(first)), "A guest who forged a sender stayed in the session");
    // The same for how a developer looks: a guest cannot hide or restyle their tag and items.
    auto costume = packet(third, PacketKind::cosmetics, sim.now);
    costume.source = id(second), costume.epoch = second.epoch;
    costume.sequence += 1000;
    costume.appearance = look;
    costume.appearance.hide_tag = costume.appearance.hide_items = true;
    check(send_packet(third, id(host), costume, true, false), "The forged outfit could not be sent");
    sim.run(20);
    const auto *theirs = find_peer(host, id(second));
    check(player_role(host, id(second), false) == developer && theirs && shows_tag(*theirs) && shows_items(*theirs) &&
              !find_peer(host, id(third)),
          "A guest changed how a developer shows, or stayed in the session after trying");
    simulated_identities.clear();
    std::cout << "Roles: lobby roles, homie, content creator, developer first, chat lines and claimed identities passed.\n";
}
// The backend's bans hold in every session, and reach one that is running: a banned guest is
// out and cannot come back, the others stay, and nobody stays with a banned host.
void global_ban_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(60); sim.fresh(3);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2];
    const auto id = [](const Session &s) { return s.transport.status().local_id; };
    const auto first_id = id(first), host_id = id(host);

    simulated_identities = {{first_id, IdentityList::banned}};
    sim.run(40);
    check(first.mode == Mode::off && first.status == banned_notice, "A banned guest stayed in the session");
    check(!find_peer(host, first_id) && !find_peer(second, first_id), "The session kept a banned guest");
    check(second.mode == Mode::join && find_peer(host, id(second)), "Banning one guest took another out");
    // A banned player whose game does not stop itself is turned away by the host all the same.
    first.mode = Mode::join;
    first.host_id = host_id;
    first.transport.join(host_id);
    host.transport.poll();
    networking(host, sim.local, sim.now);
    const auto &links = host.transport.status().peers;
    check(std::none_of(links.begin(), links.end(), [&](const auto &link) { return link.id == first_id; }) &&
              !find_peer(host, first_id),
          "The host let a banned player back in");
    stop(first, "Left");

    simulated_identities = {{host_id, IdentityList::banned}};
    sim.run(40);
    check(host.mode == Mode::off && host.status == banned_notice, "A banned host kept hosting");
    check(second.mode == Mode::off, "A guest stayed with a banned host");
    simulated_identities.clear();
    std::cout << "Global bans: a banned guest, a guest who comes back, the other guests and a banned host passed.\n";
}
// A host that is kept from reading for a few seconds (a server whose console held it up) then
// reads everything its guests sent meanwhile in one go. That is not a flood: nobody is dropped
// for it, while a real flood still is.
void stall_checks() {
    {
        // 16 s of a player's ordinary traffic read at once, counted by when it arrived.
        ReceiveBudget backlog;
        bool kept = true;
        for (std::uint64_t i = 0; i < 16 * 90; ++i) kept = kept && backlog.accept(5000000 + i * (1000000 / 90), 300);
        check(kept, "A backlog read after a stall was taken for a flood");
        ReceiveBudget flood;
        bool refused = false;
        for (std::uint64_t i = 0; i < 2000; ++i) refused = refused || !flood.accept(5000000 + i * 100, 300);
        check(refused, "A flood was let through the packet limit");
        // Lanes are read one after another, so arrival times step back and forth.
        ReceiveBudget lanes;
        refused = false;
        for (std::uint64_t i = 0; i < 2000; ++i) refused = refused || !lanes.accept(i % 2 ? 9000000 : 9500000, 300);
        check(refused, "Packets read out of arrival order started the count again");
    }
    Simulation sim(max_players, 120);
    sim.stamp = true;
    for (unsigned i = 0; i < 4; ++i) sim.add();
    sim.run(240);
    auto &host = *sim.nodes[0];
    const auto connected = [&] {
        unsigned count{};
        for (std::size_t n = 1; n < sim.nodes.size(); ++n) {
            const auto *peer = find_peer(host, sim.nodes[n]->transport.status().local_id);
            count += peer && peer->handshaken && sim.nodes[n]->mode == Mode::join;
        }
        return count;
    };
    check(connected() == 3, "The stall fixture did not connect its guests");
    // Nine seconds unread: under the ten after which a silent player is given up on, and more
    // than a second's allowance from every guest.
    const auto sent = sim.nodes[1]->transport.status().sent;
    sim.held = 0;
    sim.run(static_cast<unsigned>(9000000 / network_tick_us));
    sim.held = static_cast<std::size_t>(-1);
    check(sim.nodes[1]->transport.status().sent - sent > 2ULL * multiplayer_tick_rates.back() + 80 + 32,
          "The stall fixture did not queue more than the packet limit");
    sim.run(240);
    if (host.mode != Mode::host || connected() != 3)
        for (std::size_t n = 0; n < sim.nodes.size(); ++n)
            std::cerr << "  node " << n << " mode " << static_cast<int>(sim.nodes[n]->mode) << ": " << sim.nodes[n]->status << "\n";
    check(host.mode == Mode::host && connected() == 3, "Guests were dropped after their host was held up for a few seconds");
    SimulatedNetwork::clock = 0;
    std::cout << "Stalls: a held-up host keeps its guests; floods and out-of-order arrival are still limited.\n";
}
// What a host changes outside its physics tuning (the trainer's class values and trick
// multipliers) reaches its guests while it sets everyone's physics: when it changes, to a
// player who joins later, and from nobody but the host.
void physics_extras_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(60); sim.fresh(3);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2];
    // The simulated players share one process, so they share what "the local player" changed:
    // only the host's session sends it.
    const std::vector<std::uint8_t> boosted{1, 2, 3, 4, 5}, calmer{9, 8};
    const auto physics = [&] { for (auto &node : sim.nodes) update_physics_tuning(*node, sim.local, sim.now); };
    dingosdk::set_local_physics_extras(boosted);
    physics();
    sim.run(10);
    check(first.host_extras == boosted && second.host_extras == boosted, "The host's physics extras did not reach its guests");
    check(!host.host_extras, "The host took physics extras for itself");

    update_physics_tuning(first, sim.local, sim.now);
    std::uint64_t seen{};
    std::vector<std::uint8_t> handed;
    check(dingosdk::session_tuning_enforced() && dingosdk::host_physics_extras(seen, handed) && handed == boosted,
          "A guest's game was not handed the host's physics extras");
    update_physics_tuning(host, sim.local, sim.now);
    check(!dingosdk::session_tuning_enforced(), "A host was told another player sets its physics");

    auto &late = sim.add();
    sim.run(60); sim.fresh(4);
    check(late.host_extras == boosted, "A player who joined later did not get the host's physics extras");

    dingosdk::set_local_physics_extras(calmer);
    physics();
    sim.run(10);
    check(first.host_extras == calmer && second.host_extras == calmer && late.host_extras == calmer,
          "A change to the host's physics extras did not reach its guests");

    // A guest cannot set the others' physics.
    auto forged = packet(first, PacketKind::physics_extras, sim.now);
    forged.extras = {6, 6, 6};
    broadcast(first, forged, true, false, sim.now);
    sim.run(10);
    check(second.host_extras == calmer && late.host_extras == calmer && !host.host_extras,
          "A guest's physics extras were taken for the host's");

    // Back to the game's own: sent, so no guest keeps what it had.
    dingosdk::set_local_physics_extras({});
    physics();
    sim.run(10);
    check(first.host_extras && first.host_extras->empty() && second.host_extras && second.host_extras->empty(),
          "Guests kept physics extras the host no longer has");

    // Enforcement off: nothing kept for late joiners, and a guest's game is its own again.
    host.enforce_tuning = false;
    host.roster_dirty = true;
    physics();
    sim.run(20);
    check(host.extras_packet.empty() && !host.sent_extras, "A host that stopped setting everyone's physics kept its extras packet");
    check(!first.enforce_tuning, "The guest was not told the host stopped setting everyone's physics");
    update_physics_tuning(first, sim.local, sim.now);
    check(!dingosdk::session_tuning_enforced(), "A guest stayed under the host's physics after the host let go");
    dingosdk::set_session_tuning_enforced(false);
    dingosdk::set_host_physics_extras({});
    std::cout << "Physics extras: host to guests, late joiners, changes, forged senders and enforcement off passed.\n";
}
// A lobby host keeps a guest whose mods change scoring or physics out of linked activities: the roster
// flags them for everyone and the host passes none of their throwdown messages on.
void scoring_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(60); sim.fresh(3);
    auto &host = *sim.nodes[0], &first = *sim.nodes[1], &second = *sim.nodes[2];
    const auto first_id = first.transport.status().local_id;
    auto *first_at_host = find_peer(host, first_id);
    check(first_at_host && first_at_host->scoring == 0 && !first_at_host->member.scoring,
          "A guest with the game's own scoring did not report it, or was flagged");
    check(first.scoring_sent && first.scoring_sent->first == 0, "The guest's report was not remembered");

    auto report = packet(first, PacketKind::scoring, sim.now);
    report.scoring = 0xbadbadbadULL;
    report.text = "BigPoints";
    check(first.transport.send(first.host_id, encode_wire(report), true), "Could not queue a scoring report fixture");
    sim.run(40);
    first_at_host = find_peer(host, first_id);
    const auto *first_at_second = find_peer(second, first_id);
    check(first_at_host && first_at_host->member.scoring && first_at_host->scoring_mods == "BigPoints",
          "The host did not flag a guest whose mods change scoring or physics");
    check(first.local_scoring, "The flagged guest does not know it was flagged");
    check(first_at_second && first_at_second->member.scoring, "Other guests do not see the flag");
    check(std::any_of(second.chat.begin(), second.chat.end(),
                      [](const auto &line) { return line.text.find("change scoring or physics") != std::string::npos; }),
          "Other guests were not told in chat");

    for (auto &node : sim.nodes) node->throwdown_inbox.clear();
    send_throwdown(first, {3, 1, 2, 3});
    sim.run(10);
    check(host.throwdown_inbox.empty() && second.throwdown_inbox.empty(),
          "A flagged guest's throwdown message was passed on");
    send_throwdown(second, {4, 5, 6});
    sim.run(10);
    check(host.throwdown_inbox.size() == 1 && first.throwdown_inbox.size() == 1,
          "An unflagged guest's throwdown message stopped reaching the others");

    check(edit_score_check(host, "off") == "Mods that change scoring or physics are no longer checked.", "score-check off refused");
    sim.run(40);
    check(!find_peer(host, first_id)->member.scoring && !first.local_scoring && !find_peer(second, first_id)->member.scoring,
          "Turning the check off did not clear the flag");
    for (auto &node : sim.nodes) node->throwdown_inbox.clear();
    send_throwdown(first, {3, 1, 2, 3});
    sim.run(10);
    check(host.throwdown_inbox.size() == 1 && second.throwdown_inbox.size() == 1,
          "With the check off, the guest's throwdown messages still did not go through");
    check(edit_score_check(first, "on") != "Mods that change scoring or physics are no longer checked." && first.score_check,
          "A guest changed the host's scoring check");

    // The host's own mods count too.
    check(edit_score_check(host, "on").find("kept out") != std::string::npos, "score-check on refused");
    mods::simulated_scoring = {true, 0x1234, {"HostPoints"}};
    sim.run(40);
    check(host.local_scoring && find_peer(second, host.transport.status().local_id)->member.scoring,
          "The host's own scoring mod was not flagged");
    mods::simulated_scoring = {true, 0, {}};
}
void object_sync_checks() {
    Simulation sim(32);
    sim.local.pose.skater.resize(1); sim.local.pose.board.resize(1);
    for (unsigned i = 0; i < 31; ++i) {
        auto &node = sim.add();
        const NetworkObject object{1, "own_bk_ramp", {static_cast<float>(i), 2, 5}, {0, 0, 0, 1}};
        node.local_objects.replace(std::span(&object, 1));
    }
    sim.run(90); sim.fresh(31);
    auto &last = sim.add();
    const NetworkObject last_object{1, "own_bk_ramp", {32, 3, 5}, {0, 0, 0, 1}};
    last.local_objects.replace(std::span(&last_object, 1));
    sim.run(80); sim.fresh(32);
    for (const auto &node : sim.nodes) {
        check(player_count(*node) == 32, "A full 32-player roster was not admitted");
        for (const auto &peer : node->peers)
            if (peer.handshaken && peer.objects.objects().size() != 1)
                throw std::runtime_error("Object sync: receiver " + std::to_string(node->transport.status().local_id) +
                    ", owner " + std::to_string(peer.member.id) + ", count " + std::to_string(peer.objects.objects().size()) +
                    ", status " + node->status);
    }
    auto &owner = *sim.nodes[1];
    const auto owner_id = owner.transport.status().local_id;
    std::vector<NetworkObject> park;
    for (unsigned i = 0; i < 1024; ++i) park.push_back({i + 1ULL, "own_bk_ramp", {static_cast<float>(i), 2, 5}, {0, 0, 0, 1}});
    owner.local_objects.replace(park);
    sim.run(80);
    for (const auto &node : sim.nodes) if (node.get() != &owner)
        check(find_peer(*node, owner_id)->objects.layout() == park, "Chunked full park did not reach every player");
    park[0].position[0] = -15; park[1].rotation = {0, 1, 0, 0}; park.erase(park.begin() + 2);
    const auto before = owner.local_objects.revision();
    owner.local_objects.replace(park);
    const auto delta = owner.local_objects.updates(before);
    check(delta.size() == 1 && delta[0].objects.size() == 2 && delta[0].removed == std::vector<std::uint64_t>{3},
          "Moving/deleting a few objects resent the whole park");
    sim.run(30);
    for (const auto &node : sim.nodes) if (node.get() != &owner)
        check(find_peer(*node, owner_id)->objects.layout() == park, "Live object moves/rotations/deletion failed through the host");
    SteamTransport extra;
    check(!extra.join(sim.nodes[0]->host_id), "A 33rd player bypassed the advertised capacity");
    auto stale = packet(owner, PacketKind::objects, sim.now);
    stale.objects = owner.local_objects.updates(0).front(); stale.world = owner.world + 1;
    check(owner.transport.send(owner.host_id, encode_wire(stale), true), "Could not queue a stale-world fixture");
    sim.run(5);
    check(find_peer(*sim.nodes[0], owner_id)->objects.layout() == park, "Wrong-world object packet changed the live park");
    auto spoof = packet(*sim.nodes[2], PacketKind::objects, sim.now);
    spoof.source = owner_id; spoof.epoch = owner.epoch; spoof.objects = delta[0];
    sim.nodes[2]->transport.send(owner.host_id, encode_wire(spoof), true);
    sim.run(6);
    check(sim.nodes[2]->mode == Mode::off && find_peer(*sim.nodes[0], owner_id)->objects.layout() == park,
          "A guest could publish another player's object edits");
    stop(owner, "Owner left"); sim.run(8);
    for (const auto &node : sim.nodes) if (node->mode != Mode::off) {
        check(!find_peer(*node, owner_id), "A departed owner's object state remained in the roster");
        check(node->local_objects.objects().size() == 1, "Another owner's edits changed a local layout");
        clear_world(*node, sim.now);
        check(!node->local_objects.revision() && std::all_of(node->peers.begin(), node->peers.end(), [](const auto &peer) {
            return !peer.objects.revision() && peer.object_delivery.chunks.empty();
        }), "Map travel retained object state or queued packets from the previous world");
    }
    std::cout << "32-player objects: late join, matching owner IDs, full parks, deltas, delete, spoof rejection, disconnect and world reset passed.\n";
}
void tick_settings_checks() {
    auto& configured = session();
    stop(configured, "TPS fixture");
    check(configured.tps == 30, "Default session TPS is not 30");
    for (const auto rate : multiplayer_tick_rates) {
        const auto result = command("host-config", "code 8 " + std::to_string(rate) + " Test lobby");
        check(configured.mode == Mode::host && configured.tps == rate && configured.lobby_name == "Test lobby",
              "Host form did not apply TPS and lobby name");
        (void)result;
        const auto epoch = configured.epoch;
        (void)command("host-config", "code 8 45 Invalid");
        check(configured.epoch == epoch && configured.tps == rate, "Invalid TPS replaced an active session");
        check(configured.status == "Choose 20, 30, 60, or 120 TPS before hosting.", "A refused host left no status for the menu");
        stop(configured, "TPS fixture");
        Simulation sim(4, rate);
        sim.add(); sim.add(); sim.run(70);
        check(sim.nodes[1]->tps == rate, "Joining guest did not adopt host TPS");
        sim.add(); sim.run(70);
        check(sim.nodes.back()->tps == rate, "Late joiner did not adopt host TPS");
        for (const auto& node : sim.nodes) {
            clear_world(*node, sim.now);
            check(node->tps == rate, "Map transition reset host TPS");
        }
    }
    (void)command("host", "code 8 120 is a lobby name");
    check(configured.tps == 30 && configured.lobby_name == "120 is a lobby name", "Legacy host command changed meaning");
    // Whose physics guests skate with is the host's to switch, and is remembered for next time.
    check(configured.enforce_tuning, "A new lobby did not start with the host's physics for everyone");
    (void)command("tuning-enforce", "off");
    check(!configured.enforce_tuning && !configured.host_preferences.enforce_tuning && configured.roster_dirty,
          "The host could not let guests skate with their own physics");
    (void)command("tuning-enforce", "toggle");
    check(configured.enforce_tuning && configured.host_preferences.enforce_tuning, "The host could not switch its physics for everyone back on");
    stop(configured, "TPS fixture complete");
}
void pacing_checks() {
    bool passed = true;
    // The native callback runs once per rendered frame, not on a 50 ms timer.
    for (const auto tps : multiplayer_tick_rates) for (const unsigned fps : {30U, 59U, 60U, 75U, 144U}) {
        auto echo_storage = std::make_unique<Session>();
        auto &echo = *echo_storage;
        echo.mode = Mode::echo;
        echo.tps = tps;
        echo.secret = 73; echo.epoch = 91; echo.map = 42;
        echo.transport.open();
        NativeFrame local;
        local.ready = true;
        local.pose.skater.resize(395);
        local.pose.board.resize(17);
        for (unsigned frame = 0; frame < fps * 6; ++frame) {
            const auto now = 10000000ULL + std::uint64_t{frame} * 1000000 / fps;
            send_local(echo, local, now);
        }
        const auto count = echo.peers[0].pose_count;
        std::cout << "Pacing " << tps << " TPS at " << fps << " FPS: " << count << " poses / 6 seconds\n";
        const auto expected = std::min(tps, fps) * 6;
        passed &= count >= expected - 1 && count <= expected;
        const auto stalled = 20000000ULL;
        send_local(echo, local, stalled);
        send_local(echo, local, stalled);
        passed &= echo.peers[0].pose_count == count + 1;
    }
    // Relay arrivals alternate 35/65 ms, even though the sender is exactly 20 TPS.
    // Distance thinning must use source time; nearby delivery must not thin again.
    for (const bool uneven_source : {false, true}) for (const auto distance : {1.f, 100.f, 250.f}) {
        Simulation sim;
        auto &host = sim.add();
        sim.add(); sim.add();
        sim.run(30);
        const auto source_id = sim.nodes[1]->transport.status().local_id;
        const auto target_id = sim.nodes[2]->transport.status().local_id;
        auto *target = find_peer(host, target_id);
        target->pose_delivery = {};
        target->direct_routes.clear();
        target->route_reported = 0;
        auto pose = packet(*sim.nodes[1], PacketKind::pose, sim.now);
        pose.pose = sim.local.pose;
        const auto before = host.transport.status().sent;
        for (unsigned tick = 0; tick < 120; ++tick) {
            pose.time_us = sim.now + (uneven_source
                ? ((tick * 3ULL + 1) / 2) * 1000000 / 30 : tick * 50000ULL);
            ++pose.sequence;
            const auto arrival = pose.time_us + 100000 + (tick % 2 ? 15000 : 0);
            target->latest_root = pose.pose.root;
            target->latest_root->position[2] += distance;
            target->pose_arrival = arrival;
            broadcast(host, pose, false, true, arrival, source_id);
        }
        const auto count = host.transport.status().sent - before;
        const auto expected = distance < 60 ? 120U : distance < 170 ? 60U : 30U;
        std::cout << "Jittered relay " << distance << " m, source FPS=" << (uneven_source ? 30 : 20)
                  << ": " << count << " / " << expected << " poses\n";
        passed &= count == expected;
    }
    check(passed, "Frame/arrival timing reduced pose TPS or caused a catch-up burst");
    std::cout << "Pacing: render-frame cadence, stalls and jittered 20/10/5 TPS relay checks passed.\n";
}
void priority_mesh_checks() {
    SimulatedNetwork::audit_lanes = SimulatedNetwork::cosmetics_first = true;
    SimulatedNetwork::lane_sends = {};
    Simulation sim;
    auto &host = sim.add();
    auto &guest = sim.add();
    erase_key(host.password);
    erase_key(guest.password);
    SimulatedNetwork::hold_control_to = guest.transport.status().local_id;
    sim.run(6);
    const auto *peer = find_peer(guest, host.host_id);
    check(peer && !peer->handshaken && !peer->pending_cosmetics.empty() && !peer->appearance.value(),
          "Early cosmetic was lost or applied before welcome/admission");
    SimulatedNetwork::hold_control_to = 0;
    sim.run(20);
    sim.fresh(2);
    for (unsigned i = 2; i < 8; ++i) { auto &node = sim.add(); erase_key(node.password); }
    sim.run(80);
    sim.fresh(8);
    auto outfit = packet(host, PacketKind::cosmetics, sim.now);
    outfit.appearance = {{skater_recipe_key, 2, {}, {{1, "Updated outfit", {}}}},
                        {board_recipe_key, 1, {}, {{2, "Updated deck", {}}}}};
    broadcast(host, outfit, true, false, sim.now);
    SimulatedNetwork::lose_unreliable = true;
    sim.run(80);
    sim.fresh(8);
    for (std::size_t i = 1; i < sim.nodes.size(); ++i)
        check(find_peer(*sim.nodes[i], host.host_id)->appearance.value() == outfit.appearance,
              "Cosmetic update failed after prioritized baseline/delta delivery");
    auto &target = *sim.nodes[1];
    auto *host_link = find_peer(target, host.host_id);
    auto spoof = outfit;
    spoof.epoch += 1000;
    spoof.sequence += 10;
    defer_cosmetics(*host_link, spoof, sim.now);
    receive_cosmetics(target, sim.local, sim.now);
    check(host_link->appearance.value() == outfit.appearance, "Deferred outfit bypassed roster epoch validation");
    receive_cosmetics(target, sim.local, sim.now + 10000001);
    check(host_link->pending_cosmetics.empty(), "Unadmitted cosmetics retained beyond deadline");
    for (unsigned i = 0; i < max_players + 5; ++i) {
        spoof.source = i + 1;
        defer_cosmetics(*host_link, spoof, sim.now);
    }
    check(host_link->pending_cosmetics.size() == max_players, "Pending cosmetics exceeded source bound");
    clear_world(target, sim.now);
    check(find_peer(target, host.host_id)->pending_cosmetics.empty(), "Old outfits survived world reset");
    check(std::all_of(SimulatedNetwork::lane_sends.begin(), SimulatedNetwork::lane_sends.end(),
                      [](auto count) { return count > 0; }), "Not all Steam traffic lanes were exercised");
    SimulatedNetwork::lose_unreliable = false;
    std::cout << "Priority mesh: early cosmetics before admission, eight-player reordered delivery, "
                 "stream/reference lanes, packet loss, outfit updates, epoch validation and bounded reset passed.\n";
}
struct MapPair {
    static inline unsigned loads{};
    static inline bool missing{};
    // Match runtime ownership: large session buffers do not fit on the
    // default Windows stack when two map-transition peers are nested.
    std::unique_ptr<Session> host_storage = std::make_unique<Session>();
    std::unique_ptr<Session> guest_storage = std::make_unique<Session>();
    Session &host = *host_storage, &guest = *guest_storage;
    NativeFrame host_frame;
    std::string current = "Levels/Root/Root|Levels/Other/Other";
    bool ready = true;
    std::uint64_t now = 10000000;
    static MapLoadResult loader(std::string_view, bool submitted, std::string &detail) {
        if (missing) {
            detail = "Map not installed.";
            return MapLoadResult::missing;
        }
        if (!submitted) ++loads;
        return submitted ? MapLoadResult::waiting : MapLoadResult::queued;
    }
    MapPair(bool password = false, bool correct = true) {
        loads = 0;
        missing = false;
        host_frame.ready = true;
        host_frame.pose.skater.resize(1);
        host.mode = Mode::host;
        host.secret = guest.secret = 789;
        host.epoch = 101;
        guest.epoch = 102;
        host.map_name = "Levels/Root/Root|Levels/Beach/Beach";
        host.map = map_hash(host.map_name);
        host.transport.host(8);
        host.host_id = guest.host_id = host.transport.status().local_id;
        guest.transport.join(host.host_id);
        guest.mode = Mode::join;
        guest.awaiting_map = true;
        guest.join_started = now;
        if (password) {
            host.password = password_key("test room", host.secret);
            guest.password = password_key(correct ? "test room" : "wrong password", guest.secret);
        }
    }
    void run(unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) {
            now += network_tick_us;
            if (host.mode != Mode::off) networking(host, host_frame, now);
            if (guest.mode == Mode::off) continue;
            NativeFrame local;
            if (prepare_join_map(guest, ready, current, loader, now)) {
                local = host_frame;
                guest.map = map_hash(current);
                guest.map_name = current;
            }
            if (guest.mode != Mode::off) networking(guest, local, now);
        }
    }
};
void map_checks() {
    Packet offer;
    offer.kind = PacketKind::map_offer;
    offer.session = 1;
    offer.epoch = 2;
    offer.destination = "Levels/Root/Root|Levels/Beach/Beach";
    offer.map = map_hash(offer.destination);
    offer.map_authorized = true;
    const auto bytes = encode(offer);
    const auto decoded = decode(bytes);
    check(decoded && decoded->destination == offer.destination && decoded->map_authorized,
          "Host map offer did not round trip");
    for (std::size_t i = 0; i < bytes.size(); ++i)
        check(!decode(std::span(bytes.data(), i)), "Truncated map offer accepted");
    auto invalid = bytes;
    invalid[packet_header_size + 72] = 2;
    check(!decode(invalid), "Invalid map authorization flag accepted");
    invalid = bytes;
    invalid.back() ^= 1;
    check(!decode(invalid), "Map destination/hash mismatch accepted");
    check(!valid_map_destination("Levels/Root/Root") &&
              !valid_map_destination("Levels/Root|Bad\nLevel") &&
              !valid_map_destination("Levels/Root|Level|Other") &&
              !valid_map_destination("|Level") && valid_map_destination("Levels/Root/Root|"),
          "Invalid destination syntax accepted");
    for (bool password : {false, true}) {
        MapPair pair(password);
        pair.run(65);
        check(pair.guest.join_map_authorized && pair.guest.map_load_submitted && MapPair::loads == 1,
              "Authorized cross-map join did not queue exactly one load");
        check(!find_peer(pair.host, pair.guest.transport.status().local_id)->handshaken,
              "Guest admitted before host map loaded");
        pair.ready = false;
        pair.run(400); // Well beyond the old eight-second handshake timeout.
        check(pair.guest.mode == Mode::join && MapPair::loads == 1,
              "Loading disconnected the guest or queued duplicate loads");
        pair.current = pair.host.map_name;
        pair.run(4);
        check(pair.guest.awaiting_map, "Map identity alone bypassed skater readiness");
        pair.ready = true;
        pair.run(4);
        check(find_peer(pair.guest, pair.host.host_id)->handshaken &&
                  find_peer(pair.host, pair.guest.transport.status().local_id)->handshaken,
              "Guest failed to complete admission after native loading");
    }
    {
        MapPair pair;
        pair.current = pair.host.map_name;
        pair.run(10);
        check(MapPair::loads == 0 && find_peer(pair.guest, pair.host.host_id)->handshaken,
              "Same-map join unnecessarily reloaded the level");
    }
    {
        MapPair pair(true, false);
        pair.run(65);
        check(pair.guest.mode == Mode::off && MapPair::loads == 0,
              "Wrong password triggered a map load");
    }
    {
        MapPair pair(true);
        erase_key(pair.guest.password);
        pair.run(20);
        check(pair.guest.mode == Mode::off && MapPair::loads == 0,
              "Missing password triggered a map load");
    }
    {
        MapPair pair;
        MapPair::missing = true;
        pair.run(20);
        const auto said = "The host is on Beach, which is not installed on this PC. Install its map mod and join again.";
        check(pair.guest.mode == Mode::off && pair.guest.status == said && pair.guest.leave_notice == said,
              "Missing map did not end the pending join naming the map");
    }
    {
        MapPair pair;
        pair.run(20);
        stop(pair.guest, "Cancelled");
        pair.current = pair.host.map_name;
        pair.run(20);
        check(pair.guest.mode == Mode::off && !pair.guest.awaiting_map && pair.guest.join_destination.empty(),
              "Cancelled map load rejoined the server");
    }
    {
        MapPair pair;
        pair.run(20);
        pair.now += 180000001;
        pair.run(1);
        check(pair.guest.mode == Mode::off, "Loading join did not have a bounded deadline");
    }
    {
        MapPair pair;
        pair.run(20);
        stop(pair.host, "Host left");
        pair.run(1);
        check(pair.guest.mode == Mode::off, "Host departure left a pending map join active");
    }
    std::cout << "Automatic map joining: codec, same/different maps, password-before-load, slow loading, "
                 "skater readiness, missing maps, cancellation, timeout and host departure passed.\n";
}
struct TravelSimulation {
    Simulation sim;
    std::vector<std::string> current, queued;
    std::vector<bool> ready;
    std::vector<unsigned> loads;
    static inline TravelSimulation *active{};
    static inline std::size_t node{};
    const std::string beach = "Levels/Root/Root|Levels/Beach/Beach";
    const std::string city = "Levels/Root/Root|Levels/City/City";
    explicit TravelSimulation(unsigned count) {
        current.resize(count, beach);
        queued.resize(count);
        ready.resize(count, true);
        loads.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            auto &s = sim.add();
            s.map = map_hash(beach);
            s.map_name = beach;
            s.invite = format_invite({s.host_id, s.secret});
            auto cosmetic = *decode_wire(s.cosmetic_packet);
            cosmetic.map = s.map;
            s.cosmetic_packet = encode_wire(cosmetic);
        }
        run(80);
        sim.fresh(count);
    }
    static MapLoadResult loader(std::string_view destination, bool submitted, std::string &) {
        if (submitted) return MapLoadResult::waiting;
        active->queued[node] = destination;
        active->ready[node] = false;
        ++active->loads[node];
        return MapLoadResult::queued;
    }
    void finish(std::size_t i) {
        if (!queued[i].empty()) current[i] = queued[i];
        ready[i] = true;
    }
    void run(unsigned frames) {
        active = this;
        for (unsigned frame = 0; frame < frames; ++frame) {
            sim.now += network_tick_us;
            for (node = 0; node < sim.nodes.size(); ++node) {
                auto &s = *sim.nodes[node];
                if (s.mode == Mode::off) continue;
                refresh_host_choices(s, sim.now);
                NativeFrame local;
                if (prepare_join_map(s, ready[node], current[node], loader, sim.now) && ready[node]) {
                    local = sim.local;
                    local.pose.root.position[0] = static_cast<float>(sim.now - 10000000) / 1000000.f;
                    local.pose.root.position[2] = static_cast<float>(node);
                    sim.local.pose.root.position[0] = local.pose.root.position[0];
                }
                if (s.mode == Mode::off || !observe_local_world(s, local, current[node], sim.now)) continue;
                networking(s, local, sim.now);
                if (s.mode == Mode::off || !world_playing(s, local)) continue;
                if (s.cosmetic_packet.empty()) {
                    auto c = packet(s, PacketKind::cosmetics, sim.now);
                    c.appearance = {{skater_recipe_key, 2, {}, {{1, "Outfit" + std::to_string(node), {}}}},
                                    {board_recipe_key, 1, {}, {{2, "Deck", {}}}}};
                    s.cosmetic_packet = encode_wire(c);
                    broadcast(s, c, true, false, sim.now);
                }
                send_local(s, local, sim.now);
                auto sound = packet(s, PacketKind::audio, sim.now);
                sound.audio = {AudioSample{}};
                sound.audio[0].state.values[1] = static_cast<float>(node) + .5f;
                broadcast(s, sound, true, true, sim.now);
            }
        }
    }
    void connected() {
        const auto count = static_cast<unsigned>(sim.nodes.size());
        for (const auto &s : sim.nodes) {
            check(s->mode != Mode::off && player_count(*s) == count, "Map change lost room membership");
            check(s->secret == 73 && s->password.has_value(), "Map change replaced the invitation/password");
        }
        check(sim.nodes[0]->transport.status().peers.size() == count - 1,
              "Map change closed a guest's host connection");
    }
};
void travel_checks() {
    Packet state;
    state.kind = PacketKind::world_state;
    state.session = 1;
    state.epoch = 2;
    state.world = 3;
    state.map = 0;
    auto encoded = encode(state);
    check(decode(encoded)->destination.empty(), "Unknown-destination loading notice rejected");
    state.destination = "Levels/Root/Root|Levels/Beach/Beach";
    state.map = map_hash(state.destination);
    state.world_ready = true;
    encoded = encode(state);
    check(decode(encoded)->world == 3 && decode(encoded)->world_ready, "Transition state lost generation/readiness");
    for (std::size_t i = 0; i < encoded.size(); ++i)
        check(!decode(std::span(encoded.data(), i)), "Truncated transition accepted");
    encoded[packet_header_size + 32] = 2;
    check(!decode(encoded), "Invalid transition flag accepted");
    for (const unsigned count : {2U, 8U}) {
        TravelSimulation test(count);
        auto &host = *test.sim.nodes[0];
        const auto invite = host.invite;
        const auto old_pose = packet(host, PacketKind::pose, test.sim.now);
        auto stale = old_pose;
        stale.pose = test.sim.local.pose;
        stale.pose.root.position[0] = 9999;
        const auto stale_wire = encode_wire(stale);
        begin_host_world(host, test.city, test.sim.now);
        test.ready[0] = false;
        test.run(240);
        test.connected();
        for (unsigned i = 1; i < count; ++i)
            check(test.loads[i] == 1 && test.queued[i] == test.city, "Guest did not queue the host map exactly once");
        test.current[0] = test.city;
        test.ready[0] = true;
        for (unsigned i = 1; i + 1 < count; ++i) test.finish(i);
        test.run(240); // One guest still loading, others can skate.
        test.connected();
        check(!host.travelling && !test.sim.nodes.back()->started_map,
              "Slow guest blocked the host or used an unloaded world");
        test.finish(count - 1);
        test.run(100);
        test.connected();
        test.sim.fresh(count);
        check(host.invite == invite, "Host map switch changed the share code");
        // Same-map reload must preserve admission without an unnecessary guest load.
        const auto previous_loads = test.loads;
        begin_host_world(host, test.city, test.sim.now);
        test.ready[0] = false;
        test.run(240);
        test.connected();
        check(test.loads == previous_loads, "Same-map host reload reloaded guests unnecessarily");
        test.ready[0] = true;
        test.run(100);
        test.sim.fresh(count);
        // Returning to A cannot revive packets from the first A world.
        begin_host_world(host, test.beach, test.sim.now);
        test.ready[0] = false;
        test.run(30);
        test.current[0] = test.beach;
        test.ready[0] = true;
        for (unsigned i = 1; i < count; ++i) test.finish(i);
        test.run(100);
        test.connected();
        test.sim.fresh(count);
        for (unsigned i = 1; i < count; ++i)
            host.transport.send(test.sim.nodes[i]->transport.status().local_id, stale_wire, false, false, TrafficLane::gameplay);
        test.run(2);
        test.sim.fresh(count);
        check(host.world == 4, "World generation did not advance on every host load");
    }
    {
        TravelSimulation test(2);
        auto &host = *test.sim.nodes[0];
        begin_host_world(host, {}, test.sim.now); // Native load outside the menu.
        test.ready[0] = false;
        test.run(240);
        test.connected();
        check(test.loads[1] == 0, "Unknown host destination queued an invalid native load");
        test.current[0] = test.city;
        test.ready[0] = true;
        test.run(30);
        test.finish(1);
        test.run(100);
        test.sim.fresh(2);
    }
    {
        TravelSimulation test(2);
        auto &host = *test.sim.nodes[0];
        begin_host_world(host, test.city, test.sim.now);
        test.ready[0] = false;
        test.run(30);
        // The native host load failed and returned to its old map.
        test.ready[0] = true;
        test.run(10);
        check(host.world == 3 && host.map == map_hash(test.beach), "Failed host load did not publish a rollback");
        test.current[1] = test.city; // Finish the superseded guest request first.
        test.ready[1] = true;
        test.run(20);
        check(test.queued[1] == test.beach && test.loads[1] == 2,
              "New destination failed to replace a guest's in-flight load");
        test.finish(1);
        test.run(100);
        test.connected();
        test.sim.fresh(2);
    }
    {
        TravelSimulation test(3);
        auto &host = *test.sim.nodes[0];
        begin_host_world(host, test.city, test.sim.now);
        test.ready[0] = false;
        test.run(30);
        stop(*test.sim.nodes[2], "Cancelled during loading");
        test.current[0] = test.city;
        test.ready[0] = true;
        test.finish(1);
        test.run(100);
        check(player_count(host) == 2 && player_count(*test.sim.nodes[1]) == 2 &&
                  test.sim.nodes[2]->mode == Mode::off, "One cancelled guest disrupted the remaining room");
        test.sim.fresh(2);
        auto spoof = packet(*test.sim.nodes[1], PacketKind::world_state, test.sim.now);
        spoof.world = host.world + 1;
        spoof.destination = test.beach;
        spoof.map = map_hash(test.beach);
        test.sim.nodes[1]->transport.send(host.host_id, encode_wire(spoof), true, false);
        test.run(2);
        check(host.mode == Mode::host && host.world == 2 && host.map == map_hash(test.city),
              "Guest was able to redirect the host to another map");
    }
    std::cout << "Host travel: 2/8 players, retained membership/password/code, staggered loading, "
                 "same-map reload, A-B-A stale packets, poses/board/audio/cosmetics, native-load fallback, "
                 "failed-load rollback, superseded loads, cancellation and host-only authority passed.\n";
}
void session_controls_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 4; ++i) sim.add();
    sim.run(80);
    auto &host = *sim.nodes[0];
    auto &guest = *sim.nodes[1];
    const auto roster = [&] {
        auto p = packet(host, PacketKind::roster, sim.now);
        p.capacity = host.capacity;
        p.object_placement = host.object_placement;
        p.members.push_back({p.source, host.epoch, "Host"});
        for (const auto &peer : host.peers) if (peer.handshaken) p.members.push_back(peer.member);
        // Everyone in one party led by the host, as a roster may list them.
        for (auto &m : p.members) {
            m.party = p.members.size() > 1 ? lobby_party : 0;
            m.party_leader = m.party && m.id == p.source;
        }
        return p;
    };
    const auto stale = roster();
    constexpr auto everyone = ObjectPlacement::everyone, host_only = ObjectPlacement::host_only;
    check(edit_object_placement(guest, "off").starts_with("Only the session host") &&
              guest.object_placement == everyone, "Guest changed object placement");
    (void)edit_object_placement(host, "off");
    sim.run(4);
    for (const auto &s : sim.nodes) check(s->object_placement == host_only, "Live placement lock was not delivered");
    check(object_placement_allowed(host.object_placement, true) &&
              !object_placement_allowed(guest.object_placement, false),
          "Host-only placement did not exempt the host alone");
    (void)edit_object_placement(host, "invalid");
    check(host.object_placement == host_only, "Invalid permission changed the policy");
    send_required(host, guest.transport.status().local_id, encode_wire(stale));
    sim.run(4);
    check(guest.object_placement == host_only, "Stale roster reenabled placement");
    sim.add(); sim.run(70);
    check(sim.nodes.back()->object_placement == host_only, "Late joiner missed the placement policy");
    auto bytes = encode(roster());
    check(decode(bytes) && decode(bytes)->object_placement == host_only, "Placement policy did not round trip");
    // Roster tail: tps, placement, forced layers, layer count (2 bytes), layer
    // modes, voice policy (5 bytes), voice range (2 bytes).
    const auto placement_at = bytes.size() - 2 - 5 - world_layers().size() - 2 - 2;
    check(!decode(std::span(bytes).first(placement_at)), "Missing permission byte accepted");
    bytes[placement_at] = 3;
    check(!decode(bytes), "Invalid permission byte accepted");
    auto forged = roster();
    forged.source = sim.nodes[2]->transport.status().local_id;
    forged.epoch = sim.nodes[2]->epoch;
    forged.object_placement = everyone;
    check(send_packet(*sim.nodes[2], guest.transport.status().local_id, forged, true, false),
          "Permission spoof fixture could not send");
    sim.run(3);
    check(guest.object_placement == host_only, "Guest spoof replaced host placement policy");
    (void)edit_object_placement(host, "on"); sim.run(4);
    for (const auto &s : sim.nodes) check(s->object_placement == everyone, "Placement enable was not delivered");
    check(edit_object_placement(guest, "next").starts_with("Only the session host") &&
              guest.object_placement == everyone, "Guest cycled object placement");
    for (const auto policy : {host_only, ObjectPlacement::nobody, everyone, host_only}) {
        (void)edit_object_placement(host, "next"); sim.run(4);
        for (const auto &s : sim.nodes)
            check(s->object_placement == policy, "Repeated placement cycle reused a stale value");
    }
    (void)edit_object_placement(host, "next");
    (void)edit_object_placement(host, "next");
    (void)edit_object_placement(host, "next");
    check(host.object_placement == host_only, "Back-to-back placement cycles did not use live state");
    (void)edit_object_placement(host, "on");
    const auto victim = sim.nodes.back()->transport.status().local_id;
    const auto epoch = sim.nodes.back()->epoch;
    const auto target = std::to_string(victim) + " " + std::to_string(epoch);
    check(kick_player(guest, target).starts_with("Only the session host"), "Guest could kick a player");
    (void)kick_player(host, std::to_string(host.host_id) + " " + std::to_string(host.epoch));
    check(host.mode == Mode::host, "Host kicked itself");
    (void)kick_player(host, std::to_string(victim) + " " + std::to_string(epoch + 1));
    check(find_peer(host, victim), "Stale kick removed a player with a different epoch");
    (void)edit_object_placement(host, "off"); sim.run(3);
    removed_object_owners.clear();
    check(kick_player(host, target).find("was kicked") != std::string::npos, "Host kick failed");
    sim.run(10);
    check(std::count(removed_object_owners.begin(), removed_object_owners.end(), std::pair{victim, epoch}) >= 4,
          "Kick did not clean the departing owner's objects on every remaining peer");
    check(sim.nodes.back()->mode == Mode::off && sim.nodes.back()->object_placement == everyone,
          "Kicked guest did not disconnect and restore offline placement");
    for (std::size_t i = 0; i + 1 < sim.nodes.size(); ++i) {
        const auto &s = *sim.nodes[i];
        check(!find_peer(*sim.nodes[i], victim), "Kicked player survived in the roster");
        check(std::none_of(s.transport.status().peers.begin(), s.transport.status().peers.end(),
                           [&](const auto &p) { return p.id == victim; }), "Kick retained a direct transport");
        check(s.mode != Mode::off && s.object_placement == host_only, "Kick changed another player's session");
    }
    publish(host);
    check(host.view.host_id == host.host_id && !host.view.local_name.empty() &&
              host.view.roster.front().epoch != 0, "Session view omitted player identity");
    std::cout << "Session controls: live policy, late join, malformed/stale/spoof rejection, host-only kick, epoch checks and peer cleanup passed.\n";
}
void guest_building_checks() {
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(80);
    auto &host = *sim.nodes[0];
    auto &builder = *sim.nodes[1];
    auto &viewer = *sim.nodes[2];
    const auto builder_id = builder.transport.status().local_id;
    const auto host_id = host.host_id;
    const auto seen = [&](Session &node, std::uint64_t owner) {
        return find_peer(node, owner)->objects.objects().size();
    };
    const auto place = [](Session &node, unsigned count) {
        std::vector<NetworkObject> layout;
        for (unsigned i = 0; i < count; ++i)
            layout.push_back({i + 1ULL, "own_bk_ramp", {static_cast<float>(i), 2, 5}, {0, 0, 0, 1}});
        node.local_objects.replace(layout);
    };
    place(builder, 1); sim.run(10);
    check(seen(viewer, builder_id) == 1, "Guest objects did not reach other guests while building was allowed");
    (void)edit_object_placement(host, "host"); sim.run(4);
    // A modified guest ignores its own placement lock and keeps publishing.
    place(builder, 3); sim.run(10);
    check(find_peer(host, builder_id)->objects.objects().size() == 3,
          "Host lost the guest's upload, so its revisions would desync on re-enable");
    check(seen(viewer, builder_id) == 1 && find_peer(host, builder_id)->shared.objects().size() == 1,
          "A guest bypassed disabled building and published objects");
    place(host, 2); sim.run(10);
    check(seen(viewer, host_id) == 2 && seen(builder, host_id) == 2, "Disabling guest building blocked the host");
    auto &late = sim.add(); sim.run(70);
    check(seen(late, builder_id) == 1, "A late joiner received objects placed while building was disabled");
    (void)edit_object_placement(host, "nobody"); sim.run(4);
    check(!simulated_placement_allowed, "Session-wide lock did not lock the host's own tools");
    place(builder, 5); sim.run(10);
    check(seen(viewer, builder_id) == 1, "A guest bypassed a session-wide placement lock");
    (void)edit_object_placement(host, "everyone"); sim.run(10);
    for (auto *node : {&viewer, &late})
        check(seen(*node, builder_id) == 5, "Re-enabling guest building did not publish the current layout");
    place(builder, 6); sim.run(10);
    check(seen(viewer, builder_id) == 6, "Guest edits stopped syncing after building was re-enabled");

    place(viewer, 2); sim.run(10);
    check(clear_guest_objects(viewer, {}).starts_with("Only the session host") && seen(builder, viewer.transport.status().local_id) == 2,
          "A guest deleted other players' objects");
    simulated_guest_wipes = 0;
    check(clear_guest_objects(host, {}) == "Deleted 8 guest objects.", "Host delete reported the wrong object count");
    sim.run(10);
    check(seen(viewer, builder_id) == 0 && seen(builder, viewer.transport.status().local_id) == 0 &&
              seen(late, builder_id) == 0, "Deleting guest objects did not remove them for everyone");
    check(seen(viewer, host_id) == 2, "Deleting guest objects removed the host's own objects");
    check(simulated_guest_wipes >= 3 && !builder.clear_pending && !viewer.clear_pending,
          "Guests were not told to delete their own session objects");
    // A modified guest ignores the wipe and re-sends (and edits) its old objects.
    std::vector<NetworkObject> stale;
    for (unsigned i = 0; i < 6; ++i) stale.push_back({i + 1ULL, "own_bk_ramp", {static_cast<float>(i), 9, 5}, {0, 0, 0, 1}});
    builder.local_objects.replace(stale); sim.run(10);
    check(seen(viewer, builder_id) == 0, "A guest restored objects the host deleted");
    stale.push_back({100, "own_bk_ramp", {40, 2, 5}, {0, 0, 0, 1}});
    builder.local_objects.replace(stale); sim.run(10);
    check(seen(viewer, builder_id) == 1, "New guest objects stopped syncing after a delete");
    const auto wipes = simulated_guest_wipes;
    auto &after = sim.add(); sim.run(70);
    check(simulated_guest_wipes == wipes && !after.clear_pending && after.object_clears == host.object_clears,
          "A player joining after a delete wiped their own objects");

    const auto late_id = late.transport.status().local_id;
    (void)kick_player(host, std::to_string(late_id) + " " + std::to_string(late.epoch));
    sim.run(6);
    check(late.mode == Mode::off, "Kicked guest stayed connected");
    // Rejoin with the same Steam identity and a fresh session epoch.
    late.mode = Mode::join;
    late.epoch += 1000;
    late.started_map = true;
    late.map = 42;
    late.host_id = host_id;
    late.password = password_key("test room", late.secret);
    late.transport.join(host_id);
    sim.run(60);
    check(late.mode == Mode::off && (!find_peer(host, late_id) || !find_peer(host, late_id)->handshaken),
          "A kicked player rejoined the session");
    check(player_count(host) == 4, "Rejecting a kicked player disturbed the other guests");
    stop(host, "Host restarted");
    check(host.banned.empty(), "Kick bans outlived the session");
    std::cout << "Guest building: host-enforced freeze, patched-guest rejection, host exemption, late join, re-enable, guest object deletion and "
                 "kick bans passed.\n";
}
void world_layer_sync_checks() {
    WorldLayersModel model;
    model.choices.front() = "on";
    const auto own = model.choices;
    auto host_choices = default_world_layers();
    host_choices.front() = "off";
    host_choices.back() = "on";
    WorldLayerOverride override;
    check(override.apply(model, true, host_choices) && model.controlled_by_host &&
              model.choices == host_choices, "Host layer override did not apply");
    host_choices.front() = "default";
    check(override.apply(model, true, host_choices) && override.local == own,
          "Repeated host snapshot replaced local layer preferences");
    auto invalid = host_choices;
    invalid.back() = "invalid";
    check(!override.apply(model, true, invalid) && model.choices == host_choices,
          "Invalid layer override partially changed the world");
    model.map = WorldMap::none; model.ready = false;
    check(override.apply(model, false, {}) && !model.controlled_by_host && model.choices == own &&
              !override.local, "Disabling sync during travel did not restore local choices");
    check(unpack_world_layers(pack_world_layers(host_choices)) == host_choices,
          "World layer modes did not round trip");
    Simulation sim;
    for (unsigned i = 0; i < 3; ++i) sim.add();
    sim.run(80);
    auto &host = *sim.nodes[0]; auto &guest = *sim.nodes[1];
    const auto roster = [&] {
        auto p = packet(host, PacketKind::roster, sim.now);
        p.capacity = host.capacity; p.force_world_layers = host.force_world_layers;
        p.layers = pack_world_layers(host.layers);
        p.members.push_back({p.source, host.epoch, "Host"});
        for (const auto &peer : host.peers) if (peer.handshaken) p.members.push_back(peer.member);
        // Everyone in one party led by the host, as a roster may list them.
        for (auto &m : p.members) {
            m.party = p.members.size() > 1 ? lobby_party : 0;
            m.party_leader = m.party && m.id == p.source;
        }
        return p;
    };
    const auto stale = roster();
    simulated_host_layers.choices = host_choices;
    check(edit_world_layer_sync(guest, "on").starts_with("Only the session host") && !guest.force_world_layers,
          "Guest could force world layer sync");
    (void)edit_world_layer_sync(host, "on"); sim.run(5);
    check(guest.force_world_layers && guest.layers == host_choices, "Host layers did not synchronize");
    (void)edit_world_layer_sync(host, "invalid");
    check(host.force_world_layers, "Invalid toggle changed world layer policy");
    send_required(host, guest.transport.status().local_id, encode_wire(stale)); sim.run(4);
    check(guest.force_world_layers && guest.layers == host_choices, "Stale roster changed layer policy");
    simulated_host_layers.choices.front() = "off";
    sim.run(4);
    check(guest.layers == simulated_host_layers.choices, "Live layer changes did not propagate");
    sim.add(); sim.run(70);
    check(sim.nodes.back()->force_world_layers && sim.nodes.back()->layers == host.layers,
          "Late joiner missed forced world layers");
    auto bytes = encode(roster());
    check(decode(bytes) && decode(bytes)->force_world_layers &&
              unpack_world_layers(decode(bytes)->layers) == host.layers, "Roster lost world layer settings");
    // The voice policy (allowed byte, 4-byte revision), voice range (2 bytes) and the guest
    // noclip / No Bail byte follow the layer modes.
    const auto layers_end = bytes.size() - 5 - 2 - 1;
    for (std::size_t n = layers_end - world_layers().size() - 1; n < bytes.size(); ++n)
        check(!decode(std::span(bytes).first(n)), "Truncated world layer settings accepted");
    auto malformed = bytes; malformed[layers_end - 1] = 3;
    check(!decode(malformed), "Invalid layer mode accepted");
    malformed = bytes; malformed[layers_end - world_layers().size() - 1] = 2;
    check(!decode(malformed), "Invalid force-sync flag accepted");
    auto forged = roster();
    forged.source = sim.nodes[2]->transport.status().local_id; forged.epoch = sim.nodes[2]->epoch;
    forged.force_world_layers = false;
    check(send_packet(*sim.nodes[2], guest.transport.status().local_id, forged, true, false),
          "World layer spoof fixture failed to send");
    sim.run(4);
    check(guest.force_world_layers, "Guest disabled host's forced world layers");
    (void)edit_world_layer_sync(host, "off"); sim.run(5);
    for (const auto &s : sim.nodes) check(!s->force_world_layers, "World layer unlock did not propagate");
    check(edit_world_layer_sync(guest, "toggle").starts_with("Only the session host") &&
              !guest.force_world_layers, "Guest toggled world layer sync");
    for (const bool forced : {true, false, true, false}) {
        (void)edit_world_layer_sync(host, "toggle"); sim.run(5);
        for (const auto &s : sim.nodes)
            check(s->force_world_layers == forced, "Repeated world-layer toggle reused a stale value");
    }
    (void)edit_world_layer_sync(host, "toggle");
    (void)edit_world_layer_sync(host, "toggle");
    check(!host.force_world_layers, "Back-to-back world-layer toggles did not use live state");
    (void)edit_world_layer_sync(host, "on"); sim.run(4);
    stop(guest, "Leave layer session");
    check(!guest.force_world_layers, "Disconnected guest kept layer lock");
    TravelSimulation travel(2);
    auto &travelling_host = *travel.sim.nodes[0];
    (void)edit_object_placement(travelling_host, "off");
    (void)edit_world_layer_sync(travelling_host, "on"); travel.run(4);
    begin_host_world(travelling_host, travel.city, travel.sim.now);
    travel.ready[0] = false; travel.run(5);
    simulated_host_layers.choices.back() = "off";
    travel.current[0] = travel.city; travel.ready[0] = true; travel.run(8);
    travel.finish(1); travel.run(12);
    travel.connected();
    check(travel.sim.nodes[1]->object_placement == ObjectPlacement::host_only && travel.sim.nodes[1]->force_world_layers &&
              travel.sim.nodes[1]->layers == simulated_host_layers.choices,
          "Map travel lost host controls or world layer changes");
    simulated_host_layers = {};
    std::cout << "World layers: live host sync, local preference restoration, late joins, stale/spoof rejection and bounded modes passed.\n";
}
void distance_settings_checks() {
    Simulation sim;
    sim.positions = {0, 25, 100, 250, 500, 750, 1000, 1250};
    for (unsigned i = 0; i < 7; ++i) sim.add();
    sim.run(80);
    auto &host = *sim.nodes[0];
    const auto host_roster = [&] {
        auto p = packet(host, PacketKind::roster, sim.now);
        p.distances = host.distances; p.capacity = host.capacity;
        p.members.push_back({p.source, host.epoch, "Host"});
        for (const auto &peer : host.peers) if (peer.handshaken) p.members.push_back(peer.member);
        // Everyone in one party led by the host, as a roster may list them.
        for (auto &m : p.members) {
            m.party = p.members.size() > 1 ? lobby_party : 0;
            m.party_leader = m.party && m.id == p.source;
        }
        return p;
    };
    auto stale = host_roster();
    const auto message = edit_distances(host, "15 20 70 80");
    const MultiplayerDistances custom{15, 20, 70, 80};
    check(message.starts_with("TPS distances applied") && host.distances == custom, "Host edit did not apply");
    sim.run(4);
    for (const auto &s : sim.nodes) check(s->distances == custom, "Host edit did not synchronize to a guest");
    auto *medium = find_peer(*sim.nodes[1], host.host_id);
    auto *distant = find_peer(*sim.nodes[2], host.host_id);
    const auto before_medium = medium->pose_count, before_distant = distant->pose_count;
    sim.run(40);
    check(medium->pose_count - before_medium >= 19 && medium->pose_count - before_medium <= 21 &&
          distant->pose_count - before_distant >= 9 && distant->pose_count - before_distant <= 11,
          "New policy did not change actual host delivery rates");
    const auto nearby_id = sim.nodes[2]->transport.status().local_id;
    auto *guest_stream = find_peer(*sim.nodes[1], nearby_id);
    check(guest_stream->received_pose_interval == 100000, "New policy did not change guest-to-guest delivery");
    for (const auto *invalid : {"", "10 20 30", "10 20 30 40 extra", "-1 20 30 40", "20 20 30 40",
                               "10 40 30 50", "10 20 40 40", "10 20 30 10001", "0 2147483648 30 40", "1.5 20 30 40"}) {
        (void)edit_distances(host, invalid);
        check(host.distances == custom, "Invalid host edit changed the policy");
    }
    check(edit_distances(*sim.nodes[1], "1 2 3 4").starts_with("Only the lobby host") &&
          sim.nodes[1]->distances == custom, "Guest could edit its lobby policy");
    send_required(host, sim.nodes[1]->transport.status().local_id, encode_wire(stale));
    sim.run(4);
    check(sim.nodes[1]->distances == custom, "Old reliable roster rolled back the policy");
    sim.add(); sim.run(70);
    check(sim.nodes.back()->distances == custom, "Late joiner missed host policy");
    const auto fresh = host_roster();
    const auto bytes = encode(fresh);
    const auto decoded = decode(bytes);
    check(decoded && decoded->distances == custom, "Distance policy lost values in the codec");
    for (std::size_t n = bytes.size() - 8; n < bytes.size(); ++n)
        check(!decode(std::span(bytes).first(n)), "Truncated roster policy accepted");
    // The tick rate sits before placement, the forced flag, the layers and the voice policy.
    auto malformed = bytes; malformed[bytes.size() - 5 - world_layers().size() - 3] = 255;
    check(!decode(malformed), "Out-of-range wire policy accepted");
    auto invalid_roster = fresh; invalid_roster.distances = {20, 10, 30, 40};
    bool rejected{}; try { (void)encode(invalid_roster); } catch (...) { rejected = true; }
    check(rejected, "Encoder accepted invalid distance order");
    // An admitted direct peer still cannot publish the host's policy.
    auto forged = fresh;
    forged.source = sim.nodes[2]->transport.status().local_id; forged.epoch = sim.nodes[2]->epoch;
    forged.distances = {1, 2, 3, 4};
    check(send_packet(*sim.nodes[2], sim.nodes[1]->transport.status().local_id, forged, true, false),
          "Spoof fixture did not reach a direct path");
    sim.run(3);
    check(sim.nodes[1]->mode == Mode::join && sim.nodes[1]->distances == custom,
          "A direct guest replaced host distance settings");
    // Increasing the radii must immediately clear previous low-rate hysteresis.
    (void)edit_distances(host, "2000 2100 3000 3100"); sim.run(6);
    for (const auto &s : sim.nodes) check(s->distances == host.distances, "Larger distances did not propagate");
    check(find_peer(*sim.nodes[1], host.host_id)->received_pose_interval == 50000,
          "Increased distances retained the previous slow band");
    (void)edit_distances(host, "50 60 150 170"); sim.run(5);
    check(host.distances == MultiplayerDistances{} && sim.nodes.back()->distances == MultiplayerDistances{},
          "Default values did not restore across the lobby");
    // Settings remain part of the room across native map teardown and late loads.
    TravelSimulation travel(2);
    auto &owner = *travel.sim.nodes[0];
    (void)edit_distances(owner, "15 20 70 80"); travel.run(4);
    begin_host_world(owner, travel.city, travel.sim.now);
    travel.ready[0] = false; travel.run(5);
    (void)edit_distances(owner, "30 40 90 110"); // Applied when gameplay is ready again.
    travel.current[0] = travel.city; travel.ready[0] = true; travel.run(8);
    travel.finish(1); travel.run(12);
    travel.connected();
    check(owner.distances == MultiplayerDistances{30, 40, 90, 110} &&
          travel.sim.nodes[1]->distances == owner.distances, "Map travel lost updated host settings");
    stop(*travel.sim.nodes[1], "Disconnect settings check");
    check(travel.sim.nodes[1]->distances == MultiplayerDistances{}, "Disconnected guest retained the old lobby policy");
    std::cout << "Host distance settings: eight-player sync, live host/direct rates, late joins, reset, stale/spoof rejection, invalid inputs, and map travel passed.\n";
}
void distance_mesh_checks() {
    Simulation sim;
    sim.positions = {0, 250, 251, 500, 750, 1000, 1250, 1500};
    for (unsigned i = 0; i < 8; ++i) sim.add();
    sim.run(90);
    sim.fresh(8);
    const auto measure = [&] {
        std::array<std::array<std::uint64_t, 8>, 8> counts{};
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned j = 0; j < 8; ++j)
                if (i != j) counts[i][j] = find_peer(*sim.nodes[i], sim.nodes[j]->transport.status().local_id)->pose_count;
        sim.run(80);
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned j = 0; j < 8; ++j) {
                if (i == j) continue;
                auto *peer = find_peer(*sim.nodes[i], sim.nodes[j]->transport.status().local_id);
                const bool nearby = i == 0 || std::abs(sim.positions[i] - sim.positions[j]) < 50;
                const auto received = peer->pose_count - counts[i][j];
                check(received >= (nearby ? 78U : 18U) && received <= (nearby ? 82U : 22U),
                      "Distance policy rate differs on direct/forwarded/upstream path");
            }
        sim.fresh(8);
    };
    measure();
    const auto a = sim.nodes[1]->transport.status().local_id, b = sim.nodes[2]->transport.status().local_id;
    SimulatedNetwork::blocked.insert(SimulatedNetwork::pair(a, b));
    sim.nodes[1]->transport.disconnect(b, "Force nearby relay with distant host");
    sim.run(80);
    measure();
    SimulatedNetwork::blocked.clear();
    sim.positions[2] = 350; // The nearby pair moves to the 10 TPS band.
    sim.run(90);
    auto *peer = find_peer(*sim.nodes[1], b); const auto before = peer->pose_count;
    sim.run(80);
    check(peer->pose_count - before >= 38 && peer->pose_count - before <= 42, "Middle band is not 10 TPS");
    sim.positions[2] = 251;
    sim.run(90);
    measure();
    SimulatedNetwork::lose_unreliable = true;
    sim.run(80);
    sim.fresh(8);
    SimulatedNetwork::lose_unreliable = false;
    std::cout << "Eight-player distance delivery: 20/10/5 TPS, full upstream, nearby relay through a distant host, approach recovery and loss passed.\n";
}
void mesh_checks() {
    Packet report;
    report.kind = PacketKind::routes;
    report.session = 1;
    report.epoch = 2;
    report.map = 42;
    report.members = {{76561198000000321ULL, 9, {}}};
    const auto encoded = encode(report);
    check(decode(encoded)->members == report.members, "Route report codec changed membership");
    for (std::size_t n = 0; n < encoded.size(); ++n)
        check(!decode(std::span(encoded.data(), n)), "Truncated route report accepted");
    auto duplicate = report;
    duplicate.members.push_back(report.members.front());
    check(!valid_routes(duplicate.members), "Duplicate route identity accepted");
    Simulation sim;
    for (unsigned i = 0; i < 5; ++i) {
        sim.add();
        sim.run(70);
        sim.fresh(i + 1);
    }
    auto &host = *sim.nodes.front();
    auto bytes = host.transport.status().sent_bytes;
    sim.run(40);
    sim.fresh(5);
    const auto direct_bytes = host.transport.status().sent_bytes - bytes;
    for (std::size_t i = 1; i < 5; ++i)
        for (std::size_t j = 1; j < 5; ++j)
            if (i != j)
                check(find_peer(*sim.nodes[i], sim.nodes[j]->transport.status().local_id)->direct_ready,
                      "Guest pair failed to establish direct Steam route");
    // Expiring reports must restore every relay stream even while physical links remain up.
    SimulatedNetwork::lose_reports = true;
    sim.run(40);
    bytes = host.transport.status().sent_bytes;
    sim.run(40);
    const auto relay_bytes = host.transport.status().sent_bytes - bytes;
    sim.fresh(5);
    check(direct_bytes * 2 < relay_bytes, "Host upload did not fall substantially with direct routes");
    SimulatedNetwork::lose_reports = false;
    std::cout << "Five players: host bytes over two seconds, direct=" << direct_bytes
              << ", forced relay=" << relay_bytes << "\n";
    const auto a = sim.nodes[1]->transport.status().local_id, b = sim.nodes[2]->transport.status().local_id;
    SimulatedNetwork::blocked.insert(SimulatedNetwork::pair(a, b));
    sim.nodes[1]->transport.disconnect(b, "Simulated route loss");
    sim.run(50);
    sim.fresh(5);
    check(!find_peer(*sim.nodes[1], b)->direct_ready, "Broken direct route still active");
    const auto *destination = find_peer(host, a);
    const auto *source = find_peer(host, b);
    check(needs_relay(destination->direct_routes, destination->route_reported, source->member, sim.now),
          "Lost route did not restore host forwarding");
    SimulatedNetwork::blocked.clear();
    sim.run(90);
    sim.fresh(5);
    check(find_peer(*sim.nodes[1], b)->direct_ready, "Direct path did not recover");
    SimulatedNetwork::lose_unreliable = true;
    sim.run(100);
    sim.fresh(5);
    SimulatedNetwork::lose_unreliable = false;
    // Late joins up to the supported maximum.
    for (unsigned i = 5; i < 8; ++i) {
        sim.add();
        sim.run(70);
        sim.fresh(i + 1);
    }
    // A direct guest cannot impersonate the host or another roster member.
    auto forged = packet(*sim.nodes[2], PacketKind::pose, sim.now);
    forged.source = host.host_id;
    send_packet(*sim.nodes[2], sim.nodes[1]->transport.status().local_id, forged, false, false);
    sim.run(70);
    sim.fresh(8);
    check(sim.nodes[1]->mode == Mode::join, "Invalid direct packet ended the host session");
    // A limited-upload guest reduces optional fan-out; receivers restore relay.
    const auto limited_id = sim.nodes[1]->transport.status().local_id;
    SimulatedNetwork::queues[limited_id] = 90000;
    sim.run(180);
    sim.fresh(8);
    check(sim.nodes[1]->direct_upload.limit == 0, "Congested guest did not reduce direct uploads");
    const auto *limited = find_peer(host, limited_id);
    for (unsigned i = 2; i < 8; ++i) {
        const auto *target = find_peer(host, sim.nodes[i]->transport.status().local_id);
        check(needs_relay(target->direct_routes, target->route_reported, limited->member, sim.now),
              "Paused direct uploads did not restore relay");
    }
    SimulatedNetwork::queues.clear();
    sim.run(900);
    sim.fresh(8);
    check(sim.nodes[1]->direct_upload.limit >= 6 && sim.nodes[1]->direct_upload.limit < max_remote_players - 1,
          "Direct uploads did not recover gradually");
    // A guest reconnect uses a new epoch; old direct routes cannot suppress its data.
    auto &guest = *sim.nodes[3];
    const auto id = guest.transport.status().local_id;
    stop(guest, "Test departure");
    sim.run(10);
    check(!find_peer(host, id), "Departed player retained by host");
    guest.mode = Mode::join;
    guest.started_map = true; // The simulation reconnects while its local map remains loaded.
    guest.secret = 73;
    guest.epoch = 900;
    guest.map = 42;
    guest.host_id = host.host_id;
    guest.password = password_key("test room", 73);
    guest.transport.join(host.host_id);
    auto cosmetics = packet(guest, PacketKind::cosmetics, sim.now);
    cosmetics.appearance = {{skater_recipe_key, 2, {}, {{1, "Rejoined outfit", {}}}},
                            {board_recipe_key, 1, {}, {{2, "Deck", {}}}}};
    guest.cosmetic_packet = encode_wire(cosmetics);
    sim.run(100);
    sim.fresh(8);
    // Password admission is still exclusively controlled by the host.
    stop(*sim.nodes.back(), "Make room");
    sim.run(10);
    auto &wrong = sim.add(false);
    sim.run(30);
    check(wrong.mode == Mode::off, "Wrong password entered mesh");
    SteamTransport outsider;
    outsider.open();
    const Member victim{sim.nodes[1]->transport.status().local_id, 101, {}};
    outsider.allow_peers(std::span(&victim, 1));
    check(!outsider.connect_peer(victim.id), "Unknown identity connected to an admitted guest");
    stop(host, "Host departure");
    sim.run(5);
    for (const auto &s : sim.nodes)
        check(s->mode == Mode::off, "Guest remained after host departure");
}
} // namespace
} // namespace dingosdk::multiplayer
int main(int argc, char **argv) {
    try {
        // A small stand-in for the catalog read from the level data at startup.
        dingosdk::install_world_layer_catalog({
            {{"Levels/Game/BAM_LevelRoot/Root", dingosdk::WorldMap::bam, -1, true},
             {"Levels/Game/BAM_LevelRoot/TOD_1_Morning", dingosdk::WorldMap::bam, 0, false},
             {"Levels/Game/BAM_LevelRoot/Season_01", dingosdk::WorldMap::bam, 0, false}},
            {0},
            {{"bam_root", "Root", "", dingosdk::WorldMap::bam, 0, 0, "World"},
             {"bam_tod_1_morning", "TOD 1 Morning", "", dingosdk::WorldMap::bam, 1, 1, "Lighting"},
             {"bam_season_01", "Season 01", "", dingosdk::WorldMap::bam, 2, 2, "Seasonal"}}});
        dingosdk::simulated_host_layers = {};
        dingosdk::multiplayer::join_tick_checks();
        dingosdk::multiplayer::tick_settings_checks();
        if (argc == 2 && std::string_view(argv[1]) == "--objects-only") {
            dingosdk::multiplayer::object_sync_checks(); return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--session-controls-only") {
            dingosdk::multiplayer::session_controls_checks();
            dingosdk::multiplayer::guest_building_checks();
            dingosdk::multiplayer::world_layer_sync_checks(); return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--join-tick-only") return 0;
        if (argc == 2 && std::string_view(argv[1]) == "--pacing-only") {
            dingosdk::multiplayer::pacing_checks();
            dingosdk::multiplayer::client_tick_checks(); return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--priorities-only") {
            dingosdk::multiplayer::priority_mesh_checks();
            dingosdk::multiplayer::map_checks();
            dingosdk::multiplayer::travel_checks();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--distance-settings-only") {
            dingosdk::multiplayer::distance_settings_checks(); return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--bandwidth-only") {
            dingosdk::multiplayer::distance_mesh_checks(); return 0;
        }
        dingosdk::multiplayer::map_checks();
        if (argc == 2 && std::string_view(argv[1]) == "--map-only") return 0;
        dingosdk::multiplayer::travel_checks();
        if (argc == 2 && std::string_view(argv[1]) == "--travel-only") return 0;
        dingosdk::multiplayer::mesh_checks();
        dingosdk::multiplayer::throwdown_routing_checks();
        dingosdk::multiplayer::party_checks();
        dingosdk::multiplayer::role_checks();
        dingosdk::multiplayer::global_ban_checks();
        dingosdk::multiplayer::physics_extras_checks();
        dingosdk::multiplayer::stall_checks();
        dingosdk::multiplayer::scoring_checks();
        dingosdk::multiplayer::object_sync_checks();
        dingosdk::multiplayer::session_controls_checks();
        dingosdk::multiplayer::guest_building_checks();
        dingosdk::multiplayer::world_layer_sync_checks();
        std::cout << "Mesh admission, 2-8 players, relay fallback, route loss, packet loss, reconnect and "
                     "password checks passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
