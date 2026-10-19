#include "throwdown_relay.h"
#include "throwdown_lab.h"
#include "throwdown_wire.h"
#include "native_throwdowns.h"
#include "Extension/Skater/skate_detector.h"
#include "Extension/Skater/trick_logger.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Settings/named_settings.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
using Kind = ThrowdownMessage::Kind;
using Local = ThrowdownLocalAction::Kind;
// Remote players' ids on the local server: 0x300 + session slot. Participant, score
// and queue lists replicate ids in 10 bits and turn-based rules require 1..1022, so
// the range stays below 0x3ff and clear of the local player (2).
constexpr std::uint32_t virtual_base = 0x300;
constexpr std::size_t virtual_slots = 0x3fe - virtual_base;
// Linked modes. Jam needs joins, the start and scores; Spot Battle also turn ends and
// both of its leaderboards; S.K.A.T.E. turn ends and every attempt (letters, eliminations
// and the set trick all follow from them on each server). Turn-based modes take their
// turn order from the participant order, which is made the same everywhere.
constexpr std::string_view jam_mode = "JamSession", spot_battle_mode = "SpotBattle", skate_mode = "ThrowdownSkate";
bool relayed_mode(std::string_view series) { return series == jam_mode || series == spot_battle_mode || series == skate_mode; }
bool turn_based(std::string_view series) { return series == spot_battle_mode || series == skate_mode; }
// A remote attempt waits for that player's turn to start here; one still waiting after this
// long belongs to a turn this machine never gives them.
constexpr std::uint64_t attempt_wait_ms = 20000;
constexpr std::uint64_t offer_interval_ms = 10000, score_interval_ms = 250, gone_forget_ms = 60000;
// A copy whose spawn is never answered is given up after this long, so the next can be tried;
// its leader's following offers wait spawn_refused_ms. A leader's repeated coop challenge starts
// are declined once per decline_interval_ms.
constexpr std::uint64_t spawning_wait_ms = 30000, spawn_refused_ms = 30000, decline_interval_ms = 5000;
// While other players are in the session nobody's queue may start on its own timer:
// the leader starts it (force start) and every joined copy follows.
constexpr std::string_view queue_timer_setting = "DingoThrowdowns.QueueStartTimer";
constexpr std::string_view solo_queue_timer = "1", linked_queue_timer = "36000";

struct Key {
    std::uint64_t leader{};
    std::uint32_t id{};
    auto operator<=>(const Key &) const = default;
};
// A coop challenge linked across the session (analysis/coop-challenges-mp.md): the leader's
// start runs the same challenge on every joining machine with the others as virtual players.
struct Challenge {
    Key key;                             // the leader and its number for this run
    std::string series, challenge;       // SeriesId and Id
    bool contest{};
    std::vector<std::uint64_t> order;    // participants, leader first
    std::set<std::uint64_t> out;         // quit, opted out or left the session
    bool leader{}, started{};            // started: the local copy runs
    std::uint64_t requested_at{};        // guest: when its copy was asked to start
    std::vector<std::uint32_t> ids;      // the virtual ids in the local copy, in participant order
    std::map<std::uint64_t, std::uint32_t> departed; // players gone from the session: their id here
    // Remote inputs waiting for the local copy (or for an event type / handle): replayed in order.
    std::deque<std::pair<std::uint64_t, ThrowdownMessage>> held;
};
// Coop celebration spots (take_challenge_celebration): where each other player's skater stands.
struct Celebration {
    ChallengeCelebration raw;
    std::uint64_t at{};
    int layout = -2; // which floats of a Transform are its translation; -1: fell back to a row
    // Spot 0 (where the local skater was put) and the local skater's root from there: a skater's
    // root is not at its feet, so another player's root goes to their spot plus the same lift.
    std::array<float, 3> home{}, lift{};
    std::map<std::uint64_t, std::array<float, 3>> spots;
};
// What the StartChallenge hook (server realm) needs, published by the relay tick.
struct ChallengePlan {
    std::string series, id;           // a guest copy: exactly this challenge
    std::vector<std::uint32_t> ids;   // ... with these other players
    std::vector<std::pair<std::uint64_t, std::uint32_t>> nearby; // leader candidates: nearby players
    bool guest{}, busy{};
    struct Leader { std::string series, id; std::vector<std::uint64_t> players; };
    std::optional<Leader> leader_start; // the hook's answer to a leader start, for the tick
};

// The local player's own drop.
struct Hosted {
    skate::Rules rules;
    std::uint32_t id{}; // its MMID
    std::string series;
    std::vector<std::uint8_t> placement, settings;
    std::vector<std::uint64_t> members, added; // remote joiners in join order; those in the queue here
    bool relayed{}, started{}, start_sent{};
    std::uint64_t next_offer{};
};
// Another player's drop, spawned on the local server hosted by the leader's virtual id.
struct Mirror {
    skate::Rules rules;
    std::vector<std::uint8_t> pending_settings;
    enum class State { waiting, spawning, open, started, gone } state = State::waiting;
    std::string series;
    std::vector<std::uint8_t> placement, settings;
    std::vector<std::uint64_t> members, added; // remote players in it (never the leader or us)
    std::uint64_t token{}, gone_at{}, spawning_since{};
    std::uint32_t mmid{}, max_players{};
    bool joined{}; // the local player is in it
    std::optional<std::vector<std::uint64_t>> start; // the leader started before the copy existed
};
const char *state_name(Mirror::State s) {
    switch (s) {
    case Mirror::State::waiting: return "waiting";
    case Mirror::State::spawning: return "spawning";
    case Mirror::State::open: return "open";
    case Mirror::State::started: return "started";
    case Mirror::State::gone: return "gone";
    }
    return "?";
}
struct Score {
    std::int32_t value{};
    bool dirty{};
};
// A remote player's leaderboard write or turn end, replayed here in arrival order.
struct Remote {
    enum class What { row, turn_end, attempt } what{};
    std::uint64_t player{};
    std::uint8_t board{};
    bool add{};
    std::int32_t value{}; // row: the value; attempt: the owner's turn number
    std::array<std::uint8_t, 28> trick{};
    std::optional<skate::DetectedAttempt> detected;
};
// What the participant-order hook needs, published by the tick for the server thread.
struct Order {
    std::uint64_t leader{}, local{};
    std::map<std::uint32_t, std::uint64_t> players; // server id -> Steam ID
};
struct Relay {
    // Filled by the natives' hooks and the session; drained by the relay tick.
    std::mutex mutex;
    std::deque<ThrowdownLocalAction> actions;
    std::deque<std::pair<std::uint64_t, std::vector<std::uint8_t>>> inbox;
    std::deque<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> spawned;
    std::array<std::atomic<std::uint64_t>, virtual_slots> info{}; // UIPlayerInfo per slot
    std::optional<Order> order;                                     // while in a linked queue
    std::map<std::uint32_t, std::string> names;                     // server id -> display name
    // Relay tick (game thread) only.
    Address base{};
    std::uint64_t local{}, world{};
    bool in_world{}, prepared{};
    std::uint64_t next_prepare{}, next_info{};
    std::map<std::uint64_t, std::size_t> peers; // Steam ID -> slot
    std::optional<Hosted> hosted;
    std::map<Key, Mirror> mirrors;
    std::optional<Key> running;
    std::string running_series;
    skate::Rules rules;
    std::set<std::pair<std::uint64_t,std::int32_t>> detector_submitted,detector_applied;
    std::map<std::uint64_t, Score> scores;                          // remote totals in `running`
    std::map<Key, std::map<std::uint64_t, std::int32_t>> early;      // totals for an event not running here yet
    std::deque<Remote> remote;                                      // rows, turn ends, attempts in `running`, in order
    std::map<Key, std::vector<Remote>> early_remote;                // ... for an event not running here yet
    std::uint64_t remote_held_since{};                              // when remote.front() started waiting
    // Turn-based: how many turns each player has had in `running` on this server, and which
    // of a remote player's turns this server's timer failed before their attempt arrived.
    std::map<std::uint64_t, std::int32_t> turns, timer_failed;
    std::uint64_t active{}; // whose turn this server started last
    std::uint64_t shown{};  // who is up as the local client shows it (hiding, camera)
    std::vector<std::uint64_t> players; // the running event's participants
    std::set<std::uint64_t> quit;       // participants who quit it (their turns resolve at once)
    // Participants who left the session during it: the virtual id they have in it here.
    std::map<std::uint64_t, std::uint32_t> departed;
    std::vector<std::string> notices;   // for the local player's chat
    std::uint64_t spectating{};         // whose skater the turn camera follows
    Score own;
    std::uint64_t own_sent_at{}, next_token = 1;
    std::vector<std::vector<std::uint8_t>> outgoing;
    std::string timer, status_line; // status_line: under `mutex`
    std::uint64_t next_timer{}, next_status{};
    bool force_start_enabled{};
    std::optional<Challenge> challenge;
    std::uint32_t next_challenge = 1;
    ChallengePlan plan;                 // under `mutex`
    std::optional<Celebration> celebration;
    std::vector<std::uint32_t> celebration_ids;   // the last linked challenge's virtual players, in order
    std::optional<std::array<float, 3>> position; // the local skater
    std::map<std::uint64_t, std::string> player_names; // Steam ID -> session name
    std::set<std::uint64_t> party;                     // the local player's party members
    std::map<std::string, std::uint64_t> named;        // session name -> Steam ID (0: shared); under `mutex`
    // Party beacons (analysis/party-re/beacons.md): the local player's, as its client asked for
    // it, and the other players' in this world, each shown natively as theirs.
    struct Beacon {
        std::array<float, 16> location{};
        std::uint32_t revision{};
        bool shown{};
        std::uint64_t retry_at{};
    };
    std::optional<std::array<float, 16>> own_beacon;
    std::uint32_t beacon_revision{};
    std::uint64_t beacon_sent_at{};
    std::map<std::uint64_t, Beacon> beacons;
    // Leaders whose last drop could not be spawned here: their new offers wait until then (ms).
    std::map<std::uint64_t, std::uint64_t> spawn_refused;
    // When a coop challenge from each leader was last declined: one answer per leader at a time.
    std::map<std::uint64_t, std::uint64_t> declined;
    bool party_settings{};
    std::uint64_t next_party_settings{};
};
Relay &relay() {
    static auto *value = new Relay;
    return *value;
}
// throwdown_relay_waits_offboard, worked out each relay tick.
std::atomic<bool> waiting_offboard{}, offboard_enabled{true}, challenge_invites_enabled{true};
std::string describe(const Relay &r);

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
template <class... Args> void note(std::format_string<Args...> text, Args &&...args) {
    logging::log(logging::Level::info, logging::Channel::progression, "Throwdowns: {}",
                 std::format(text, std::forward<Args>(args)...));
}
bool contains(const std::vector<std::uint64_t> &list, std::uint64_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}
void erase(std::vector<std::uint64_t> &list, std::uint64_t id) { std::erase(list, id); }

