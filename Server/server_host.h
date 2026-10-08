#pragma once
#include "server_activity.h"
#include "server_config.h"
#include "speed_check.h"
#include "Engine/Game/Multiplayer/chat_rate.h"
#include "Extension/Multiplayer/Net/delta_codec.h"
#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/sound_codec.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include "Extension/Multiplayer/Session/party_book.h"
#include "Extension/Multiplayer/Session/password.h"
#include "Extension/Multiplayer/Session/room.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Engine/Game/World/world_layers.h"
#include <unordered_map>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace dingosdk::server {
using namespace multiplayer;

// The host side of a ReSkate session without a game: admission, the roster,
// map changes and relaying every player's poses, outfits, voice, chat and
// objects. It follows the protocol the in-game host speaks (session_*.cpp),
// minus the host's own skater.
class Host {
  public:
    using Log = std::function<void(const std::string &)>;
    Host(ServerConfig &config, SteamTransport &transport, Log log);
    // Opens the listener and starts a session under the given server identity.
    bool start(std::string &error);
    void tick(std::uint64_t now);
    // A console line, or an admin's request (`admin` = their SteamID64, 0 for the console).
    std::string command(std::string_view line, std::uint64_t admin = 0);
    void stop(const std::string &reason);

    std::string invite() const;
    std::string map_name() const;
    unsigned players() const;
    std::uint64_t secret() const { return secret_; }
    // The UDP port players may connect straight to, or 0 (config connection).
    std::uint16_t direct_port() const { return direct_port_; }

    enum class VoteKind { map, kick, time };

  private:
    struct ChatBudget {
        std::uint64_t since{};
        unsigned messages{};
        // A few messages at once, then they recover every 5 s.
        bool accept(std::uint64_t now, unsigned burst = 6) noexcept {
            if (now < since || now - since >= 5000000) { since = now; messages = 0; }
            return ++messages <= burst;
        }
    };
    // What is sent and received, in bytes, by what it carries (traffic_kind): poses, sound,
    // voice, outfits, objects, the rest. `last` is the last whole half minute.
    struct Traffic {
        std::array<std::uint64_t, 6> out{}, in{};
        std::uint64_t snapshots{}; // whole states sent reliably; the rest of a stream is differences
    };
    struct Counted {
        Traffic total, mark, last;
    };
    struct Guest {
        Member member;
        Counted traffic;
        std::uint64_t password_challenge{};
        bool handshaken{}, map_authorized{}, world_ready = true;
        std::uint64_t last_map_offer{}, travel_since{}, connected_at{}, last_packet{};
        std::uint64_t loading_since{}; // not ready in this world since (reloading on their own)
        std::uint32_t ready_sequence{};
        ReceiveBudget budget;
        DeltaSender sender;
        DeltaReceiver receiver;
        PoseBuffer poses;
        AudioBuffer audio;
        AppearanceBuffer appearance;
        std::vector<std::uint8_t> cosmetic_packet;
        struct PendingCosmetics { Packet packet; std::uint64_t received{}; };
        std::vector<PendingCosmetics> pending_cosmetics;
        std::optional<Transform> latest_root;
        // Where they last stood and faced, and when they last moved from it: a player standing
        // still (in a menu, away from the keyboard, watching) is sent on at the low rate.
        // Their earlier poses, as encoded: the one before, and ones kept a quarter of a second
        // and a second (measure_pose).
        struct Earlier { std::vector<std::uint8_t> raw; std::uint64_t time{}; };
        Earlier pose_last, pose_quarter, pose_second;
        // Their own last poses, rounded (pose_codec.h): what the others' differences are built
        // on. `keep` is how long one stays, by the slowest rate anyone was sent it at: a
        // player sent this one once a second must still find it here when their ack arrives.
        struct KeptPose {
            std::uint32_t sequence{};
            std::uint64_t time_us{}, kept_at{};
            std::uint8_t keep{};
            pose_codec::QuantPose pose;
        };
        std::deque<KeptPose> kept_poses;
        // What this player is being sent of each other one, and what of it they acked.
        pose_batch::Sender pose_sender;
        // Their own poses arrive the same way (receive_poses): the last ones rebuilt, and which
        // of their messages were read in full, which they are told once a pass.
        std::unordered_map<std::uint16_t, pose_batch::Stream> upload_streams;
        pose_batch::Ack upload_ack;
        bool upload_ack_due{};
        // Poses due to them this pass (flush_poses).
        struct QueuedPose {
            std::uint64_t source{};
            std::uint32_t sequence{};
            pose_batch::Rate rate{};
            bool collision{}, hold_fingers{};
            std::uint8_t tier{}; // 0 full rate, 1 half, 2 low, 3 out of sight
        };
        std::vector<QueuedPose> queued_poses;
        // Skaters' sound (sound_codec.h): what this player holds of each other one's, the
        // samples due to them this pass, and what is held here of their own.
        sound_codec::Sender sound_sender;
        struct QueuedSound {
            std::uint64_t source{}, time_us{};
            std::uint32_t sequence{};
            std::shared_ptr<const std::vector<AudioSample>> samples;
        };
        std::vector<QueuedSound> queued_sound;
        std::unordered_map<std::uint16_t, sound_codec::In> sound_in;
        Transform still_at;
        std::uint64_t moved_at{};
        std::uint64_t pose_arrival{};
        std::array<PoseDelivery, max_players> pose_delivery;
        CrowdLimits crowd; // how far the full and half rates reach for them in a crowd
        std::vector<Member> direct_routes;
        std::uint64_t route_reported{};
        std::uint32_t route_sequence{};
        bool received_voice{};
        std::uint32_t voice_sequence{};
        VoiceBudget voice_budget;
        OutfitBudget outfit_budget;
        SoundBudget sound_budget;
        ChatRate chat_rate;
        ChatBudget admin_budget, throwdown_budget, party_budget;
        SpeedCheck speed;           // how fast their game runs, from their pose timestamps
        bool speeding{};            // flagged: out of linked activities (config speed_check)
        std::uint64_t speed_normal_since{}; // flagged, but measuring normal again since then
        // How their mods change trick scoring, as they reported it (Engine/Vfs/mod_scoring.h):
        // nothing until the report arrives, 0 for the game's own.
        std::optional<std::uint64_t> scoring;
        std::string scoring_mods;
        bool scoring_flagged{};     // out of linked activities (config score_check)
        ChatBudget scoring_budget;
        std::uint64_t bans_sent{}; // the ban list revision this admin has
        std::uint64_t undelivered_since{}; // Steam has refused what they must be sent since
        // The players they have not been shown yet (introduce): on joining, and after a map
        // change, a player is sent the others a few a second, nearest first, instead of every
        // outfit and every stream at once. Nothing of a player in here is sent to them.
        std::set<std::uint64_t> unmet;
        std::uint64_t next_introduction{};
        bool maps_sent{};          // this player has the server's map list (send_maps)
        // The owner's own upload, and what the server shares of it.
        ObjectState objects, shared;
        std::uint64_t shared_from{};
        std::set<std::uint64_t> cleared;
        struct ObjectDelivery {
            std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sent;
            std::vector<ObjectChunk> chunks;
            std::uint64_t source{}, epoch{};
            std::size_t next{}, cursor{};
            std::uint64_t failing_since{}; // Steam has refused their object updates since
        } object_delivery;
    };

