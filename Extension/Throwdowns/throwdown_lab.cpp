#include "throwdown_lab.h"
#include "native_type_scan.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_throwdowns.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "native_throwdowns.h"
#include "throwdown_relay.h"
#include "throwdown_debug_text.h"
#include "skate_trick_rule.h"
#include "graph_throttle.h"
#include "virtual_player_names.h"
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

// Research tools for multiplayer throwdowns. See analysis/throwdowns-native-mp.md §8
// for the experiments they serve; nothing here runs unless a console command asks,
// except the participant-change log and the GetPlayerName null fix.
namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
namespace engine = addr::engine;
namespace throwdowns = addr::native_throwdowns;
constexpr std::uint32_t client_realm = 0xbf0f9789, server_realm = 0x98be5555;

// Runtime event hashes (== EBX TypeNameHash for events) and field NameHashes. MMId
// is hashed case-insensitively: MMID, MMid, MMId and Mmid all share one hash.
constexpr std::uint32_t event_create = 0x7fac81bb, event_update = 0xb025c7fc, event_host_set = 0x9c8de5a9,
    event_add_participant = 0xdc10392b, event_add_ai = 0x72d73827, event_debug_spawn = 0xceaca85a,
    event_force_start = 0x303baecc, event_destroy = 0xf7a6803a, event_request_turn_end = 0xb2fc396c,
    event_player_score = 0xf133feac, event_skate_submit = 0xe85319b1, event_host_exited = 0xed07dca8,
    event_remove_participant = 0xaa3f9957, event_force_destroy = 0x4ab455f0;
// Coop challenges: a participant's finished attempt ({NetworkedPayload, +8 Data CriteriaData[] of
// 0x14 bytes, +0x10 SentIndexes i32[]}), merged per player by ActivityBaseCriteria.
constexpr std::uint32_t event_criteria_attempt = 0xb1163054, graph_criteria_attempt = 0x4bc91cfd;
constexpr std::uint32_t criteria_data_size = 0x14;
// Activity onboarding (the intro, "waiting for players"): each participant's client sends
// OnboardingSetRuleComplete {NetworkedEventPayload, +8 EventHandleId}; OnboardingSetRule
// (server 0x48cf5628) moves on once every participant has, else after its timeout (30 s).
constexpr std::uint32_t event_onboarding_complete = 0xc867aece;
// Slam targets are shared: SlamObjectHitNetworked {NetworkPayload, +8 Identifier, +0xc EventHandle}
// disables one for every participant (TargetChallengeWipeoutLogicRule 0xa235ec09).
constexpr std::uint32_t event_slam_hit = 0x453fe31f, event_leave_in_progress = 0x2ac7b512;
// CoopChallengeCelebrationRuleData.Activate (client): builds CoopChallengeCelebrationLocation in its
// locals: +0 PlayerLocation, +0x90 PlayerCount, +0xa0/+0xd0/+0x100 Player2..4 (0x30 bytes each).
constexpr std::uint32_t graph_celebration = 0x4234de87;
// Party beacons (analysis/party-re/beacons.md): PlayerSpawnedEntityManager's requests
// {NetworkedEventPayload, +0x10 Location (LinearTransform), +0x50 EntityId} (Despawn: +8 EntityId),
// client -> server, route 5. The beacon's EntityId is djb2("BEACON").
constexpr std::uint32_t event_beacon_spawn = 0xa121937d, event_beacon_move = 0x11c0be72,
    event_beacon_despawn = 0x799b3953, beacon_entity = 0x9ccf4481, field_location = 0x5498f4fd,
    field_entity_id = 0x2b12f9db;
// The graphs that name a beacon's owner (FindPlayerById(OwningUid) then GetPlayerName): the prompt
// text (PopulateReplicatedEntityInteractableWithPlayerData) and DropPlayerActionOnComplete; and the
// prompt's interaction, which finds the owner again by that name for their player card.
constexpr std::array<std::uint32_t, 4> beacon_name_graphs{0x183eeb92, 0xee49757f, 0x009ba416, 0x029ef9df};
constexpr std::uint32_t graph_beacon_inspect = 0x3d615152;
// `challenge virtual <n>` ids: clear of 0x300 + session slot for small sessions.
constexpr std::uint32_t challenge_virtual_base = 0x3c0;
constexpr std::uint32_t field_params = 3750769597U, field_parameters = 1886488519U, field_series = 1836954386U,
    field_mmid = 1723498148U, field_player_uid = 2665367800U, field_number_of_events = 2233253064U,
    field_host = 3697733452U, field_max_players = 2731482382U, field_party_only = 471685798U,
    field_instance = 383480460U, field_spawn = 3315120185U, field_teleport = 1635752012U, field_duration = 195801372U,
    field_bounds_scale = 2998125697U, field_event_handle = 4060261669U, field_payload = 3761469171U,
    field_is_cancelling = 3498825930U, field_player_score = 2296579872U;
// Sender attribution. Server handlers name an event sender from the connection it
// arrived on (the payload EID is stamped in transit), so an injected event cannot say
// who sent it. The local client to local server path is ordered, though: every
// tracked event the client sends (its own and injected ones) is queued here, and
// while a handler graph of that event runs, the id natives answer the injected
// sender if that event is the oldest one not yet handled.
constexpr std::string_view sender_marker = "rsk:"; // still written, for traces
struct Handled { std::uint32_t event; std::array<std::uint32_t, 3> graphs; };
constexpr std::array handled_events{
    Handled{0xb2fc396c, {0x04e2faaf}},                 // RequestTurnEnd: TurnBasedServer.OnRequestTurnEnd
    Handled{0xe85319b1, {0xc4a37359}},                 // Skate_SubmitAttempt: ServerSkateThrowdown
    Handled{0xf133feac, {0xe9010373, 0x80307eb2}},     // PlayerScoreUpdated: Jam, Spot Battle servers
    Handled{0xaa3f9957, {0xc7bb3bf5}},                 // RemoveParticipantFromCommunityEvent: MpActivityCoordinator
    Handled{0x4ab455f0, {0xe1e432ff}},                 // ForceDestroyCurrentEvent: ServerMpActivityBase
    Handled{0xb1163054, {0x4bc91cfd}},                 // AttemptFinishedForCriteria: ActivityBaseCriteria
    Handled{0xc867aece, {0x48cf5628}},                 // OnboardingSetRuleComplete: OnboardingSetRule
    Handled{0x453fe31f, {0xa235ec09}},                 // SlamObjectHitNetworked: TargetChallengeWipeoutLogicRule
    Handled{0x2ac7b512, {0xad0980ba}},                 // LeaveInProgressRequested: ServerMpActivityBase
    Handled{0xa121937d, {0x64f40bf8}},                 // SpawnEntityForPlayer: PlayerSpawnedEntityManager
    Handled{0x11c0be72, {0x2ab5ec18}},                 // MoveSpawnedEntityForPlayer: PlayerSpawnedEntityManager
    Handled{0x799b3953, {0x6e8bd812}}};                // DespawnEntityForPlayer: PlayerSpawnedEntityManager
// Client*Throwdown.EnterEnd and ClientThrowdownBase.EnterLeaveInProgress: the local
// player's throwdown is over (its leaderboard goes with it).
constexpr std::array<std::uint32_t, 5> throwdown_end_graphs{0x224de9ab, 0xd373badf, 0x2ea80cac, 0x1d82a056, 0x91cd7c71};
const Handled* handled(std::uint32_t event) {
    for (const auto& h : handled_events) if (h.event == event) return &h;
    return nullptr;
}
// MpThrowdownCoordinator.OnDebugSpawnThrowdownWithParams (server): local Lbcc holds
// coordinator+0x58, the MMID the queue it creates gets (its log line prints it).
constexpr std::uint32_t graph_debug_spawn = 0xd47edd3b, debug_spawn_mmid_local = 0xbcc;
// ThrowdownRegistration.QueueFilled (server) creates the event: Ldc4 = its EventHandle (the
// manager's event counter), Ldd0 = its RandomSeed.
constexpr std::uint32_t graph_queue_filled = 0xbd73369d, queue_filled_handle_local = 0xdc4, queue_filled_seed_local = 0xdd0;
// TurnBasedServerESM Round End onUpdate: L390 = TurnBasedServer.TurnOrderStyle when it reorders.
constexpr std::uint32_t graph_round_end_update = 0xcc05e680, round_end_style_local = 0x390;
// TurnBasedServerESM Turn End onBegin (substate 2; the client ESM runs the same graph, so the
// realm matters): the server just ended the turn of the player it started last. The client's
// "Local End Turn" state is no signal: it is also entered at the start of the local turn,
// while the replicated state still says Turn End but the active player already moved on.
constexpr std::uint32_t graph_turn_end_begin = 0xe0d0d09b;
// TurnBasedServerESM Turn Start onBegin: L3b8 = TurnStateData.ActivePlayerUID, the player whose
// turn starts (after skipping the eliminated), as it goes to IsValidId1To1022.
constexpr std::uint32_t graph_turn_start = 0x65e575ef, turn_start_active_local = 0x3b8, turn_start_handle_local = 0x3e0;
// ServerSkateThrowdown.OnActivityTimerEnded: L378 = the attempt outcome when the timer ran
// out; below 0 (pending) the active player's turn is failed by the timer.
constexpr std::uint32_t graph_skate_timer = 0xe9997d45, skate_timer_outcome_local = 0x378, skate_timer_handle_local = 0x30c;
// Skate_SubmitAttempt payload (0x2C): +8 EventHandle, +0xC WasSuccessful, +0x10 CompositeTrickRecord.
constexpr std::uint32_t attempt_landed_offset = 0xc, attempt_trick_offset = 0x10, attempt_trick_size = 0x1c;
// The throwdown client's own leaderboard writes: ClientThrowdownBase.UpdateCurrentScore (Jam's
// board, at round start) and ClientSpotBattleThrowdown.UpdateCurrentScore (round + overall).
// Other activities running at the same time write leaderboards of their own.
constexpr std::array<std::uint32_t, 2> throwdown_score_graphs{0xac8e7dbc, 0x80fa0b0d};

struct Job {
    enum class Kind { ai, spawn, start, destroy, join, end_turn, score, remove, total, row, attempt, destroy_event,
                      criteria_attempt, onboarding_complete, challenge_start, challenge_slam, challenge_leave,
                      beacon_move, beacon_remove } kind;
    std::uint32_t uid{}, max_players{};
    std::string series;                            // spawn: mode when no placement is recorded
    float score{}; bool flag{true};                // score: value and the native's flag
    std::optional<std::array<float, 3>> position;  // spawn: fresh parameters at this spot
    std::string challenge;                          // challenge_start: its Id (series in `series`)
    std::vector<std::uint8_t> criteria, indexes;    // criteria_attempt: a relayed attempt (else the captured one)
    std::array<float, 16> location{};               // beacon_move: where the beacon stands
    // Relay jobs name their queue (series + mmid) instead of the lab's current one;
    // a relay spawn carries its parameter streams and the token its MMID is reported under.
    std::uint32_t mmid{};
    std::uint64_t token{};
    std::vector<std::uint8_t> placement, settings;
    std::array<std::uint8_t, 28> trick{}; // attempt (flag = landed)
};
struct Lab {
    Address base{};
    std::atomic<bool> installed{};
    void (*participants_changed)(Address, const Address*, const Address*){};
    void (*player_name)(const Address*, Address){};
    std::uint32_t (*player_id_by_name)(const Address*){};
    std::uint32_t (*player_id)(const Address*){};
    void (*leaderboard_score)(Address, bool, std::uint32_t, float){};
    Address (*set_participants)(Address, Address, Address, Address){};
    Address (*order_participants)(Address, Address, Address, Address){};
    Address* (*find_player_by_id)(Address*, std::uint32_t){};
    Address* (*find_player_by_name)(Address*, const Address*){};
    void (*player_persona_id)(const Address*, std::uint64_t*){};
    void (*start_challenge)(Address, Address, Address*, Address, bool, std::uint64_t, std::uint32_t, bool, bool){};
    // Coop challenge lab (`challenge`): virtual participants for the next challenge start, the
    // ids that answer GetPlayerName with a virtual EID (virtual_player_names.h), the last start
    // seen, and the local client's last finished attempt (replayed as a virtual player).
    std::atomic<unsigned> challenge_armed{};
    std::array<std::atomic<std::uint32_t>, 4> challenge_named{};
    std::string challenge_start;                 // under `mutex`
    struct CriteriaAttempt { std::vector<std::uint8_t> data; std::vector<std::uint8_t> indexes; std::uint32_t count{}, index_count{}; };
    std::optional<CriteriaAttempt> criteria_attempt; // under `mutex`
    std::atomic<unsigned> criteria_runs{}, criteria_attempts{};
    std::uint32_t challenge_event_handle{};     // the local challenge copy's EventHandle (onboarding)
    std::optional<ChallengeCelebration> celebration; // under `mutex`
    // The server activity of the challenge that started last with the local player (its
    // participants arrive right after StartChallenge); removing the local player from it ends it.
    std::atomic<bool> challenge_activity_pending{};
    std::atomic<Address> challenge_activity{};
    // S.K.A.T.E.: the turn order in use (server) and whether this round's setter missed.
    std::mutex skate_mutex;
    std::vector<std::uint32_t> skate_order;
    std::atomic<bool> setter_missed{};
    int round_letters{}; // letters when the round began (-1: unknown)
    std::array<Address, 4> leaderboards{}; // leaderboards the local client wrote to, newest first
    struct Sent { std::uint32_t event, sender; std::uint64_t ms; };
    std::mutex sent_mutex;
    std::deque<Sent> sent; // tracked client events not yet handled, oldest first
    std::atomic<bool> any_injected_sent{};
    std::uint32_t event_handle{}; // last EventHandle the local client sent (the running event)
    std::mutex mutex;
    std::string series;
    std::uint32_t mmid{};
    std::vector<std::uint8_t> params; // CreateThrowdownQueueParams stream of the last local placement
    std::vector<std::uint8_t> host_params; // ... and of its HostSetThrowdownParameters (the host's settings)
    std::string params_note;
    std::map<std::uint32_t, Address> types;
    // Bumped when a level is left. The types above belong to the level; a heap scan begun
    // before it was left found ones that are gone by the time it reports them.
    std::atomic<std::uint64_t> world{};
    std::atomic<bool> type_scan_running{};     // prepare_throwdown_injection's worker
    std::atomic<std::uint64_t> type_scan_retry{}; // after a scan that missed some: not before this
    std::atomic<std::uint64_t> beacon_scan_retry{}; // the same for the party beacon events
    std::deque<Job> jobs;
    std::atomic<bool> pending{};
    // Spawns sent and not yet run by the server, oldest first (their MMIDs are read
    // off the server graph in the same order).
    struct Spawn { std::uint64_t token{}; std::uint32_t max_players{}; std::uint64_t ms{}; };
    std::deque<Spawn> spawns;
    std::atomic<bool> spawn_outstanding{};
    std::uint32_t hosted_mmid{};          // MMID of the local player's own drop (Update/HostSet)
    std::atomic<bool> leaderboard_live{}; // leaderboards[0] belongs to the throwdown running now
    std::atomic<bool> local_entered{};    // the server just made an event of the local player's queue
    std::atomic<std::uint32_t> server_active{}; // turn-based: whose turn the local server started last
    std::array<std::atomic<std::uint32_t>, 16> players{};
    // `throwdown trace`: every authored graph run in the window, per realm.
    struct Seen { std::uint32_t count{}; std::uint64_t first_ms{}, last_ms{}; };
    std::atomic<bool> tracing{};
    std::mutex trace_mutex;
    std::unordered_map<std::uint64_t, Seen> trace; // (realm << 32 | graph hash)
    std::uint64_t trace_start_ms{}, trace_end_ms{};
    std::atomic<unsigned> participant_logs{};
};
Lab& lab() { static auto* value = new Lab; return *value; }
thread_local bool injecting = false;
// The virtual challenge participant FindPlayerById just failed to find: the GetPlayerName
// right after it (the graphs' GetPlayerName(FindPlayerById(uid))) names it.
thread_local std::uint32_t missing_named_player = 0;
// A relayed player a party beacon graph just failed to find (their beacon's owner): the next
// GetPlayerName answers their session name. And the Steam ID FindPlayerByName could not find
// by that name, for the GetPlayerPersonaId after it.
thread_local std::uint32_t missing_relay_player = 0;
thread_local std::uint64_t missing_persona = 0;
bool challenge_named(std::uint32_t id) {
    if (!id) return false;
    for (const auto& slot : lab().challenge_named) if (slot.load(std::memory_order_relaxed) == id) return true;
    return false;
}

