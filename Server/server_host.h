#pragma once
#include "server_activity.h"
#include "worker_pool.h"
#include "server_config.h"
#include "board_check.h"
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
#include <deque>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <array>

namespace dingosdk::server {
class ServerPlugins;
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
    // The level players load for the map, as their games name it: a destination's level, after
    // its "|". What the server's plugins see as reskate.level.current().
    std::string map_level() const;
    // "maps reload": reads Mods again and says what changed. Also after a map was updated.
    std::string reload_maps();
    // The mod folder of the map in play while anyone is on (an update leaves it alone), or
    // nothing; and whether "maps update" was typed since this was last asked.
    std::string map_folder_in_play() const;
    bool take_map_update_request() { return std::exchange(map_update_requested_, false); }
    // The plugins players are told to have ("plugins.required"): the config's, less those a
    // check on Thunderstore found are not plugins (refuse_plugins).
    std::vector<std::string> required_plugins() const;
    void refuse_plugins(std::set<std::string> refused) { refused_plugins_ = std::move(refused); }
    unsigned players() const;
    std::uint64_t secret() const { return secret_; }
    // The UDP port players may connect straight to, or 0 (config connection).
    std::uint16_t direct_port() const { return direct_port_; }

    const ServerConfig &config() const { return config_; }
    bool voice_allowed() const { return voice_policy_.allowed; }
    bool has_password() const { return password_.has_value(); }

    struct PlayerInfo {
        std::uint64_t id{};
        std::string name;
        unsigned ping{};
        bool admin{};
        bool voice_muted{};
        bool text_muted{};
        float x{}, y{}, z{};
    };