std::uint32_t virtual_id(const Relay &r, std::uint64_t player) {
    const auto it = r.peers.find(player);
    if (it != r.peers.end() && it->second < virtual_slots) return virtual_base + static_cast<std::uint32_t>(it->second);
    // A participant of the running throwdown who left the session keeps its id in it.
    const auto gone = r.departed.find(player);
    return gone != r.departed.end() ? gone->second : 0;
}
// A server participant id back to the player: the local player or a relayed one.
std::optional<std::uint64_t> player_of(const Relay &r, std::uint32_t id) {
    if (id && id == local_native_player_id()) return r.local;
    if (id < virtual_base || id >= virtual_base + virtual_slots) return {};
    // In the running throwdown an id stays its departed participant's, even if a newcomer
    // now has that session slot.
    for (const auto &[player, gone] : r.departed)
        if (gone == id) return player;
    for (const auto &[player, slot] : r.peers)
        if (slot == id - virtual_base) return player;
    return {};
}

void send(Relay &r, const ThrowdownMessage &m) {
    try {
        r.outgoing.push_back(encode_throwdown(m));
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::progression, "Throwdowns: message not sent: {}", e.what());
    }
}
ThrowdownMessage message(Kind kind, std::uint64_t leader, std::uint32_t id) {
    ThrowdownMessage m;
    m.kind = kind; m.leader = leader; m.id = id;
    return m;
}

// ---------------------------------------------------------------------------
// The local player's own drop.
std::vector<std::uint8_t> detected_bytes(const skate::DetectedAttempt& result) {
    if(!result.id || result.display.size()>1000 || result.signature.size()>64 ||
       (result.landed && result.signature.empty()))throw std::invalid_argument("Invalid detector result shape");
    for(const auto& token:result.signature)if(token.size()>1000)throw std::invalid_argument("Detector token too large");
    auto j=Json::object();j["id"]=result.id;j["landed"]=result.landed;
    j["violation"]=static_cast<unsigned>(result.violation);j["display"]=result.display;j["evidence"]=result.evidence;
    auto tokens=Json::array();for(const auto& token:result.signature)tokens.push_back(token);j["signature"]=tokens;
    const auto text=j.dump();if(text.size()>3500)throw std::invalid_argument("Detected trick exceeds packet budget");
    return {text.begin(),text.end()};
}
std::optional<skate::DetectedAttempt> read_detected(const ThrowdownMessage& message) {
    try {
        const auto j=Json::parse(message.detected.begin(),message.detected.end());
        if(!j.at("landed").is_boolean())return {};
        skate::DetectedAttempt result;result.id=j.at("id").get<std::uint64_t>();result.landed=j.at("landed").get<bool>();
        const auto violation=j.at("violation").get<unsigned>();
        if(!result.id || violation>static_cast<unsigned>(skate::Category::grounded_rotation) || (result.landed && violation))return {};
        if(j.contains("evidence")){if(!j.at("evidence").is_object())return {};result.evidence=j.at("evidence");}
        result.violation=static_cast<skate::Category>(violation);result.display=j.at("display").get<std::string>();
        if(result.display.size()>1000 || !j.at("signature").is_array() || j.at("signature").size()>64)return {};
        for(const auto& token:j.at("signature")){if(!token.is_string() || token.string().size()>1000)return {};result.signature.push_back(token.string());}
        if(result.landed && result.signature.empty())return {};
        if(result.landed!=message.add)return {};result.native=message.trick;return result;
    }catch(...){return {};}
}
void send_offer(Relay &r, std::uint64_t now) {
    auto &h = *r.hosted;
    if(h.series==skate_mode && !h.rules.verified)return;
    auto m = message(Kind::offer, r.local, h.id);
    if(h.series==skate_mode)m.rules_mask=h.rules.values;
    m.series = h.series; m.placement = h.placement; m.settings = h.settings; m.order = h.members;
    const auto before = r.outgoing.size();
    send(r, m);
    if (r.outgoing.size() == before) {
        h.relayed = false; // too large to describe; stays local
        return;
    }
    h.next_offer = now + offer_interval_ms;
}
// The drop is gone without having started (cancelled, left, replaced, world change).
void close_hosted(Relay &r) {
    if (r.hosted && r.hosted->relayed && !r.hosted->started) {
        send(r, message(Kind::close, r.local, r.hosted->id));
        note("your {} (MMID {:#x}) closed for the other players.", r.hosted->series, r.hosted->id);
    }
    r.hosted.reset();
}
void send_start(Relay &r, std::vector<std::uint64_t> order) {
    auto &h = *r.hosted;
    if (h.start_sent || !h.relayed) return;
    std::erase(order, 0ULL);
    if (order.empty() || order.front() != r.local) {
        erase(order, r.local);
        order.insert(order.begin(), r.local);
    }
    if (order.size() > max_throwdown_order) order.resize(max_throwdown_order);
    auto m = message(Kind::start, r.local, h.id);
    if(h.series==skate_mode){if(!h.rules.verified)return;m.rules_mask=h.rules.values;m.settings=h.settings;}
    m.order = order;
    send(r, m);
    h.start_sent = true;
    note("your {} started with {} player(s); the other players' copies follow.", h.series, order.size());
}
void add_to_hosted(Relay &r) {
    if (!r.hosted || r.hosted->started || !r.in_world) return;
    auto &h = *r.hosted;
    for (const auto player : h.members)
        if (!contains(h.added, player))
            if (const auto id = virtual_id(r, player); id && queue_throwdown_add(id, h.series, h.id)) h.added.push_back(player);
}

// ---------------------------------------------------------------------------
// Other players' drops.
void destroy_mirror(Relay &r, Mirror &m, std::uint64_t now) {
    if (m.state == Mirror::State::gone || m.state == Mirror::State::started) return;
    // A spawning copy is destroyed once its MMID is known (spawned()).
    if (m.state == Mirror::State::open && !queue_throwdown_destroy(m.mmid))
        logging::write(logging::Level::warning, logging::Channel::progression, "Throwdowns: a linked drop could not be removed.");
    m.state = Mirror::State::gone;
    m.gone_at = now;
    m.joined = false;
    (void)r;
}
void add_to_mirror(Relay &r, Mirror &m) {
    if (m.state != Mirror::State::open || !r.in_world) return;
    for (const auto player : m.members)
        if (!contains(m.added, player))
            if (const auto id = virtual_id(r, player); id && queue_throwdown_add(id, m.series, m.mmid)) m.added.push_back(player);
}
void start_mirror(Relay &r, const Key &key, Mirror &m, const std::vector<std::uint64_t> &order) {
    if(!m.pending_settings.empty()) {
        if(!queue_throwdown_settings(virtual_id(r,key.leader),m.mmid,m.pending_settings)){m.start=order;return;}
        m.settings=std::move(m.pending_settings);m.pending_settings.clear();
    }
    for (const auto player : order)
        if (player != r.local && player != key.leader && !contains(m.members, player)) m.members.push_back(player);
    add_to_mirror(r, m);
    if (!queue_throwdown_start(m.series, m.mmid)) {
        m.start = order; // retried by the tick
        return;
    }
    m.start.reset();
    m.state = Mirror::State::started;
    note("the leader started; starting your copy of their {} (MMID {:#x}).", m.series, m.mmid);
}
// Another player joined `m`. A copy the local player has not joined disappears once
// full, as the leader's own does.
void mirror_member(Relay &r, Mirror &m, std::uint64_t player, std::uint64_t now) {
    // No queue holds more players than one offer can list.
    if (player == r.local || contains(m.members, player) || m.members.size() >= max_throwdown_order) return;
    m.members.push_back(player);
    if (!m.joined && m.max_players && 1 + m.members.size() >= m.max_players) {
        destroy_mirror(r, m, now);
        return;
    }
    add_to_mirror(r, m);
}
void leave_mirrors(Relay &r, std::uint32_t except) {
    for (auto &[key, m] : r.mirrors)
        if (m.joined && (!except || m.mmid != except)) {
            m.joined = false;
            if (m.state == Mirror::State::open) send(r, message(Kind::leave, key.leader, key.id));
        }
}

// A participant who quit the running throwdown still has turns on every server (no native
// event removes another player). Each machine resolves them at once, the same way: a miss in
// S.K.A.T.E. (letters until eliminated; a setter loses the set), an ended turn in Spot Battle.
void resolve_quit_turn(Relay &r, std::uint64_t player) {
    if (!turn_based(r.running_series)) return;
    Remote entry{r.running_series == skate_mode ? Remote::What::attempt : Remote::What::turn_end, player};
    entry.value = r.turns[player];
    if (entry.value <= 0) return;
    if (r.remote.size() < 512) r.remote.push_back(entry);
}

// The running throwdown is over for the local player (it ended, or they quit or were let go).
void end_running(Relay &r) {
    skate::end();trick_logger::observe_setup_menu(0,0);r.rules={};r.detector_submitted.clear();r.detector_applied.clear();
    r.running.reset();
    r.shown = 0;
    r.scores.clear();
    r.remote.clear();
    r.turns.clear();
    r.timer_failed.clear();
    r.active = 0;
    r.players.clear();
    r.quit.clear();
    r.departed.clear();
    r.own = {};
    if (r.hosted && r.hosted->started) r.hosted.reset();
    std::erase_if(r.mirrors, [](const auto &entry) { return entry.second.state == Mirror::State::started; });
}