    ServerConfig &config_;
    SteamTransport &transport_;
    Log log_;
    ActivityLog activity_; // what players do, for the console (config_.activity_log)
    // The running player vote (server_votes.cpp), and when each player may start another.
    struct Vote {
        VoteKind kind{};
        std::uint64_t starter{}, target{}; // target: the player a kick vote is about
        std::string value, label;          // value: the map or time; label: "change the map to ..."
        std::set<std::uint64_t> yes, no;
        std::uint64_t ends{};
        unsigned shown_yes{}, shown_no{};  // the tally last announced
    };
    std::optional<Vote> vote_;
    bool vote_recount_{}; // a player left: recount in tick(), never while guests_ is being walked
    std::map<std::uint64_t, std::uint64_t> vote_cooldowns_;
    std::uint64_t map_since_{}; // rotation clock start: the last map change, or while nobody is on
    bool rotation_warned_{};    // players were told the next map is a minute away
    // Parties (server_party.cpp): the server owns them; each roster carries them to everyone.
    PartyBook parties_;
    std::uint64_t party_revision_{};
    std::map<std::uint64_t, std::unique_ptr<Guest>> guests_;
    std::set<std::uint64_t> kicked_;
    JoinBackoff join_backoff_; // Steam IDs whose attempts to join keep failing
    std::optional<PasswordKey> password_;
    std::uint64_t id_{}, secret_{}, epoch_{}, map_{}, world_ = 1;
    std::uint32_t sequence_{};
    VoicePolicy voice_policy_;
    std::uint32_t object_clears_{};
    WorldLayerChoices layers_;
    bool roster_dirty_ = true, running_{};
    std::uint64_t bans_revision_ = 1; // bumped whenever the ban list or the admins change
    std::uint64_t now_{}, last_roster_{}, last_world_state_{}, next_object_update_{}, travel_started_{};
    std::string relay_status_; // Steam's relay network as last logged
    std::uint16_t direct_port_{};
    std::uint64_t next_relay_check_{};
    std::uint64_t next_crowd_{}; // when the crowd limits are worked out again
    std::uint64_t next_network_log_{}; // the log's once-a-minute line about the connections
    // How the server's own loop is keeping up, for `net` and that line: passes of tick() in a
    // minute, the time spent in them, the longest one and the longest wait between two. A long
    // pass or gap is the server itself stalling, which every player feels at once.
    struct Loop {
        std::uint64_t since{}, passes{}, busy_us{}, longest_pass_us{}, longest_gap_us{};
    } loop_, last_loop_, worst_loop_;
    std::uint64_t pass_started_{};
    Counted traffic_;
    // What a pose costs to send as a difference from references of different ages, measured on
    // every fourth pose that arrives (net). A nearer reference changes less, so it packs
    // smaller: this says by how much, before the server is made to send them that way.
    struct PoseSizes {
        std::uint64_t samples{}, whole{}, last{}, quarter{}, second{};
        std::array<std::uint64_t, 4> sent{}, sent_bytes{}; // differences sent, by rate: full, half, low, out of sight
        std::uint64_t whole_sent{}, whole_sent_bytes{}, held{}; // whole poses sent; differences sent without fingers
        // Of the poses measured, what had changed since the pose before, by the kind of field:
        // how many, and the bytes they take before packing. Says what a pose's size is made of.
        struct Changed { std::uint64_t fields{}, bytes{}; };
        std::array<Changed, 4> changed{}; // positions as floats, positions in mm, rotations, scales
        std::uint64_t bones{};
    } pose_sizes_;
    void measure_pose(Guest &from, const Packet &packet);
    Guest::KeptPose *keep_pose(Guest &from, const Packet &packet);
    void flush_poses();
    void pose_ack(Guest &guest, const pose_batch::Ack &ack);
    std::uint64_t traffic_mark_{}, traffic_window_us_{}; // when `mark` was taken, and how long `last` covers
    void meet_later(Guest &guest);
    void introduce();
    std::string loop_report() const;
    std::string network_report(std::string_view player, bool console);