// Guarded same-process copies: realm() and the hooks run for events and expression
// graphs many times a frame, where a system call per read added up.
template<class T> T read(Address at) {
    T result{};
    if (!memory::peek(at, result)) throw std::runtime_error("Throwdown lab: memory unavailable");
    return result;
}
std::uint32_t realm() {
    const auto base = lab().base;
    Address context{};
    reinterpret_cast<Address (*)(Address*)>(base + engine::current_context)(&context);
    const auto offset = read<std::uint32_t>(base + engine::context_type_offset);
    return context && offset < 0x1000000 ? read<std::uint32_t>(context + offset) : 0;
}
const char* realm_name(std::uint32_t r) { return r == client_realm ? "client" : r == server_realm ? "server" : "?"; }

// ---------------------------------------------------------------------------
// Reflection of data-defined types. A type object points at its record:
// {u32 hash, u16 flags (kind = bits 5-9), u16 size, ..., u16 fieldCount @+0x2a,
// field table @+0x60}, 0x18-byte fields {u32 nameHash, u16 offset @+8 (upper half
// holds flags on data-defined types), type object @+0x10}.
constexpr unsigned kind_struct = 2, kind_reference = 3, kind_array = 4, kind_string = 7;
struct TypeView { Address record{}; unsigned kind{}; std::uint16_t size{}; };
TypeView view_type(Address object) {
    TypeView v;
    v.record = read<Address>(object);
    v.kind = (read<std::uint16_t>(v.record + 4) >> 5) & 0x1f;
    v.size = read<std::uint16_t>(v.record + 6);
    return v;
}
struct Field { Address type{}; std::uint32_t offset{}; std::uint16_t size{}; unsigned kind{}; };
std::optional<Field> find_field(Address type, std::uint32_t name_hash) {
    const auto view = view_type(type);
    if (view.kind != kind_struct) return {};
    const auto count = read<std::uint16_t>(view.record + 0x2a);
    const auto fields = read<Address>(view.record + 0x60);
    if (!fields || count > 96) return {};
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = fields + i * 0x18;
        if (read<std::uint32_t>(entry) != name_hash) continue;
        Field field{read<Address>(entry + 0x10), read<std::uint16_t>(entry + 8)};
        if (!field.type) return {};
        const auto f = view_type(field.type);
        field.size = f.size; field.kind = f.kind;
        return field;
    }
    return {};
}
// Whether `type` has a field of `size` bytes at `offset` (payload layouts read off the graphs).
bool field_at(Address type, std::uint32_t offset, std::uint16_t size) {
    const auto view = view_type(type);
    if (view.kind != kind_struct) return false;
    const auto count = read<std::uint16_t>(view.record + 0x2a);
    const auto fields = read<Address>(view.record + 0x60);
    if (!fields || count > 96) return false;
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = fields + i * 0x18;
        if (read<std::uint16_t>(entry + 8) != offset) continue;
        const auto field_type = read<Address>(entry + 0x10);
        return field_type && view_type(field_type).size == size;
    }
    return false;
}
std::string read_string(Address at) {
    const auto text = read<Address>(at);
    std::string chars;
    // Up to 256 characters, copied to the end of a page at a time (the bytes after
    // the terminator may belong to a page that is not mapped).
    std::array<char, 256> chunk{};
    while (text && chars.size() < chunk.size()) {
        const auto from = text + chars.size();
        const auto size = std::min<std::size_t>(chunk.size() - chars.size(), 0x1000 - from % 0x1000);
        if (!memory::peek_bytes(from, chunk.data(), size)) break;
        const auto end = std::find(chunk.data(), chunk.data() + size, '\0');
        chars.append(chunk.data(), end);
        if (end != chunk.data() + size) break;
    }
    return chars;
}
void assign_string(Address at, std::string_view text) {
    reinterpret_cast<void (*)(Address, const char*, std::uint32_t)>(lab().base + throwdowns::string_assign_chars)(
        at, text.data(), static_cast<std::uint32_t>(text.size()));
}

// Build-independent byte stream of a value (strings and arrays by content), so a
// recorded CreateThrowdownQueueParams can be rebuilt inside another event instance.
void put_u16(std::vector<std::uint8_t>& out, std::size_t value) {
    if (value > 0xffff) throw std::runtime_error("Throwdown lab: element count exceeds the stream format");
    out.push_back(static_cast<std::uint8_t>(value)); out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void capture_value(Address object, Address value, std::vector<std::uint8_t>& out, unsigned depth) {
    if (depth > 8 || out.size() > 8192) throw std::runtime_error("Throwdown lab: value deeper or larger than expected");
    const auto type = view_type(object);
    switch (type.kind) {
    case kind_struct: {
        const auto count = read<std::uint16_t>(type.record + 0x2a);
        const auto fields = read<Address>(type.record + 0x60);
        if (count > 96 || (count && !fields)) throw std::runtime_error("Throwdown lab: struct layout differs");
        for (unsigned i = 0; i < count; ++i)
            capture_value(read<Address>(fields + i * 0x18 + 0x10), value + read<std::uint16_t>(fields + i * 0x18 + 8), out, depth + 1);
        return;
    }
    case kind_string: {
        const auto chars = read_string(value);
        put_u16(out, chars.size());
        out.insert(out.end(), chars.begin(), chars.end());
        return;
    }
    case kind_array: {
        const auto data = read<Address>(value);
        const auto count = data ? read<std::uint32_t>(data - 4) & 0x7fffffffU : 0U;
        const auto element = read<Address>(type.record + 0x30);
        if (count > 256 || !element) throw std::runtime_error("Throwdown lab: array layout differs");
        const auto stride = view_type(element).size;
        put_u16(out, count);
        for (unsigned i = 0; i < count; ++i) capture_value(element, data + std::size_t{i} * stride, out, depth + 1);
        return;
    }
    case kind_reference: return; // an asset pointer is not portable; the default stays
    default: {
        if (!type.size || type.size > 64) throw std::runtime_error("Throwdown lab: scalar size differs");
        std::array<std::uint8_t, 64> bytes{};
        if (!memory::read_bytes(value, bytes.data(), type.size)) throw std::runtime_error("Throwdown lab: value unreadable");
        out.insert(out.end(), bytes.begin(), bytes.begin() + type.size);
    }
    }
}
struct StreamReader {
    const std::vector<std::uint8_t>& bytes; std::size_t at{};
    const std::uint8_t* take(std::size_t size) {
        if (size > bytes.size() - at) throw std::runtime_error("Throwdown lab: stream truncated");
        at += size; return bytes.data() + at - size;
    }
    std::size_t u16() { const auto* p = take(2); return p[0] | (std::size_t{p[1]} << 8); }
};
void restore_value(Address object, Address value, StreamReader& in, unsigned depth) {
    if (depth > 8) throw std::runtime_error("Throwdown lab: stream deeper than expected");
    const auto type = view_type(object);
    switch (type.kind) {
    case kind_struct: {
        const auto count = read<std::uint16_t>(type.record + 0x2a);
        const auto fields = read<Address>(type.record + 0x60);
        if (count > 96 || (count && !fields)) throw std::runtime_error("Throwdown lab: struct layout differs");
        for (unsigned i = 0; i < count; ++i)
            restore_value(read<Address>(fields + i * 0x18 + 0x10), value + read<std::uint16_t>(fields + i * 0x18 + 8), in, depth + 1);
        return;
    }
    case kind_string: {
        const auto length = in.u16();
        if (length > 1024) throw std::runtime_error("Throwdown lab: string too long");
        const auto* text = reinterpret_cast<const char*>(in.take(length));
        if (length) assign_string(value, std::string_view(text, length));
        return;
    }
    case kind_array: {
        const auto count = in.u16();
        const auto element = read<Address>(type.record + 0x30);
        if (count > 256 || !element) throw std::runtime_error("Throwdown lab: array layout differs");
        if (!count) return; // the default-constructed instance already holds the empty array
        const auto stride = view_type(element).size;
        if (!stride) throw std::runtime_error("Throwdown lab: array element size differs");
        auto owner = read<Address>(value);
        const auto block = reinterpret_cast<Address (*)(Address*, std::size_t, std::size_t, Address)>(lab().base + engine::array_allocate)(
            &owner, std::size_t{count} * stride + 8, 8, 0);
        if (!block) throw std::runtime_error("Throwdown lab: array allocation failed");
        std::memset(reinterpret_cast<void*>(block), 0, std::size_t{count} * stride + 8);
        const auto data = block + 8;
        *reinterpret_cast<std::uint32_t*>(data - 8) = static_cast<std::uint32_t>(count);
        *reinterpret_cast<std::uint32_t*>(data - 4) = static_cast<std::uint32_t>(count);
        *reinterpret_cast<Address*>(value) = data;
        for (std::size_t i = 0; i < count; ++i) restore_value(element, data + i * stride, in, depth + 1);
        return;
    }
    case kind_reference: return;
    default:
        if (!type.size || type.size > 64) throw std::runtime_error("Throwdown lab: scalar size differs");
        std::memcpy(reinterpret_cast<void*>(value), in.take(type.size), type.size);
    }
}

// Consumes one value of `object` from the stream without writing anything.
void skip_value(Address object, StreamReader& in, unsigned depth) {
    if (depth > 8) throw std::runtime_error("Throwdown lab: stream deeper than expected");
    const auto type = view_type(object);
    switch (type.kind) {
    case kind_struct: {
        const auto count = read<std::uint16_t>(type.record + 0x2a);
        const auto fields = read<Address>(type.record + 0x60);
        for (unsigned i = 0; i < count; ++i) skip_value(read<Address>(fields + i * 0x18 + 0x10), in, depth + 1);
        return;
    }
    case kind_string: in.take(in.u16()); return;
    case kind_array: {
        const auto count = in.u16();
        const auto element = read<Address>(type.record + 0x30);
        for (std::size_t i = 0; i < count; ++i) skip_value(element, in, depth + 1);
        return;
    }
    case kind_reference: return;
    default: in.take(type.size);
    }
}
// Restores only the listed top-level fields of a struct from a full stream of it
// (what MPThrowdownCoordinator.OnHostSetThrowdownParameters merges onto a queue).
void overlay_fields(Address object, Address value, const std::vector<std::uint8_t>& bytes,
                    std::initializer_list<std::uint32_t> wanted) {
    StreamReader in{bytes};
    const auto record = view_type(object).record;
    const auto count = read<std::uint16_t>(record + 0x2a);
    const auto fields = read<Address>(record + 0x60);
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = fields + i * 0x18;
        const auto field_type = read<Address>(entry + 0x10);
        if (std::ranges::find(wanted, read<std::uint32_t>(entry)) != wanted.end()) {
            // An array field that already holds elements would leak them; overlaid
            // arrays come from default-constructed (empty) instances only.
            restore_value(field_type, value + read<std::uint16_t>(entry + 8), in, 1);
        } else skip_value(field_type, in, 1);
    }
}

std::string describe_params(Address type, Address value) {
    const auto text = [&](std::uint32_t hash) {
        const auto f = find_field(type, hash);
        return f && f->kind == kind_string ? read_string(value + f->offset) : std::string{};
    };
    const auto u32 = [&](std::uint32_t hash) {
        const auto f = find_field(type, hash);
        return f && f->size == 4 ? read<std::uint32_t>(value + f->offset) : 0U;
    };
    std::array<float, 3> spawn{};
    if (const auto f = find_field(type, field_spawn); f && f->size >= 12) memory::read(value + f->offset, spawn);
    const auto party = find_field(type, field_party_only);
    return std::format("series {} max players {} host {} party-only {} at ({:.1f}, {:.1f}, {:.1f})", text(field_series),
        u32(field_max_players), u32(field_host), party ? read<std::uint8_t>(value + party->offset) : 0,
        spawn[0], spawn[1], spawn[2]);
}