std::string_view mode_name(std::string_view series) {
    return series == jam_mode ? "Jam Session" : series == spot_battle_mode ? "Spot Battle"
         : series == skate_mode ? "S.K.A.T.E. throwdown" : "throwdown";
}
// The local player is the only one left in the running throwdown that started with others:
// the rest quit or left the session. It cannot go on (their turns would come up forever, a
// Jam would score against nobody), so end it on the local server as the local player, who
// takes part (ServerMpActivityBase.OnForceDestroyCurrentEvent checks just that). False while
// that cannot be sent yet; the tick tries again.
bool alone(const Relay &r) {
    if (!r.running || r.players.size() < 2) return false;
    return std::all_of(r.players.begin(), r.players.end(), [&](std::uint64_t p) {
        return p == r.local || r.quit.contains(p) || (!r.peers.contains(p) && !r.departed.contains(p));
    });
}
bool end_alone(Relay &r) {
    const auto id = local_native_player_id();
    if (!r.running || !r.in_world || !r.prepared || !id || !queue_throwdown_destroy_event(id)) return false;
    note("everyone else left the linked {}; it ended for the local player.", r.running_series);
    r.notices.push_back(std::format("Everyone else left the {}, so it ended.", mode_name(r.running_series)));
    end_running(r);
    return true;
}

void set_running(Relay &r, const Key &key, const std::string &series) {
    r.running = key;
    r.running_series = series;
    r.rules={};r.detector_submitted.clear();r.detector_applied.clear();
    if(series==skate_mode) {
        if(key.leader==r.local && r.hosted)r.rules=r.hosted->rules;
        else if(const auto it=r.mirrors.find(key);it!=r.mirrors.end())r.rules=it->second.rules;
        skate::begin(r.rules);trick_logger::observe_setup_menu(r.rules.values,r.rules.verified?0xff:0);
    } else skate::end();
    r.scores.clear();
    r.remote.clear();
    r.turns.clear();
    r.timer_failed.clear();
    r.active = 0;
    r.shown = 0;
    r.players.clear();
    r.quit.clear();
    r.departed.clear();
    r.own = {};
    if (const auto it = r.early.find(key); it != r.early.end())
        for (const auto &[player, value] : it->second) r.scores[player] = {value, true};
    if (const auto it = r.early_remote.find(key); it != r.early_remote.end())
        r.remote.assign(it->second.begin(), it->second.end());
    r.early.clear();
    r.early_remote.clear();
}
// The running throwdown's participants are known: only they have rows, turns and totals in it.
// What arrived early from anyone else is dropped.
void set_players(Relay &r, const std::vector<std::uint64_t> &players) {
    r.players = players;
    std::erase_if(r.scores, [&](const auto &entry) { return !contains(r.players, entry.first); });
    std::erase_if(r.remote, [&](const Remote &entry) { return !contains(r.players, entry.player); });
}

// ---------------------------------------------------------------------------
void on_challenge_local(Relay &r, const ThrowdownLocalAction &a, std::uint64_t now);
void on_beacon_local(Relay &r, const ThrowdownLocalAction &a, std::uint64_t now);
void on_local(Relay &r, ThrowdownLocalAction &a, std::uint64_t now) {
    switch (a.kind) {
    case Local::beacon_placed:
    case Local::beacon_removed: on_beacon_local(r, a, now); return;
    case Local::challenge_started:
    case Local::challenge_attempt:
    case Local::challenge_slam:
    case Local::challenge_ended: on_challenge_local(r, a, now); return;
    case Local::created:
        close_hosted(r); // a new drop replaces the old one
        break;
    case Local::exited: {
        if (a.cancelling) { close_hosted(r); break; }
        if (!a.mmid || a.placement.empty()) break;
        if (!r.hosted || r.hosted->id != a.mmid) {
            close_hosted(r);
            Hosted h;
            h.id = a.mmid; h.series = a.series; h.relayed = relayed_mode(a.series);
            r.hosted = std::move(h);
            if (r.hosted->relayed)
                note("your {} (MMID {:#x}) is shown to the other players.", a.series, a.mmid);
            else
                note("your {} stays local: this mode is not linked to other players.", a.series);
        }
        auto &h = *r.hosted;
        h.placement = std::move(a.placement);
        h.settings = std::move(a.settings);
        h.rules={a.rules_mask,a.rules_verified};
        if(h.series==skate_mode && !h.rules.verified)r.notices.push_back("S.K.A.T.E. setup rules could not be verified; this drop cannot link.");
        if (h.relayed && !h.started) send_offer(r, now);
        break;
    }
    case Local::joined:
        // Joining a queue removes the player from any other one and destroys a drop it hosts.
        if (r.hosted && a.mmid != r.hosted->id) close_hosted(r);
        leave_mirrors(r, a.mmid);
        for (auto &[key, m] : r.mirrors)
            if (m.mmid == a.mmid && m.state == Mirror::State::open && !m.joined) {
                m.joined = true;
                send(r, message(Kind::join, key.leader, key.id));
                note("you joined a linked {} (MMID {:#x}).", m.series, m.mmid);
            }
        break;
    case Local::left:
        leave_mirrors(r, 0);
        if (r.hosted && !r.hosted->started) close_hosted(r); // the host leaving destroys its queue
        break;
    case Local::force_started:
        if (r.hosted && r.hosted->id == a.mmid && !r.hosted->started) {
            std::vector<std::uint64_t> order{r.local};
            order.insert(order.end(), r.hosted->members.begin(), r.hosted->members.end());
            send_start(r, std::move(order));
        }
        break;
    case Local::destroy_requested:
        if (r.hosted && r.hosted->id == a.mmid && !r.hosted->started) close_hosted(r);
        for (auto &[key, m] : r.mirrors)
            if (m.mmid == a.mmid && m.state == Mirror::State::open) { m.state = Mirror::State::gone; m.gone_at = now; }
        break;
    case Local::entered: {
        // Which queue became the event: the copy the player joined, else its own drop.
        // Participants outside that queue mean some other activity.
        std::vector<std::uint64_t> players;
        bool foreign{};
        for (const auto id : a.participants) {
            if (const auto player = player_of(r, id)) players.push_back(*player);
            else foreign = true;
        }
        for (auto &[key, m] : r.mirrors) {
            if (!m.joined || (m.state != Mirror::State::open && m.state != Mirror::State::started)) continue;
            const bool ours = !foreign && std::all_of(players.begin(), players.end(), [&](std::uint64_t p) {
                return p == r.local || p == key.leader || contains(m.members, p);
            });
            if (!ours) continue;
            m.state = Mirror::State::started;
            m.start.reset();
            set_running(r, key, m.series);
            set_players(r, players);
            note("your copy of a linked {} started with {} player(s).", m.series, players.size());
            return;
        }
        if (r.hosted && !r.hosted->started && !foreign &&
            std::all_of(players.begin(), players.end(), [&](std::uint64_t p) { return p == r.local || contains(r.hosted->members, p); })) {
            send_start(r, players);
            r.hosted->started = true;
            if (r.hosted->relayed) {
                set_running(r, {r.local, r.hosted->id}, r.hosted->series);
                set_players(r, players);
            }
        }
        break;
    }
    case Local::ended:
        if (r.running) {
            // A last total the throttle held back still counts for players still skating.
            if (r.own.dirty) {
                auto m = message(Kind::score, r.running->leader, r.running->id);
                m.value = r.own.value;
                send(r, m);
            }
            note("the linked throwdown ended here.");
        }
        end_running(r);
        break;
    case Local::score:
        if (r.running && r.running_series == jam_mode) { r.own.value = a.score; r.own.dirty = true; }
        break;
    case Local::row:
        // Every write in order (the round board adds): no throttling.
        if (r.running && r.running_series == spot_battle_mode) {
            auto m = message(Kind::row, r.running->leader, r.running->id);
            m.board = a.board; m.add = a.add; m.value = a.score;
            send(r, m);
        }
        break;
    case Local::turn_ended:
        // The local server ended the local player's turn (their request or its timer). S.K.A.T.E.
        // needs no relay: every server ends a turn by itself after its (relayed) attempt.
        if (r.running && r.running_series == spot_battle_mode && a.player && a.player == local_native_player_id() &&
            r.turns[r.local] > 0) {
            auto m = message(Kind::turn_end, r.running->leader, r.running->id);
            m.value = r.turns[r.local];
            send(r, m);
        }
        break;
    case Local::turn_started:
        if (r.running) {
            const auto player = player_of(r, a.player);
            r.active = player.value_or(0);
            if (player) ++r.turns[*player];
            if(r.rules.verified)skate::turn(player && *player==r.local);
            if (player && r.quit.contains(*player)) resolve_quit_turn(r, *player);
        }
        break;
    case Local::turn_shown:
        if (r.running)
            if (const auto player = player_of(r, a.player)) r.shown = *player;
        break;
    case Local::quit:
        // Quitting a coop challenge: the other copies let the local player go.
        if (r.challenge && r.challenge->started && !r.running)
            send(r, message(Kind::challenge_leave, r.challenge->key.leader, r.challenge->key.id));
        if (r.running) {
            send(r, message(Kind::leave, r.running->leader, r.running->id));
            note("you quit the linked throwdown; the other players resolve your turns.");
            // The local server would keep running it with only relayed players, and its turns,
            // timers and end would mix with the next throwdown's: end it, as one of them.
            for (const auto player : r.players)
                if (player != r.local)
                    if (const auto id = virtual_id(r, player); id && queue_throwdown_destroy_event(id)) break;
            end_running(r); // nothing more to follow, hide or replay here
        }
        break;
    case Local::attempt:
        if(r.rules.verified)break;
        // Only an attempt made on the local player's own turn counts (its server rejects the
        // rest, e.g. a wipeout while watching); each one is replayed in that same turn.
        if (r.running && r.running_series == skate_mode && a.player && a.player == local_native_player_id() &&
            r.turns[r.local] > 0) {
            auto m = message(Kind::attempt, r.running->leader, r.running->id);
            m.value = r.turns[r.local]; m.add = a.add; m.trick = a.trick;
            send(r, m);
            note("own S.K.A.T.E. attempt ({}) sent for turn {}.", a.add ? "landed" : "missed", m.value);
        }
        break;
    case Local::timer_failed:
        if (r.running && r.running_series == skate_mode)
            if (const auto player = player_of(r, a.player)) {
                const auto turn = r.turns[*player];
                if (*player == r.local && turn > 0) {
                    // Out of time with nothing submitted: the others fail this turn the same way.
                    auto m = message(Kind::attempt, r.running->leader, r.running->id);
                    m.value = turn; m.add = false;
                    if(r.rules.verified) {
                        if(!r.detector_submitted.insert({r.local,turn}).second)break;
                        m.rules_mask=r.rules.values;
                        skate::DetectedAttempt miss;miss.id=static_cast<std::uint64_t>(turn);m.detected=detected_bytes(miss);
                    }
                    send(r, m);
                    note("own S.K.A.T.E. turn {} ran out of time; sent as a miss.", turn);
                } else if (*player != r.local) {
                    r.timer_failed[*player] = turn;
                    note("this machine's timer failed player {:#x}'s turn {} before their attempt arrived.", *player, turn);
                }
            }
        break;
    }
}

