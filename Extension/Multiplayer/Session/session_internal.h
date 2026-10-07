#pragma once
#include "Engine/Game/Multiplayer/chat_rate.h"
#include "session.h"
#include "Extension/Customization/developer_hoodie.h"
#include "Extension/Customization/developer_board.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Voice/voice_chat.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Multiplayer/Steam/steam_lobbies.h"
#include "Extension/Multiplayer/Steam/steam_server_browser.h"
#include "room.h"
#include "party_book.h"
#include "Extension/Multiplayer/Net/delta_codec.h"
#include "password.h"
#include "client_timing.h"
#include "monotonic_clock.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Session state shared by session.cpp and the session_*.cpp files.
namespace dingosdk::multiplayer::session_detail {
enum class Mode { off, host, join, echo };
// Flood control for throwdown messages (chat has ChatRate, Engine/Game/Multiplayer/chat_rate.h).
struct ChatBudget {
    std::uint64_t since{};
    unsigned messages{};
    bool accept(std::uint64_t now, unsigned burst = 6) noexcept {
        if (now < since || now - since >= 5000000) { since = now; messages = 0; }
        return ++messages <= burst;
    }
};
// Throwdown messages per sender per 5 s: offers, joins, starts and ~4 scores a second.
inline constexpr unsigned throwdown_burst = 60;
// The party the echo test shows its mirrored player in (a real lobby has none until its
// players form one).
inline constexpr std::uint32_t lobby_party = 1;
struct Peer {
    // Guests: this owner's layout as relayed by the host. Host: the owner's own
    // upload, which is never forwarded directly. The host publishes `shared`
    // instead, under its own revisions, so it alone decides what others see.
    ObjectState objects, shared;
    std::uint64_t shared_from{};
    // Host: object IDs removed by "delete all guest objects". Re-uploads of
    // them stay hidden; newly placed objects get fresh IDs and sync normally.
    std::set<std::uint64_t> cleared;
    struct ObjectDelivery {
        std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sent;
        std::vector<ObjectChunk> chunks;
        std::uint64_t source{}, epoch{};
        std::size_t next{}, cursor{};
    } object_delivery;
    struct PendingCosmetics { Packet packet; std::uint64_t received{}; };
    std::vector<PendingCosmetics> pending_cosmetics;
    Member member;
    DeltaSender sender;
    DeltaReceiver receiver;
    std::uint64_t password_challenge{};
    bool handshaken{}, render_failed{}, visible{}, direct_ready{}, map_authorized{};
    std::uint64_t last_map_offer{};
    bool world_ready = true;
    std::uint64_t travel_since{};
    std::uint32_t ready_sequence{};
    std::uint64_t next_dial{}, last_direct_hello{}, last_direct_pose{}, route_reported{};
    std::uint32_t route_sequence{};
    std::vector<Member> direct_routes;
    std::uint64_t pose_arrival{}, pose_count{}, rate_at{}, rate_count{};
    float pose_hz{};
    std::uint64_t native_rate_count{}, board_rate_count{};
    float native_pose_hz{}, native_board_hz{};
    NativeAnimationStats animation_rate_base;
    float native_animation_hz{}, native_animation_ms{}, pose_apply_ms{};
    std::uint32_t received_pose_interval = 50000;
    std::optional<Transform> latest_root;
    bool player_collision{}; // the player has the game's party collision on (from their poses)
    // One entry per pose source relayed or sent to this recipient (at most max_players).
    std::vector<PoseDelivery> pose_delivery;
    std::uint64_t connected_at{}, last_packet{}, last_cosmetic_apply{};
    // A skater removed for lack of poses is not spawned again before this: a spawn is a heavy
    // native pass, and a player's poses may stop and start again and again.
    std::uint64_t next_spawn{};
    ReceiveBudget budget;
    PoseBuffer poses;
    Pose render_pose;
    // Far from the local skater and the camera, render_pose is sampled every far_interval
    // (0: every frame) and the skater keeps it in between.
    std::uint64_t far_interval{}, next_far_sample{};
    AppearanceBuffer appearance;
    // Bumped per accepted outfit; render compares it with the one its actor wears
    // (0 after a spawn) to spread native recipe applies over frames.
    std::uint64_t cosmetic_revision{}, applied_cosmetics{};
    DeveloperHoodieState developer_hoodie;
    DeveloperBoardState developer_board;
    AudioBuffer audio;
    std::uint32_t voice_sequence{};
    bool received_voice{};
    VoiceBudget voice_budget;
    OutfitBudget outfit_budget;
    SoundBudget sound_budget;
    ChatRate chat_rate;
    // Chat proofs (session_receive.cpp): the copies of their lines this player sent us over
    // their own Steam connection, and the lines the host passed on as theirs that wait for one.
    struct ChatProof {
        std::uint32_t sequence{};
        std::string text;
        std::uint64_t at{};
    };
    std::vector<ChatProof> chat_proofs, chat_waiting;
    ChatBudget throwdown_budget, party_budget;
    // Host: how the guest's mods change trick scoring, as they reported it (Engine/Vfs/mod_scoring.h):
    // nothing until the report arrives, 0 for the game's own. member.scoring is the verdict.
    std::optional<std::uint64_t> scoring;
    std::string scoring_mods;
    ChatBudget scoring_budget;
    std::optional<AudioState> presented_audio;
    std::uint64_t next_audio_update{}, next_ui_update{};
    bool ui_visible{};
    std::vector<std::uint8_t> cosmetic_packet;
    std::string native_status, cosmetic_status;
};
struct PrivateRequest {
    std::string action, argument, password;
    std::uint64_t queued{};
    ~PrivateRequest() { erase_password(password); }
};
struct Session {
    VoiceChat voice;
    VoiceSettings voice_settings;
    VoicePolicy voice_policy;
    unsigned tps = multiplayer_default_tps;
    ObjectState local_objects;
    std::uint64_t next_object_update{};
    ClientTiming client_timing;
    std::uint64_t next_publish{}, last_client_log{}, next_party_update{};
    MultiplayerDistances distances;
    ObjectPlacement object_placement = ObjectPlacement::everyone;
    // What guests may use: the host's choice, or the host's roster for a guest.
    bool guest_noclip = true, guest_no_bail = true, guest_boosts = true;
    // Host: guests skate with its physics tuning. Guest: the host's roster says so (a
    // dedicated server's guests skate with the game's own).
    bool enforce_tuning = true;
    // Host: players whose mods change trick scoring (itself included) are kept out of linked
    // throwdowns and coop challenges. Guest: the report last sent to the host. Both: whether the
    // roster flags the local player.
    bool score_check = true;
    std::optional<std::pair<std::uint64_t, std::string>> scoring_sent;
    bool local_scoring{};
    std::uint64_t next_scoring_check{};
    // The local skater has the game's party collision on; sent with its poses.
    bool local_player_collision{};
    // Guest: the host's physics tuning differences, once it sent them.
    std::optional<std::vector<std::uint8_t>> host_tuning;
    // Host: the differences last sent, the packet that carried them (for players who join
    // later) and when to look at its tuning again.
    std::optional<std::vector<std::uint8_t>> sent_tuning;
    std::vector<std::uint8_t> tuning_packet;
    std::uint64_t next_tuning_check{};
    // The physics the tuning does not carry (Engine/Game/Multiplayer/session_physics.h).
    // Guest: the host's, once it sent them. Host: which of its own were last sent, the packet
    // that carried them (for players who join later) and when to look again.
    std::optional<std::vector<std::uint8_t>> host_extras;
    std::uint64_t sent_extras{};
    std::vector<std::uint8_t> extras_packet;
    std::uint64_t next_extras_check{};
    std::uint8_t server_votes{}; // guest of a dedicated server: the votes it runs
    // Host: bumped per "delete all guest objects". Guest: the last value seen
    // (unset until the first roster) and whether a local wipe is outstanding.
    std::optional<std::uint32_t> object_clears;
    bool clear_pending{};
    bool force_world_layers{};
    // Players the host kicked. They cannot reconnect until the session ends.
    std::set<std::uint64_t> banned;
    // Host: Steam IDs whose attempts to join keep failing wait longer each time (room.h).
    JoinBackoff join_backoff;
    // Players banned for good (every session this PC hosts), from the local profile.
    std::vector<MultiplayerBan> bans;
    bool bans_loaded{};
    // Sorted IDs of `bans` for the per-frame connection check; rebuilt when marked dirty.
    std::vector<std::uint64_t> ban_ids;
    bool ban_ids_dirty = true;
    WorldLayerChoices layers{default_world_layers()};
    ParkChoices parks;
    std::uint64_t next_park_update{};
    std::optional<PasswordKey> password;
    std::string lobby_password, lobby_name;
    std::mutex request_mutex;
    std::deque<std::unique_ptr<PrivateRequest>> requests;
    SteamTransport transport;
    SteamLobbies lobbies{make_steam_lobby_api()};
    SteamServerBrowser servers;
    // Guest of a dedicated server: whether the roster lists us as an admin,
    // and the server's voice range from it.
    bool server_admin{};
    float roster_voice_range = default_voice_range;
    // The server's ban list, as sent to us while we are one of its admins.
    std::vector<MultiplayerBan> server_bans;
    std::uint32_t server_ban_total{};
    std::vector<std::string> server_maps; // level assets the dedicated server allows
    std::vector<std::string> server_map_pool; // admins: the pool's assets in rotation order (empty: every map)
    unsigned server_map_rotation{};           // minutes per map (0: off)
    std::string map_label;                    // the host's name for join_destination (may be empty)
    std::string leave_notice;                 // take_leave_notice()
    std::uint64_t joined_public_lobby{};
    std::array<Peer, max_remote_players> peers;
    // Players take the lowest free slots, so every one sits below this mark
    // (note_slot raises it, trim_slots lowers it). Loops stop here, not at max_players.
    std::size_t used_slots{};
    bool public_host{}, gameplay_ready{}, started_map{}, roster_dirty{};
    bool awaiting_map{}, join_map_authorized{}, map_load_submitted{};
    std::uint64_t join_started{}, last_map_request{}, last_map_load_check{};
    std::string join_destination;
    std::uint64_t world = 1, travel_started{}, last_world_state{}, last_world_ready{};
    std::uint32_t world_state_sequence{};
    bool travelling{}, host_world_ready = true;
    unsigned capacity = max_players;
    // Local display preferences, loaded once from the profile. Never sent to peers.
    bool nametags = true, chat_visible = true, display_preferences_loaded{};
    bool custom_nametags = true; // ReSkate's nametags instead of the game's (Hud/custom_nametags.h)
    bool chat_filter = true;     // bad words in chat show as **** (Engine/Core/Text/word_filter.h)
    // Chat bubbles above each skater's head (Hud/custom_nametags.h, nametag_overlay.cpp).
    bool chat_bubbles = true, chat_bubbles_own{};
    float chat_bubbles_distance = 40.f, chat_bubbles_duration = 5.f;
    int chat_bubbles_history = 3;
    bool game_menu{};            // a game menu is up or the game's UI is hidden: no chat on screen
    bool hooks_prepared{};       // the remote-player hooks were installed (once per process)
    // Host settings remembered between sessions and game restarts.
    struct HostPreferences {
        bool loaded{}, public_lobby{true}, password_required{}, world_layer_sync{};
        unsigned capacity = max_players, tps = multiplayer_default_tps;
        std::string lobby_name;
        MultiplayerDistances distances;
        ObjectPlacement placement = ObjectPlacement::everyone;
        float voice_range = default_voice_range;
        bool guest_noclip = true, guest_no_bail = true, guest_boosts = true;
        bool enforce_tuning = true;
        bool score_check = true;
    } host_preferences;
    // Host: how far proximity voice is forwarded at all (listeners fade it by their own distance).
    float voice_range = default_voice_range;
    DirectUploadBudget direct_upload;
    Mode mode = Mode::off;
    std::uintptr_t base{}, context{}, parent{};
    std::uint64_t secret{}, epoch{}, map{}, host_id{}, next_send{}, last_hello{}, last_roster{},
        last_cosmetic_capture{}, last_routes{}, last_network_log{}, network_now{};
    std::uint32_t sequence{}, roster_sequence{};
    std::uint64_t local_pose_count{}, logged_local_pose_count{};
    std::size_t last_skater_bones{}, last_board_bones{};
    std::optional<Appearance> sent_appearance;
    std::optional<Transform> local_root;
    std::vector<std::uint8_t> cosmetic_packet;
    std::string available_map, cosmetic_capture_status, map_name, invite, status = "Multiplayer is off.",
                                                                          native_status;
    MultiplayerModel view;
    // Text chat: the log lives on the client thread; chat_view is its copy for
    // the overlay, guarded by `mutex` like `view`.
    std::deque<MultiplayerChatLine> chat;
    std::uint64_t chat_sequence{};
    ChatRate local_chat_rate;
    MultiplayerChat chat_view;
    std::optional<std::uint64_t> chat_signature; // of everything chat_view shows (publish_chat)
    // Masked names and text of the lines in `chat`, by sequence, while the filter is on.
    std::map<std::uint64_t, std::pair<std::string, std::string>> chat_masked;
    // Steam friends as sorted IDs, from the social snapshot revision they were read from.
    std::optional<std::uint64_t> friend_revision;
    std::vector<std::uint64_t> friend_ids;
    // What the throwdown relay reads each tick, kept between ticks: its player list is
    // rebuilt when the admitted players change, and names are refreshed once a second.
    ThrowdownRelayInput throwdown_input;
    std::uint64_t throwdown_names_at{};
    // The remote object owners (id, epoch, revision) and map last handed to the native
    // runtime; unchanged ones skip the rebuild (session_send.cpp, sync_objects).
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> object_owners_sent;
    std::string object_map_sent;
    bool object_owners_valid{};
    std::uint64_t object_owners_at{};
    // Throwdown messages received since the last relay tick: sender, encoded message.
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> throwdown_inbox;
    // Parties (session_party.cpp): the local player's own roster entry, and the invites the
    // host passed on (newest last). Whoever hosts owns the parties: a dedicated server, or in
    // a lobby the host's game, in `parties`. Nobody is in one until they form it.
    PartyBook parties;
    std::uint64_t parties_revision{}; // the book's revision the last roster was built from
    std::uint32_t local_party{};
    bool local_party_leader{}, local_party_open{};
    bool local_speeding{}; // the dedicated server flagged our game speed: no linked activities
    struct PartyInvite { std::uint64_t from{}, received{}; };
    std::vector<PartyInvite> party_invites;
    std::uint64_t party_revision{}; // bumped when any party membership or invite changes
    std::mutex mutex;
};
std::uint64_t nonce();
Session &session();
// The slots below used_slots: every one that may hold a player.
inline std::span<Peer> active_peers(Session &s) { return {s.peers.data(), s.used_slots}; }
inline std::span<const Peer> active_peers(const Session &s) { return {s.peers.data(), s.used_slots}; }
inline void note_slot(Session &s, std::size_t slot) { s.used_slots = std::max(s.used_slots, slot + 1); }
// Lowers used_slots past free trailing slots. Freed slots were reset by reset_peer.
inline void trim_slots(Session &s) {
    while (s.used_slots && !s.peers[s.used_slots - 1].member.id) --s.used_slots;
}
// Runs fn(peer) for each slot below used_slots with that slot selected for the native adapters.
template <class F> void each_active_peer(Session &s, F &&fn) {
    for (std::size_t slot = 0; slot < s.used_slots; ++slot) {
        const PeerScope scope(slot);
        fn(s.peers[slot]);
    }
}
// A guest whose host is a dedicated server (a Steam game server, not a player).
inline bool dedicated_host(const Session &s) { return s.mode == Mode::join && game_server_steam_id(s.host_id); }
// Whether Steam itself vouches for this player's identity to this PC: a host's guests and a
// guest's host are connected directly, and so is another guest once the direct handshake is
// done. Anyone else is known only from the host's roster, which a host can fill as it likes,
// so what rests on who a player is (the developer and friend marks) waits for this.
inline bool steam_vouched(const Session &s, const Peer &peer) {
    return s.mode != Mode::join || peer.member.id == s.host_id || peer.direct_ready;
}
// Whether a player shows the tag the backend gives them, and whether the items that come with
// it animate: not until their appearance has arrived, and not when it says they turned that
// off (Appearance::hide_tag, Appearance::hide_items).
inline bool shows_tag(const Peer &peer) {
    const auto &look = peer.appearance.value();
    return look && !look->hide_tag;
}
inline bool shows_items(const Peer &peer) {
    const auto &look = peer.appearance.value();
    return look && !look->hide_items;
}
// How a player has each of their marked cosmetics animate: what their appearance says.
inline const MarkStyles &mark_styles(const Peer &peer) {
    static const MarkStyles standard{};
    const auto &look = peer.appearance.value();
    return look ? look->marks : standard;
}
Peer *find_peer(Session &s, std::uint64_t id);
unsigned player_count(const Session &s);
void reset_peer(Session &s, std::size_t slot);
void stop(Session &s, std::string reason);
void clear_world(Session &s, std::uint64_t now);
bool world_playing(const Session &s, const NativeFrame &local);
// session_view.cpp
void load_host_preferences(Session &s);
void save_host_preferences(const Session &s);
void publish(Session &s, const NativeFrame *local = nullptr);
void publish_chat(Session &s);
void load_bans(Session &s);
void save_bans(const Session &s);
bool is_banned(Session &s, std::uint64_t id);
// Re-reads friend_ids when the Steam social snapshot has changed.
void refresh_friends(Session &s);
// `marks`: whether the line may show the badge the backend gives its sender (player_role).
void add_chat(Session &s, std::uint64_t sender, std::string name, std::string text, bool local = false, bool marks = true);
// The colour and badge of one of the backend's categories.
std::pair<std::uint32_t, std::string> mark_role(IdentityList list);
// A player's role colour and badge ("Dev", "Creator", "Centrix", "Homie", "Admin", "Host", "Friend" or none), shown in chat
// and on their nametag. `local`: the local player.
std::pair<std::uint32_t, std::string> player_role(Session &s, std::uint64_t id, bool local, bool marks = true);
// Sends one line from this player; returns why not when it cannot.
std::string send_chat(Session &s, std::string_view typed);
// A "/" command for a dedicated server (votes, and any server command for its admins): sent
// like chat but never shown as a line; the server answers in chat.
std::string send_chat_command(Session &s, std::string_view typed);
// "/p <message>" in a lobby: one line for the local player's party only, relayed by the host.
std::string send_party_chat(Session &s, std::string_view typed);
// The "/" commands this session offers (the chat overlay lists them as the player types "/").
std::vector<MultiplayerChatCommand> chat_commands(const Session &s);
// One encoded throwdown message from this player to everyone else (through the host).
void send_throwdown(Session &s, std::vector<std::uint8_t> message);
// session_send.cpp
Packet packet(Session &s, PacketKind kind, std::uint64_t now);
void reset_direct(Session &s, Peer &p, std::uint64_t now);
void disconnect(Session &s, std::uint64_t id, const std::string &reason);
bool send_packet(Session &s, std::uint64_t id, const Packet &p, bool reliable, bool fresh,
                 std::span<const std::uint8_t> raw = {}, std::span<const std::uint8_t> wire = {});
void send_required(Session &s, std::uint64_t id, const std::vector<std::uint8_t> &bytes);
void send_world_state(Session &s, std::uint64_t now);
void begin_host_world(Session &s, std::string_view destination, std::uint64_t now);
void broadcast(Session &s, const Packet &packet, bool reliable, bool fresh, std::uint64_t now,
               std::uint64_t except = 0);
void sync_objects(Session &s, const NativeFrame &local, std::uint64_t now);
void refresh_host_choices(Session &s, std::uint64_t now);
void send_roster(Session &s, std::uint64_t now);
// Host: sends its physics tuning when it changes. Guest: skates with the session's tuning
// while the host enforces it, and with its own otherwise.
void update_physics_tuning(Session &s, const NativeFrame &local, std::uint64_t now);
// How the local mods change trick scoring (Engine/Vfs/mod_scoring.h). Guest: reported to the host
// or dedicated server once known and whenever it changes. Host: judged like a guest's.
void update_scoring(Session &s, std::uint64_t now);
// Host: flags or clears a guest from its report and the host's choice.
void judge_scoring(Session &s, Peer &peer);
// The chat line for the local player's own mods changing scoring or physics.
std::string own_scoring_notice();
void send_local(Session &s, const NativeFrame &local, std::uint64_t now, std::uint64_t captured_at = 0,
                bool pose_captured = true);
// session_receive.cpp
void apply_roster(Session &s, const Packet &p, std::uint64_t now);
// Parties (session_party.cpp).
std::uint32_t party_of(const Session &s, std::uint64_t id); // 0 = none; the local player too
// Another player in the local player's party.
bool party_member(const Session &s, std::uint64_t id);
std::uint64_t party_leader(const Session &s); // the local party's leader, 0 without one
// The local player's roster entry changed (roster from the host, or the host's own).
void set_local_party(Session &s, const Member &local);
void receive_party(Session &s, const Packet &p, std::uint64_t now);
void expire_party_invites(Session &s, std::uint64_t now);
// Sends a request to whoever owns the parties (a lobby's host answers its own at once);
// returns why it can't, or empty.
std::string send_party_request(Session &s, PartyAction action, std::uint64_t player);
// A lobby's host: answers `from`'s party request, as a dedicated server does.
void host_party_request(Session &s, std::uint64_t from, PartyAction action, std::uint64_t player, std::uint64_t now);
// A lobby's host: `from`'s "/p" line, relayed to the rest of their party only.
void host_party_chat(Session &s, std::uint64_t from, std::string_view text, std::uint64_t now);
// A lobby's host, every network tick: players who left are out of their parties, lapsed
// invites are withdrawn, and a change sends a new roster.
void tick_host_parties(Session &s, std::uint64_t now);
// A lobby's host: each roster member's party as the book has it.
void fill_roster_parties(Session &s, std::vector<Member> &members);
bool accept_data(Peer &peer, const Packet &p, std::uint64_t now);
// The same, moving a pose into the playback buffer (the packet's pose is left empty).
bool accept_data(Peer &peer, Packet &&p, std::uint64_t now);
void networking(Session &s, const NativeFrame &local, std::uint64_t now);
// session_commands.cpp
void apply_distances(Session &s, const MultiplayerDistances &distances);
void apply_object_placement(Session &s, ObjectPlacement policy);
// Stores what guests may use and applies it to the local player (the host and a dedicated
// server's admins are exempt).
void apply_guest_tools(Session &s, bool noclip, bool no_bail, bool boosts);
// Shows the chosen nametags: ReSkate's (and none of the game's nametags or compass
// arrows), the game's own, or none.
void apply_nametags(const Session &s);
} // namespace dingosdk::multiplayer::session_detail