// ---------------------------------------------------------------------------
// Hooks.
// Every participant change: server natives (set/add/remove) and the client's diff
// of the replicated list. The client ids show what survived replication.
std::vector<std::uint32_t> id_list(const Address* vector) {
    std::vector<std::uint32_t> ids;
    if (!vector) return ids;
    const auto begin = read<Address>(reinterpret_cast<Address>(vector));
    const auto end = read<Address>(reinterpret_cast<Address>(vector) + 8);
    for (auto at = begin; at && at < end && at < begin + 4 * 64; at += 4) ids.push_back(read<std::uint32_t>(at));
    return ids;
}
void participants_changed_hook(Address activity, const Address* added, const Address* removed) {
    {
        profile_runtime::PreserveError preserve;
        try {
            auto& l = lab();
            const auto current = realm();
            const auto joined = id_list(added);
            if (l.participant_logs.fetch_add(1) < 400) {
                const auto text = [](const std::vector<std::uint32_t>& ids) {
                    std::string out;
                    for (const auto id : ids) out += std::format("{}{:#x}", out.empty() ? "" : " ", id);
                    return out;
                };
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown lab: participants changed ({} realm, activity {:#x}): added [{}] removed [{}].",
                    realm_name(current), activity, text(joined), text(id_list(removed)));
            }
            // The server sets an event's participants when a queue the local player is
            // in becomes the event (ThrowdownRegistration.QueueFilled), host first.
            const auto local = local_native_player_id();
            // The coop challenge that just started: its activity, and its end for the local player.
            if (current == server_realm && local) {
                if (std::ranges::find(joined, local) != joined.end() && l.challenge_activity_pending.exchange(false))
                    l.challenge_activity.store(activity, std::memory_order_release);
                const auto left = id_list(removed);
                if (activity && activity == l.challenge_activity.load(std::memory_order_acquire) &&
                    std::ranges::find(left, local) != left.end()) {
                    l.challenge_activity.store(0, std::memory_order_release);
                    throwdown_relay_local({ThrowdownLocalAction::Kind::challenge_ended});
                }
            }
            if (current == server_realm && local) {
                const auto left = id_list(removed);
                if (std::ranges::find(left, local) != left.end()) {
                    // Quit (LeaveInProgress), or the event let the player go: its boards are
                    // no longer the local player's and nothing is relayed for it any more.
                    {
                        std::lock_guard lock(l.mutex);
                        l.leaderboard_live.store(false, std::memory_order_release);
                        l.leaderboards = {};
                        l.event_handle = 0;
                    }
                    throwdown_relay_local({ThrowdownLocalAction::Kind::ended});
                }
            }
            if (current == server_realm && local && std::ranges::find(joined, local) != joined.end()) {
                l.local_entered.store(true, std::memory_order_release);
                ThrowdownLocalAction action{ThrowdownLocalAction::Kind::entered};
                action.participants = joined;
                throwdown_relay_local(std::move(action));
            }
        } catch (...) {}
    }
    lab().participants_changed(activity, added, removed);
}
// ThrowdownRegistration.QueueFilled hands its queue to SetMpActivityParticipants in join
// order, and that list is the turn order. Joins that cross on the network leave the
// machines with different orders, so a linked throwdown's ids are put into the one order
// every machine derives the same way (throwdown_relay_order) first.
bool skate_running();
Address set_participants_hook(Address activity, Address array, Address a3, Address a4) {
    {
        profile_runtime::PreserveError preserve;
        try {
            const auto data = array ? read<Address>(array) : 0;
            const auto count = data ? read<std::uint32_t>(data - 4) & 0x7fffffffU : 0U;
            if (count >= 2 && count <= 64 && realm() == server_realm) {
                std::vector<std::uint32_t> ids(count);
                for (std::size_t i = 0; i < count; ++i) ids[i] = read<std::uint32_t>(data + 4 * i);
                const auto before = ids;
                const bool reordered = throwdown_relay_order(ids) && ids != before;
                if (skate_running()) {
                    // The first S.K.A.T.E. order (Round End keeps it from here, see above).
                    std::lock_guard lock(lab().skate_mutex);
                    lab().skate_order = ids;
                    lab().setter_missed.store(false);
                    lab().round_letters = 0; // nobody has a letter yet
                }
                if (reordered) {
                    std::memcpy(reinterpret_cast<void*>(data), ids.data(), ids.size() * 4);
                    std::string from, to;
                    for (std::size_t i = 0; i < count; ++i) {
                        from += std::format(" {:#x}", before[i]);
                        to += std::format(" {:#x}", ids[i]);
                    }
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdowns: linked event participants put in the shared order:{} ->{}.", from, to);
                }
            }
        } catch (...) {}
    }
    return lab().set_participants(activity, array, a3, a4);
}
bool skate_running() {
    std::lock_guard lock(lab().mutex);
    return lab().series == "ThrowdownSkate";
}
std::vector<std::uint32_t> read_ids(Address array) {
    const auto data = array ? read<Address>(array) : 0;
    const auto count = data ? read<std::uint32_t>(data - 4) & 0x7fffffffU : 0U;
    if (count > 64) throw std::runtime_error("Throwdown lab: id array larger than expected");
    std::vector<std::uint32_t> ids(count);
    for (std::size_t i = 0; i < count; ++i) ids[i] = read<std::uint32_t>(data + 4 * i);
    return ids;
}
// S.K.A.T.E. as played here: the setter keeps setting while they land their trick and someone
// fails to copy it; when the setter misses, or everyone copies it, the next player sets. The
// authored Round End instead reorders by RoundRobin, which swaps the first and last player
// every round (the setter changed even after landing; with three players the middle one never
// set). The server raises RequestRoundEnd only when the setter misses; letters handed out this
// round come from the S.K.A.T.E. HUD's list (unknown: the setter keeps it).
Address order_participants_hook(Address component, Address array, Address a3, Address a4) {
    {
        profile_runtime::PreserveError preserve;
        try {
            auto& l = lab();
            if (array && realm() == server_realm && skate_running()) {
                auto ids = read_ids(array);
                std::lock_guard lock(l.skate_mutex);
                auto next = l.skate_order;
                auto sorted_ids = ids, sorted_next = next;
                std::ranges::sort(sorted_ids);
                std::ranges::sort(sorted_next);
                if (!next.empty() && sorted_ids == sorted_next) {
                    const bool missed = l.setter_missed.exchange(false);
                    const auto letters = skate_letters_total();
                    const bool copied = !missed && letters >= 0 && l.round_letters >= 0 && letters == l.round_letters;
                    l.round_letters = letters;
                    if (missed || copied) std::rotate(next.begin(), next.begin() + 1, next.end());
                    std::memcpy(reinterpret_cast<void*>(read<Address>(array)), next.data(), next.size() * 4);
                    std::string text;
                    for (const auto id : next) text += std::format(" {:#x}", id);
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdowns: S.K.A.T.E. next round order:{} ({}).", text,
                        missed ? "the setter missed; the next player sets"
                               : copied ? "everyone copied the trick; the next player sets"
                                        : "the setter landed and sets again");
                    ids = next;
                }
                l.skate_order = ids;
            }
        } catch (...) {}
    }
    return lab().order_participants(component, array, a3, a4);
}
// GetPlayerName returns without writing its output for a player id with no native
// player, so authored loops over participants reuse the previous participant's
// name and send that player someone else's rank. Answer "" instead, as the native
// itself does when it has no realm.
void player_name_hook(const Address* player, Address out) {
    const auto named = std::exchange(missing_named_player, 0U);
    const auto relayed = std::exchange(missing_relay_player, 0U);
    {
        profile_runtime::PreserveError preserve;
        try {
            Address ref{}, object{};
            if (player && out && memory::peek(reinterpret_cast<Address>(player), ref) &&
                (!ref || (memory::peek(ref, object) && !object))) {
                // A virtual coop challenge participant gets a name of its own (virtual_player_names.h),
                // a party beacon's owner their session name.
                const auto name = named ? virtual_eid(named) : relayed ? throwdown_relay_player_name(relayed) : std::string{};
                reinterpret_cast<void (*)(Address, const char*, std::uint32_t)>(lab().base + throwdowns::string_assign)(
                    out, name.c_str(), static_cast<std::uint32_t>(name.size()));
                return;
            }
        } catch (...) {}
    }
    lab().player_name(player, out);
}
std::uint32_t executing_graph();
Address* find_player_by_id_hook(Address* out, std::uint32_t id) {
    const auto result = lab().find_player_by_id(out, id);
    missing_named_player = 0;
    missing_relay_player = 0;
    const bool named = challenge_named(id);
    if (named || (id >= 0x300 && id < 0x3ff)) {
        profile_runtime::PreserveError preserve;
        Address player{};
        if (out && memory::peek(reinterpret_cast<Address>(out), player) && !player) {
            if (named) missing_named_player = id;
            else if (std::ranges::find(beacon_name_graphs, executing_graph()) != beacon_name_graphs.end())
                missing_relay_player = id;
        }
    }
    return result;
}
// The beacon prompt's interaction finds its owner by the name the graphs above stored: a relayed
// player has no native player, so remember their Steam ID for the persona lookup that follows,
// which then opens their player card (a UIPlayerInfo keyed by it, native_party).
Address* find_player_by_name_hook(Address* out, const Address* name) {
    const auto result = lab().find_player_by_name(out, name);
    missing_persona = 0;
    profile_runtime::PreserveError preserve;
    try {
        Address player{}, text{};
        if (out && name && memory::peek(reinterpret_cast<Address>(out), player) && !player &&
            executing_graph() == graph_beacon_inspect && memory::peek(reinterpret_cast<Address>(name), text) && text) {
            std::array<char, 129> chars{};
            if (memory::peek_bytes(text, chars.data(), chars.size() - 1)) {
                const std::string_view typed(chars.data());
                if (!typed.empty()) missing_persona = throwdown_relay_player_named(typed);
            }
        }
    } catch (...) {}
    return result;
}
void player_persona_id_hook(const Address* player, std::uint64_t* out) {
    const auto persona = std::exchange(missing_persona, 0ULL);
    lab().player_persona_id(player, out);
    profile_runtime::PreserveError preserve;
    Address ref{};
    if (persona && out && player && memory::peek(reinterpret_cast<Address>(player), ref) && !ref) *out = persona;
}
// A native u32 array's elements (data pointer, count at -4).
std::vector<std::uint32_t> native_u32s(Address wrapper) {
    std::vector<std::uint32_t> values;
    const auto data = wrapper ? read<Address>(wrapper) : 0;
    if (!data) return values;
    const auto count = read<std::uint32_t>(data - 4) & 0x7fffffff;
    for (std::uint32_t i = 0; i < count && i < 64; ++i) values.push_back(read<std::uint32_t>(data + 4 * i));
    return values;
}
void remember_player(std::uint32_t id);
// Every challenge start, solo or coop (StartChallenge): logged, and with `challenge virtual <n>`
// armed, given n virtual participants and made coop, the way a linked copy would start.
void start_challenge_hook(Address coordinator, Address series, Address* participants, Address id, bool coop,
                          std::uint64_t group, std::uint32_t host, bool available, bool contest) {
    auto& l = lab();
    {
        profile_runtime::PreserveError preserve;
        try {
            // The Coop button starts with the requester's game group as participants and its
            // leader as host; without a native group that leader is 0. The starting player hosts
            // (every machine hosts its own copy of a linked challenge).
            if (!host) {
                const auto first = native_u32s(reinterpret_cast<Address>(participants));
                if (!first.empty()) host = first.front();
            }
            // Armed before a multiplayer session: a linked challenge has only real players.
            const auto armed = multiplayer_session_active() ? 0U : l.challenge_armed.exchange(0);
            for (auto& slot : l.challenge_named) slot.store(0, std::memory_order_relaxed);
            const auto before = native_u32s(reinterpret_cast<Address>(participants));
            const auto series_text = series ? read_string(series) : std::string{};
            const auto id_text = id ? read_string(id) : std::string{};
            // The other players of a linked coop challenge (throwdown_relay.cpp), or the lab's.
            std::vector<std::uint32_t> others;
            if (armed) {
                for (unsigned k = 0; k < armed && k < l.challenge_named.size(); ++k) {
                    others.push_back(challenge_virtual_base + 1 + k);
                    remember_player(challenge_virtual_base + 1 + k); // UI lookups answer it with the local record
                }
            } else if (const auto plan = throwdown_relay_challenge_players(series_text, id_text)) {
                others = *plan;
            }
            if (!others.empty() && participants) {
                for (std::size_t k = 0; k < others.size() && k < l.challenge_named.size(); ++k) {
                    const auto player = others[k];
                    l.challenge_named[k].store(player, std::memory_order_relaxed);
                    if (std::ranges::find(before, player) != before.end()) continue;
                    auto* slot = reinterpret_cast<std::uint32_t* (*)(Address*, Address)>(l.base + throwdowns::array_append_u32)(participants, 0);
                    if (slot) *slot = player;
                }
                coop = true;
                // Available (WasActivityAvailableToHost) gates the backend begin, without which the end
                // never reaches the local player (the rewards stay on "claiming"). The sender works it
                // out for the local player alone: a party member with challenges hidden, or without this
                // one, still plays the leader's challenge.
                available = true;
            }
            l.challenge_activity_pending.store(true, std::memory_order_release);
            set_native_challenge_coop(coop); // its objective list shows the coop objective
            {
                ThrowdownLocalAction action{ThrowdownLocalAction::Kind::challenge_started};
                action.series = series_text;
                action.challenge = id_text;
                action.add = contest;
                action.participants = native_u32s(reinterpret_cast<Address>(participants));
                throwdown_relay_local(std::move(action));
            }
            std::string ids;
            for (const auto player : native_u32s(reinterpret_cast<Address>(participants))) ids += std::format(" {:#x}", player);
            auto text = std::format("{} {} participants [{} ] coop {} group {:#x} host {:#x} available {} contest {}{}",
                series ? read_string(series) : std::string("?"), id ? read_string(id) : std::string("?"), ids, coop, group,
                host, available, contest, armed ? std::format(" (+{} virtual)", armed) : std::string{});
            logging::log(logging::Level::info, logging::Channel::progression, "Coop challenge lab: StartChallenge {}.", text);
            std::lock_guard lock(l.mutex);
            l.challenge_start = std::move(text);
        } catch (...) {}
    }
    l.start_challenge(coordinator, series, participants, id, coop, group, host, available, contest);
}
// Puts `count` elements of `element` bytes into a native array field that holds the empty array.
void fill_native_array(Address wrapper, const std::uint8_t* bytes, std::uint32_t count, std::uint32_t element) {
    if (!count) return;
    const auto current = read<Address>(wrapper);
    if (current && (read<std::uint32_t>(current - 4) & 0x7fffffff))
        throw std::runtime_error("Coop challenge lab: the event's array is not empty");
    const auto block = reinterpret_cast<Address (*)(Address, std::uint64_t, std::uint64_t, Address)>(
        lab().base + throwdowns::array_allocate)(wrapper, std::uint64_t{count} * element + 8, 8, 0);
    if (!block) throw std::runtime_error("Coop challenge lab: array allocation failed");
    *reinterpret_cast<std::uint32_t*>(block) = count;     // capacity
    *reinterpret_cast<std::uint32_t*>(block + 4) = count; // count
    std::memcpy(reinterpret_cast<void*>(block + 8), bytes, std::size_t{count} * element);
    *reinterpret_cast<Address*>(wrapper) = block + 8;
}