void on_challenge_message(Relay &r, std::uint64_t sender, const ThrowdownMessage &m, std::uint64_t now);
void on_beacon_message(Relay &r, std::uint64_t sender, const ThrowdownMessage &m);
void on_message(Relay &r, std::uint64_t sender, const ThrowdownMessage &m, std::uint64_t now) {
    if (!r.peers.contains(sender)) return;
    if (m.kind == Kind::beacon) { on_beacon_message(r, sender, m); return; }
    if (m.kind >= Kind::challenge_start) { on_challenge_message(r, sender, m, now); return; }
    const Key key{m.leader, m.id};
    const auto mirror = r.mirrors.find(key);
    switch (m.kind) {
    case Kind::offer: {
        if (m.leader != sender || !relayed_mode(m.series)) return;
        if (mirror != r.mirrors.end()) {
            // A copy removed here (full, or left) stays removed while the leader keeps offering it.
            if (mirror->second.state == Mirror::State::gone) mirror->second.gone_at = now;
            if(mirror->second.series==skate_mode && mirror->second.state!=Mirror::State::started) {
                mirror->second.rules={m.rules_mask,true};
                if(mirror->second.state==Mirror::State::waiting)mirror->second.settings=m.settings;
            }
            for (const auto player : m.order) mirror_member(r, mirror->second, player, now);
            return;
        }
        // A leader whose last drop could not be shown here waits before another is tried.
        if (const auto refused = r.spawn_refused.find(m.leader); refused != r.spawn_refused.end() && now < refused->second) return;
        // A leader hosts one drop at a time: a new one replaces its older copies.
        for (auto &[other, copy] : r.mirrors)
            if (other.leader == m.leader) destroy_mirror(r, copy, now);
        Mirror copy;
        // A S.K.A.T.E. leader offers its drop only once its setup rules are verified.
        copy.rules={m.rules_mask,m.series==skate_mode};
        copy.series = m.series; copy.placement = m.placement; copy.settings = m.settings;
        for (const auto player : m.order)
            if (player != r.local && player != m.leader && !contains(copy.members, player)) copy.members.push_back(player);
        r.mirrors.emplace(key, std::move(copy));
        note("{} is offered by another player (their MMID {:#x}); showing it here.", m.series, m.id);
        return;
    }
    case Kind::close:
        if (m.leader != sender || mirror == r.mirrors.end()) return;
        destroy_mirror(r, mirror->second, now);
        return;
    case Kind::join:
        if (m.leader == r.local) {
            if (!r.hosted || r.hosted->id != m.id || r.hosted->started || !r.hosted->relayed ||
                contains(r.hosted->members, sender))
                return;
            r.hosted->members.push_back(sender);
            add_to_hosted(r);
            note("a player joined your {} (virtual id {:#x}).", r.hosted->series, virtual_id(r, sender));
        } else if (mirror != r.mirrors.end()) {
            mirror_member(r, mirror->second, sender, now);
        }
        return;
    case Kind::leave: {
        if (r.running && *r.running == key) {
            // Quit the running throwdown: resolve their turns from now on (this one too if up).
            if (std::find(r.players.begin(), r.players.end(), sender) != r.players.end() && r.quit.insert(sender).second) {
                note("player {:#x} quit the linked throwdown.", sender);
                if (r.active == sender) resolve_quit_turn(r, sender);
            }
            return;
        }
        const auto remove = [&](std::vector<std::uint64_t> &members, std::vector<std::uint64_t> &added) {
            if (!contains(members, sender)) return;
            erase(members, sender);
            if (contains(added, sender)) {
                erase(added, sender);
                if (r.in_world) queue_throwdown_remove(virtual_id(r, sender));
            }
        };
        if (m.leader == r.local && r.hosted && r.hosted->id == m.id && !r.hosted->started)
            remove(r.hosted->members, r.hosted->added);
        else if (mirror != r.mirrors.end() && mirror->second.state != Mirror::State::started)
            remove(mirror->second.members, mirror->second.added);
        return;
    }
    case Kind::start: {
        if (m.leader != sender || mirror == r.mirrors.end()) return;
        auto &copy = mirror->second;
        if(copy.series==skate_mode) {
            if(!copy.rules.verified || m.settings.empty())return;
            copy.rules={m.rules_mask,true};
            if(copy.settings!=m.settings)copy.pending_settings=m.settings;
        }
        if (!copy.joined) { destroy_mirror(r, copy, now); return; }
        if (copy.state == Mirror::State::open) start_mirror(r, key, copy, m.order);
        else if (copy.state == Mirror::State::waiting || copy.state == Mirror::State::spawning) copy.start = m.order;
        return;
    }
    case Kind::row:
    case Kind::turn_end:
    case Kind::attempt: {
        Remote entry{m.kind == Kind::row ? Remote::What::row : m.kind == Kind::turn_end ? Remote::What::turn_end
                                                                                         : Remote::What::attempt,
                     sender, m.board, m.add, m.value, m.trick};
        // A S.K.A.T.E. attempt is judged by the sender's trick detector and carries its result.
        const bool skate_event=(mirror!=r.mirrors.end() && mirror->second.series==skate_mode) ||
            (m.leader==r.local && r.hosted && r.hosted->id==m.id && r.hosted->series==skate_mode) ||
            (r.running && *r.running==key && r.running_series==skate_mode);
        if(m.kind==Kind::attempt && (skate_event || !m.detected.empty())) {
            if(!r.running || *r.running!=key) {
                const auto expected=mirror!=r.mirrors.end()?mirror->second.rules:
                    (m.leader==r.local && r.hosted && r.hosted->id==m.id?r.hosted->rules:skate::Rules{});
                if(!expected.verified || expected.values!=m.rules_mask)return;
            }
            entry.detected=read_detected(m);if(!entry.detected)return;
            if(r.running && *r.running==key) {
                if(!r.rules.verified || m.rules_mask!=r.rules.values || !contains(r.players,sender))return;
                if(!r.detector_submitted.insert({sender,m.value}).second)return;
            }
        }
        if (r.running && *r.running == key) {
            // Only a participant has turns and rows in it.
            if (contains(r.players, sender) && r.remote.size() < 512) r.remote.push_back(entry);
        } else if ((m.leader == r.local && r.hosted && r.hosted->id == m.id) || mirror != r.mirrors.end()) {
            if (auto &list = r.early_remote[key]; list.size() < 512) list.push_back(entry);
        }
        return;
    }
    case Kind::score:
        if (r.running && *r.running == key) {
            if (contains(r.players, sender)) r.scores[sender] = {m.value, true};
        } else if ((m.leader == r.local && r.hosted && r.hosted->id == m.id) || mirror != r.mirrors.end()) {
            r.early[key][sender] = m.value;
        }
        return;
    }
}

void on_spawned(Relay &r, std::uint64_t token, std::uint32_t mmid, std::uint32_t capacity, std::uint64_t now) {
    for (auto &[key, m] : r.mirrors) {
        if (m.token != token) continue;
        if (!mmid) {
            m.state = Mirror::State::gone;
            m.gone_at = now;
            r.spawn_refused[key.leader] = now + spawn_refused_ms;
            return;
        }
        m.mmid = mmid;
        m.max_players = capacity;
        if (m.state == Mirror::State::gone) { // closed while it was spawning
            queue_throwdown_destroy(mmid);
            return;
        }
        m.state = Mirror::State::open;
        note("another player's {} is now joinable here (MMID {:#x}, up to {} players).", m.series, mmid, capacity);
        if (m.start) { start_mirror(r, key, m, *m.start); return; }
        if (m.max_players && 1 + m.members.size() >= m.max_players) { destroy_mirror(r, m, now); return; }
        add_to_mirror(r, m);
        return;
    }
    // Not ours any more (world change): nothing should show it.
    if (mmid) queue_throwdown_destroy(mmid);
}