    Guest *find(std::uint64_t id);
    Packet packet(PacketKind kind, std::uint64_t now);
    unsigned capacity() const { return config_.max_players + 1; }
    // What the connection layer is opened for: every extra slot there could be as well, since
    // the reserved players and admins change while the server runs. Who gets in is may_join's.
    unsigned connection_capacity() const { return static_cast<unsigned>(multiplayer::max_players); }
    std::string guest_name(const Guest &) const;
    std::string player_name(std::string_view wanted, std::uint64_t id) const;
    bool is_admin(std::uint64_t id) const;
    bool is_banned(std::uint64_t id) const;
    void save();

    // `detail` is for the log alone: the player is told `reason`.
    void drop(std::uint64_t id, const std::string &reason, const std::string &detail = {});
    bool send_packet(Guest &, const Packet &, bool reliable, bool fresh, std::span<const std::uint8_t> raw = {},
                     std::span<const std::uint8_t> wire = {});
    void send_required(Guest &, const std::vector<std::uint8_t> &bytes);
    void broadcast(const Packet &, bool reliable, bool fresh, std::uint64_t except = 0);
    void send_roster();
    void send_world_state();
    void send_chat(std::string_view text, Guest *only = nullptr);
    void send_bans(Guest &admin);
    void send_maps(Guest &admin);
    void change_map(std::string_view map); // a level name, level path or destination
    std::string wire_map_label() const;
    bool same_map(std::string_view asset) const { return map_hash(map_destination(asset)) == map_; }
    bool accept_data(Guest &source, const Packet &);
    // `received_at`: the transport's arrival time for the message (TransportMessage::arrived).
    void receive(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at, const Packet *ready = nullptr);
    void receive_poses(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at);
    void receive_sound(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at);
    void receive_cosmetics();
    void sync_objects();
    void apply_layers();
    // Chat commands and votes (server_votes.cpp).
    void chat_command(Guest &, std::string_view line);
    void start_vote(Guest &, VoteKind, std::string_view argument);
    void cast_vote(Guest &, bool yes);
    void check_vote(bool expired);
    void cancel_vote(const std::string &why);
    const VoteSetting &vote_setting(VoteKind) const;
    std::uint8_t enabled_votes() const;
    void reply(Guest &, std::string_view text);
    Guest *match_player(std::string_view text);
    void tick_rotation();
    std::string pool_text() const;     // the map pool, one map a line
    std::string rotation_text() const; // the rotation's interval and next map
    void resend_maps();                // after the pool or the admins change
    // Parties (server_party.cpp).
    void party_request(Guest &, PartyAction, std::uint64_t player);
    void party_command(Guest &, std::string_view line); // "/party ..."
    void party_chat(Guest &, std::string_view text);    // "/p <text>": to the sender's party only
    void party_left(std::uint64_t id, const std::string &name); // a player left the server
    bool check_speed(Guest &, std::uint64_t sent); // a pose they sent at `sent` (their clock); false: kicked
    void check_scoring(Guest &);                   // after their report, or a score_check change
    void tick_parties();
    void send_party(Guest &to, PartyAction, std::uint64_t player);
    void party_notice(std::uint32_t party, std::string_view text, std::uint64_t except = 0);
    std::string party_status(std::uint64_t id) const;
};
} // namespace dingosdk::server