std::uint64_t now_ms();
// The graph whose natives are running now (the hooks below run inside it).
std::uint32_t executing_graph() {
    const auto vm = profile_runtime::executing_expression;
    if (!vm) return 0;
    const auto resource = *reinterpret_cast<const Address*>(vm + 0x38);
    return resource ? *reinterpret_cast<const std::uint32_t*>(resource + 0x10) : 0;
}
// Sender the running handler graph must see, or 0 for "the connection's own".
std::uint32_t injected_sender() {
    auto& l = lab();
    if (!l.any_injected_sent.load(std::memory_order_acquire)) return 0;
    const auto graph = executing_graph();
    if (!graph) return 0;
    std::lock_guard lock(l.sent_mutex);
    for (const auto& sent : l.sent) {
        const auto* h = handled(sent.event);
        if (h && std::ranges::find(h->graphs, graph) != h->graphs.end()) return sent.sender;
    }
    return 0;
}
void note_sent(std::uint32_t event, std::uint32_t sender) {
    if (!handled(event)) return;
    auto& l = lab();
    std::lock_guard lock(l.sent_mutex);
    const auto now = now_ms();
    // An event nobody handled within a few seconds (no such activity running) is dropped.
    while (!l.sent.empty() && now - l.sent.front().ms > 5000) l.sent.pop_front();
    if (l.sent.size() < 256) l.sent.push_back({event, sender, now});
    if (sender) l.any_injected_sent.store(true, std::memory_order_release);
}
// After a handler graph returns, its event is done.
void note_handled(std::uint32_t graph) {
    auto& l = lab();
    std::lock_guard lock(l.sent_mutex);
    for (auto it = l.sent.begin(); it != l.sent.end(); ++it) {
        const auto* h = handled(it->event);
        if (!h || std::ranges::find(h->graphs, graph) == h->graphs.end()) continue;
        if (it->sender) {
            static std::atomic<unsigned> logged{};
            if (logged.fetch_add(1) < 32)
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown lab: server graph {:#x} handled event {:#x} as player {:#x}.", graph, it->event, it->sender);
        }
        l.sent.erase(it);
        break;
    }
    bool injected{};
    for (const auto& sent : l.sent) injected = injected || sent.sender;
    if (!injected) l.any_injected_sent.store(false, std::memory_order_release);
}
std::uint32_t player_id_by_name_hook(const Address* name) {
    {
        profile_runtime::PreserveError preserve;
        try { if (const auto sender = injected_sender()) return sender; } catch (...) {}
    }
    return lab().player_id_by_name(name);
}
// A leaderboard's manager and its place in it, as SetLeaderboardPlayerScore finds them.
struct Board { Address manager{}; std::uint64_t key{}; int index = -1; std::size_t count{}; };
Board board_of(Address leaderboard) {
    Board b;
    const auto owner = read<Address>(leaderboard + 0x58);
    if (!(owner & 0xffffffff00000000ULL)) return b;
    const auto base = lab().base;
    b.manager = reinterpret_cast<Address (*)(Address, Address)>(base + throwdowns::component_lookup)(
        owner, base + throwdowns::leaderboard_manager_type);
    if (!b.manager) return b;
    b.key = read<std::uint64_t>(leaderboard + 0x38);
    const auto begin = read<Address>(b.manager + 0x78), end = read<Address>(b.manager + 0x80);
    if (end < begin || end - begin > 8 * 64) return b;
    b.count = (end - begin) / 8;
    for (std::size_t i = 0; i < b.count; ++i)
        if (read<std::uint64_t>(begin + 8 * i) == b.key) { b.index = static_cast<int>(i); break; }
    return b;
}
// The running throwdown's EventHandle: read off ThrowdownRegistration.QueueFilled on the
// server when the local player's queue became the event (pump_throwdown_lab), and kept up
// to date by the client's own requests. Injected requests carry it.
std::uint32_t live_event_handle() {
    std::lock_guard lock(lab().mutex);
    return lab().event_handle;
}
// Every row the local client writes: shows which leaderboards exist and what the
// native flag means per call site, and keeps them for `throwdown score`. The local
// player's own writes are relayed by the leaderboard's index in the event's manager
// (Spot Battle adds each line to the round board and sets the overall board to the best
// round); the index is the same on every machine, the manager's keys are not.
void leaderboard_score_hook(Address leaderboard, bool flag, std::uint32_t player, float score) {
    {
        profile_runtime::PreserveError preserve;
        const auto graph = executing_graph();
        const bool throwdown = std::ranges::find(throwdown_score_graphs, graph) != throwdown_score_graphs.end();
        try {
            const auto local = local_native_player_id();
            if (throwdown && player && player == local) {
                const auto b = board_of(leaderboard);
                static std::atomic<unsigned> described{};
                if (described.fetch_add(1) < 16)
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdown lab: own row on board {} of {} (manager {:#x}, graph {:#x}): {} {}.", b.index, b.count,
                        b.manager, executing_graph(), flag ? "add" : "set", score);
                if (b.index >= 0) {
                    ThrowdownLocalAction action{ThrowdownLocalAction::Kind::row};
                    action.board = static_cast<std::uint8_t>(b.index);
                    action.add = flag;
                    action.score = static_cast<std::int32_t>(std::lround(score));
                    throwdown_relay_local(std::move(action));
                }
            }
        } catch (...) {}
        try {
            auto& l = lab();
            std::lock_guard lock(l.mutex);
            // Only the throwdown's own boards are kept: rows for relayed players go there.
            if (throwdown && l.leaderboards[0] != leaderboard) {
                std::rotate(l.leaderboards.rbegin(), l.leaderboards.rbegin() + 1, l.leaderboards.rend());
                l.leaderboards[0] = leaderboard;
            }
            if (throwdown) l.leaderboard_live.store(true, std::memory_order_release);
            static std::atomic<unsigned> logged{};
            if (logged.fetch_add(1) < 64)
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown lab: leaderboard {:#x} row for player {:#x}: {} (flag {}, graph {:#x}{}).",
                    leaderboard, player, score, flag, graph, throwdown ? "" : ", not a throwdown board");
        } catch (...) {}
    }
    lab().leaderboard_score(leaderboard, flag, player, score);
}
// Condition probe: the operands and result of every AND / OR / NOT evaluated while a
// throwdown registration graph runs (in order), so a false term in the chain that
// decides whether a drop is shown and joinable can be read off the log.
constexpr std::array<std::uint32_t, 7> probed_graphs{
    0xb0c0ef5c,  // ThrowdownRegistration.InitWrapper
    0x170bc8dd,  // OnIsLocalPlayerInActivityChanged / OnMPActivityIdleStateChanged
    0xe7a43930,  // OnNetworkedEntityComponentDataUnmarshaled
    0x9c20247e,  // OnEntitlementsChanged
    0x95080714,  // ClientVars.OnGameGroupIdUpdated
    0x2713b85c,  // ThrowdownEventInteractable.GetIsEnabled
    0xcafb98b6}; // ThrowdownEventInteractable.OnHostAutoInteract
void probe(const char* op, int a, int b, bool result) {
    try {
        const auto graph = executing_graph();
        if (std::ranges::find(probed_graphs, graph) == probed_graphs.end()) return;
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) >= 1500) return;
        profile_runtime::PreserveError preserve;
        if (b < 0)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown lab probe {:08x}: {}({})={}", graph, op, a, static_cast<int>(result));
        else
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown lab probe {:08x}: {}({},{})={}", graph, op, a, b, static_cast<int>(result));
    } catch (...) {}
}
void and_probe(const bool* a, const bool* b, bool* out) { *out = *a && *b; probe("AND", *a, *b, *out); }
void or_probe(const bool* a, const bool* b, bool* out) { *out = *a || *b; probe("OR", *a, *b, *out); }
void not_probe(const bool* a, bool* out) { *out = !*a; probe("NOT", *a, -1, *out); }
std::uint32_t player_id_hook(const Address* player) {
    {
        profile_runtime::PreserveError preserve;
        try { if (const auto sender = injected_sender()) return sender; } catch (...) {}
    }
    return lab().player_id(player);
}

// ---------------------------------------------------------------------------
// Injection. Events are built in a natively constructed instance and sent through
// SendNetworkedEvent from the client realm, exactly as the local client's own
// requests are (route 5 reaches the local server's handlers).
struct Instance {
    Address type{}, allocator{}, value{};
    explicit Instance(Address t) : type(t) {
        if (!type) throw std::runtime_error("Throwdown lab: event type not found");
        const auto base = lab().base;
        allocator = read<Address>(base + throwdowns::event_allocator);
        value = reinterpret_cast<Address (*)(Address, Address)>(base + throwdowns::type_construct)(type, allocator);
        if (!value) throw std::runtime_error("Throwdown lab: event instance could not be constructed");
    }
    ~Instance() { reinterpret_cast<void (*)(Address, Address, Address)>(lab().base + throwdowns::type_destroy)(type, allocator, value); }
    Instance(const Instance&) = delete; Instance& operator=(const Instance&) = delete;
    void set_u32(std::uint32_t hash, std::uint32_t v, Address in = 0, Address at = 0) {
        const auto f = find_field(in ? in : type, hash);
        if (!f || f->size != 4) throw std::runtime_error(std::format("Throwdown lab: field {:#x} missing", hash));
        *reinterpret_cast<std::uint32_t*>((at ? at : value) + f->offset) = v;
    }
    // The sender EID: a string, or a struct whose first string field is it.
    std::string set_sender(std::string_view text) {
        auto f = find_field(type, field_payload);
        if (!f) throw std::runtime_error("Throwdown lab: event has no NetworkedEventPayload");
        Address at = value + f->offset;
        if (f->kind == kind_struct) {
            const auto record = view_type(f->type).record;
            const auto count = read<std::uint16_t>(record + 0x2a);
            const auto fields = read<Address>(record + 0x60);
            std::optional<std::uint32_t> inner;
            for (unsigned i = 0; i < count && fields; ++i) {
                const auto t = read<Address>(fields + i * 0x18 + 0x10);
                if (t && view_type(t).kind == kind_string) { inner = read<std::uint16_t>(fields + i * 0x18 + 8); break; }
            }
            if (!inner) throw std::runtime_error(std::format("Throwdown lab: payload struct ({} fields) has no string", count));
            at += *inner;
        } else if (f->kind != kind_string)
            throw std::runtime_error(std::format("Throwdown lab: payload kind {} size {}", f->kind, f->size));
        assign_string(at, text);
        return std::format("payload kind {} size {}", f->kind, f->size);
    }
    void set_string(std::uint32_t hash, std::string_view text) {
        const auto f = find_field(type, hash);
        if (!f || f->kind != kind_string) throw std::runtime_error(std::format("Throwdown lab: field {:#x} missing", hash));
        assign_string(value + f->offset, text);
    }
    void send(std::uint32_t route = 5, std::uint32_t sender = 0) {
        const auto hash = read<std::uint32_t>(read<Address>(type));
        note_sent(hash, sender);
        Address t = type;
        const std::uint32_t r = route;
        const std::array<Address, 4> arguments{reinterpret_cast<Address>(&t), value, reinterpret_cast<Address>(&r), 0};
        injecting = true;
        struct Done { ~Done() { injecting = false; } } done;
        reinterpret_cast<void (*)(Address, Address, const Address*)>(lab().base + throwdowns::send_event)(0, 0, arguments.data());
    }
};
// Whether `object` is the type object of the event `hash` right now. An event's type is
// defined by data the level's bundles carry: it is freed when the level is left and made
// again, anywhere, by the next one, and the memory the old one was in soon holds something
// else. The game constructs an event by calling through its type record (type_construct
// calls record+0x30), so a record that no longer names the event, or whose constructor is
// not code, must never be handed to it.
bool type_alive(Address object, std::uint32_t hash) {
    Address record{}, construct{};
    std::uint32_t name{};
    if (!memory::peek(object, record) || !memory::peek(record, name) || name != hash ||
        !memory::peek(record + 0x30, construct)) return false;
    const auto base = lab().base;
    if (construct >= base && construct - base < supported_build::game_image_size) return true;
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<const void*>(construct), &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & PAGE_GUARD) &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
Address type_of(std::uint32_t hash) {
    std::lock_guard lock(lab().mutex);
    const auto it = lab().types.find(hash);
    if (it == lab().types.end()) return 0;
    if (type_alive(it->second, hash)) return it->second;
    lab().types.erase(it); // gone with the level it was found in; the next scan finds this level's
    return 0;
}
std::string run_job(const Job& job) {
    auto& l = lab();
    std::string series; std::uint32_t mmid{}; std::vector<std::uint8_t> params, host;
    {
        std::lock_guard lock(l.mutex);
        series = l.series; mmid = l.mmid; params = l.params; host = l.host_params;
    }
    if (job.mmid) { series = job.series; mmid = job.mmid; }
    if (job.token) { params = job.placement; host = job.settings; }
    switch (job.kind) {
    case Job::Kind::ai: {
        Instance e(type_of(event_add_ai));
        e.set_u32(field_player_uid, job.uid);
        e.set_string(field_series, series);
        e.set_u32(field_mmid, mmid);
        e.send();
        return std::format("AddAIParticipantToCommunityEvent sent: player {:#x} into {} queue MMID {:#x}", job.uid, series, mmid);
    }
    case Job::Kind::spawn: {
        Instance e(type_of(event_debug_spawn));
        const auto p = find_field(e.type, field_parameters);
        if (!p || p->kind != kind_struct) throw std::runtime_error("Throwdown lab: DebugSpawn Parameters missing");
        const auto at = e.value + p->offset;
        if (job.position) {
            // No placement recorded: a plain queue of `series` where the skater stands.
            const auto& pos = *job.position;
            const auto vector = [&](std::uint32_t hash, std::size_t count) {
                const auto f = find_field(p->type, hash);
                if (!f || f->size < count * 4) throw std::runtime_error(std::format("Throwdown lab: vector field {:#x} missing", hash));
                std::array<float, 4> v{pos[0], pos[1], pos[2], 0};
                std::memcpy(reinterpret_cast<void*>(at + f->offset), v.data(), count * 4);
            };
            vector(field_spawn, 4);
            vector(field_teleport, 3);
            if (const auto f = find_field(p->type, field_series); f && f->kind == kind_string) assign_string(at + f->offset, job.series);
            if (const auto f = find_field(p->type, field_duration); f && f->size == 4) *reinterpret_cast<std::int32_t*>(at + f->offset) = 120;
            if (const auto f = find_field(p->type, field_bounds_scale); f && f->size == 4) *reinterpret_cast<float*>(at + f->offset) = 1.0f;
        } else {
            StreamReader reader{params};
            restore_value(p->type, at, reader, 0);
            if (reader.at != params.size()) throw std::runtime_error("Throwdown lab: recorded parameters have trailing bytes");
            // A peer's placement goes into the game as is: positions and scale must be real numbers.
            const auto check_floats = [&](std::uint32_t hash, unsigned count, float limit) {
                const auto f = find_field(p->type, hash);
                if (!f || f->size < count * 4) return;
                for (unsigned i = 0; i < count; ++i)
                    if (const auto v = read<float>(at + f->offset + i * 4); !std::isfinite(v) || std::fabs(v) > limit)
                        throw std::runtime_error("Throwdown lab: placement out of range");
            };
            check_floats(field_spawn, 3, 100000.0f);
            check_floats(field_teleport, 3, 100000.0f);
            check_floats(field_bounds_scale, 1, 1000.0f);
            // Update carries the placement, HostSet the host's settings: merge as the server does.
            if (!host.empty() && host != params) {
                // Clear the arrays the overlay replaces (the placement stream normally has them empty).
                for (const auto hash : {1330235344U /* ToggleValues */, 350847324U /* AdditionalParameters */})
                    if (const auto f = find_field(p->type, hash); f && f->kind == kind_array && read<Address>(at + f->offset))
                        if ((read<std::uint32_t>(read<Address>(at + f->offset) - 4) & 0x7fffffffU) != 0)
                            throw std::runtime_error("Throwdown lab: placement stream already has settings arrays");
                overlay_fields(p->type, at, host, {field_duration, field_max_players, 1330235344U, 295956140U /* WinCondition */,
                                                   350847324U});
            }
        }
        e.set_u32(field_host, job.uid, p->type, at);
        // One player cannot be a full queue: with MaxPlayers <= 1 the host alone
        // fills it, the event is created at once and the registration destroyed.
        if (const auto f = find_field(p->type, field_max_players); f && f->size == 4) {
            auto& max = *reinterpret_cast<std::uint32_t*>(at + f->offset);
            if (job.max_players) max = job.max_players; else if (max < 2) max = 4;
        }
        // Party-only resolves the host's game group through its native player,
        // which a virtual host lacks, so the host would never enter the queue.
        if (const auto f = find_field(p->type, field_party_only); f && f->size == 1) *reinterpret_cast<std::uint8_t*>(at + f->offset) = 0;
        if (const auto f = find_field(p->type, field_instance); f && f->kind == kind_string) assign_string(at + f->offset, "");
        e.set_u32(field_number_of_events, 1);
        const auto summary = describe_params(p->type, at);
        std::uint32_t player_limit{};
        if (const auto f = find_field(p->type, field_max_players); f && f->size == 4) player_limit = read<std::uint32_t>(at + f->offset);
        {
            std::lock_guard lock(l.mutex);
            // A relay mirror leaves the lab's own queue alone; its MMID goes to the relay.
            if (!job.token) {
                if (const auto f = find_field(p->type, field_series); f && f->kind == kind_string) l.series = read_string(at + f->offset);
                l.mmid = 0; // read back from the server graph (pump_throwdown_lab)
            }
            l.spawns.push_back({job.token, player_limit, now_ms()});
        }
        l.spawn_outstanding.store(true, std::memory_order_release);
        e.send();
        return "DebugSpawnThrowdownWithParams sent: " + summary;
    }
    case Job::Kind::destroy_event: {
        // ServerMpActivityBase.OnForceDestroyCurrentEvent destroys the event its sender takes part
        // in: sent as a player still in the one the local player just quit, it goes too.
        Instance e(type_of(event_force_destroy));
        e.send(5, job.uid);
        return std::format("ForceDestroyCurrentEvent sent as player {:#x}", job.uid);
    }
    case Job::Kind::remove: {
        // The server takes the sender as the player leaving (GetPlayerId(FindPlayerByName(Eid)))
        // and removes it from every queue: ordered attribution answers `uid` there.
        Instance e(type_of(event_remove_participant));
        e.send(5, job.uid);
        return std::format("RemoveParticipantFromCommunityEvent sent as player {:#x}", job.uid);
    }
    case Job::Kind::attempt: {
        // That player's own client's Skate_SubmitAttempt, sent as them (ordered attribution
        // answers their id to the handler's GetPlayerIdByName) with this event's handle.
        const auto handle = live_event_handle();
        if (!handle) throw std::runtime_error("Throwdown lab: no running event handle");
        Instance e(type_of(event_skate_submit));
        if (!field_at(e.type, attempt_landed_offset, 1) || !field_at(e.type, attempt_trick_offset, attempt_trick_size))
            throw std::runtime_error("Throwdown lab: Skate_SubmitAttempt layout differs");
        e.set_u32(field_event_handle, handle);
        *reinterpret_cast<std::uint8_t*>(e.value + attempt_landed_offset) = job.flag ? 1 : 0;
        std::memcpy(reinterpret_cast<void*>(e.value + attempt_trick_offset), job.trick.data(), job.trick.size());
        e.send(5, job.uid);
        return std::format("Skate_SubmitAttempt sent as player {:#x} ({}) for event {:#x}", job.uid,
                           job.flag ? "landed" : "missed", handle);
    }
    case Job::Kind::row: {
        // What that player's client wrote on its own machine, replayed on the same board here:
        // the row write behind SetLeaderboardPlayerScore, with the board's key from this
        // event's manager (found from any board the local client has written).
        Address leaderboard{};
        {
            std::lock_guard lock(l.mutex);
            if (l.leaderboard_live.load(std::memory_order_acquire)) leaderboard = l.leaderboards[0];
        }
        if (!leaderboard) return std::format("row of player {:#x} dropped: the throwdown ended", job.uid);
        const auto b = board_of(leaderboard);
        if (!b.manager || job.max_players >= b.count)
            throw std::runtime_error(std::format("Throwdown lab: board {} not in this event ({} boards)", job.max_players, b.count));
        const auto key = read<std::uint64_t>(read<Address>(b.manager + 0x78) + 8 * std::size_t{job.max_players});
        reinterpret_cast<void (*)(Address, const std::uint64_t*, bool, std::uint32_t, float)>(l.base + throwdowns::leaderboard_write)(
            b.manager, &key, job.flag, job.uid, job.score);
        return std::format("player {:#x} board {} {} {}", job.uid, job.max_players, job.flag ? "add" : "set", job.score);
    }
    case Job::Kind::total: {
        Address leaderboard{};
        std::uint32_t handle{};
        {
            std::lock_guard lock(l.mutex);
            if (l.leaderboard_live.load(std::memory_order_acquire)) leaderboard = l.leaderboards[0];
            handle = l.event_handle;
        }
        if (!leaderboard) return std::format("score {} of player {:#x} dropped: the throwdown ended", job.score, job.uid);
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) < 40)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdowns: relayed player {:#x} total {} written to leaderboard {:#x}.", job.uid, job.score, leaderboard);
        // The row (HUD rank, crown, results) takes the total as it is (flag 0 sets, 1 adds).
        l.leaderboard_score(leaderboard, false, job.uid, job.score);
        // The Jam server's own score list, as that player's client would have reported
        // it; needs the running event's handle, known once the local client has scored.
        std::string list = "server list skipped (event handle unknown)";
        if (handle) {
            if (const auto type = type_of(event_player_score)) {
                Instance e(type);
                const auto f = find_field(e.type, field_player_score);
                if (!f || f->size != 4) throw std::runtime_error("Throwdown lab: PlayerScore field missing");
                *reinterpret_cast<std::int32_t*>(e.value + f->offset) = static_cast<std::int32_t>(job.score);
                e.set_u32(field_event_handle, handle);
                e.send(5, job.uid);
                list = std::format("PlayerScoreUpdated sent for event {:#x}", handle);
            }
        }
        return std::format("player {:#x} total {} on leaderboard {:#x}; {}", job.uid, job.score, leaderboard, list);
    }
    case Job::Kind::start: {
        Instance e(type_of(event_force_start));
        e.set_string(field_series, series);
        e.set_u32(field_mmid, mmid);
        e.send();
        return std::format("ForceStartThrowdown sent for {} MMID {:#x}", series, mmid);
    }
    case Job::Kind::join: {
        // What the details page's Join button sends; the server takes the local
        // player (the sender) as the joiner.
        Instance e(type_of(event_add_participant));
        e.set_string(field_series, series);
        e.set_u32(field_mmid, mmid);
        e.send();
        return std::format("AddParticipantToCommunityEvent sent: local player joins {} queue MMID {:#x}", series, mmid);
    }
    case Job::Kind::end_turn: {
        const auto handle = job.mmid ? job.mmid : live_event_handle(); // mmid: an explicit handle
        if (!handle) throw std::runtime_error("Throwdown lab: no running event handle");
        Instance e(type_of(event_request_turn_end));
        const auto note = e.set_sender(std::format("{}{:x}", sender_marker, job.uid));
        e.set_u32(field_event_handle, handle);
        e.send(5, job.uid);
        return std::format("RequestTurnEnd sent as player {:#x} for event {:#x} ({})", job.uid, handle, note);
    }
    case Job::Kind::score: {
        Address leaderboard{};
        { std::lock_guard lock(l.mutex); leaderboard = l.leaderboards[job.max_players]; }
        if (!leaderboard) throw std::runtime_error("Throwdown lab: no leaderboard seen yet");
        l.leaderboard_score(leaderboard, job.flag, job.uid, job.score);
        return std::format("leaderboard {:#x}: player {:#x} score {} (flag {})", leaderboard, job.uid, job.score, job.flag);
    }
    case Job::Kind::criteria_attempt: {
        // The local client's last AttemptFinishedForCriteria, replayed as `uid`: its server merges
        // it into that player's criteria and the team total (ActivityBaseCriteria 0x4bc91cfd).
        Lab::CriteriaAttempt attempt;
        if (!job.criteria.empty()) {
            attempt.data = job.criteria;
            attempt.indexes = job.indexes;
            attempt.count = static_cast<std::uint32_t>(job.criteria.size() / criteria_data_size);
            attempt.index_count = static_cast<std::uint32_t>(job.indexes.size() / 4);
        } else {
            std::lock_guard lock(l.mutex);
            if (!l.criteria_attempt) throw std::runtime_error("Coop challenge lab: no attempt captured");
            attempt = *l.criteria_attempt;
        }
        Instance e(type_of(event_criteria_attempt));
        fill_native_array(e.value + 8, attempt.data.data(), attempt.count, criteria_data_size);
        fill_native_array(e.value + 0x10, attempt.indexes.data(), attempt.index_count, 4);
        e.send(5, job.uid);
        return std::format("AttemptFinishedForCriteria ({} criteria) sent as player {:#x}", attempt.count, job.uid);
    }
    case Job::Kind::challenge_start: {
        // The Start button's solo request, from the local client: StartChallenge then runs with the
        // relay's participants (start_challenge_hook).
        using Start = void (*)(Address, const Address*, const Address*, bool);
        const auto assign = reinterpret_cast<void (*)(Address, const char*, std::uint32_t)>(l.base + throwdowns::string_assign);
        const auto destroy = reinterpret_cast<void (*)(Address)>(l.base + throwdowns::string_destroy);
        Address series_text = l.base + throwdowns::empty_string, id_text = l.base + throwdowns::empty_string;
        struct Strings {
            void (*destroy)(Address); Address* a; Address* b;
            ~Strings() { destroy(reinterpret_cast<Address>(a)); destroy(reinterpret_cast<Address>(b)); }
        } strings{destroy, &series_text, &id_text};
        assign(reinterpret_cast<Address>(&series_text), job.series.c_str(), static_cast<std::uint32_t>(job.series.size()));
        assign(reinterpret_cast<Address>(&id_text), job.challenge.c_str(), static_cast<std::uint32_t>(job.challenge.size()));
        reinterpret_cast<Start>(l.base + throwdowns::challenge_start_request)(0, &series_text, &id_text, job.flag);
        return std::format("challenge start requested: {} {}{}", job.series, job.challenge, job.flag ? " (contest)" : "");
    }
    case Job::Kind::challenge_slam: {
        Instance e(type_of(event_slam_hit));
        *reinterpret_cast<std::int32_t*>(e.value + 8) = static_cast<std::int32_t>(job.max_players);
        std::uint32_t handle{};
        { std::lock_guard lock(l.mutex); handle = l.challenge_event_handle; }
        *reinterpret_cast<std::uint32_t*>(e.value + 0xc) = handle;
        e.send(5, job.uid);
        return std::format("SlamObjectHitNetworked (target {}, event {:#x}) sent as player {:#x}", job.max_players, handle, job.uid);
    }
    case Job::Kind::challenge_leave: {
        Instance e(type_of(event_leave_in_progress));
        e.send(5, job.uid);
        return std::format("LeaveInProgressRequested sent as player {:#x}", job.uid);
    }
    case Job::Kind::beacon_move: {
        // Never SpawnEntityForPlayer: with a beacon of theirs already standing it would ask the
        // local player (the connection) whether to move it. Move replaces it in one step.
        Instance e(type_of(event_beacon_move));
        const auto f = find_field(e.type, field_location);
        if (!f || f->size < sizeof(job.location)) throw std::runtime_error("Throwdown lab: beacon Location layout differs");
        std::memcpy(reinterpret_cast<void*>(e.value + f->offset), job.location.data(), sizeof(job.location));
        e.set_u32(field_entity_id, beacon_entity);
        e.send(5, job.uid);
        return std::format("MoveSpawnedEntityForPlayer (beacon at {:.1f}, {:.1f}, {:.1f}) sent as player {:#x}",
                           job.location[12], job.location[13], job.location[14], job.uid);
    }
    case Job::Kind::beacon_remove: {
        Instance e(type_of(event_beacon_despawn));
        e.set_u32(field_entity_id, beacon_entity);
        e.send(5, job.uid);
        return std::format("DespawnEntityForPlayer (beacon) sent as player {:#x}", job.uid);
    }
    case Job::Kind::onboarding_complete: {
        // As the local client's own OnboardingSetRuleComplete, for `uid` (EventHandleId in max_players).
        Instance e(type_of(event_onboarding_complete));
        *reinterpret_cast<std::uint32_t*>(e.value + 8) = job.max_players;
        e.send(5, job.uid);
        return std::format("OnboardingSetRuleComplete (event {:#x}) sent as player {:#x}", job.max_players, job.uid);
    }
    case Job::Kind::destroy: {
        Instance e(type_of(event_destroy));
        e.set_u32(field_mmid, mmid);
        e.send();
        return std::format("RequestThrowdownDestroyGlobal sent for MMID {:#x}", mmid);
    }
    }
    return {};
}

