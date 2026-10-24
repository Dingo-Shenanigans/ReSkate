#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Research tools for multiplayer throwdowns (analysis/throwdowns-native-mp.md §8):
// inject the events that put a player id with no native player into the local
// throwdown (AddAIParticipantToCommunityEvent), spawn a queue hosted by such an id
// (DebugSpawnThrowdownWithParams), start or destroy it, and log every participant
// change in both realms. Console: `throwdown <verb> ...`.
namespace dingosdk::multiplayer {
// Called once by initialize_native_throwdowns after its own hooks are installed.
void initialize_throwdown_lab(std::uintptr_t base) noexcept;
// Called after every authored expression (vm = the expression that just ran); sends
// queued events from the client realm and reads results off server graphs.
void pump_throwdown_lab(std::uintptr_t vm) noexcept;
// Substitutes only the preview prefab in this invocation's constant page.
// The shipped graph resource and official Throwdowns remain unchanged.
void execute_throwdown_preview_expression(std::uintptr_t vm,std::uint32_t pc,std::uintptr_t profiler,
    void (*runner)(std::uintptr_t,std::uint32_t,std::uintptr_t));
// Before the game changes level state to `next`: forgets the event types found in a level that
// is being left, which are freed with it.
void throwdown_lab_before_level_transition(unsigned next) noexcept;
// Native level generation, including offline play; never tied to Steam hosting.
std::uint64_t native_throwdown_world() noexcept;
// Called by the SendNetworkedEvent hook for every event the local client sends.
void observe_throwdown_send(std::uint32_t hash, std::uintptr_t type, std::uintptr_t payload) noexcept;
// A plugin mode's native flag registration (native_flag_session.h): its flag mesh, its held-open
// queue, and the waiting card's Start (consumed: true) and Leave; a running plugin match's Quit.
bool consume_flag_throwdown_send(std::uint32_t hash, std::uintptr_t type, std::uintptr_t payload) noexcept;
// Ask the last solo host's native event to destroy itself, and then allow the
// original Leave request/confirmation to perform normal client teardown.
void prepare_native_solo_throwdown_leave(std::uint32_t hash) noexcept;
// True only for the local host's native Throwdown configured for one player.
// Its authored Start action can bypass the normal two-participant UI gate.
bool native_solo_throwdown_waiting() noexcept;
// A real native queue creation/join retires any hidden custom donor banner.
std::uint64_t native_throwdown_queue_revision() noexcept;
// True for ids the lab put into a throwdown; the UI player lookups answer them with
// the local player's record so rows and host lookups resolve.
bool throwdown_lab_player(std::uint32_t player_id) noexcept;
// `skater` is the local skater's position when known (spawn without a recorded placement).
std::string throwdown_lab_command(std::string_view arguments, std::optional<std::array<float, 3>> skater);
// Coop challenge research (analysis/coop-challenges-mp.md): `challenge virtual <n>` gives the
// next challenge start n virtual participants and makes it coop; `challenge attempt [id]`
// replays the local player's last finished attempt as that player. Console: `challenge ...`.
std::string challenge_lab_command(std::string_view arguments);

// Injection for the throwdown relay (throwdown_relay.cpp); game thread, in a world.
// Each queues one event that the local client sends to its own server from the next
// client-realm expression. False when the event types cannot be found.
// Finds the event types once per process by a heap scan on a worker thread: false until it
// has, true from then on. Poll it; it never blocks.
bool prepare_throwdown_injection() noexcept;
// DebugSpawnThrowdownWithParams hosted by `host`; the queue's MMID comes back through
// throwdown_relay_spawned(token, ...).
// `instance` names the drop's flag (InstanceId, a plugin mode's ReSkate.<flag>; empty: the
// series' own), and `capacity` holds its queue at that size (0: as the settings say).
bool queue_throwdown_spawn(std::uint64_t token, std::uint32_t host, const std::string &series,
                           const std::vector<std::uint8_t> &placement, const std::vector<std::uint8_t> &settings,
                           const std::string &instance = {}, std::uint32_t capacity = 0) noexcept;
// A plain `series` queue at `at`, hosted by the local player (a plugin's drop): its MMID comes back
// through throwdown_relay_spawned(token, ...), its parameters through throwdown_relay_host_placement.
bool queue_throwdown_host(std::uint64_t token, const std::string &series, const std::array<float, 3> &at, std::uint32_t most_players) noexcept;
// The native registration/POI for another player's flag of plugin mode `key` ("<plugin>/<id>",
// registered here), with that mode's flag mesh. Its donor queue remains open; membership and the
// start stay with the mode's plugin. False for a mode no plugin here registered.
bool queue_custom_throwdown_flag(std::uint64_t token,std::uint32_t host,const std::string& key,
    const std::array<float,3>& at,float yaw) noexcept;
bool queue_throwdown_add(std::uint32_t player, const std::string &series, std::uint32_t mmid) noexcept;
bool queue_throwdown_remove(std::uint32_t player) noexcept; // RemoveParticipantFromCommunityEvent as `player`
bool queue_throwdown_start(const std::string &series, std::uint32_t mmid) noexcept;
bool queue_throwdown_destroy(std::uint32_t mmid) noexcept;
// `player`'s total in the running throwdown: its leaderboard row and the server's own
// score list. False until the local client has opened this throwdown's leaderboard.
bool queue_throwdown_score(std::uint32_t player, std::int32_t score) noexcept;
// One leaderboard write as `player`'s own client made it: board index in the event's
// leaderboard manager, added or set. False until this throwdown's leaderboard exists.
bool queue_throwdown_row(std::uint32_t player, std::uint8_t board, bool add, std::int32_t value) noexcept;
// RequestTurnEnd as `player` (ends that player's turn if it is theirs). False while the
// running event's handle is unknown.
bool queue_throwdown_end_turn(std::uint32_t player) noexcept;
// ForceDestroyCurrentEvent as `player`: ends the event on the local server that `player` is in
// (one the local player quit keeps running here with only relayed players otherwise).
bool queue_throwdown_destroy_event(std::uint32_t player) noexcept;
// Coop challenges (throwdown_relay.cpp; analysis/coop-challenges-mp.md). Game thread.
// The local client's own solo start request for challenge `id` of `series`, as its Start button
// sends it; the relay's StartChallenge plan makes it the linked coop copy.
bool queue_challenge_start(const std::string &series, const std::string &id, bool contest) noexcept;
// AttemptFinishedForCriteria as `player`, with its owner's arrays bit-exact.
bool queue_challenge_attempt(std::uint32_t player, std::vector<std::uint8_t> criteria,
                             std::vector<std::uint8_t> indexes) noexcept;
// SlamObjectHitNetworked {identifier, the local copy's EventHandle} as `player`. False while the
// local copy's handle is unknown (it is read when the local client finishes the onboarding).
bool queue_challenge_slam(std::uint32_t player, std::int32_t identifier) noexcept;
// LeaveInProgressRequested as `player`: the local copy lets that player go.
bool queue_challenge_leave(std::uint32_t player) noexcept;
// Party beacons: MoveSpawnedEntityForPlayer {location, BEACON} as `player` puts that player's
// beacon there (replacing one already standing); DespawnEntityForPlayer removes it. False while
// the event types are not found yet (they are looked for in the background: call again).
bool queue_beacon_move(std::uint32_t player, const std::array<float, 16> &location) noexcept;
bool queue_beacon_remove(std::uint32_t player) noexcept;
// The local coop celebration's spots since the last call: PlayerLocation then Player2..4, each the
// 12 floats of the event's Transform, as CoopChallengeCelebrationRuleData.Activate built them.
struct ChallengeCelebration {
    std::array<std::array<float, 12>, 4> spots{};
    std::uint32_t count{};
};
std::optional<ChallengeCelebration> take_challenge_celebration() noexcept;
// Skate_SubmitAttempt as `player` (their client's own verdict and trick record, bit-exact).
// False while the running event's handle is unknown.
bool queue_throwdown_attempt(std::uint32_t player, bool landed, const std::array<std::uint8_t, 28> &trick) noexcept;
}