// ---------------------------------------------------------------------------
// Coop challenges.
std::string player_name(const Relay &r, std::uint64_t player) {
    const auto it = r.player_names.find(player);
    return it != r.player_names.end() && !it->second.empty() ? it->second : std::string("Another player");
}
std::uint32_t challenge_id_of(const Relay &r, const Challenge &c, std::uint64_t player) {
    if (const auto gone = c.departed.find(player); gone != c.departed.end()) return gone->second;
    return virtual_id(r, player);
}
void end_challenge(Relay &r) {
    r.challenge.reset();
    r.celebration.reset();
    std::lock_guard lock(r.mutex);
    r.plan.ids.clear();
    r.plan.guest = false;
    r.plan.leader_start.reset();
}
// One remote input into the running local copy; false while it cannot go yet.
bool apply_challenge_input(Relay &r, Challenge &c, std::uint64_t sender, const ThrowdownMessage &m) {
    const auto id = challenge_id_of(r, c, sender);
    if (!id) return true; // nobody to replay it as
    switch (m.kind) {
    case Kind::challenge_attempt: return queue_challenge_attempt(id, m.criteria, m.indexes);
    case Kind::challenge_slam: return queue_challenge_slam(id, m.value);
    case Kind::challenge_leave:
    case Kind::challenge_optout: return queue_challenge_leave(id);
    default: return true;
    }
}
void pump_challenge(Relay &r, std::uint64_t now) {
    if (!r.challenge) return;
    auto &c = *r.challenge;
    if (!c.started) {
        // A guest copy that never started (the challenge is locked here, or another activity runs).
        if (now - c.requested_at > 15000) {
            send(r, message(Kind::challenge_optout, c.key.leader, c.key.id));
            r.notices.push_back(std::format("Could not join {}'s coop challenge.", player_name(r, c.key.leader)));
            note("the coop challenge copy did not start; opted out.");
            end_challenge(r);
        }
        return;
    }
    while (!c.held.empty() && r.in_world && r.prepared) {
        if (!apply_challenge_input(r, c, c.held.front().first, c.held.front().second)) break;
        c.held.pop_front();
    }
}
// Whether a decline may be sent to `leader` now (and notes that it is).
bool decline_due(Relay &r, std::uint64_t leader, std::uint64_t now) {
    auto &last = r.declined[leader];
    if (last && now >= last && now - last < decline_interval_ms) return false;
    last = now;
    return true;
}
void on_challenge_message(Relay &r, std::uint64_t sender, const ThrowdownMessage &m, std::uint64_t now) {
    const Key key{m.leader, m.id};
    if (m.kind == Kind::challenge_start) {
        if (m.leader != sender || !contains(m.order, r.local)) return;
        const auto busy = !r.in_world || r.running || (r.challenge && r.challenge->key != key);
        // Only a party member pulls us into their challenge.
        const bool stranger = !r.party.contains(m.leader);
        if (busy || stranger || !challenge_invites_enabled.load(std::memory_order_relaxed)) {
            // One answer per leader every few seconds: repeated starts are not each answered.
            if (decline_due(r, m.leader, now)) {
                send(r, message(Kind::challenge_optout, m.leader, m.id));
                note("coop challenge from {:#x} declined ({}).", m.leader, busy ? "busy" : stranger ? "not in our party" : "invites off");
            }
            return;
        }
        if (r.challenge) return; // already joining this one
        Challenge c;
        c.key = key; c.series = m.series; c.challenge = m.challenge; c.contest = m.add; c.order = m.order;
        c.requested_at = now;
        for (const auto player : m.order)
            if (player != r.local)
                if (const auto id = virtual_id(r, player)) c.ids.push_back(id);
        {
            std::lock_guard lock(r.mutex);
            r.plan.guest = true; r.plan.series = c.series; r.plan.id = c.challenge; r.plan.ids = c.ids;
        }
        if (!queue_challenge_start(c.series, c.challenge, c.contest)) {
            if (decline_due(r, m.leader, now)) send(r, message(Kind::challenge_optout, m.leader, m.id));
            std::lock_guard lock(r.mutex);
            r.plan.guest = false;
            return;
        }
        r.notices.push_back(std::format("{} started a coop challenge: you are in it. Quit it from the pause menu to leave.",
                                        player_name(r, m.leader)));
        note("joining {:#x}'s coop challenge {} {} with {} player(s).", m.leader, c.series, c.challenge, c.order.size());
        r.challenge = std::move(c);
        return;
    }
    if (!r.challenge || r.challenge->key != key || !contains(r.challenge->order, sender)) return;
    auto &c = *r.challenge;
    if (m.kind == Kind::challenge_leave || m.kind == Kind::challenge_optout) {
        if (!c.out.insert(sender).second) return;
        note("player {:#x} is out of the coop challenge.", sender);
        if (!c.started) {
            // Not started here yet: the copy starts without them.
            const auto id = virtual_id(r, sender);
            std::erase(c.ids, id);
            std::lock_guard lock(r.mutex);
            std::erase(r.plan.ids, id);
            return;
        }
    } else if (c.out.contains(sender)) {
        return;
    }
    if (c.held.size() < 256) c.held.emplace_back(sender, m);
    pump_challenge(r, now);
}
void on_challenge_local(Relay &r, const ThrowdownLocalAction &a, std::uint64_t now) {
    switch (a.kind) {
    case Local::challenge_started: {
        std::optional<ChallengePlan::Leader> leader;
        bool guest{};
        {
            std::lock_guard lock(r.mutex);
            leader = std::exchange(r.plan.leader_start, std::nullopt);
            guest = std::exchange(r.plan.guest, false) && r.plan.series == a.series && r.plan.id == a.challenge;
            r.plan.ids.clear();
        }
        if (guest && r.challenge && !r.challenge->started) {
            r.challenge->started = true;
            note("the linked coop challenge started here.");
            pump_challenge(r, now);
            return;
        }
        if (leader && leader->series == a.series && leader->id == a.challenge && !leader->players.empty()) {
            Challenge c;
            c.key = {r.local, r.next_challenge++};
            c.series = a.series; c.challenge = a.challenge; c.contest = a.add;
            c.order.push_back(r.local);
            for (const auto player : leader->players) {
                c.order.push_back(player);
                c.ids.push_back(virtual_id(r, player));
            }
            c.leader = true; c.started = true;
            auto m = message(Kind::challenge_start, c.key.leader, c.key.id);
            m.series = c.series; m.challenge = c.challenge; m.add = c.contest; m.order = c.order;
            send(r, m);
            std::string names;
            for (const auto player : leader->players) names += (names.empty() ? "" : ", ") + player_name(r, player);
            r.notices.push_back(std::format("Coop challenge with {}.", names));
            note("started a coop challenge {} {} with {} nearby party member(s).", c.series, c.challenge, leader->players.size());
            r.challenge = std::move(c);
        }
        return;
    }
    case Local::challenge_attempt:
        if (r.challenge && r.challenge->started) {
            auto m = message(Kind::challenge_attempt, r.challenge->key.leader, r.challenge->key.id);
            m.criteria = a.criteria; m.indexes = a.indexes;
            send(r, m);
        }
        return;
    case Local::challenge_slam:
        if (r.challenge && r.challenge->started) {
            auto m = message(Kind::challenge_slam, r.challenge->key.leader, r.challenge->key.id);
            m.value = a.score;
            send(r, m);
        }
        return;
    case Local::challenge_ended:
        if (r.challenge) note("the linked coop challenge ended here.");
        // The celebration runs before the activity lets the player go; its spots stay a little.
        r.challenge.reset();
        {
            std::lock_guard lock(r.mutex);
            r.plan.guest = false;
            r.plan.leader_start.reset();
        }
        return;
    default: return;
    }
}
// Where each other participant stands in the local celebration. Its Transform layout is found
// from where the local skater was put (spot 0) once it got there.
void update_celebration(Relay &r, std::uint64_t now) {
    if (r.challenge) r.celebration_ids = r.challenge->ids;
    if (auto raw = take_challenge_celebration(); raw && !r.celebration_ids.empty()) r.celebration = Celebration{*raw, now};
    if (!r.celebration) return;
    auto &c = *r.celebration;
    if (now - c.at > 25000) { r.celebration.reset(); return; }
    if (c.layout != -2 || now - c.at < 800 || !r.position) return;
    constexpr std::array<std::array<int, 3>, 4> layouts{{{3, 7, 11}, {0, 1, 2}, {4, 5, 6}, {8, 9, 10}}};
    const auto at = [](const std::array<float, 12> &t, const std::array<int, 3> &l) {
        return std::array<float, 3>{t[l[0]], t[l[1]], t[l[2]]};
    };
    float best = 1e9f;
    for (int i = 0; i < static_cast<int>(layouts.size()); ++i) {
        const auto p = at(c.raw.spots[0], layouts[i]);
        float d2{};
        for (int k = 0; k < 3; ++k) d2 += (p[k] - (*r.position)[k]) * (p[k] - (*r.position)[k]);
        if (d2 < best) { best = d2; c.layout = i; }
    }
    if (best > 4.f * 4.f) c.layout = -1;
    c.home = c.layout >= 0 ? at(c.raw.spots[0], layouts[static_cast<std::size_t>(c.layout)]) : *r.position;
    for (int k = 0; k < 3; ++k) c.lift[k] = (*r.position)[k] - c.home[k];
    // Participant order after the local player: the k-th virtual player takes Player(k+2).
    const auto &ids = r.celebration_ids;
    for (std::size_t k = 0; k < ids.size() && k + 1 < c.raw.spots.size(); ++k) {
        const auto player = player_of(r, ids[k]);
        if (!player) continue;
        std::array<float, 3> spot = c.home;
        if (c.layout >= 0) spot = at(c.raw.spots[k + 1], layouts[static_cast<std::size_t>(c.layout)]);
        else spot[0] += 1.5f * static_cast<float>(k + 1);
        c.spots[*player] = spot;
    }
    note("coop celebration: {} other player(s) placed ({}).", c.spots.size(),
         c.layout >= 0 ? "at their spots" : "beside the local player");
}
// Published for the StartChallenge hook: who a leader start would invite, or the guest copy.
void publish_challenge_plan(Relay &r, const ThrowdownRelayInput &in) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> nearby;
    for (const auto &peer : in.peers)
        if (peer.nearby && peer.party && nearby.size() + 1 < max_challenge_players)
            if (const auto id = virtual_id(r, peer.id)) nearby.emplace_back(peer.id, id);
    std::lock_guard lock(r.mutex);
    r.plan.nearby = std::move(nearby);
    r.plan.busy = !r.in_world || r.running.has_value() || r.challenge.has_value();
}