std::optional<std::uint32_t> parse_u32(std::string_view text) {
    int radix = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) { text.remove_prefix(2); radix = 16; }
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, radix);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}
void remember_player(std::uint32_t id) {
    for (auto& slot : lab().players) {
        auto current = slot.load();
        if (current == id) return;
        if (!current && slot.compare_exchange_strong(current, id)) return;
    }
}
// Type objects of the injected events; found on the heap and kept until the level is left.
// `worker`: called on a thread of its own, which scans as the party focus's does (background
// priority, only memory the game has in RAM). The full scan read every committed page and paged
// the game's memory in while it skated, every few seconds while a type was missing (2026-10-15).
std::string ensure_types(std::initializer_list<std::uint32_t> hashes, bool worker = false) {
    std::vector<NativeTypeQuery> queries;
    for (const auto hash : hashes) if (!type_of(hash)) queries.push_back({hash, 0});
    if (queries.empty()) return {};
    const auto world = lab().world.load(std::memory_order_acquire);
    const auto missing = find_native_types(queries, worker);
    std::lock_guard lock(lab().mutex);
    if (lab().world.load(std::memory_order_acquire) != world) return "the level changed while the event types were looked for";
    for (const auto& q : queries) if (q.object) lab().types[q.hash] = q.object;
    return missing ? std::format("{} event type(s) not found on the heap; is the game in the world?", missing) : std::string{};
}
std::string queue(Job job, std::initializer_list<std::uint32_t> hashes) {
    if (auto error = ensure_types(hashes); !error.empty()) return error;
    {
        std::lock_guard lock(lab().mutex);
        lab().jobs.push_back(job);
    }
    lab().pending.store(true, std::memory_order_release);
    return {};
}
} // namespace

void initialize_throwdown_lab(Address base) noexcept {
    auto& l = lab();
    if (l.base || !base) return;
    l.base = base;
    try {
        struct Helper { Address rva; std::array<unsigned char, 19> prefix; };
        for (const auto& helper : {Helper{throwdowns::string_assign, throwdowns::string_assign_prefix},
                                   Helper{throwdowns::type_construct, throwdowns::type_construct_prefix},
                                   Helper{throwdowns::type_destroy, throwdowns::type_destroy_prefix},
                                   Helper{throwdowns::component_lookup, throwdowns::component_lookup_prefix},
                                   Helper{throwdowns::leaderboard_write, throwdowns::leaderboard_write_prefix},
                                   Helper{throwdowns::leaderboard_event_handle, throwdowns::leaderboard_event_handle_prefix},
                                   Helper{throwdowns::array_append_u32, throwdowns::array_append_u32_prefix},
                                   Helper{throwdowns::array_allocate, throwdowns::array_allocate_prefix},
                                   Helper{throwdowns::challenge_start_request, throwdowns::challenge_start_request_prefix},
                                   Helper{throwdowns::string_destroy, throwdowns::string_destroy_prefix}})
            if (read<std::array<unsigned char, 19>>(base + helper.rva) != helper.prefix)
                throw std::runtime_error("Throwdown lab: helper fingerprint differs");
        struct Hook { Address rva; std::array<unsigned char, 19> prefix; void* replacement; void** original; };
        const std::array hooks{
            Hook{throwdowns::participants_changed, throwdowns::participants_changed_prefix,
                 reinterpret_cast<void*>(&participants_changed_hook), reinterpret_cast<void**>(&l.participants_changed)},
            Hook{throwdowns::player_name, throwdowns::player_name_prefix,
                 reinterpret_cast<void*>(&player_name_hook), reinterpret_cast<void**>(&l.player_name)},
            Hook{throwdowns::player_id_by_name, throwdowns::player_id_by_name_prefix,
                 reinterpret_cast<void*>(&player_id_by_name_hook), reinterpret_cast<void**>(&l.player_id_by_name)},
            Hook{throwdowns::player_id, throwdowns::player_id_prefix,
                 reinterpret_cast<void*>(&player_id_hook), reinterpret_cast<void**>(&l.player_id)},
            Hook{throwdowns::leaderboard_score, throwdowns::leaderboard_score_prefix,
                 reinterpret_cast<void*>(&leaderboard_score_hook), reinterpret_cast<void**>(&l.leaderboard_score)},
            Hook{throwdowns::set_participants, throwdowns::set_participants_prefix,
                 reinterpret_cast<void*>(&set_participants_hook), reinterpret_cast<void**>(&l.set_participants)},
            Hook{throwdowns::order_participants, throwdowns::order_participants_prefix,
                 reinterpret_cast<void*>(&order_participants_hook), reinterpret_cast<void**>(&l.order_participants)},
            Hook{throwdowns::find_player_by_id, throwdowns::find_player_by_id_prefix,
                 reinterpret_cast<void*>(&find_player_by_id_hook), reinterpret_cast<void**>(&l.find_player_by_id)},
            Hook{throwdowns::find_player_by_name, throwdowns::find_player_by_name_prefix,
                 reinterpret_cast<void*>(&find_player_by_name_hook), reinterpret_cast<void**>(&l.find_player_by_name)},
            Hook{throwdowns::player_persona_id, throwdowns::player_persona_id_prefix,
                 reinterpret_cast<void*>(&player_persona_id_hook), reinterpret_cast<void**>(&l.player_persona_id)},
            Hook{throwdowns::start_challenge, throwdowns::start_challenge_prefix,
                 reinterpret_cast<void*>(&start_challenge_hook), reinterpret_cast<void**>(&l.start_challenge)}};
        for (const auto& hook : hooks) if (read<std::array<unsigned char, 19>>(base + hook.rva) != hook.prefix)
            throw std::runtime_error("Throwdown lab: hook fingerprint differs");
        std::size_t prepared{};
        for (const auto& hook : hooks) {
            if (hook_prepare(reinterpret_cast<void*>(base + hook.rva), hook.replacement, hook.original) != HookOk) {
                while (prepared) hook_remove(reinterpret_cast<void*>(base + hooks[--prepared].rva));
                throw std::runtime_error("Throwdown lab: cannot prepare hooks");
            }
            ++prepared;
        }
        for (const auto& hook : hooks) if (hook_enable(reinterpret_cast<void*>(base + hook.rva)) != HookOk)
            throw std::runtime_error("Throwdown lab: cannot enable hooks");
        l.installed.store(true, std::memory_order_release);
    } catch (const std::exception& e) { logging::write(logging::Level::warning, logging::Channel::progression, e.what()); }
    // The condition probe replaces the game's AND / OR / NOT, which every authored
    // condition runs through; opt in with RESKATE_THROWDOWN_PROBE=1.
    if (!GetEnvironmentVariableW(L"RESKATE_THROWDOWN_PROBE", nullptr, 0)) return;
    try {
        struct Op { Address rva; std::array<unsigned char, 9> prefix; void* replacement; };
        const std::array ops{Op{throwdowns::bool_and, throwdowns::bool_and_prefix, reinterpret_cast<void*>(&and_probe)},
                             Op{throwdowns::bool_or, throwdowns::bool_or_prefix, reinterpret_cast<void*>(&or_probe)},
                             Op{throwdowns::bool_not, throwdowns::bool_not_prefix, reinterpret_cast<void*>(&not_probe)}};
        unsigned count{};
        for (const auto& op : ops)
            if (read<std::array<unsigned char, 9>>(base + op.rva) == op.prefix &&
                hook_prepare(reinterpret_cast<void*>(base + op.rva), op.replacement, nullptr) == HookOk &&
                hook_enable(reinterpret_cast<void*>(base + op.rva)) == HookOk) ++count;
        logging::log(logging::Level::info, logging::Channel::progression, "Throwdown lab: condition probe on {} of 3 boolean ops.", count);
    } catch (...) {}
}

