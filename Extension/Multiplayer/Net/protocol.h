#pragma once
#include "Extension/Multiplayer/Remote/cosmetics.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include "Extension/Multiplayer/Remote/audio_state.h"
#include "Extension/Multiplayer/Net/effects.h"
#include "Extension/Multiplayer/Voice/voice_state.h"
#include "Engine/Game/Multiplayer/distance_settings.h"
#include "Engine/Game/Multiplayer/object_placement.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/tick_settings.h"
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Game/World/world_layers.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::multiplayer {
constexpr std::size_t max_skater_bones = 512, max_board_bones = 64;
constexpr std::size_t max_packet = 24576;
constexpr std::size_t packet_header_size = 64;
constexpr std::uint16_t protocol_version = 47;
// A dedicated server's chat lines unless its owner says otherwise: violet (#8E5CFF) and lavender (#D9C8FF).
inline constexpr std::uint32_t default_server_chat_badge = 0xffff5c8eU, default_server_chat_text = 0xffffc8d9U;
constexpr std::size_t max_throwdown_message = 4096;
// Packet::tuning: the host's SkatePhysicsTuning differences (Extension/Skater/physics_tuning.h).
constexpr std::size_t max_physics_tuning = 16384;
// Votes a dedicated server runs (Packet::server_votes).
constexpr std::uint8_t server_vote_map = 1, server_vote_kick = 2, server_vote_time = 4;
// What else a running vote can be (ServerVote::kind; never in Packet::server_votes): a poll,
// a question with answers that runs nothing, and a vote the server's owner defined.
constexpr std::uint8_t server_vote_poll = 8, server_vote_custom = 16;
// The vote a dedicated server is running, or has just finished (Packet::vote): games show it
// with its tally and let the player answer. `id` is 0 when there is none. `kind` is one of the
// server_vote_* bits; `seconds` is what is left of a running one. A poll has its answers and a
// count for each instead of yes, no and needed.
constexpr std::size_t max_vote_label = 120;
constexpr std::size_t max_vote_answers = 6, max_vote_answer = 48;
constexpr std::uint8_t vote_running = 0, vote_passed = 1, vote_failed = 2, vote_cancelled = 3;
struct ServerVote {
    std::uint32_t id{};
    std::uint8_t kind{}, outcome{vote_running};
    std::uint16_t yes{}, no{}, needed{}, seconds{};
    std::uint64_t starter{}, target{}; // target: the player a kick vote is about, who has no vote in it
    std::string label;                 // "change the map to ...", or a poll's question
    std::vector<std::string> answers;  // poll: 2 to max_vote_answers
    std::vector<std::uint16_t> counts; // poll: one per answer
    bool operator==(const ServerVote &) const = default;
};
// Who may start a poll on a dedicated server (Packet::server_polls).
enum class ServerPolls : std::uint8_t { off = 0, admins = 1, everyone = 2 };
// A vote a dedicated server's owner defined: "/vote <name> [choice]" (Packet::server_custom_votes).
// The name and each choice are valid_server_vote_name; the description is chat text or empty.
constexpr std::size_t server_custom_vote_limit = 16, server_vote_name_bytes = 16, server_vote_description_bytes = 80,
                      server_vote_max_choices = 8;
struct ServerCustomVote {
    std::string name;                 // "restart"
    std::string description;          // "Reload the current map"
    std::vector<std::string> choices; // what the player picks from; empty: the vote takes no argument
    bool operator==(const ServerCustomVote &) const = default;
};
// A dedicated server's announcement, shown as a card for `seconds` (Packet::announcement).
// `id` tells one from the next; 0 is none. `text` is one chat line.
struct ServerAnnouncement {
    std::uint32_t id{};
    std::uint16_t seconds{};
    std::string text;
    bool operator==(const ServerAnnouncement &) const = default;
};
enum class PacketKind : std::uint16_t {
    hello = 1,
    welcome = 2,
    pose = 3,
    away = 4,
    cosmetics = 5,
    audio = 6,
    roster = 7,
    challenge = 11,
    routes = 12,
    peer_hello = 13,
    peer_welcome = 14,
    map_request = 15,
    map_offer = 16,
    world_state = 17,
    world_ready = 18,
    objects = 19,
    voice = 20,
    chat = 21,
    // A dedicated server's admin asking it to change a setting, and the
    // server's answer: one line of text either way (valid_admin_text).
    admin = 22,
    // A dedicated server's ban list, sent to its admins (newest first, capped).
    bans = 23,
    // The levels a dedicated server's admins may switch it to (level assets).
    maps = 24,
    // One throwdown message (Extension/Throwdowns/throwdown_wire.h), opaque here:
    // relayed through the host or dedicated server like chat.
    throwdown = 25,
    // The host or dedicated server sending one player somewhere (tpall, tphere).
    teleport = 26,
    // The host's skate physics tuning, as differences from the game's own (opaque here;
    // empty = the game's). Only the host sends it; a dedicated server never needs to.
    physics_tuning = 27,
    // A party request to whoever hosts, or its notice to one player (PartyAction).
    party = 28,
    // A player telling the host or dedicated server how its mods change trick scoring
    // (Engine/Vfs/mod_scoring.h): sent once known, again whenever it changes.
    scoring = 29,
    // The host's physics that its tuning does not carry: the trainer's tuning-class values and
    // trick multipliers (Engine/Game/Multiplayer/session_physics.h; opaque here, empty = the
    // game's own). Like physics_tuning, only the host sends it and a dedicated server never does.
    physics_extras = 30,
    // A player's skater touching the world (effects.h): the game's sparks, dust and puffs for
    // it, shown on that player's skater by everyone near. Unreliable, relayed like voice.
    effects = 31
};
// Packet::party_action. Requests go from a player to whoever hosts, a dedicated server or a
// lobby's host (party_player = the other player involved, 0 for leave/open/close); invited
// and withdrawn go from the host to the invitee (party_player = the inviter). The host
// answers everything else with a chat line and the next roster.
enum class PartyAction : std::uint8_t {
    invite = 1,    // invite party_player into the sender's party
    accept = 2,    // accept party_player's invite
    decline = 3,   // decline party_player's invite
    join = 4,      // join party_player's party (open, or invited)
    leave = 5,     // leave the sender's party
    kick = 6,      // the leader removes party_player
    promote = 7,   // the leader hands the lead to party_player
    open = 8,      // the leader lets anyone join
    close = 9,     // the leader makes the party invite-only
    invited = 10,  // host -> invitee: party_player invited you
    withdrawn = 11 // host -> invitee: party_player's invite can no longer be accepted
};
bool valid_party_request(PartyAction action, std::uint64_t player) noexcept;
// Steam accounts in the public universe. Players are individual accounts; a
// dedicated server is a game server: anonymous (type 4) with a new ID each time
// it starts, or signed in with a login token (type 3) with the same ID always.
inline bool individual_steam_id(std::uint64_t id) noexcept {
    return (id >> 56) == 1 && ((id >> 52) & 15) == 1 && (id & 0xffffffffULL);
}
inline bool game_server_steam_id(std::uint64_t id) noexcept {
    const auto type = (id >> 52) & 15;
    return (id >> 56) == 1 && (type == 3 || type == 4) && (id & 0xffffffffULL);
}
inline bool persistent_server_steam_id(std::uint64_t id) noexcept {
    return game_server_steam_id(id) && ((id >> 52) & 15) == 3;
}
// A player's own name as sent in their hello: at most this many bytes.
constexpr std::size_t max_member_name = 64;
constexpr std::size_t max_admin_text = 320;
constexpr std::size_t max_ban_rows = 256;
constexpr std::size_t max_server_maps = 128, max_map_asset = 128;
// A level asset as a server's map list carries it: printable ASCII, no '|'.
bool valid_map_asset(std::string_view asset) noexcept;
bool valid_map_pool(std::span<const std::uint16_t> pool, std::size_t maps) noexcept; // distinct indices below `maps`
bool valid_map_label(std::string_view label) noexcept; // empty, or a name like a member's
// How long a host goes on waiting for a player who says they are fetching the map
// (Packet::map_fetching), from the first time they say so: a large map on a slow line. Past
// it they are held to the time a load takes again.
inline constexpr std::uint64_t map_fetch_limit_us = 45ull * 60 * 1000000;
// The Thunderstore package a map comes from, so a player without it can be offered it:
// "Owner-Name", or "Owner-Name-1.2.3" when the host knows its version. Empty for the game's own
// maps and for a map mod that is not from Thunderstore.
inline constexpr std::size_t max_map_package = 96;
bool valid_map_package(std::string_view package) noexcept;
// That name for a mod folder and the version its manifest gives; empty when the folder is not
// named as a package: "Owner-Name", as the launcher installs one, or "Owner-Name-1.2.3", as its
// zip unpacks by hand. The manifest's version comes before the folder's; one that is not three
// numbers is left off.
std::string map_package_name(std::string_view folder, std::string_view version);
struct Transform {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
    bool operator==(const Transform &) const = default;
};
struct Pose {
    Transform root;
    // board[0] is the separate board entity's world transform; remaining
    // entries are its rig pose. Empty means the board is currently unavailable.
    std::vector<Transform> skater, board;
};
struct Member {
    std::uint64_t id{}, epoch{};
    std::string name;
    bool admin{}; // roster: may change a dedicated server's settings
    // roster: the player's party (0 = none), whether they lead it, and (on the leader) whether
    // anyone may join it. Each party has exactly one leader.
    std::uint32_t party{};
    bool party_leader{}, party_open{};
    // roster: a dedicated server measured the player's game running fast (a speedhack): nobody
    // plays linked throwdowns or coop challenges with them (Server/speed_check.h).
    bool speeding{};
    // roster: the host or dedicated server found the player's mods change trick scoring: the same
    // as speeding, nobody plays linked throwdowns or coop challenges with them.
    bool scoring{};
    bool operator==(const Member &) const = default;
};
bool valid_roster(std::span<const Member>, unsigned capacity) noexcept;
bool valid_routes(std::span<const Member>) noexcept;
struct Packet {
    PacketKind kind = PacketKind::pose;
    std::uint32_t sequence{};
    std::uint64_t session{}, map{}, epoch{}, time_us{}, source{};
    std::uint64_t world = 1;
    std::array<std::uint8_t, 32> build{}, proof{};
    std::uint64_t challenge{};
    std::string destination;
    bool map_authorized{};
    bool world_ready{};
    // map_request, world_ready: the player does not have the map and is downloading it
    // (never with world_ready). A host gives such a player longer than a load takes.
    bool map_fetching{};
    Pose pose;
    std::uint32_t pose_interval_us = 50000;
    // pose: the sender has the game's party collision on (the top bit of the pose's rate field)
    bool player_collision{};
    Appearance appearance;
    std::vector<AudioSample> audio;
    VoiceData voice;
    VoicePolicy voice_policy;
    unsigned capacity = max_players;
    std::vector<Member> members;
    MultiplayerDistances distances; // Authoritative host policy carried by its reliable roster.
    unsigned tps = multiplayer_default_tps;
    // Session placement policy, including for players joining later. Clients
    // lock their editor and native tools from it; the host enforces it by
    // freezing guest layouts (see publish_guest_objects).
    ObjectPlacement object_placement = ObjectPlacement::everyone;
    // Objects each player may have placed (object_placement.h); 0: no limit.
    unsigned object_limit{};
    // Roster: players may place objects at another size than their own. Off: a dedicated server
    // shares every player's objects at their own size (its admins' excepted).
    bool object_scaling{true};
    // Roster: players see each other's skater effects (effects.h, and the trails and fire of
    // each other's costumes and skateboards). Off: a dedicated server relays none and games
    // neither send theirs nor show other players'.
    bool sync_effects{true};
    // Bumped each time the host deletes all guest objects. Guests delete their
    // own session objects when it changes after their first roster.
    std::uint32_t object_clears{};
    // Roster: the colours of a dedicated server's own chat lines, its badge and name and the
    // text after them (IM_COL32 layout; the server's owner chooses them).
    std::uint32_t chat_badge = default_server_chat_badge, chat_text = default_server_chat_text;
    ServerVote vote; // roster
    bool force_world_layers{};
    WorldLayerState layers = WorldLayerState(world_layers().size()); // all "default"
    ParkChoices parks; // One allowlisted selection byte per shared native park slot.
    ObjectChunk objects;
    // chat: one message (valid_chat_text); hello: the sender's name (may be
    // empty); admin: a request or its answer (valid_admin_text).
    std::string text;
    float voice_range = default_voice_range; // roster: how far the host forwards proximity voice
    // roster: whether guests may use noclip (and teleport) / No Bail / the forward and up boosts
    // (the host and a dedicated server's admins always may)
    bool guest_noclip = true, guest_no_bail = true, guest_boosts = true;
    // roster: guests skate with the host's physics tuning (a dedicated server's: the game's own)
    bool enforce_tuning = true;
    // roster: the votes a dedicated server lets players start (server_vote_* bits)
    std::uint8_t server_votes{};
    std::uint8_t server_polls{};                       // roster: who may start a poll (ServerPolls)
    std::vector<ServerCustomVote> server_custom_votes; // roster: the owner's own votes that are on
    ServerAnnouncement announcement;                   // roster
    std::vector<MultiplayerBan> bans;         // bans: newest first
    std::uint32_t ban_total{};                // bans: how many the server has in all
    std::vector<std::string> maps;            // maps: level assets
    std::vector<std::uint16_t> map_pool;      // maps: the pool as indices into `maps`, rotation order (empty: every map)
    std::uint16_t map_rotation{};             // maps: minutes per map (0: off)
    std::string map_label;                    // map_offer, world_state: the map's name for people (may be empty)
    std::string map_package;                  // map_offer, world_state: valid_map_package (may be empty)
    std::vector<std::uint8_t> throwdown;      // throwdown: one encoded message (1..max_throwdown_message bytes)
    std::array<float, 3> teleport{};          // teleport: where the receiver goes (world position)
    std::vector<std::uint8_t> tuning;         // physics_tuning: 0..max_physics_tuning bytes
    std::vector<std::uint8_t> extras;         // physics_extras: 0..max_physics_extras bytes
    std::vector<Impact> impacts;              // effects: 1..max_impacts contacts
    PartyAction party_action = PartyAction::leave; // party: what is asked or told
    std::uint64_t party_player{};                   // party: the other player (see PartyAction)
    // scoring: the sender's scoring fingerprint, 0 for the game's own; `text` names the mods
    // that change it (empty, or valid_admin_text).
    std::uint64_t scoring{};
};
// Empty means a compatible greeting. The caller supplies its current local
// map/session and, after the handshake, the already accepted peer epoch.
std::string_view greeting_error(const Packet &, std::uint64_t session, std::uint64_t map,
                                const std::array<std::uint8_t, 32> &build,
                                std::uint64_t peer_epoch = 0) noexcept;
bool valid_transform(const Transform &) noexcept;
// A chat message: 1..multiplayer_chat_max_bytes of UTF-8 with no control characters.
bool valid_chat_text(std::string_view) noexcept;
// A player name in a hello or roster: UTF-8 without control characters.
bool valid_member_name(std::string_view) noexcept;
bool valid_admin_text(std::string_view) noexcept;
// 1 to server_vote_name_bytes of a-z, 0-9, - and _.
bool valid_server_vote_name(std::string_view) noexcept;
bool valid_server_custom_vote(const ServerCustomVote &) noexcept;
bool valid_server_vote(const ServerVote &) noexcept; // its label and, for a poll, its answers
// The message a player typed, made valid: control characters and broken UTF-8
// dropped, surrounding blanks trimmed, cut to the byte limit on a character boundary.
std::string clean_chat_text(std::string_view);
// A player's name as a roster carries it (valid_roster): the same cleaning, at most 128 bytes.
std::string clean_roster_name(std::string_view);
bool valid_pose(const Pose &) noexcept;
std::vector<std::uint8_t> encode(const Packet &, bool compact_pose = false);
// Rounds every rotation in a pose to a multiple of 2^bits in the 16-bit form a compact pose
// packs it in (the root to at most 2^6). The pose still encodes and decodes as any other;
// what changes is that a bone turning by less than a step is the same bytes as before, so a
// difference from a reference leaves it out, and the low bits of the ones that did change are
// zero and pack away. One step is about 0.0025 degrees times 2^bits: 4 bits is 0.04 degrees,
// 7 bits a third of a degree.
void coarsen_rotations(Pose &pose, unsigned bits) noexcept;
// Keeps every bone's scale within 1/limit to limit on each axis (1: no scaling at all). A mod
// that resizes part of a skater (a head four times the size) does it with a bone's scale,
// which travels in the pose and so shows to everyone, mod or not.
void limit_bone_scale(Pose &pose, float limit) noexcept;
// Keeps every bone of the body and the board within `limit` metres of the bone it hangs from
// (0: no limit). A bone's place is relative to its parent and, on the game's rigs, never
// changes: the longest is a thigh, 0.44 m. A hacked client that moves them stretches its
// skater across the map for everyone. The skater's few bones that follow the board and the
// body's place in a fall (free_skater_bones) do move, by metres, and are held to
// free_bone_reach instead; so is the board's own rig from the board.
inline constexpr std::array<std::uint16_t, 14> free_skater_bones{3, 50, 283, 375, 376, 377, 378, 379, 380, 390, 391, 392, 393, 394};
inline constexpr float free_bone_reach = 100.f;
// What a game holds every pose it is sent to, whatever sent it: twice the server's standard
// reach, and the most a server may allow a bone to be resized.
inline constexpr float client_bone_reach = 2.f, client_bone_scale = 8.f;
void limit_bone_reach(Pose &pose, float limit) noexcept;
// The same, with a pose encoded at another update interval (a recipient thinned by
// distance) instead of the packet's own, so the packet need not be copied for it.
std::vector<std::uint8_t> encode(const Packet &, bool compact_pose, std::uint32_t pose_interval_us);
std::optional<Packet> decode(std::span<const std::uint8_t>) noexcept;
bool newer_sequence(std::uint32_t candidate, std::uint32_t previous) noexcept;
std::uint64_t map_hash(std::string_view) noexcept;
bool valid_map_destination(std::string_view) noexcept;
// Normalized lerp between two transforms. Runs for every joint of every remote player each
// frame, so it is inline and plain arithmetic (std::lerp's edge-case branches and a divide
// per component were ~1% of a multiplayer client frame, profiled 2026-10-02).
inline Transform interpolate(const Transform &a, const Transform &b, float t) {
    Transform out;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    for (unsigned i = 0; i < 3; ++i) {
        out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
        out.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * t;
    }
    const float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] +
                      a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    float norm{};
    for (unsigned i = 0; i < 4; ++i) {
        out.rotation[i] = a.rotation[i] + (sign * b.rotation[i] - a.rotation[i]) * t;
        norm += out.rotation[i] * out.rotation[i];
    }
    const float inverse = 1.0f / std::sqrt(norm);
    for (float &v : out.rotation)
        v *= inverse;
    return out;
}
void offset_pose(Pose &, const std::array<float, 3> &);
std::array<float, 16> to_matrix(const Transform &);
Transform from_matrix(const std::array<float, 16> &);
struct Invite {
    std::uint64_t steam_id{}, secret{};
};
std::optional<Invite> parse_invite(std::string_view) noexcept;
std::string format_invite(Invite);