// Everything linked goes away: the session ended or the world changed.
// ---------------------------------------------------------------------------
// Party beacons. Each player's beacon is theirs alone: their own server spawned it, and every
// other machine puts the same beacon in its world as that player's (MoveSpawnedEntityForPlayer
// sent as their virtual id), so the world VFX, map icon and player card work natively there.
constexpr std::size_t max_shown_beacons = 24;         // world entities and map icons are not free
constexpr std::uint64_t beacon_resend_ms = 10000;     // for players who arrive later
void send_beacon(Relay &r, std::uint64_t now) {
    auto m = message(Kind::beacon, r.local, r.beacon_revision);
    m.add = r.own_beacon.has_value();
    if (r.own_beacon) m.location = *r.own_beacon;
    send(r, m);
    r.beacon_sent_at = now;
}
void on_beacon_local(Relay &r, const ThrowdownLocalAction &a, std::uint64_t now) {
    if (a.kind == ThrowdownLocalAction::Kind::beacon_placed) {
        // A second first placement is refused by the server (it asks to move the old one;
        // a move follows when the player agrees).
        if (!a.add && r.own_beacon) return;
        if (!std::all_of(a.location.begin(), a.location.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1e6f; }))
            return;
        r.own_beacon = a.location;
        note("the local player's party beacon is at ({:.1f}, {:.1f}, {:.1f}).", a.location[12], a.location[13], a.location[14]);
    } else {
        if (!r.own_beacon) return;
        r.own_beacon.reset();
        note("the local player's party beacon was removed.");
    }
    ++r.beacon_revision;
    send_beacon(r, now);
}
void hide_beacon(Relay &r, std::uint64_t player, Relay::Beacon &b, std::uint32_t id = 0) {
    if (!id) id = virtual_id(r, player);
    if (b.shown && r.in_world && id) queue_beacon_remove(id);
    b.shown = false;
}
void on_beacon_message(Relay &r, std::uint64_t sender, const ThrowdownMessage &m) {
    if (m.leader != sender) return;
    if (!m.add) {
        const auto it = r.beacons.find(sender);
        if (it == r.beacons.end()) return;
        hide_beacon(r, sender, it->second);
        r.beacons.erase(it);
        note("player {:#x} removed their party beacon.", sender);
        return;
    }
    auto &b = r.beacons[sender];
    if (b.revision == m.id && b.location == m.location) return; // a resend for players arriving later
    b.location = m.location;
    b.revision = m.id;
    b.shown = false; // moved: shown again where it now is (the move replaces the old one)
    b.retry_at = 0;
}
void pump_beacons(Relay &r, std::uint64_t now) {
    if (r.own_beacon && now - r.beacon_sent_at >= beacon_resend_ms) send_beacon(r, now);
    if (!r.in_world) return;
    auto shown = static_cast<std::size_t>(std::count_if(r.beacons.begin(), r.beacons.end(),
                                                        [](const auto &b) { return b.second.shown; }));
    for (auto &[player, b] : r.beacons) {
        if (b.shown || now < b.retry_at || shown >= max_shown_beacons) continue;
        const auto id = virtual_id(r, player);
        // Not ready yet (the event types are still being looked for): try again shortly.
        if (id && queue_beacon_move(id, b.location)) {
            b.shown = true;
            ++shown;
            note("showing player {:#x}'s party beacon as player {:#x}.", player, id);
        } else b.retry_at = now + 1000;
    }
}

void reset(Relay &r, bool destroy_copies, std::uint64_t now) {
    // Other players' beacons shown here go with the session (the world keeps no copies).
    for (auto &[player, b] : r.beacons)
        if (destroy_copies) hide_beacon(r, player, b);
    r.beacons.clear();
    close_hosted(r);
    if (destroy_copies && r.in_world && r.prepared)
        for (auto &[key, m] : r.mirrors) destroy_mirror(r, m, now);
    r.mirrors.clear();
    r.spawn_refused.clear();
    r.declined.clear();
    r.running.reset();
    r.scores.clear();
    r.early.clear();
    r.remote.clear();
    r.early_remote.clear();
    r.own = {};
    end_challenge(r);
}

void update_peers(Relay &r, const ThrowdownRelayInput &in, std::uint64_t now) {
    std::map<std::uint64_t, std::size_t> next;
    for (const auto &peer : in.peers)
        if (peer.id && peer.id != in.local && peer.slot < virtual_slots) next[peer.id] = peer.slot;
    bool arrived{};
    for (const auto &[player, slot] : next) arrived |= !r.peers.contains(player);
    for (const auto &[player, slot] : r.peers) {
        const auto it = next.find(player);
        if (it != next.end() && it->second == slot) continue;
        // Gone from the session (or moved slot, which only a reconnect does): take
        // them out of every queue under the id they had.
        const auto id = virtual_base + static_cast<std::uint32_t>(slot);
        const auto drop = [&](std::vector<std::uint64_t> &members, std::vector<std::uint64_t> &added) {
            erase(members, player);
            if (contains(added, player)) {
                erase(added, player);
                if (r.in_world) queue_throwdown_remove(id);
            }
        };
        if (r.hosted && !r.hosted->started) drop(r.hosted->members, r.hosted->added);
        for (auto &[key, m] : r.mirrors) {
            if (key.leader == player) destroy_mirror(r, m, now);
            else if (m.state != Mirror::State::started) drop(m.members, m.added);
        }
        r.scores.erase(player);
        // Gone: so is their beacon here.
        if (const auto beacon = r.beacons.find(player); beacon != r.beacons.end()) {
            hide_beacon(r, player, beacon->second, id);
            r.beacons.erase(beacon);
        }
        // Gone from a coop challenge: every copy lets them go.
        if (r.challenge && contains(r.challenge->order, player) && player != r.local &&
            r.challenge->out.insert(player).second) {
            auto &c = *r.challenge;
            c.departed[player] = id;
            if (c.started) {
                auto left = message(Kind::challenge_leave, c.key.leader, c.key.id);
                if (c.held.size() < 256) c.held.emplace_back(player, left);
            }
        }
        // Gone in the middle of the running throwdown: the same as quitting it. Their id there
        // stays theirs, so their turns resolve as a quitter's.
        if (r.running && contains(r.players, player) && player != r.local) {
            r.departed[player] = id;
            if (r.quit.insert(player).second) {
                note("player {:#x} left the session during the linked throwdown.", player);
                if (r.active == player) resolve_quit_turn(r, player);
            }
        }
    }
    r.peers = std::move(next);
    if (arrived && r.hosted && r.hosted->relayed && !r.hosted->started) r.hosted->next_offer = now;
    if (arrived && r.own_beacon) r.beacon_sent_at = 0; // they learn where it is at once
}

void refresh_player_info(Relay &r, std::uint64_t now) {
    if (now < r.next_info) return;
    r.next_info = now + 500;
    std::array<std::uint64_t, virtual_slots> fresh{};
    Address ui{}, manager{};
    if (r.base && memory::read(r.base + addr::engine::ui_manager, ui) && ui) memory::read(ui + 0x140, manager);
    if (manager)
        for (const auto &[player, slot] : r.peers) fresh[slot] = native_party_player_info(manager, slot);
    for (std::size_t i = 0; i < virtual_slots; ++i) r.info[i].store(fresh[i], std::memory_order_release);
}

// While the local player is in a linked queue, the ids its server may turn into an event.
void publish_order(Relay &r) {
    std::optional<Order> order;
    const auto add_players = [&](Order &o) {
        o.local = r.local;
        if (const auto local = local_native_player_id()) o.players[local] = r.local;
        for (const auto &[player, slot] : r.peers) o.players[virtual_base + static_cast<std::uint32_t>(slot)] = player;
    };
    for (const auto &[key, m] : r.mirrors)
        if (m.joined && (m.state == Mirror::State::open || m.state == Mirror::State::started)) {
            order.emplace();
            order->leader = key.leader;
            add_players(*order);
            break;
        }
    if (!order && r.hosted && r.hosted->relayed && !r.hosted->started) {
        order.emplace();
        order->leader = r.local;
        add_players(*order);
    }
    std::lock_guard lock(r.mutex);
    r.order = std::move(order);
}

// Turn-based: the camera follows whoever is up when it is not the local player.
void follow_turn(Relay &r) {
    const auto up = r.shown ? r.shown : r.active;
    const bool watching = r.running && turn_based(r.running_series) && up && up != r.local &&
                          r.peers.contains(up) && !r.quit.contains(up);
    const auto target = watching ? up : 0;
    if (target != r.spectating) {
        if (target) note("following player {:#x}'s turn.", target);
        r.spectating = target;
    }
    spectate_party_member(r.spectating);
}

// Settings the party features need (analysis/party-re): the coop party sync (the Coop button's
// 5 s "waiting for party") knows only server players, which ReSkate parties are not, and ends
// with nobody; with it off the button starts at once and the relay invites the party. Party
// beacons are player-spawned entities, whose manager removes itself when they are off. Parties
// themselves (DelMarUI.EnableParties, IsPartiesEnabled 023023d7) gate every party button of the
// Social menu (Invite showed "Party Invites Disabled", Join "Party Closed"); this build ships
// them off.
void apply_party_settings(Relay &r, std::uint64_t now) {
    if (r.party_settings || now < r.next_party_settings) return;
    for (const auto &[name, value] : {std::pair{"DingoActivity.EnableCoopChallengeSyncTimer", "0"},
                                      std::pair{"DingoActivity.EnablePlayerSpawnedEntities", "1"},
                                      std::pair{"DelMarUI.EnableParties", "1"}}) {
        const auto result = change_named_setting(name, value, false);
        if (result.starts_with("error: ")) {
            r.next_party_settings = now + 1000; // not registered yet early in startup
            return;
        }
    }
    note("party settings applied (parties on, coop party sync off, player-spawned entities on).");
    r.party_settings = true;
}
void apply_queue_timer(Relay &r, bool linked, std::uint64_t now) {
    apply_party_settings(r, now);
    const auto wanted = linked ? linked_queue_timer : solo_queue_timer;
    const bool force_start = linked && !r.force_start_enabled;
    if ((r.timer == wanted && !force_start) || now < r.next_timer) return;
    if (r.timer != wanted) {
        // Settings are not registered yet early in startup: retry a second later.
        if (change_named_setting(queue_timer_setting, wanted, false).starts_with("error: ")) { r.next_timer = now + 1000; return; }
        r.timer = wanted;
        note("queue start timer {} s ({}).", wanted, linked ? "players in the session: the leader starts" : "solo");
    }
    if (force_start) {
        if (change_named_setting("DingoThrowdowns.EnableForceStartThrowdown", "1", false).starts_with("error: ")) r.next_timer = now + 1000;
        else r.force_start_enabled = true;
    }
}

