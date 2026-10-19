#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

// Links the players' own throwdowns in a ReSkate multiplayer session
// (analysis/throwdowns-native-mp.md). Every machine runs the same throwdown on its
// own server with the other players as virtual participants (0x300 + their session
// slot): a leader's drop is spawned on everyone else's server hosted by the
// leader's virtual id, joins and leaves are replayed as that player's, the
// leader's start starts every joined copy, and scores become leaderboard rows.
// Jam sessions, Spot Battles (turn ends and both leaderboards relayed) and S.K.A.T.E.
// (each attempt replayed once, in the owner's same turn).
namespace dingosdk::multiplayer {
struct ThrowdownPeer {
    std::uint64_t id{};  // Steam ID
    std::size_t slot{};  // session slot on this machine
    std::string name;    // as the session shows it
    bool nearby{};         // close enough to the local skater to be invited into a coop challenge
    // In the local player's party: only party members are invited into each other's coop
    // challenges.
    bool party{};
};
struct ThrowdownRelayInput {
    std::uint64_t local{};            // this player's Steam ID; 0 without a session
    std::vector<ThrowdownPeer> peers; // the other players (never a dedicated server)
    bool in_world{};                  // the world is loaded and playing
    std::uint64_t world{};            // changes whenever the map or world changes
    std::string local_name;           // this player's name (also without a session)
    std::optional<std::array<float, 3>> position; // the local skater, while in the world
    // The dedicated server flagged the local player's game speed, or the host or server found
    // their mods change scoring or physics: their linked throwdown or coop challenge ends for them (the
    // others already dropped them), and no new one links.
    bool barred{};
};
// Multiplayer session tick (game thread). Returns the encoded messages to send to
// every other player.
std::vector<std::vector<std::uint8_t>> tick_throwdown_relay(std::uintptr_t base, const ThrowdownRelayInput &input);
// One message from another player, as relayed by the session (game thread).
void receive_throwdown_relay(std::uint64_t sender, std::span<const std::uint8_t> message);
std::string throwdown_relay_status();
// Lines for the local player's chat since the last call (a linked throwdown ended because
// everyone else left it). Game thread.
std::vector<std::string> take_throwdown_relay_notices();

// What the local player did, as seen by the throwdown natives (throwdown_lab.cpp).
struct ThrowdownLocalAction {
    enum class Kind {
        created,           // started placing a new drop
        exited,            // left its drop's details page (cancelling, or the drop is configured)
        joined,            // joined the queue `mmid` (the Join button)
        left,              // left the queue it was in
        force_started,     // force-started its own queue `mmid`
        destroy_requested, // asked the server to destroy the queue `mmid`
        entered,           // its queue became an event with these participants (server ids)
        ended,             // the throwdown it was in ended or it left it
        score,             // its running score in the current throwdown (Jam: PlayerScoreUpdated)
        row,               // its client wrote its own row on leaderboard `board` (Spot Battle)
        turn_ended,        // its turn ended (the client entered Local End Turn)
        turn_started,      // the local server started `player`'s turn
        attempt,           // its client submitted a S.K.A.T.E. attempt (add = landed) while `player` was up
        timer_failed,      // the local server's turn timer failed `player`'s pending S.K.A.T.E. turn
        quit,              // it quit the running throwdown (LeaveInProgressRequested)
        turn_shown,        // the client now shows `player` as the one who is up (replicated turn state)
        // Coop challenges (analysis/coop-challenges-mp.md):
        challenge_started, // StartChallenge ran: `series`, `challenge`, add = contest, participants (server ids)
        challenge_attempt, // its client finished an attempt: `criteria`, `indexes` (AttemptFinishedForCriteria)
        challenge_slam,    // its client hit slam target `score` (Identifier)
        challenge_ended,   // the challenge it was in let it go (ended, or it quit)
        // Party beacons (analysis/party-re/beacons.md):
        beacon_placed,     // its client asked for its beacon at `location` (add = moved: always
                           // happens; else a first placement, which a beacon already standing refuses)
        beacon_removed     // its client asked to remove its beacon
    } kind{};
    std::string series;
    std::uint32_t mmid{};
    bool cancelling{};
    std::uint8_t rules_mask{};
    bool rules_verified{};
    std::vector<std::uint8_t> placement, settings; // exited: CreateThrowdownQueueParams streams
    std::vector<std::uint32_t> participants;       // entered
    std::int32_t score{};                          // score, row
    std::uint8_t board{};                          // row: index in the event's leaderboard manager
    bool add{};                                    // row: added (else set); attempt: landed; beacon_placed: moved
    std::uint32_t player{};                        // turn_started, timer_failed; attempt: whose turn it was
    std::array<std::uint8_t, 28> trick{};          // attempt: CompositeTrickRecord
    std::string challenge;                         // challenge_started: the challenge's Id
    std::vector<std::uint8_t> criteria, indexes;   // challenge_attempt
    std::array<float, 16> location{};              // beacon_placed: the LinearTransform
};
void throwdown_relay_local(ThrowdownLocalAction action) noexcept;
// A mirror spawn queued with `token` ran on the local server (mmid 0: it did not).
void throwdown_relay_spawned(std::uint64_t token, std::uint32_t mmid, std::uint32_t capacity) noexcept;
// UIPlayerInfo model of a relayed player's virtual id (rows, host card, name), or 0.
std::uint64_t throwdown_relay_player_info(std::uint32_t player_id) noexcept;
// Server realm, while a queue becomes an event: puts the participant ids of a linked
// throwdown into the order every machine uses (leader first, then by Steam ID), which is
// the turn order. False (ids untouched) for anything else. Any thread.
bool throwdown_relay_order(std::span<std::uint32_t> ids) noexcept;
// Turn-based linked throwdowns show only the player whose turn it is, as retail does:
// the others' skaters are hidden. Game thread (the session's render).
bool throwdown_relay_hides(std::uint64_t player) noexcept;
// Display name for a server player id: the local player or a relayed player ("" if
// unknown). Offline the game has no names of its own. Any thread.
std::string throwdown_relay_player_name(std::uint32_t player_id);
// The relayed player a display name belongs to (Steam ID), or 0 when none or several do.
// Any thread.
std::uint64_t throwdown_relay_player_named(std::string_view name) noexcept;
// S.K.A.T.E.: true while another player is up in a linked throwdown. The local skater then
// waits on foot: each turn start's teleport lands it off the board and it cannot get back
// on until its own turn (whose teleport puts it on the board). Any thread.
bool throwdown_relay_waits_offboard() noexcept;

// Coop challenges. Starting any challenge with party members nearby invites them: every
// machine runs the same challenge with the others as virtual participants, and each player's
// finished attempts and slam hits are replayed as theirs everywhere.
// StartChallenge (server realm, any thread): the virtual ids of the other players of the linked
// challenge now starting, in the shared order; nothing for a challenge the local player plays alone.
std::optional<std::vector<std::uint32_t>> throwdown_relay_challenge_players(std::string_view series,
                                                                           std::string_view id) noexcept;
// The session's render (game thread): how far a player's pose (its root now at `root`) moves to
// stand at their spot in the local coop celebration, or nothing outside one.
std::optional<std::array<float, 3>> throwdown_relay_celebration_offset(std::uint64_t player,
                                                                      const std::array<float, 3> &root) noexcept;
// Whether starting a challenge invites nearby party members (on by default).
void set_challenge_invites(bool enabled) noexcept;
bool challenge_invites() noexcept;
// The console's switch for that (on by default).
void set_throwdown_offboard(bool enabled) noexcept;
bool throwdown_offboard_enabled() noexcept;
} // namespace dingosdk::multiplayer