void observe_throwdown_send(std::uint32_t hash, Address type, Address payload) noexcept {
    auto& l = lab();
    if (!l.installed.load(std::memory_order_acquire) || injecting || !type || !payload) return;
    if (handled(hash)) { try { note_sent(hash, 0); } catch (...) {} }
    if (hash == event_onboarding_complete) {
        // The local player finished the activity's onboarding: the virtual challenge participants
        // (who have no client to send theirs) finish it with them.
        try {
            const auto handle = read<std::uint32_t>(payload + 8);
            unsigned queued{};
            {
                std::lock_guard lock(l.mutex);
                l.types[hash] = type;
                l.challenge_event_handle = handle;
                for (const auto& slot : l.challenge_named)
                    if (const auto player = slot.load(std::memory_order_relaxed); player && l.jobs.size() < 256) {
                        Job job{Job::Kind::onboarding_complete, player};
                        job.max_players = handle;
                        l.jobs.push_back(std::move(job));
                        ++queued;
                    }
            }
            if (queued) {
                l.pending.store(true, std::memory_order_release);
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Coop challenge lab: onboarding done (event {:#x}); finishing it for {} virtual participant(s).", handle, queued);
            }
        } catch (...) {}
        return;
    }
    if (hash == event_criteria_attempt) {
        // The local player finished a challenge attempt: keep it for `challenge attempt`.
        try {
            Lab::CriteriaAttempt attempt;
            const auto data = read<Address>(payload + 8), indexes = read<Address>(payload + 0x10);
            attempt.count = data ? read<std::uint32_t>(data - 4) & 0x7fffffff : 0;
            attempt.index_count = indexes ? read<std::uint32_t>(indexes - 4) & 0x7fffffff : 0;
            if (attempt.count > 64 || attempt.index_count > 64) return;
            attempt.data.resize(std::size_t{attempt.count} * criteria_data_size);
            attempt.indexes.resize(std::size_t{attempt.index_count} * 4);
            if ((attempt.count && !memory::peek_bytes(data, attempt.data.data(), attempt.data.size())) ||
                (attempt.index_count && !memory::peek_bytes(indexes, attempt.indexes.data(), attempt.indexes.size())))
                return;
            std::string text;
            for (std::uint32_t i = 0; i < attempt.count; ++i) {
                float progress{}, maximum{};
                std::memcpy(&progress, attempt.data.data() + i * criteria_data_size + 4, 4);
                std::memcpy(&maximum, attempt.data.data() + i * criteria_data_size + 8, 4);
                std::int32_t index = -1;
                if (i < attempt.index_count) std::memcpy(&index, attempt.indexes.data() + i * 4, 4);
                text += std::format(" [{}] {}/{}{}", index, progress, maximum,
                                    attempt.data[i * criteria_data_size + 0x11] ? " done" : "");
            }
            if (l.criteria_attempts.fetch_add(1) < 40)
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Coop challenge lab: the local player finished an attempt:{}.", text.empty() ? " (no criteria)" : text);
            ThrowdownLocalAction action{ThrowdownLocalAction::Kind::challenge_attempt};
            action.criteria = attempt.data;
            action.indexes = attempt.indexes;
            {
                std::lock_guard lock(l.mutex);
                l.types[hash] = type;
                l.criteria_attempt = std::move(attempt);
            }
            throwdown_relay_local(std::move(action));
        } catch (...) {}
        return;
    }
    if (hash == event_beacon_spawn || hash == event_beacon_move || hash == event_beacon_despawn) {
        // The local player placed, moved or removed their party beacon: the other players show it.
        try {
            const bool despawn = hash == event_beacon_despawn;
            if (read<std::uint32_t>(payload + (despawn ? 8 : 0x50)) != beacon_entity) return;
            ThrowdownLocalAction action{despawn ? ThrowdownLocalAction::Kind::beacon_removed : ThrowdownLocalAction::Kind::beacon_placed};
            action.add = hash == event_beacon_move;
            if (!despawn) action.location = read<std::array<float, 16>>(payload + 0x10);
            { std::lock_guard lock(l.mutex); l.types[hash] = type; }
            throwdown_relay_local(std::move(action));
        } catch (...) {}
        return;
    }
    if (hash == event_slam_hit) {
        // The local player hit a slam target: every other copy disables it too.
        try {
            ThrowdownLocalAction action{ThrowdownLocalAction::Kind::challenge_slam};
            action.score = read<std::int32_t>(payload + 8);
            { std::lock_guard lock(l.mutex); l.types[hash] = type; }
            throwdown_relay_local(std::move(action));
        } catch (...) {}
        return;
    }
    if (hash == event_remove_participant) {
        throwdown_relay_local({ThrowdownLocalAction::Kind::left});
        return;
    }
    if (hash == 0x2ac7b512) { // LeaveInProgressRequested: the Quit action, in an event
        { std::lock_guard lock(l.mutex); l.types[hash] = type; }
        throwdown_relay_local({ThrowdownLocalAction::Kind::quit});
        return;
    }
    if (hash == event_host_exited) {
        try {
            ThrowdownLocalAction action{ThrowdownLocalAction::Kind::exited};
            if (const auto f = find_field(type, field_is_cancelling); f && f->size == 1)
                action.cancelling = read<std::uint8_t>(payload + f->offset) != 0;
            {
                std::lock_guard lock(l.mutex);
                action.series = l.series; action.mmid = l.hosted_mmid;
                action.placement = l.params; action.settings = l.host_params;
            }
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdowns: local host left the details page of {} MMID {:#x}{}.", action.series, action.mmid,
                action.cancelling ? " (cancelled)" : "");
            throwdown_relay_local(std::move(action));
        } catch (...) {}
        return;
    }
    if (hash == event_skate_submit) {
        // The local player's S.K.A.T.E. attempt, bit-exact, for the other players' servers.
        try {
            if (field_at(type, attempt_landed_offset, 1) && field_at(type, attempt_trick_offset, attempt_trick_size)) {
                ThrowdownLocalAction action{ThrowdownLocalAction::Kind::attempt};
                action.add = read<std::uint8_t>(payload + attempt_landed_offset) != 0;
                action.trick = read<std::array<std::uint8_t, 28>>(payload + attempt_trick_offset);
                action.player = l.server_active.load(std::memory_order_acquire);
                throwdown_relay_local(std::move(action));
            } else {
                logging::write(logging::Level::warning, logging::Channel::progression,
                    "Throwdowns: Skate_SubmitAttempt layout differs; attempts are not relayed.");
            }
        } catch (...) {}
    }
    if (hash == event_request_turn_end || hash == event_player_score || hash == event_skate_submit) {
        // ActivityLeaderboard.OnPlayerScoreUpdated (client realm) turns the local player's
        // own PlayerScoreUpdated into their leaderboard row, but offline the client never
        // receives the event it sends, so the row (HUD score, rank, crown) stayed at 0.
        // Write it here. PlayerScore is the running total (or best, by win condition)
        // and flag 1 would add it to the row, so it is set instead.
        if (hash == event_player_score) {
            try {
                // Jam only: Spot Battle writes its own rows (UpdateCurrentScore), on two boards.
                Address leaderboard{};
                bool jam{};
                {
                    std::lock_guard lock(l.mutex);
                    jam = l.series == "JamSession";
                    leaderboard = l.leaderboards[0];
                }
                const auto f = find_field(type, field_player_score);
                const auto local = local_native_player_id();
                if (jam && f && f->size == 4) {
                    const auto score = read<std::int32_t>(payload + f->offset);
                    if (leaderboard && local && l.leaderboard_score) {
                        l.leaderboard_score(leaderboard, false, local, static_cast<float>(score));
                        static std::atomic<unsigned> logged{};
                        if (logged.fetch_add(1) < 8)
                            logging::log(logging::Level::info, logging::Channel::progression,
                                "Throwdowns: own score {} written to leaderboard {:#x} (offline the client never receives its own PlayerScoreUpdated).",
                                score, leaderboard);
                    }
                    ThrowdownLocalAction action{ThrowdownLocalAction::Kind::score};
                    action.score = score;
                    throwdown_relay_local(std::move(action));
                }
            } catch (...) {}
        }
        try {
            if (const auto f = find_field(type, field_event_handle); f && f->size == 4) {
                const auto handle = read<std::uint32_t>(payload + f->offset);
                std::lock_guard lock(l.mutex);
                l.types[hash] = type;
                if (handle != l.event_handle) {
                    l.event_handle = handle;
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdown lab: running event handle {:#x} (from {:#x}).", handle, hash);
                }
            }
        } catch (...) {}
        return;
    }
    if (hash != event_create && hash != event_update && hash != event_host_set && hash != event_add_participant &&
        hash != event_force_start && hash != event_destroy) return;
    try {
        std::string series; std::uint32_t mmid{};
        std::vector<std::uint8_t> params; std::string note;
        bool placed{};
        if (const auto f = find_field(type, field_mmid); f && f->size == 4) mmid = read<std::uint32_t>(payload + f->offset);
        if (const auto f = find_field(type, field_series); f && f->kind == kind_string) series = read_string(payload + f->offset);
        if (const auto p = find_field(type, field_params); p && p->kind == kind_struct) {
            capture_value(p->type, payload + p->offset, params, 0);
            note = describe_params(p->type, payload + p->offset);
            if (const auto f = find_field(p->type, field_series); f && f->kind == kind_string)
                series = read_string(payload + p->offset + f->offset);
            std::array<float, 3> spawn{};
            if (const auto f = find_field(p->type, field_spawn); f && f->size >= 12) memory::read(payload + p->offset + f->offset, spawn);
            placed = spawn[0] != 0 || spawn[1] != 0 || spawn[2] != 0;
        }
        {
            std::lock_guard lock(l.mutex);
            l.types[hash] = type;
            if (!series.empty()) l.series = series;
            if (mmid) l.mmid = mmid;
            if ((hash == event_update || hash == event_host_set) && mmid) l.hosted_mmid = mmid;
            if (hash == event_host_set && !params.empty()) l.host_params = params;
            if (hash == event_create) { l.host_params.clear(); l.hosted_mmid = 0; }
            // Create, then Update (the placement) and HostSet (the host's final settings)
            // arrive in that order; keep the latest one that carries a position.
            if (!params.empty() && (placed || hash == event_create)) {
                l.params = std::move(params);
                l.params_note = std::string(hash == event_host_set ? "HostSet" : hash == event_update ? "Update" : "Create") + ": " + note;
            }
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown lab: local client sent {:#x}; series {} MMID {:#x}{}{}.", hash, l.series, l.mmid,
                note.empty() ? "" : "; ", note);
        }
        using Kind = ThrowdownLocalAction::Kind;
        const auto kind = hash == event_create ? Kind::created : hash == event_add_participant ? Kind::joined
                        : hash == event_force_start ? Kind::force_started : hash == event_destroy ? Kind::destroy_requested
                        : Kind::score;
        if (kind != Kind::score) {
            ThrowdownLocalAction action{kind};
            action.series = series;
            action.mmid = mmid;
            throwdown_relay_local(std::move(action));
        }
    } catch (...) {}
}

namespace {
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void trace_expression(Address vm) {
    auto& l = lab();
    const auto now = now_ms();
    if (now >= l.trace_end_ms) {
        if (!l.tracing.exchange(false)) return;
        std::vector<std::pair<std::uint64_t, Lab::Seen>> rows;
        {
            std::lock_guard lock(l.trace_mutex);
            rows.assign(l.trace.begin(), l.trace.end());
            l.trace.clear();
        }
        std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second.first_ms < b.second.first_ms; });
        logging::log(logging::Level::info, logging::Channel::progression,
            "Throwdown lab trace: {} graphs ran (realm, hash, count, first s, last s):", rows.size());
        for (const auto& [key, seen] : rows)
            logging::log(logging::Level::info, logging::Channel::progression, "Throwdown lab trace: {} {:08x} {} {:.2f} {:.2f}",
                realm_name(static_cast<std::uint32_t>(key >> 32)), static_cast<std::uint32_t>(key), seen.count,
                (seen.first_ms - l.trace_start_ms) / 1000.0, (seen.last_ms - l.trace_start_ms) / 1000.0);
        return;
    }
    // The expression that just returned is still alive: its resource and hash are plain reads.
    const auto resource = *reinterpret_cast<const Address*>(vm + 0x38);
    if (!resource) return;
    const auto hash = *reinterpret_cast<const std::uint32_t*>(resource + 0x10);
    // realm() without ReadProcessMemory: this runs after every expression while tracing.
    static const auto offset = read<std::uint32_t>(l.base + engine::context_type_offset);
    Address context{};
    reinterpret_cast<Address (*)(Address*)>(l.base + engine::current_context)(&context);
    const std::uint32_t current = context && offset < 0x1000000 ? *reinterpret_cast<const std::uint32_t*>(context + offset) : 0U;
    const auto key = (std::uint64_t{current} << 32) | hash;
    std::lock_guard lock(l.trace_mutex);
    auto& seen = l.trace[key];
    if (!seen.count++) seen.first_ms = now;
    seen.last_ms = now;
}

// The pump runs after every expression the game executes, thousands a frame, and almost all
// are none of the graphs it watches: one sorted lookup of the running graph decides
// (~0.5% of a busy multiplayer client frame before, profiled 2026-10-03). Keep this list in
// step with the graphs the pump's body compares. The handled graphs' zero padding stays in, as
// the body's search also matched a graph of 0.
bool lab_graph(Address vm) noexcept {
    static const auto watched = [] {
        std::vector<std::uint32_t> v{graph_celebration, graph_criteria_attempt, graph_queue_filled, graph_round_end_update,
                                     graph_turn_start, graph_skate_timer, graph_turn_end_begin,
                                     0xc33322b2, 0xc4a37359, 0xcc6467cf, 0xf073dd02};
        for (const auto& h : handled_events) v.insert(v.end(), h.graphs.begin(), h.graphs.end());
        std::ranges::sort(v);
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    const auto resource = *reinterpret_cast<const Address*>(vm + 0x38);
    const auto graph = resource ? *reinterpret_cast<const std::uint32_t*>(resource + 0x10) : 0U;
    return std::ranges::binary_search(watched, graph) || (resource && throttled_graph(graph));
}
} // namespace

void pump_throwdown_lab(Address vm) noexcept {
    auto& l = lab();
    if (vm && l.installed.load(std::memory_order_acquire) && lab_graph(vm)) {
        profile_runtime::PreserveError preserve;
        try {
            const auto resource = *reinterpret_cast<const Address*>(vm + 0x38);
            const auto graph = resource ? *reinterpret_cast<const std::uint32_t*>(resource + 0x10) : 0U;
            if (graph == graph_celebration && realm() == client_realm) {
                // CoopChallengeCelebrationLocation as the graph built it in its locals (page 2).
                const auto frame = read<std::uint32_t>(resource + 0x20);
                const auto locals = read<Address>(read<Address>(vm + 0x30) + ((frame + 15U) & ~15U) + 16);
                if (locals) {
                    ChallengeCelebration c;
                    constexpr std::array<std::uint32_t, 4> offsets{0x0, 0xa0, 0xd0, 0x100};
                    for (std::size_t i = 0; i < offsets.size(); ++i) c.spots[i] = read<std::array<float, 12>>(locals + offsets[i]);
                    c.count = read<std::uint32_t>(locals + 0x90);
                    std::string text;
                    for (const auto& spot : c.spots) {
                        text += " [";
                        for (const auto v : spot) text += std::format(" {:.2f}", v);
                        text += " ]";
                    }
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Coop challenge: celebration for {} player(s):{}.", c.count, text);
                    std::lock_guard lock(l.mutex);
                    l.celebration = c;
                }
            }
            if (graph == graph_criteria_attempt && l.criteria_runs.fetch_add(1) < 64) {
                // How often the global event's handler runs per attempt, and as whom.
                std::uint32_t sender{};
                {
                    std::lock_guard lock(l.sent_mutex);
                    for (const auto& sent : l.sent)
                        if (sent.event == event_criteria_attempt) { sender = sent.sender; break; }
                }
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Coop challenge lab: criteria handler ran ({} realm), attributed to {}.", realm_name(realm()),
                    sender ? std::format("virtual player {:#x}", sender) : std::string("the sender (local)"));
            }
            for (const auto& h : handled_events)
                if (std::ranges::find(h.graphs, graph) != h.graphs.end()) { note_handled(graph); break; }
            if (graph == graph_queue_filled || graph == graph_round_end_update) {
                const auto frame = read<std::uint32_t>(resource + 0x20);
                const auto locals = read<Address>(read<Address>(vm + 0x30) + ((frame + 15U) & ~15U) + 16);
                if (locals && graph == graph_queue_filled && l.local_entered.exchange(false, std::memory_order_acq_rel)) {
                    const auto handle = read<std::uint32_t>(locals + queue_filled_handle_local);
                    const auto seed = read<std::uint32_t>(locals + queue_filled_seed_local);
                    { std::lock_guard lock(l.mutex); l.event_handle = handle; }
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdowns: the local player's queue became event {:#x} (random seed {:#x}).", handle, seed);
                }
                // Research: which order rule Spot Battle rounds use (Fixed 0 keeps the first order).
                static std::atomic<std::int32_t> style{-1};
                if (locals && graph == graph_round_end_update) {
                    const auto value = read<std::int32_t>(locals + round_end_style_local);
                    if (value != style.exchange(value))
                        logging::log(logging::Level::info, logging::Channel::progression,
                            "Throwdown lab: turn-based round end read TurnOrderStyle {}.", value);
                }
            }
            if (graph == graph_turn_start || graph == graph_skate_timer) {
                const auto frame = read<std::uint32_t>(resource + 0x20);
                const auto locals = read<Address>(read<Address>(vm + 0x30) + ((frame + 15U) & ~15U) + 16);
                // Only the event the local player is in: one it left may still be running here.
                const auto foreign = [&](std::uint32_t local_offset) {
                    const auto handle = read<std::uint32_t>(locals + local_offset);
                    std::lock_guard lock(l.mutex);
                    return handle && l.event_handle && handle != l.event_handle;
                };
                if (locals && graph == graph_turn_start && !foreign(turn_start_handle_local)) {
                    const auto active = read<std::uint32_t>(locals + turn_start_active_local);
                    l.server_active.store(active, std::memory_order_release);
                    static std::atomic<unsigned> logged{};
                    if (logged.fetch_add(1) < 2000)
                        logging::log(logging::Level::info, logging::Channel::progression,
                            "Throwdowns: server turn started for player {:#x}.", active);
                    ThrowdownLocalAction action{ThrowdownLocalAction::Kind::turn_started};
                    action.player = active;
                    throwdown_relay_local(std::move(action));
                }
                if (locals && graph == graph_skate_timer && read<std::int32_t>(locals + skate_timer_outcome_local) < 0 &&
                    !foreign(skate_timer_handle_local)) {
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdowns: S.K.A.T.E. turn of player {:#x} ran out of time.", l.server_active.load());
                    ThrowdownLocalAction action{ThrowdownLocalAction::Kind::timer_failed};
                    action.player = l.server_active.load(std::memory_order_acquire);
                    throwdown_relay_local(std::move(action));
                }
            }
            if (graph == 0xc33322b2) skate_debug_text_ran(l.base, vm); // ClientSkateThrowdown.OnDebugPrint
            // Debug-only and offline-waiting graphs that re-post themselves every frame.
            throttle_graph_ran(l.base, vm);
            // ServerSkateThrowdown's Skate_SubmitAttempt handler: the no-repeat rule.
            if (graph == 0xc4a37359) skate_attempt_ran(l.base, vm);
            // TurnBasedServer.OnRequestRoundEnd: only S.K.A.T.E. raises it, when the setter misses.
            if (graph == 0xcc6467cf) l.setter_missed.store(true, std::memory_order_release);
            // TurnStateData.OnNetworkedEntityComponentDataUnmarshaled (client): Lb4 = ActivePlayerUID
            // as the HUD, the visibility rule and the spectate rule see it.
            if (graph == 0xf073dd02) {
                const auto frame = read<std::uint32_t>(resource + 0x20);
                const auto locals = read<Address>(read<Address>(vm + 0x30) + ((frame + 15U) & ~15U) + 16);
                if (locals) {
                    ThrowdownLocalAction action{ThrowdownLocalAction::Kind::turn_shown};
                    action.player = read<std::uint32_t>(locals + 0xb4);
                    throwdown_relay_local(std::move(action));
                }
            }
            if (graph == graph_turn_end_begin && realm() == server_realm) {
                ThrowdownLocalAction action{ThrowdownLocalAction::Kind::turn_ended};
                action.player = l.server_active.load(std::memory_order_acquire);
                throwdown_relay_local(std::move(action));
            }

        } catch (...) {}
    }
    // A spawn the server never ran (no coordinator, or rejected) must not take the
    // next spawn's MMID.
    if (l.spawn_outstanding.load(std::memory_order_acquire)) {
        std::optional<Lab::Spawn> expired;
        {
            std::unique_lock lock(l.mutex, std::try_to_lock);
            if (lock && !l.spawns.empty() && now_ms() - l.spawns.front().ms > 5000) {
                expired = l.spawns.front();
                l.spawns.pop_front();
                if (l.spawns.empty()) l.spawn_outstanding.store(false, std::memory_order_release);
            }
        }
        if (expired) {
            logging::log(logging::Level::warning, logging::Channel::progression,
                "Throwdowns: the server did not run a queued DebugSpawnThrowdownWithParams (token {}).", expired->token);
            if (expired->token) throwdown_relay_spawned(expired->token, 0, 0);
        }
    }
    if (l.tracing.load(std::memory_order_acquire) && vm) {
        profile_runtime::PreserveError preserve;
        try { trace_expression(vm); } catch (...) {}
    }
    if (l.spawn_outstanding.load(std::memory_order_acquire) && vm) {
        profile_runtime::PreserveError preserve;
        try {
            const auto resource = read<Address>(vm + 0x38);
            if (resource && read<std::uint32_t>(resource + 0x10) == graph_debug_spawn) {
                // VM registers: pages = *(vm+0x30) + align16(frame bytes); [0] constants,
                // [1] ports, [2] locals.
                const auto frame = read<std::uint32_t>(resource + 0x20);
                const auto pages = read<Address>(vm + 0x30) + ((frame + 15U) & ~15U);
                const auto locals = read<Address>(pages + 16);
                const auto mmid = locals ? read<std::uint32_t>(locals + debug_spawn_mmid_local) : 0U;
                std::optional<Lab::Spawn> spawn;
                {
                    std::lock_guard lock(l.mutex);
                    if (!l.spawns.empty()) { spawn = l.spawns.front(); l.spawns.pop_front(); }
                    if (l.spawns.empty()) l.spawn_outstanding.store(false, std::memory_order_release);
                    if (mmid && (!spawn || !spawn->token)) l.mmid = mmid;
                }
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown lab: server ran DebugSpawnThrowdownWithParams ({} realm); queue MMID {:#x}.",
                    realm_name(realm()), mmid);
                if (spawn && spawn->token) throwdown_relay_spawned(spawn->token, mmid, spawn->max_players);
            }
        } catch (...) {}
    }
    if (!l.pending.load(std::memory_order_acquire)) return;
    profile_runtime::PreserveError preserve;
    // A relay spawn that fails here never reaches the server: the relay is told, so the copy
    // is dropped instead of waiting for an MMID that will not come.
    std::uint64_t spawn_token{};
    try {
        if (realm() != client_realm) return;
        Job job{};
        {
            std::unique_lock lock(l.mutex, std::try_to_lock);
            if (!lock || l.jobs.empty()) return;
            job = l.jobs.front(); l.jobs.pop_front();
            if (l.jobs.empty()) l.pending.store(false, std::memory_order_release);
        }
        spawn_token = job.token;
        const auto result = run_job(job);
        spawn_token = 0;
        logging::log(logging::Level::info, logging::Channel::progression, "Throwdown lab: {}.", result);
    } catch (const std::exception& e) {
        logging::log(logging::Level::warning, logging::Channel::progression, "Throwdown lab: injection failed: {}", e.what());
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::progression, "Throwdown lab: injection failed.");
    }
    if (spawn_token) throwdown_relay_spawned(spawn_token, 0, 0);
}

void throwdown_lab_before_level_transition(unsigned next) noexcept {
    // 14, 22 and 3 leave a level or sublevel, 24 shuts down: the level's bundles are unloaded,
    // and the events' types with them.
    if (next != 14 && next != 22 && next != 3 && next != 24) return;
    try {
        auto& l = lab();
        std::lock_guard lock(l.mutex);
        l.world.fetch_add(1, std::memory_order_acq_rel);
        l.types.clear();
    } catch (...) {}
}

bool throwdown_lab_player(std::uint32_t player_id) noexcept {
    if (!player_id) return false;
    for (const auto& slot : lab().players) if (slot.load(std::memory_order_relaxed) == player_id) return true;
    return false;
}

namespace {
constexpr std::array relay_events{event_debug_spawn, event_add_ai, event_force_start, event_destroy,
                                  event_remove_participant, event_player_score, event_request_turn_end,
                                  event_skate_submit, event_force_destroy};
// The beacon events' types, looked for in the background in each level (only the local player's
// own beacon would otherwise show them); retried while the world is still loading them.
bool beacon_types_ready() {
    auto& l = lab();
    if (type_of(event_beacon_move) && type_of(event_beacon_despawn)) return true;
    if (now_ms() < l.beacon_scan_retry.load(std::memory_order_acquire) || l.type_scan_running.exchange(true)) return false;
    try {
        std::thread([] {
            auto& l = lab();
            try {
                if (!ensure_types({event_beacon_move, event_beacon_despawn}, true).empty())
                    l.beacon_scan_retry.store(now_ms() + 10000, std::memory_order_release);
            } catch (...) { l.beacon_scan_retry.store(now_ms() + 10000, std::memory_order_release); }
            l.type_scan_running.store(false, std::memory_order_release);
        }).detach();
    } catch (...) { l.type_scan_running.store(false, std::memory_order_release); }
    return false;
}
bool queue_relay_job(Job job, std::uint32_t event) noexcept {
    try {
        auto& l = lab();
        if (!l.installed.load(std::memory_order_acquire) || !type_of(event)) return false;
        {
            std::lock_guard lock(l.mutex);
            if (l.jobs.size() >= 256) return false;
            l.jobs.push_back(std::move(job));
        }
        l.pending.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
} // namespace

bool prepare_throwdown_injection() noexcept {
    auto& l = lab();
    if (!l.installed.load(std::memory_order_acquire)) return false;
    if (std::ranges::all_of(relay_events, [](std::uint32_t hash) { return type_of(hash) != 0; })) return true;
    // The heap scan takes a few hundred milliseconds: on the game thread it stalled the frame a
    // drop appeared in. A worker runs it and the caller polls until the types are cached.
    if (now_ms() < l.type_scan_retry.load(std::memory_order_acquire) || l.type_scan_running.exchange(true)) return false;
    try {
        std::thread([] {
            auto& l = lab();
            try {
                const auto error = ensure_types({event_debug_spawn, event_add_ai, event_force_start, event_destroy,
                                                 event_remove_participant, event_player_score, event_request_turn_end,
                                                 event_skate_submit, event_force_destroy, event_criteria_attempt,
                                                 event_slam_hit, event_leave_in_progress, event_onboarding_complete}, true);
                if (!error.empty()) {
                    l.type_scan_retry.store(now_ms() + 5000, std::memory_order_release);
                    logging::log(logging::Level::warning, logging::Channel::progression, "Throwdowns: {}", error);
                }
            } catch (...) { l.type_scan_retry.store(now_ms() + 5000, std::memory_order_release); }
            l.type_scan_running.store(false, std::memory_order_release);
        }).detach();
    } catch (...) { l.type_scan_running.store(false, std::memory_order_release); }
    return false;
}
bool queue_throwdown_spawn(std::uint64_t token, std::uint32_t host, const std::string& series,
                           const std::vector<std::uint8_t>& placement, const std::vector<std::uint8_t>& settings) noexcept {
    if (!token || !host || placement.empty()) return false;
    try {
        Job job{Job::Kind::spawn, host};
        job.token = token; job.series = series; job.placement = placement; job.settings = settings;
        return queue_relay_job(std::move(job), event_debug_spawn);
    } catch (...) { return false; }
}
bool queue_throwdown_add(std::uint32_t player, const std::string& series, std::uint32_t mmid) noexcept {
    if (!player || !mmid) return false;
    try {
        Job job{Job::Kind::ai, player};
        job.series = series; job.mmid = mmid;
        return queue_relay_job(std::move(job), event_add_ai);
    } catch (...) { return false; }
}
bool queue_throwdown_remove(std::uint32_t player) noexcept {
    return player && queue_relay_job({Job::Kind::remove, player}, event_remove_participant);
}
bool queue_throwdown_start(const std::string& series, std::uint32_t mmid) noexcept {
    if (!mmid) return false;
    try {
        Job job{Job::Kind::start};
        job.series = series; job.mmid = mmid;
        return queue_relay_job(std::move(job), event_force_start);
    } catch (...) { return false; }
}
bool queue_throwdown_destroy(std::uint32_t mmid) noexcept {
    if (!mmid) return false;
    Job job{Job::Kind::destroy};
    job.mmid = mmid;
    return queue_relay_job(std::move(job), event_destroy);
}
bool queue_throwdown_row(std::uint32_t player, std::uint8_t board, bool add, std::int32_t value) noexcept {
    if (!player || !lab().leaderboard_live.load(std::memory_order_acquire)) return false;
    Job job{Job::Kind::row, player, board};
    job.flag = add;
    job.score = static_cast<float>(value);
    return queue_relay_job(std::move(job), event_player_score);
}
bool queue_throwdown_attempt(std::uint32_t player, bool landed, const std::array<std::uint8_t, 28>& trick) noexcept {
    if (!player || !live_event_handle()) return false;
    Job job{Job::Kind::attempt, player};
    job.flag = landed;
    job.trick = trick;
    return queue_relay_job(std::move(job), event_skate_submit);
}
bool queue_challenge_start(const std::string& series, const std::string& id, bool contest) noexcept {
    try {
        auto& l = lab();
        if (!l.installed.load(std::memory_order_acquire) || series.empty() || id.empty()) return false;
        Job job{Job::Kind::challenge_start};
        job.series = series; job.challenge = id; job.flag = contest;
        {
            std::lock_guard lock(l.mutex);
            if (l.jobs.size() >= 256) return false;
            l.jobs.push_back(std::move(job));
        }
        l.pending.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
bool queue_challenge_attempt(std::uint32_t player, std::vector<std::uint8_t> criteria, std::vector<std::uint8_t> indexes) noexcept {
    try {
        if (!player || criteria.empty() || criteria.size() % criteria_data_size) return false;
        Job job{Job::Kind::criteria_attempt, player};
        job.criteria = std::move(criteria); job.indexes = std::move(indexes);
        return queue_relay_job(std::move(job), event_criteria_attempt);
    } catch (...) { return false; }
}
bool queue_challenge_slam(std::uint32_t player, std::int32_t identifier) noexcept {
    std::uint32_t handle{};
    { std::lock_guard lock(lab().mutex); handle = lab().challenge_event_handle; }
    if (!player || !handle) return false;
    Job job{Job::Kind::challenge_slam, player};
    job.max_players = static_cast<std::uint32_t>(identifier);
    return queue_relay_job(std::move(job), event_slam_hit);
}
bool queue_challenge_leave(std::uint32_t player) noexcept {
    return player && queue_relay_job({Job::Kind::challenge_leave, player}, event_leave_in_progress);
}
bool queue_beacon_move(std::uint32_t player, const std::array<float, 16>& location) noexcept {
    try {
        if (!player || !lab().installed.load(std::memory_order_acquire) || !beacon_types_ready()) return false;
        Job job{Job::Kind::beacon_move, player};
        job.location = location;
        return queue_relay_job(std::move(job), event_beacon_move);
    } catch (...) { return false; }
}
bool queue_beacon_remove(std::uint32_t player) noexcept {
    try {
        return player && lab().installed.load(std::memory_order_acquire) && beacon_types_ready() &&
               queue_relay_job({Job::Kind::beacon_remove, player}, event_beacon_despawn);
    } catch (...) { return false; }
}
std::optional<ChallengeCelebration> take_challenge_celebration() noexcept {
    auto& l = lab();
    std::lock_guard lock(l.mutex);
    return std::exchange(l.celebration, std::nullopt);
}
bool queue_throwdown_destroy_event(std::uint32_t player) noexcept {
    return player && queue_relay_job({Job::Kind::destroy_event, player}, event_force_destroy);
}
bool queue_throwdown_end_turn(std::uint32_t player) noexcept {
    return player && live_event_handle() && queue_relay_job({Job::Kind::end_turn, player}, event_request_turn_end);
}
bool queue_throwdown_score(std::uint32_t player, std::int32_t score) noexcept {
    if (!player || !lab().leaderboard_live.load(std::memory_order_acquire)) return false;
    Job job{Job::Kind::total, player};
    job.score = static_cast<float>(score);
    return queue_relay_job(std::move(job), event_player_score);
}

std::string throwdown_lab_command(std::string_view arguments, std::optional<std::array<float, 3>> skater) {
    auto& l = lab();
    if (!l.installed.load(std::memory_order_acquire)) return "Throwdown lab is not installed (see ReSkate.log).";
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < arguments.size();) {
        const auto start = arguments.find_first_not_of(' ', at);
        if (start == std::string_view::npos) break;
        const auto end = arguments.find(' ', start);
        words.push_back(arguments.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        at = end == std::string_view::npos ? arguments.size() : end;
    }
    const auto verb = words.empty() ? std::string_view("status") : words[0];
    if (verb == "relay") return throwdown_relay_status();
    const auto number = [&](std::size_t i) { return i < words.size() ? parse_u32(words[i]) : std::nullopt; };
    const auto current = [&] {
        std::lock_guard lock(l.mutex);
        return std::format("series {}, MMID {:#x}, parameters {}", l.series.empty() ? "(none)" : l.series, l.mmid,
            l.params.empty() ? "(none recorded)" : l.params_note);
    };
    if (verb == "status" || verb == "help")
        return "Throwdown lab: " + current() + ".\n"
               "  throwdown ai <player id>            add a player id with no native player to the current queue\n"
               "  throwdown spawn <host id> [max] [mode]  queue hosted by <host id>, at the recorded placement or,\n"
               "                                      with [mode] or nothing recorded, where you stand\n"
               "  throwdown mirror <host id>          replace your placed drop by the same drop hosted by <host id>\n"
               "  throwdown join                      join the current queue as the local player (the Join button)\n"
               "  throwdown start | destroy           force start / destroy the current queue (MMID above)\n"
               "  throwdown mmid <value>              use another queue\n"
               "  throwdown trace [seconds]           log every authored graph run\n"
               "  throwdown relay                     multiplayer: linked drops, mirrors and players\n"
               "Place a throwdown (or join one) first: its series, MMID and settings are recorded.";
    if (verb == "trace") {
        const auto seconds = number(1).value_or(60);
        {
            std::lock_guard lock(l.trace_mutex);
            l.trace.clear();
            l.trace_start_ms = now_ms();
            l.trace_end_ms = l.trace_start_ms + std::uint64_t{std::min(seconds, 900U)} * 1000;
        }
        l.tracing.store(true, std::memory_order_release);
        return std::format("Throwdown lab: tracing authored graphs for {} s; the list is logged when it ends.", std::min(seconds, 900U));
    }
    if (verb == "mmid") {
        const auto value = number(1);
        if (!value) return "Usage: throwdown mmid <value>";
        std::lock_guard lock(l.mutex);
        l.mmid = *value;
        return std::format("Throwdown lab: MMID set to {:#x}.", *value);
    }
    // Everything below sends throwdown events as a player, the local one included: in a
    // multiplayer session that would fake scores, attempts and starts for everyone.
    if (multiplayer_session_active())
        return "Throwdown lab: off during a multiplayer session (it can fake throwdown scores, attempts and starts).";
    bool recorded{};
    {
        std::lock_guard lock(l.mutex);
        recorded = !l.params.empty();
        if (verb != "spawn" && verb != "endturn" && verb != "score" && verb != "attempt" && (l.series.empty() || !l.mmid))
            return "Throwdown lab: no queue known yet. Place, join or spawn a throwdown first.";
    }
    std::string error;
    if (verb == "ai") {
        const auto uid = number(1);
        if (!uid || !*uid) return "Usage: throwdown ai <player id> (e.g. 0x3c5; 0x7e000005 tests the 10-bit truncation)";
        remember_player(*uid);
        error = queue({Job::Kind::ai, *uid}, {event_add_ai});
    } else if (verb == "spawn") {
        const auto uid = number(1);
        if (!uid || !*uid) return "Usage: throwdown spawn <host id> [max players] [JamSession|SpotBattle|ThrowdownSkate]";
        Job job{Job::Kind::spawn, *uid, number(2).value_or(0)};
        if (words.size() > 3 || !recorded) {
            if (!skater) return "Throwdown lab: no placement recorded and the skater position is unknown.";
            job.position = skater;
            job.series = words.size() > 3 ? std::string(words[3]) : std::string("JamSession");
        }
        remember_player(*uid);
        error = queue(std::move(job), {event_debug_spawn});
    } else if (verb == "start") {
        error = queue({Job::Kind::start}, {event_force_start});
    } else if (verb == "endturn") {
        const auto uid = number(1);
        if (!uid || !*uid) return "Usage: throwdown endturn <player id>";
        {
            std::lock_guard lock(l.mutex);
            if (!l.event_handle && !l.leaderboard_live.load(std::memory_order_acquire))
                return "Throwdown lab: no running event seen yet (its handle comes from its leaderboard or the local client's turn requests).";
        }
        Job job{Job::Kind::end_turn, *uid};
        job.mmid = number(2).value_or(0);
        error = queue(std::move(job), {event_request_turn_end});
    } else if (verb == "mirror") {
        // What a guest game will do with a leader's drop: take the drop the local player
        // placed (its recorded settings), destroy it, and spawn the same drop hosted by <id>.
        const auto uid = number(1);
        if (!uid || !*uid) return "Usage: throwdown mirror <host id>";
        if (!recorded) return "Throwdown lab: place a throwdown first (its settings are what gets mirrored).";
        if (auto missing = ensure_types({event_destroy, event_debug_spawn}); !missing.empty()) return "Throwdown lab: " + missing;
        remember_player(*uid);
        std::lock_guard lock(l.mutex);
        l.jobs.push_back({Job::Kind::destroy});
        l.jobs.push_back({Job::Kind::spawn, *uid, number(2).value_or(0)});
        l.pending.store(true, std::memory_order_release);
        return std::format("Throwdown lab: destroying your drop and spawning it again hosted by {:#x} ({}).", *uid, l.params_note);
    } else if (verb == "score") {
        const auto uid = number(1);
        float value{};
        if (!uid || words.size() < 3 || std::from_chars(words[2].data(), words[2].data() + words[2].size(), value).ec != std::errc{})
            return "Usage: throwdown score <player id> <score> [flag 0|1] [leaderboard 0-3]";
        Job job{Job::Kind::score, *uid, number(4).value_or(0) & 3};
        job.score = value;
        job.flag = number(3).value_or(1) != 0;
        remember_player(*uid);
        {
            std::lock_guard lock(l.mutex);
            if (!l.leaderboards[job.max_players]) return "Throwdown lab: no leaderboard seen yet (the local client writes one when a round starts).";
        }
        std::lock_guard lock(l.mutex);
        l.jobs.push_back(std::move(job));
        l.pending.store(true, std::memory_order_release);
        return std::format("Throwdown lab: score queued for player {:#x}.", *uid);
    } else if (verb == "attempt") {
        const auto uid = number(1);
        if (!uid || !*uid) return "Usage: throwdown attempt <player id> [landed 0|1] (a S.K.A.T.E. attempt as that player)";
        if (auto missing = ensure_types({event_skate_submit}); !missing.empty()) return "Throwdown lab: " + missing;
        if (!live_event_handle()) return "Throwdown lab: no running event handle yet.";
        Job job{Job::Kind::attempt, *uid};
        job.flag = number(2).value_or(0) != 0;
        std::lock_guard lock(l.mutex);
        l.jobs.push_back(std::move(job));
        l.pending.store(true, std::memory_order_release);
        return std::format("Throwdown lab: attempt queued as player {:#x}.", *uid);
    } else if (verb == "join") {
        error = queue({Job::Kind::join}, {event_add_participant});
    } else if (verb == "destroy") {
        error = queue({Job::Kind::destroy}, {event_destroy});
    } else return std::format("Unknown throwdown lab verb \"{}\"; try: throwdown help", verb);
    return error.empty() ? std::format("Throwdown lab: {} queued ({}); the result is logged when the game sends it.", verb, current())
                         : "Throwdown lab: " + error;
}
std::string challenge_lab_command(std::string_view arguments) {
    auto& l = lab();
    if (!l.installed.load(std::memory_order_acquire)) return "Coop challenge lab is not installed (see ReSkate.log).";
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < arguments.size();) {
        const auto start = arguments.find_first_not_of(' ', at);
        if (start == std::string_view::npos) break;
        const auto end = arguments.find(' ', start);
        words.push_back(arguments.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        at = end == std::string_view::npos ? arguments.size() : end;
    }
    const auto verb = words.empty() ? std::string_view("status") : words[0];
    const auto number = [&](std::size_t i) { return i < words.size() ? parse_u32(words[i]) : std::nullopt; };
    if ((verb == "virtual" || verb == "attempt") && multiplayer_session_active())
        return "Coop challenge lab: off during a multiplayer session (it can fake challenge players and attempts).";
    if (verb == "virtual") {
        const auto count = number(1);
        if (!count || *count > 3) return "Usage: challenge virtual <0-3>";
        l.challenge_armed.store(*count);
        return *count ? std::format("Coop challenge lab: the next challenge you start gets {} virtual participant(s) "
                                    "({:#x}..) and is coop. Start one (Solo is fine) at a challenge marker.",
                                    *count, challenge_virtual_base + 1)
                      : "Coop challenge lab: no virtual participants for the next challenge.";
    }
    if (verb == "attempt") {
        const auto uid = number(1).value_or(challenge_virtual_base + 1);
        {
            std::lock_guard lock(l.mutex);
            if (!l.criteria_attempt) return "Coop challenge lab: finish one attempt yourself first; it is replayed as the virtual player.";
            if (!l.types.contains(event_criteria_attempt)) return "Coop challenge lab: the attempt event type is not known yet.";
            if (l.jobs.size() >= 256) return "Coop challenge lab: too many queued jobs.";
            l.jobs.push_back({Job::Kind::criteria_attempt, uid});
        }
        l.pending.store(true, std::memory_order_release);
        return std::format("Coop challenge lab: your last attempt queued as player {:#x}; the handler runs are logged.", uid);
    }
    if (verb == "clear") {
        l.challenge_armed.store(0);
        for (auto& slot : l.challenge_named) slot.store(0);
        return "Coop challenge lab: virtual participants cleared.";
    }
    if (verb != "status" && verb != "help") return std::format("Unknown challenge lab verb \"{}\"; try: challenge help", verb);
    std::string named;
    for (const auto& slot : l.challenge_named)
        if (const auto id = slot.load()) named += std::format(" {:#x} ({})", id, virtual_eid(id));
    std::lock_guard lock(l.mutex);
    std::string attempt = "none";
    if (l.criteria_attempt) attempt = std::format("{} criteria", l.criteria_attempt->count);
    return std::format("Coop challenge lab: armed {} virtual; named{}; last start: {}; last own attempt: {}; "
                       "handler runs {}.\n"
                       "  challenge virtual <0-3>   the next challenge you start gets virtual participants and is coop\n"
                       "  challenge attempt [id]    replay your last finished attempt as virtual player [id]\n"
                       "  challenge clear           forget the virtual participants",
                       l.challenge_armed.load(), named.empty() ? " none" : named,
                       l.challenge_start.empty() ? "none yet" : l.challenge_start, attempt, l.criteria_runs.load());
}
} // namespace dingosdk::multiplayer