void maintain(Relay &r, std::uint64_t now) {
    if (!r.in_world) return;
    // The event types belong to the level: each one's are found by a background heap scan,
    // started as soon as the player is in a world with others so a drop never waits for it.
    // Asked again four times a second rather than remembered, since a level change (in a
    // session or out of one) leaves them to be found again.
    if (now >= r.next_prepare) {
        r.next_prepare = now + 250;
        r.prepared = prepare_throwdown_injection();
    }
    std::erase_if(r.spawn_refused, [&](const auto &entry) { return now >= entry.second; });
    std::erase_if(r.declined, [&](const auto &entry) { return now < entry.second || now - entry.second >= decline_interval_ms; });
    if (r.prepared) {
        // A spawn nothing answered (the lab reports failures; this is the backstop) must not
        // hold every other copy back.
        for (auto &[key, m] : r.mirrors)
            if (m.state == Mirror::State::spawning && now >= m.spawning_since && now - m.spawning_since > spawning_wait_ms) {
                m.state = Mirror::State::gone;
                m.gone_at = now;
                r.spawn_refused[key.leader] = now + spawn_refused_ms;
                note("a linked {} never spawned here; giving up on it.", m.series);
            }
        // One spawn at a time: the server reports MMIDs in order.
        if (std::none_of(r.mirrors.begin(), r.mirrors.end(),
                         [](const auto &entry) { return entry.second.state == Mirror::State::spawning; }))
            for (auto &[key, m] : r.mirrors) {
                if (m.state != Mirror::State::waiting) continue;
                const auto host = virtual_id(r, key.leader);
                if (!host) { m.state = Mirror::State::gone; m.gone_at = now; continue; }
                m.token = r.next_token++;
                if (queue_throwdown_spawn(m.token, host, m.series, m.placement, m.settings)) {
                    m.state = Mirror::State::spawning;
                    m.spawning_since = now;
                }
                break;
            }
        for (auto &[key, m] : r.mirrors) {
            if (m.state != Mirror::State::open) continue;
            if (m.start) start_mirror(r, key, m, *m.start);
            else add_to_mirror(r, m);
        }
        add_to_hosted(r);
        if (r.running) {
            for (auto &[player, score] : r.scores)
                if (score.dirty)
                    if (const auto id = virtual_id(r, player); id && queue_throwdown_score(id, score.value)) {
                        score.dirty = false;
                        static std::atomic<unsigned> logged{};
                        if (logged.fetch_add(1) < 40) note("player {:#x}'s total {} queued for its row (id {:#x}).", player, score.value, id);
                    }
            // Rows add up, attempts decide letters and turn ends follow both, so they are
            // replayed strictly in order; one that cannot go yet (no leaderboard or event handle
            // so far, or an attempt whose turn has not started here) holds the rest.
            while (!r.remote.empty()) {
                const auto &entry = r.remote.front();
                const auto id = entry.detected && entry.player==r.local?local_native_player_id():virtual_id(r, entry.player);
                bool done = !id || (entry.detected && r.detector_applied.contains({entry.player,entry.value})); // gone or already applied
                if (id && !done) switch (entry.what) {
                case Remote::What::row: done = queue_throwdown_row(id, entry.board, entry.add, entry.value); break;
                case Remote::What::turn_end:
                case Remote::What::attempt: {
                    // Both belong to that player's value-th turn: wait for it to start here, and
                    // never let one fall into another turn (a turn end would cut it short).
                    const char *what = entry.what == Remote::What::attempt ? "attempt" : "turn end";
                    const auto turn = r.turns[entry.player];
                    const auto failed = r.timer_failed.find(entry.player);
                    if (turn > entry.value || (turn == entry.value && (r.active != entry.player ||
                                                                       (failed != r.timer_failed.end() && failed->second == turn)))) {
                        note("player {:#x}'s {} for turn {} arrived after this machine moved on.", entry.player, what, entry.value);
                        done = true;
                    } else if (turn == entry.value) {
                        if(entry.detected) {
                            if(entry.detected->violation!=skate::Category::none)
                                r.notices.push_back(skate::forbidden(entry.detected->violation));
                            skate::prepare_native(id,*entry.detected);
                        }
                        done = entry.what == Remote::What::attempt ? queue_throwdown_attempt(id, entry.add, entry.trick)
                                                                   : queue_throwdown_end_turn(id);
                    } else if (r.remote_held_since && now - r.remote_held_since > attempt_wait_ms) {
                        note("player {:#x}'s {} for turn {} dropped: that turn never started here.", entry.player, what, entry.value);
                        done = true;
                    }
                    break;
                }
                }
                if (!done) {
                    if (!r.remote_held_since) r.remote_held_since = now;
                    break;
                }
                if(entry.detected)r.detector_applied.insert({entry.player,entry.value});
                r.remote.pop_front();
                r.remote_held_since = 0;
            }
        }
    }
    if (r.hosted && r.hosted->relayed && !r.hosted->started && now >= r.hosted->next_offer) send_offer(r, now);
    std::erase_if(r.mirrors, [&](const auto &entry) {
        return entry.second.state == Mirror::State::gone && now - entry.second.gone_at > gone_forget_ms;
    });
    if (r.running && r.own.dirty && now - r.own_sent_at >= score_interval_ms) {
        auto m = message(Kind::score, r.running->leader, r.running->id);
        m.value = r.own.value;
        send(r, m);
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) < 40) note("own total {} sent for {:#x}'s throwdown {:#x}.", m.value, m.leader, m.id);
        r.own.dirty = false;
        r.own_sent_at = now;
    }
}
} // namespace

std::vector<std::vector<std::uint8_t>> tick_throwdown_relay(std::uintptr_t base, const ThrowdownRelayInput &input) {
    auto &r = relay();
    const auto now = now_ms();
    std::deque<ThrowdownLocalAction> actions;
    std::deque<std::pair<std::uint64_t, std::vector<std::uint8_t>>> inbox;
    std::deque<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> spawned;
    {
        std::lock_guard lock(r.mutex);
        actions.swap(r.actions);
        inbox.swap(r.inbox);
        spawned.swap(r.spawned);
    }
    r.base = base;
    {
        // Names for the HUD (S.K.A.T.E. rows): the local player always, relayed players by id.
        std::map<std::uint32_t, std::string> names;
        if (const auto local = local_native_player_id(); local && !input.local_name.empty()) names[local] = input.local_name;
        for (const auto &peer : input.peers)
            if (peer.slot < virtual_slots && !peer.name.empty()) names[virtual_base + static_cast<std::uint32_t>(peer.slot)] = peer.name;
        std::lock_guard lock(r.mutex);
        r.names = std::move(names);
    }
    const bool linked = input.local && !input.peers.empty();
    try {
        apply_queue_timer(r, linked, now);
        if (!linked) {
            if (r.local || !r.mirrors.empty() || r.hosted) {
                if (r.local) note("session over; linked throwdowns removed.");
                r.in_world = input.in_world;
                if (input.barred) {
                    // Flagged (game speed, or mods that change scoring or physics): out of the linked coop
                    // challenge as well. The session's chat already says why.
                    if (r.challenge && r.challenge->started)
                        if (const auto id = local_native_player_id()) queue_challenge_leave(id);
                    if (r.running || r.challenge)
                        r.notices.push_back("You were taken out of the throwdown or challenge you were in.");
                }
                // The throwdown it was in had the others in it: nobody is left to play against.
                if (r.running && r.players.size() >= 2) end_alone(r);
                reset(r, true, now);
            }
            if (r.spectating) spectate_party_member(0);
            r.spectating = 0;
            waiting_offboard.store(false, std::memory_order_release);
            r.local = 0;
            r.peers.clear();
            r.outgoing.clear();
            {
                std::lock_guard lock(r.mutex);
                r.order.reset();
            }
            for (auto &info : r.info) info.store(0, std::memory_order_release);
            for (auto &[token, mmid, max] : spawned)
                if (mmid && input.in_world) queue_throwdown_destroy(mmid);
            if (now >= r.next_status) {
                r.next_status = now + 500;
                std::lock_guard lock(r.mutex);
                r.status_line = describe(r);
            }
            return {};
        }
        if (r.local != input.local || r.world != input.world) {
            // A new session or a new world: nothing linked survives (the level took the queues).
            // A new world took the local player's beacon too.
            if (r.world != input.world) r.own_beacon.reset();
            r.in_world = false;
            reset(r, false, now);
            r.local = input.local;
            r.world = input.world;
        }
        r.in_world = input.in_world;
        r.position = input.position;
        r.player_names.clear();
        r.party.clear();
        std::map<std::string, std::uint64_t> named;
        for (const auto &peer : input.peers) {
            r.player_names[peer.id] = peer.name;
            if (peer.party) r.party.insert(peer.id);
            if (!peer.name.empty()) {
                const auto [it, added] = named.try_emplace(peer.name, peer.id);
                if (!added) it->second = 0; // two players share it: it names neither
            }
        }
        {
            std::lock_guard lock(r.mutex);
            r.named = std::move(named);
        }
        update_peers(r, input, now);
        refresh_player_info(r, now);
        for (auto &action : actions) on_local(r, action, now);
        for (const auto &[token, mmid, max] : spawned) on_spawned(r, token, mmid, max, now);
        for (const auto &[sender, bytes] : inbox) {
            if (const auto m = decode_throwdown(bytes)) on_message(r, sender, *m, now);
            else logging::write(logging::Level::warning, logging::Channel::progression, "Throwdowns: unreadable message from another player dropped.");
        }
        if(r.running && r.rules.verified && r.active==r.local && r.turns[r.local]>0) {
            if(auto result=skate::take()) {
                const auto turn=r.turns[r.local];
                if(r.detector_submitted.insert({r.local,turn}).second) {
                    auto m=message(Kind::attempt,r.running->leader,r.running->id);
                    m.rules_mask=r.rules.values;m.value=turn;m.add=result->landed;m.trick=result->native;
                    try {m.detected=detected_bytes(*result);}catch(...) {
                        result->landed=false;result->signature.clear();result->display.clear();m.add=false;m.detected=detected_bytes(*result);
                        r.notices.push_back("Detected trick exceeds the network limit; attempt failed.");
                    }
                    auto evidence=Json::object();evidence["rule_version"]=skate::rules_version;evidence["turn"]=turn;
                    evidence["attempt_id"]=result->id;evidence["landed"]=result->landed;evidence["display"]=result->display;
                    evidence["violation"]=static_cast<unsigned>(result->violation);evidence["signature"]=result->signature;
                    trick_logger::detected_attempt(std::move(evidence));
                    logging::log(logging::Level::info,logging::Channel::progression,"S.K.A.T.E. detector outcome: turn {}, landed {}, result '{}'",turn,result->landed,result->display);
                    send(r,m);Remote entry{Remote::What::attempt,r.local,0,m.add,turn,m.trick};entry.detected=*result;r.remote.push_front(std::move(entry));
                }
            }
        }
        for(const auto& diagnostic:skate::take_diagnostics())
            logging::log(logging::Level::info,logging::Channel::progression,
                "S.K.A.T.E. detector: turn {}, active {:#x}: {}",r.turns[r.local],r.active,diagnostic);
        if(skate::take_bridge_error()) {
            r.notices.push_back("S.K.A.T.E. detector could not apply a native result; leaving the match.");
            if(r.running)send(r,message(Kind::leave,r.running->leader,r.running->id));
            queue_throwdown_destroy_event(local_native_player_id());end_running(r);
        }
        maintain(r, now);
        if (alone(r)) end_alone(r);
        pump_challenge(r, now);
        pump_beacons(r, now);
        update_celebration(r, now);
        publish_challenge_plan(r, input);
        publish_order(r);
        follow_turn(r);
        // Who is up as the local client shows it: it changes seconds before the turn
        // start teleport, so that teleport already knows whether to land on foot.
        const auto up = r.shown ? r.shown : r.active;
        waiting_offboard.store(offboard_enabled.load(std::memory_order_relaxed) && r.running &&
                                   r.running_series == skate_mode && up && up != r.local,
                               std::memory_order_release);
        if (now >= r.next_status) {
            r.next_status = now + 500;
            auto text = describe(r);
            std::lock_guard lock(r.mutex);
            r.status_line = std::move(text);
        }
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::progression, "Throwdowns: relay error: {}", e.what());
    }
    return std::exchange(r.outgoing, {});
}