class AudioBuffer {
  public:
    bool push(const Packet &, std::uint64_t arrival);
    std::optional<AudioState> sample(std::uint64_t now);
    void clear();
    std::size_t size() const { return frames_.size(); }

  private:
    struct Frame {
        AudioState state;
        std::uint64_t source{};
        bool event{};
    };
    std::deque<Frame> frames_;
    std::optional<AudioState> current_;
    std::uint64_t epoch_{}, last_arrival_{}, played_source_{}, seen_{};
    std::int64_t clock_offset_{};
    std::uint32_t sequence_{};
};

// Reliable appearance messages have their own receive sequence. An older
// reliable packet can arrive after a newer unreliable pose without being lost.
class AppearanceBuffer {
  public:
    bool push(const Packet &p);
    const std::optional<Appearance> &value() const { return value_; }
    void clear() {
        value_.reset();
        epoch_ = 0;
        sequence_ = 0;
    }

  private:
    std::optional<Appearance> value_;
    std::uint64_t epoch_{};
    std::uint32_t sequence_{};
};

// Arrival times use one local monotonic clock; remote clocks need not agree.
enum class PosePlaybackMode { unavailable, buffered, predicted, held };
struct PosePlayback {
    PosePlaybackMode mode = PosePlaybackMode::unavailable;
    std::uint64_t prediction_us{};
    bool correcting{};
};
class PoseBuffer {
  public:
    bool push(const Packet &, std::uint64_t arrival_us);
    // For packets returned by decode(), which already performed the complete
    // untrusted transform walk. The rvalue form moves the pose into the buffer.
    bool push_validated(const Packet &, std::uint64_t arrival_us);
    bool push_validated(Packet &&, std::uint64_t arrival_us);
    // Reuses the caller-owned vectors. The optional-returning overloads remain
    // for tests and infrequent callers, while render playback avoids allocating
    // two transform arrays on every client callback.
    bool sample(std::uint64_t now_us, Pose &out) const;
    std::optional<Pose> sample(std::uint64_t now_us) const;
    bool sample_remote(std::uint64_t now_us, Pose &out);
    // Whether a pose arrived within this long: sampling gives up after a second without one.
    bool heard_within(std::uint64_t now_us, std::uint64_t age_us) const;
    std::optional<Pose> sample_remote(std::uint64_t now_us);
    PosePlayback playback() const { return playback_; }
    void clear();
    std::size_t size() const { return frames_.size(); }

  private:
    struct Frame {
        Pose pose;
        std::uint64_t arrival, source;
    };
    std::deque<Frame> frames_;
    std::uint64_t epoch_{};
    std::uint32_t sequence_{};
    bool sender_clock_{};
    std::int64_t clock_offset_{};
    std::uint32_t interpolation_delay_us_ = 100000;
    std::uint64_t playback_at_{}, correction_start_{}, correction_end_{};
    std::array<float, 3> correction_{};
    PosePlayback playback_;
    Pose anchors_; // scratch for arrival continuity, which needs only the anchor transforms
    template <class P> bool push_frame(P &&, std::uint64_t arrival_us);
    std::uint64_t target_time(std::uint64_t now_us) const;
    // anchors_only fills just the transforms prediction and correction move (see offset_pose).
    bool sample_frames(std::uint64_t now_us, Pose &out, bool anchors_only) const;
    bool predict(std::uint64_t now_us, PosePlayback &state, Pose &out, bool anchors_only = false) const;
    void correct(Pose &, std::uint64_t now_us) const;
};
} // namespace dingosdk::multiplayer