    std::vector<PlayerInfo> player_list() const;
    void send_player_chat(std::uint64_t steam_id, std::string_view text);
    void broadcast_player_chat(std::string_view text);
    bool teleport_player(std::uint64_t steam_id, float x, float y, float z);
    bool is_banned(std::uint64_t id) const;
    // A player's locks (lock_*, server_config.h), and making them exactly `locks` as an admin's
    // lock and unlock would (logged, and the player told): the tools that changed.
    std::uint8_t player_locks(std::uint64_t id) const { return locks_of(id); }
    std::uint8_t set_locks(std::uint64_t id, std::uint8_t locks, const std::string &source);
    std::optional<std::array<float, 3>> player_position(std::uint64_t steam_id) const;
    // The role a player has (data/roles.json: given them, or the default), here or not; or none.
    const Role *player_role(std::uint64_t id) const { return role_of(id); }
    // Where each player in the world last said their skater was (their latest pose's root), and
    // how long ago that arrived: what the server's plugins see of the skaters.
    struct SkaterPose {
        std::uint64_t id{};
        Transform root;
        std::uint64_t age_us{};
    };
    std::vector<SkaterPose> skater_poses() const;
    void drop(std::uint64_t id, const std::string &reason, const std::string &detail = {});
    void change_map(std::string_view map); // a level name, level path or destination
    std::uint64_t world_id() const { return world_; }
    // A server plugin changes the map, or the time of day for everyone, with the checks of the
    // console's map and tod (and saved as they save): "" when done, else why not.
    std::string plugin_map(std::string_view map);
    std::string plugin_time_of_day(std::string_view time);
    // The last chat lines, for the server's plugins (chat.read): each player's line as it is passed
    // on, the server's own lines to everyone (sender 0), and a player's /command the server does
    // not know itself, while a plugin hears chat. Sequences count from 1.
    struct ChatRecord {
        std::uint64_t sequence{}, sender{};
        std::string name, text;
        bool server{};
    };
    std::vector<ChatRecord> chat_since(std::uint64_t sequence) const;
    // The park mods set up (maps.park_mods), for the server's plugins: which fit the map running
    // now, and which are spawned, with how many objects and minutes left (0: until removed).
    struct ParkStatus {
        std::string name, map;
        bool here{}, spawned{};
        std::size_t objects{};
        unsigned minutes_left{};
    };
    std::vector<ParkStatus> park_status() const;
    // One spawned (for `minutes`, 0 until removed) or removed for a server plugin, as "park-mod add"
    // and "remove" do, with their checks and limits and their "[parks]" lines logged "by plugin
    // <id>": "" when it is done, else why not.
    std::string plugin_park(std::string_view plugin, bool spawn, std::string_view name, unsigned minutes);
    // A plugin message from the server's plugins (server_plugins.h) to one player, or everyone (0).
    void send_plugin_message(std::uint64_t to, const std::vector<std::uint8_t> &bytes);
    // Cards and polls for the server's plugins (server_votes.cpp), on the players' screens: each
    // player's own, shown over the server's announcement and vote until it ends, then those again.
    // `players` empty is everyone on now. A card is up `seconds` (0: as long as an announcement);
    // a poll runs `seconds` (0: votes.poll_seconds), one id for all its players, closed for each
    // as they answer ("/2"); a player's newer poll replaces theirs. "" when shown, else why not.
    std::string plugin_card(std::string_view plugin, const std::vector<std::uint64_t> &players, std::string_view text, unsigned seconds);
    std::string plugin_poll(std::string_view plugin, const std::vector<std::uint64_t> &players, std::string_view question,
                            const std::vector<std::string> &answers, unsigned seconds, std::uint32_t &id);
    std::string plugin_poll_close(std::string_view plugin, std::uint32_t id);
    void plugin_screens_release(std::string_view plugin); // a plugin that stopped: its cards and polls go
    // A player's answer (from 1) to a plugin's poll, or (answer 0) the poll closed for them and why:
    // "timeout", "left", "replaced" or "closed".
    struct PollNews {
        std::string plugin;
        std::uint32_t id{};
        std::uint64_t player{};
        std::size_t answer{};
        std::string text, why;
    };
    std::vector<PollNews> take_poll_news() { return std::exchange(poll_news_, {}); }
    // One player hidden from others for the server's plugins (server_host.cpp). Wholly (`skater`):
    // they leave those players' rosters, and nothing of them (poses, sound, voice, outfit, effects,
    // objects) is sent to them, so their games take the skater away as for a player who left.
    // Otherwise only their voice and/or sound is held back. `viewers` empty is everyone on now but
    // the target. Refused while the target shares a party or a throwdown with one of them. Shown
    // again (`viewers` empty: everyone this plugin hid them from), they are introduced anew.
    // "" when done, else why not.
    std::string plugin_hide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t> &viewers, bool skater, bool voice,
                            bool sound);
    std::string plugin_unhide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t> &viewers);
    void plugin_hides_release(std::string_view plugin); // a plugin that stopped: everyone it hid is shown again

    enum class VoteKind { map, kick, time, custom, poll, extend };

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
        // When they first said they are fetching the map (Packet::map_fetching), 0 for never
        // in this world: while they go on saying so, within map_fetch_limit_us of then, the
        // time they are given to load starts over.
        std::uint64_t fetching_since{};
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
        // When they last did something a player at their game does: moved, spoke, typed in chat
        // or changed their objects (0 until their game has loaded the map). And whether they have
        // been told they are about to be removed for being away.
        std::uint64_t active_at{};
        bool away_warned{};
        std::uint64_t present_at{}; // their game's last presence notice counted (PacketKind::present)
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
        EffectBudget effect_budget;
        // Their poses read this pass, passed on once all of the pass's messages are read
        // (relay_poses), and how many of their effects packets were passed on in it.
        std::vector<Packet> relay_poses;
        unsigned effects_pass{};
        ChatRate chat_rate;
        std::uint64_t typing_notice{}; // last accepted "still typing", so a flood is not relayed
        bool typing{};                 // an "on" was passed along and no "off" since
        ChatBudget admin_budget, throwdown_budget, party_budget, plugin_budget;
        SpeedCheck speed;           // how fast their game runs, from their pose timestamps
        bool speeding{};            // flagged: out of linked activities (config speed_check)
        std::uint64_t speed_normal_since{}; // flagged, but measuring normal again since then
        BoardCheck board;           // whether their skateboard is being kept on another player
        bool board_hidden{};        // flagged: their board is not shown to the others (config board_check)
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
        std::uint64_t permissions_sent{}; // the roles revision their permissions were sent at
        std::uint64_t words_sent{};       // the words revision the server's filtered words were sent at
        std::uint64_t next_maps_sent{};   // the revision the next vote's maps were sent at
        // The owner's own upload, and what the server shares of it.
        ObjectState objects, shared;
        std::uint64_t shared_from{};
        std::set<std::uint64_t> cleared;
        // Objects they placed in the last minute, and until when none of theirs are shared, for
        // placing more than a person does (sync_objects).
        std::uint64_t placed_since{}, placed{}, objects_held_until{};
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
    bool map_update_requested_{};
    std::set<std::string> refused_plugins_;
    ActivityLog activity_; // what players do, for the console (config_.activity_log)
    // The running player vote (server_votes.cpp), and when each player may start another.
    // A poll is one too: it has answers instead of yes and no, and runs nothing (but the console's poll-run).
    struct Vote {
        VoteKind kind{};
        std::size_t custom{}; // custom: which of config_.votes.custom
        std::uint32_t id{}; // told to games, so each can tell one vote from the next
        std::uint64_t starter{}, target{}; // target: the player a kick vote is about
        std::string value, label;          // value: the map, time or command; label: "change the map to ..." or the question
        std::set<std::uint64_t> yes, no;
        std::vector<std::string> answers;            // poll
        std::map<std::uint64_t, std::size_t> chosen; // poll: each voter's answer
        std::string run;                             // poll-run: the console command for the winner ("{answer}")
        bool next_map{};                             // poll: the rotation's vote for the next map (start_next_map_vote)
        std::uint64_t ends{};
    };
    std::optional<Vote> vote_;
    // What games are shown of the vote (the roster carries it): the running one with its
    // tally, or the one just finished with how it ended, for a few seconds.
    multiplayer::ServerVote vote_shown_;
    std::uint64_t vote_shown_until_{};
    std::uint32_t vote_ids_{};
    void show_vote(const Vote &vote, std::uint8_t outcome);
    // The announcement games show (the roster carries it), until announcement_until_.
    multiplayer::ServerAnnouncement announcement_;
    std::uint64_t announcement_until_{};
    std::uint64_t announcement_for_{}; // the one player shown it, or 0: everyone
    std::uint32_t announcement_ids_{};
    // The plugins' cards and polls, by the player they are shown to (plugin_card, plugin_poll).
    struct PluginCard {
        std::string plugin;
        multiplayer::ServerAnnouncement card;
        std::uint64_t until{};
    };
    struct PluginPoll {
        std::string plugin;
        std::uint32_t id{};
        std::string question;
        std::vector<std::string> answers;
        std::uint64_t ends{};
    };
    std::map<std::uint64_t, PluginCard> plugin_cards_;
    std::map<std::uint64_t, PluginPoll> plugin_polls_;
    std::vector<PollNews> poll_news_;
    // The players the server's plugins hide (plugin_hide), by who they are hidden from: each
    // target's plugin, and what of them is held back.
    struct Hidden {
        std::string plugin;
        bool skater{}, voice{}, sound{};
    };
    std::map<std::uint64_t, std::map<std::uint64_t, Hidden>> hidden_;
    std::uint64_t announced_at_{}; // the last timed announcement, or while nobody is on
    std::size_t next_announcement_{};
    void active(Guest &guest) {
        guest.active_at = now_;
        guest.away_warned = false;
    }
    void remove_away();
    bool vote_recount_{}; // a player left: recount in tick(), never while guests_ is being walked
    std::map<std::uint64_t, std::uint64_t> vote_cooldowns_;
    std::uint64_t map_since_{}; // rotation clock start: the last map change, or while nobody is on
    bool rotation_warned_{};    // players were told the next map is a minute away
    unsigned rotation_told_{};  // the minutes-left marks already announced: bit 0 ten, 1 five, 2 one
    bool rotation_voted_{};     // this map's vote for the next one was held
    // The maps that vote offers, drawn a minute before it so games can have their pictures
    // ready, and their revision (0: never drawn, nothing to send).
    std::vector<multiplayer::NextMap> next_maps_;
    std::uint64_t next_maps_revision_{};
    void draw_next_maps();
    std::mt19937 random_{std::random_device{}()};
    bool start_next_map_vote();                 // false when the pool has nothing to choose between
    void finish_next_map_vote(const Vote &done, const std::vector<unsigned> &count);
    void extend_map(unsigned minutes);          // the rotation's clock, that much longer from now
    // Parties (server_party.cpp): the server owns them; each roster carries them to everyone.
    PartyBook parties_;
    std::uint64_t party_revision_{};
    std::map<std::uint64_t, std::unique_ptr<Guest>> guests_;
    // Kicked players and when each may join again (0: not before the server restarts).
    std::map<std::uint64_t, std::uint64_t> kicked_;
    // Keeps a player out for `minutes` (0: until the server restarts), or for as long as
    // kick_minutes says when none is given, and says how long for their notice and the log:
    // " for 10 minutes", or " until the server restarts".
    std::string keep_out(std::uint64_t id, std::optional<unsigned> minutes = std::nullopt);
    // The name each player last joined under, for banning one who has left by their SteamID.
    std::map<std::uint64_t, std::string> seen_names_;
    // Warnings each player has had for words that are not allowed at all ("word_warnings"),
    // kept while the server runs so that leaving and joining again does not clear them.
    std::map<std::uint64_t, unsigned> word_warnings_;
    // Whether this chat line may go on; if not, its player has been warned or kicked.
    bool allowed_words(Guest &guest, std::string_view text);
    JoinBackoff join_backoff_; // Steam IDs whose attempts to join keep failing
    std::optional<PasswordKey> password_;
    std::uint64_t id_{}, secret_{}, epoch_{}, map_{}, world_ = 1;
    // Plugins' server copies (server_plugins.h), from start on.
    std::shared_ptr<ServerPlugins> plugins_;
    std::uint32_t sequence_{};
    VoicePolicy voice_policy_;
    std::uint32_t object_clears_{};
    WorldLayerChoices layers_;
    bool roster_dirty_ = true, running_{};
    std::uint64_t bans_revision_ = 1; // bumped whenever the ban list or the roles change
    std::uint64_t roles_revision_ = 1; // bumped whenever the roles or who has them change
    // The server's own filtered words as its players have them, and their revision: 0 until
    // there are any, so a server without the file sends nothing.
    std::vector<std::string> filtered_words_;
    std::uint64_t words_revision_{};
    bool role_tags_checked_{};
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
    struct Flushed {
        Traffic traffic;
        PoseSizes sizes;
        std::vector<std::pair<Guest *, std::uint32_t>> whole;
    };
    // The threads that share a pass's sending (none: this one does it all), and the lock
    // each send into the transport is made under.
    std::unique_ptr<WorkerPool> workers_;
    std::mutex send_mutex_;
    void measure_pose(Guest &from, const Packet &packet);
    Guest::KeptPose *keep_pose(Guest &from, const Packet &packet);
    // What sending one player their part of a pass added to the server's own counts, and the
    // whole poses it sent (the player they are of, and which): see flush_player.
    struct PoseSizes;
    struct Flushed;
    void flush_player(std::uint64_t id, Guest &g, Flushed &sent);
    void flush_poses();
    void relay_poses();
    // Poses and effects that arrived while the server was behind and were not passed on: in
    // all, as of the last log line about it, and when that line was written.
    std::uint64_t shed_{}, shed_logged_{}, shed_log_at_{};
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
    // Roles (roles.h): a player's, whether it gives a command or a permission, whether it gives
    // any command at all, and its rank (-1 with no role: below every role).
    const Role *role_of(std::uint64_t id) const { return config_.roles.of(id); }
    bool can(std::uint64_t id, std::string_view command) const { return allows(role_of(id), command); }
    bool has(std::uint64_t id, std::uint32_t permission) const {
        const auto *role = role_of(id);
        return role && (role->granted & permission);
    }
    // What every player does unless their role says otherwise (chat, votes, parties, private messages).
    bool may(std::uint64_t id, std::uint32_t permission) const { return server::may(role_of(id), permission); }
    std::string role_refusal(std::uint64_t id, std::string_view what) const {
        const auto *role = role_of(id);
        return "Your role" + (role ? " (" + role->name + ")" : std::string{}) + " cannot " + std::string(what) + " on this server.";
    }
    bool is_staff(std::uint64_t id) const { return staff(role_of(id)); }
    int rank(std::uint64_t id) const {
        const auto *role = role_of(id);
        return role ? role->rank : -1;
    }
    void send_permissions(Guest &);    // what their role lets them do
    // Tools taken from single players (lock_*, server_config.h): by an admin or the console
    // (`manual`), or for a throwdown they play in (`automatic`, config throwdown_locks). In
    // memory only. A throwdown's end when the player leaves; an admin's are kept for their
    // SteamID until unlocked or the server restarts, so leaving and joining again does not lift
    // them. A locked player's roster says the tool is off for them, and while noclip, No Bail or
    // the boosts are locked their permissions go without "movement", so their role cannot give
    // the tool back.
    struct Locks {
        std::uint8_t manual{}, automatic{};
        std::uint8_t all() const noexcept { return manual | automatic; }
    };
    std::map<std::uint64_t, Locks> locks_;
    std::set<std::uint64_t> in_throwdown_; // the players throwdown_locks were applied to, while theirs runs
    std::uint8_t locks_of(std::uint64_t id) const {
        const auto found = locks_.find(id);
        return found == locks_.end() ? 0 : found->second.all();
    }
    // Who changed a player's locks, which says what they are told: an admin (or the console), a
    // throwdown starting or ending, or the throwdown-locks setting changing while theirs runs.
    enum class LockBy { admin, throwdown, setting };
    // Locks (`lock`) or unlocks (otherwise) `tools` for the player `id` (`guest`, or null when they
    // are not connected), as `source` ("console", an admin's name, "throwdown"). A throwdown's,
    // and the setting's, are the automatic ones; an admin's unlock takes back both. Returns the
    // tools that changed.
    std::uint8_t change_locks(std::uint64_t id, Guest *guest, std::uint8_t tools, bool lock, LockBy by, const std::string &source);
    // A player's name for the lock lines: connected, last seen, or their SteamID.
    std::string lock_name(std::uint64_t id, const Guest *guest) const;
    void update_throwdown_locks(); // after the throwdowns or the setting change
    void send_words(Guest &);          // the server's own filtered chat words (server_words.h)
    void send_next_maps(Guest &);      // the maps the vote for the next one will offer
    void roles_changed();              // after any change to the roles or who has them
    void save();

    // `detail` is for the log alone: the player is told `reason`.
    bool send_packet(Guest &, const Packet &, bool reliable, bool fresh, std::span<const std::uint8_t> raw = {},
                     std::span<const std::uint8_t> wire = {});
    void send_required(Guest &, const std::vector<std::uint8_t> &bytes);
    // `only`: to these players alone; `skip`: not to these.
    void broadcast(const Packet &, bool reliable, bool fresh, std::uint64_t except = 0,
                   const std::set<std::uint64_t> *only = nullptr, const std::set<std::uint64_t> *skip = nullptr);
    void send_roster();
    void send_world_state();
    void send_chat(std::string_view text, Guest *only = nullptr);
    std::deque<ChatRecord> chat_log_;
    std::uint64_t chat_sequence_{};
    void record_chat(std::uint64_t sender, std::string name, std::string text, bool server);
    // A /command the server does not know, for the plugins when one hears chat: true when taken.
    bool chat_to_plugins(Guest &guest, std::string_view line);
    // The time of day on every map (tod): "" when set, else why not.
    std::string set_time_of_day(std::string_view time);
    void send_bans(Guest &admin);
    void send_maps(Guest &admin);
    std::string wire_map_label() const;
    void fetching(Guest &guest);
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
    void run_custom_command(Guest &, const CustomCommand &, std::string_view argument);
    // Starting a vote or poll: nullptr is the server console; the text says why it did not start ("" = it did).
    std::string start_vote(Guest *starter, VoteKind, std::string_view argument, std::size_t custom = 0);
    std::string start_poll(Guest *starter, std::string_view text, std::string run = {});
    void cast_vote(Guest &, bool yes);
    void answer_poll(Guest &, std::size_t answer);
    // Whether a chat command (without its "/") answers the running vote or poll for this player.
    bool answers_vote(const Guest &, std::string_view line) const;
    std::string end_poll(Guest *by);
    void check_vote(bool expired);
    void cancel_vote(const std::string &why);
    const VoteSetting &vote_setting(VoteKind, std::size_t custom = 0) const;
    std::uint8_t enabled_votes() const;
    multiplayer::ServerPolls enabled_polls() const;
    // The owner's votes that are on, for the roster; and "extend" with them while it can run.
    std::vector<multiplayer::ServerCustomVote> custom_votes() const;
    bool extend_vote_on() const; // "/vote extend": on, and the map is on a timer that staying adds to
    std::set<std::uint64_t> vote_voters(const Vote &) const;         // who may vote in it
    unsigned votes_needed(const Vote &, unsigned cast) const;
    std::vector<unsigned> poll_count(const Vote &) const;
    std::string running_vote_text() const; // what is running, and how to answer it
    // The "votes" and "announcements" commands (server_votes.cpp); the bool: a setting changed.
    std::pair<std::string, bool> votes_command(std::string_view argument);
    std::pair<std::string, bool> announcements_command(std::string_view argument);
    // The announcement card on every player's screen, or only on player `to`'s (not in chat).
    void announce(std::string_view text, std::uint64_t to = 0);
    void tick_announcements(); // the owner's messages in turn, on their timer
    // The plugins' cards and polls (plugin_card, plugin_poll): a poll closed for one player, with
    // why; a number they typed, true when it was about their plugin poll (answered or refused);
    // what ran out; a player who left; and the card and vote player `id`'s roster carries instead
    // of `card` and `vote`.
    // The players a plugin named who are on now (everyone for none); empty, and why, when none is.
    std::vector<Guest *> plugin_screen_players(const std::vector<std::uint64_t> &players, std::string &why);
    void close_plugin_poll(std::uint64_t player, std::string_view why);
    bool answer_plugin_poll(Guest &, std::size_t answer);
    void tick_plugin_screens();
    void plugin_screens_left(std::uint64_t player);
    void plugin_screens_roster(std::uint64_t id, multiplayer::ServerAnnouncement &card, multiplayer::ServerVote &vote) const;
    // What of `target` a plugin holds back from `viewer`, or null; whether either of two players is
    // wholly hidden from the other; and a player who left, taken out of every list.
    const Hidden *hidden_from(std::uint64_t viewer, std::uint64_t target) const;
    bool hidden_between(std::uint64_t a, std::uint64_t b) const;
    void hidden_left(std::uint64_t player);
    void reply(Guest &, std::string_view text, unsigned max_lines = 12);
    Guest *match_player(std::string_view text);
    // Sends each mover to a ring of `radius` around `at`, 1 m up, so nobody lands inside anyone; how many were sent.
    unsigned teleport_ring(const std::vector<Guest *> &movers, const std::array<float, 3> &at, float radius = 2.5f);
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
    bool check_board(Guest &, const Pose &);       // a pose of theirs; false: kicked
    void tell_admins(const std::string &text);     // a chat line to every admin in the game
    void check_scoring(Guest &);                   // after their report, or a score_check change
    void tick_parties();
    void send_party(Guest &to, PartyAction, std::uint64_t player);
    void party_notice(std::uint32_t party, std::string_view text, std::uint64_t except = 0);
    std::string party_status(std::uint64_t id) const;
    // Park mods (server_parks.cpp): parks from config_.park_mods, spawned for everyone as the
    // layout of the server's own SteamID and epoch, which no player can have.
    struct SpawnedPark {
        std::string name;
        std::vector<NetworkObject> objects; // with the server's own IDs
        unsigned minutes{};                 // 0: until removed
        std::uint64_t expires{};
        bool warned{}; // told a minute before
    };
    std::vector<SpawnedPark> parks_spawned_;
    ObjectState park_objects_; // never reset: its revisions only grow, so no game holds a newer one
    std::uint64_t park_object_ids_{};
    bool by_vote_{}; // a passed custom vote's command is running (as the console)
    std::string by_plugin_; // a server plugin's park-mod is running (plugin_park): "plugin <id>"
    std::string park_mod_command(std::string_view argument, std::uint64_t admin);
    void publish_parks();
    void remove_parks(std::string_view reason); // all of them, with the reason for the log
    void tick_parks();                          // expiry, and the minute's warning
};
} // namespace dingosdk::server