void receive_throwdown_relay(std::uint64_t sender, std::span<const std::uint8_t> message) {
    auto &r = relay();
    std::lock_guard lock(r.mutex);
    if (r.inbox.size() < 256) r.inbox.emplace_back(sender, std::vector<std::uint8_t>(message.begin(), message.end()));
}

void throwdown_relay_local(ThrowdownLocalAction action) noexcept {
    try {
        auto &r = relay();
        std::lock_guard lock(r.mutex);
        if (r.actions.size() < 256) r.actions.push_back(std::move(action));
    } catch (...) {}
}

void throwdown_relay_spawned(std::uint64_t token, std::uint32_t mmid, std::uint32_t capacity) noexcept {
    try {
        auto &r = relay();
        std::lock_guard lock(r.mutex);
        r.spawned.emplace_back(token, mmid, capacity);
    } catch (...) {}
}

bool throwdown_relay_hides(std::uint64_t player) noexcept {
    const auto &r = relay();
    // Who is up as the local client shows it (it moves to the next player as soon as the
    // server picks them); before the first turn state arrives, the first in turn order.
    const auto up = r.shown ? r.shown : r.active ? r.active : r.players.empty() ? 0 : r.players.front();
    return r.running && turn_based(r.running_series) && up && player != up &&
           std::find(r.players.begin(), r.players.end(), player) != r.players.end();
}

bool throwdown_relay_waits_offboard() noexcept { return waiting_offboard.load(std::memory_order_acquire); }
void set_throwdown_offboard(bool enabled) noexcept {
    offboard_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) waiting_offboard.store(false, std::memory_order_release);
}
bool throwdown_offboard_enabled() noexcept { return offboard_enabled.load(std::memory_order_relaxed); }

bool throwdown_relay_order(std::span<std::uint32_t> ids) noexcept {
    try {
        auto &r = relay();
        std::lock_guard lock(r.mutex);
        if (!r.order) return false;
        const auto &order = *r.order;
        // Only a linked queue's participants: the local player and known players.
        const auto local = std::find_if(order.players.begin(), order.players.end(),
                                        [&](const auto &entry) { return entry.second == order.local; });
        if (local == order.players.end() || std::find(ids.begin(), ids.end(), local->first) == ids.end()) return false;
        // Leader first, then by Steam ID; ids no player is known for keep their places after them.
        const auto rank = [&](std::uint32_t id) -> std::pair<int, std::uint64_t> {
            const auto it = order.players.find(id);
            if (it == order.players.end()) return {2, 0};
            return {it->second == order.leader ? 0 : 1, it->second};
        };
        std::stable_sort(ids.begin(), ids.end(), [&](std::uint32_t a, std::uint32_t b) { return rank(a) < rank(b); });
        return true;
    } catch (...) { return false; }
}

std::string throwdown_relay_player_name(std::uint32_t player_id) {
    auto &r = relay();
    std::lock_guard lock(r.mutex);
    const auto it = r.names.find(player_id);
    return it == r.names.end() ? std::string{} : it->second;
}

std::uint64_t throwdown_relay_player_named(std::string_view name) noexcept {
    try {
        auto &r = relay();
        std::lock_guard lock(r.mutex);
        const auto it = r.named.find(std::string(name));
        return it == r.named.end() ? 0 : it->second;
    } catch (...) { return 0; }
}

std::uint64_t throwdown_relay_player_info(std::uint32_t player_id) noexcept {
    if (player_id < virtual_base || player_id >= virtual_base + virtual_slots) return 0;
    return relay().info[player_id - virtual_base].load(std::memory_order_acquire);
}

std::vector<std::string> take_throwdown_relay_notices() { return std::exchange(relay().notices, {}); }

std::optional<std::vector<std::uint32_t>> throwdown_relay_challenge_players(std::string_view series,
                                                                           std::string_view id) noexcept {
    try {
        auto &r = relay();
        std::lock_guard lock(r.mutex);
        auto &p = r.plan;
        // A guest copy: exactly the challenge the leader started, with the leader's order.
        if (p.guest && p.series == series && p.id == id) return p.ids;
        if (p.busy || p.nearby.empty() || !challenge_invites_enabled.load(std::memory_order_relaxed)) return std::nullopt;
        // The local player starts one with players nearby: they are invited (and opt out if they must).
        ChallengePlan::Leader leader{std::string(series), std::string(id), {}};
        std::vector<std::uint32_t> ids;
        for (const auto &[player, virtual_player] : p.nearby) {
            leader.players.push_back(player);
            ids.push_back(virtual_player);
        }
        p.leader_start = std::move(leader);
        return ids;
    } catch (...) { return std::nullopt; }
}
std::optional<std::array<float, 3>> throwdown_relay_celebration_offset(std::uint64_t player,
                                                                      const std::array<float, 3> &root) noexcept {
    const auto &r = relay();
    if (!r.celebration) return std::nullopt;
    const auto &c = *r.celebration;
    const auto it = c.spots.find(player);
    if (it == c.spots.end()) return std::nullopt;
    const auto &spot = it->second;
    float d2{};
    for (int k = 0; k < 3; ++k) d2 += (root[k] - c.home[k]) * (root[k] - c.home[k]);
    // In their own celebration (their machine put them at spot 0 too): the whole pose moves over,
    // board and height as they are. Anywhere else, their root goes where the local one stands.
    std::array<float, 3> offset{};
    for (int k = 0; k < 3; ++k) offset[k] = d2 < 4.f * 4.f ? spot[k] - c.home[k] : spot[k] + c.lift[k] - root[k];
    return offset;
}
void set_challenge_invites(bool enabled) noexcept { challenge_invites_enabled.store(enabled, std::memory_order_relaxed); }
bool challenge_invites() noexcept { return challenge_invites_enabled.load(std::memory_order_relaxed); }

std::string throwdown_relay_status() {
    auto &r = relay();
    std::lock_guard lock(r.mutex);
    return r.status_line.empty() ? std::string("Throwdown relay: not running yet.") : r.status_line;
}

namespace {
// Built by the relay tick, which owns the state it describes.
std::string describe(const Relay &r) {
    if (!r.local) return "Throwdown relay: no multiplayer session with other players.";
    std::string text = std::format("Throwdown relay: {} other player(s){}.", r.peers.size(), r.in_world ? "" : ", world not ready");
    for (const auto &[player, slot] : r.peers)
        text += std::format("\n  player {:#x} -> id {:#x}", player, virtual_base + slot);
    if (r.hosted)
        text += std::format("\n  your {} MMID {:#x}: {}, {} joined{}", r.hosted->series, r.hosted->id,
                            r.hosted->relayed ? "linked" : "local only", r.hosted->members.size(),
                            r.hosted->started ? ", started" : "");
    for (const auto &[key, m] : r.mirrors)
        text += std::format("\n  {:#x}'s {} (their {:#x}): {} MMID {:#x}, {} other(s){}", key.leader, m.series, key.id,
                            state_name(m.state), m.mmid, m.members.size(), m.joined ? ", you joined" : "");
    if (r.running)
        text += std::format("\n  running: {:#x}'s throwdown {:#x}, {} remote score(s)", r.running->leader, r.running->id, r.scores.size());
    if (r.challenge)
        text += std::format("\n  coop challenge: {} {} led by {:#x}, {} player(s), {} out, {}{}", r.challenge->series,
                            r.challenge->challenge, r.challenge->key.leader, r.challenge->order.size(), r.challenge->out.size(),
                            r.challenge->started ? "running here" : "starting", r.challenge->held.empty() ? "" :
                            std::format(", {} input(s) waiting", r.challenge->held.size()));
    text += std::format("\n  coop challenge invites {}", challenge_invites_enabled.load() ? "on" : "off");
    return text;
}
} // namespace
} // namespace dingosdk::multiplayer
