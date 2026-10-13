#include "native_throwdowns.h"
#include "grouped_number.h"
#include "throwdown_lab.h"
#include "throwdown_relay.h"
#include "virtual_player_names.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/native_throwdowns.h"
#include "Engine/Game/Build/20260929/localization.h"
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <set>
#include <string_view>

// Offline adapters for throwdowns. Each one stands in for a single backend or
// session answer that the authored flow waits on; everything else stays native.
namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
namespace engine = addr::engine;
namespace party = addr::native_party;
namespace throwdowns = addr::native_throwdowns;
constexpr std::uint32_t client_realm = 0xbf0f9789, server_realm = 0x98be5555;
struct State {
    Address base{};
    bool attempted{};
    std::atomic<bool> installed{};
    bool (*mode_available)(const char* const*){};
    bool (*others_allowed)(){};
    std::uint64_t (*player_info)(std::uint32_t){};
    std::uint64_t (*service_player_info)(Address, std::uint32_t){};
    void (*send_event)(Address, Address, const Address*){};
    Address (*parameters)(Address, Address){};
    std::atomic<bool> throwdown_active{}; // hosting: created a throwdown
    std::atomic<bool> throwdown_joined{}; // in one someone else placed
};
State& state() { static auto* value = new State; return *value; }
struct PendingParameters { Address vm{}, callback{}; };
thread_local PendingParameters pending_parameters;
// These adapters run inside native calls made for every networked event and after
// every expression graph: guarded same-process copies, not a system call per read.
template<class T> T read(Address at) {
    T result{};
    if (!memory::peek(at, result)) throw std::runtime_error("Throwdown memory unavailable");
    return result;
}
bool active() { return state().installed.load(std::memory_order_acquire) &&
    profile_runtime::local_runtime().active.load(std::memory_order_acquire); }
std::uint32_t realm() {
    Address context{};
    reinterpret_cast<Address (*)(Address*)>(state().base + engine::current_context)(&context);
    const auto offset = read<std::uint32_t>(state().base + engine::context_type_offset);
    return context && offset < 0x1000000 ? read<std::uint32_t>(context + offset) : 0;
}
// Native player id of the hosted offline server's only real player, or zero.
// Steam roster entries are UI models and never appear in this native list.
std::uint32_t sole_offline_player_id() {
    const auto base = state().base;
    const auto server = read<Address>(base + engine::game_server);
    if (!server || read<Address>(server) != base + engine::server_vtable) return 0;
    const auto context = read<Address>(server + 8);
    const auto manager_offset = read<std::uint32_t>(base + engine::context_player_manager_offset);
    if (!context || manager_offset > 0x1000000) return 0;
    const auto manager = read<Address>(context + manager_offset);
    if (!manager || read<Address>(manager) != base + engine::server_player_manager_vtable) return 0;
    const auto first = read<Address>(manager + 0xc8), last = read<Address>(manager + 0xd0);
    if (!first || last - first != 8) return 0;
    const auto player = read<Address>(first);
    // Registration offsets locate sibling base classes of the same player, so
    // the differences are signed and small.
    const auto parent = static_cast<std::int64_t>(read<std::uint32_t>(base + engine::player_parent_offset));
    const auto identity = static_cast<std::int64_t>(read<std::uint32_t>(base + throwdowns::player_identity_offset)) - parent;
    if (player < 0x100000 || identity < -0x10000 || identity > 0x10000) return 0;
    return read<std::uint32_t>(static_cast<Address>(static_cast<std::int64_t>(player) + identity) + 0x68);
}

// The throwdowner flow leaves its mode-select state in the same frame unless
// this native reports at least one of the modes it names. Retail fills the map
// behind it (service + 0xb0) from the throwdowns_v1 Mode records: JamSession,
// SpotBattle and ThrowdownSkate. No such service exists offline, so the map
// stays empty and every throwdown silently became a Jam. Race has no backend
// record and stays unavailable; DingoThrowdowns.Enable* still gates each mode.
bool mode_available_hook(const char* const* mode) {
    if (state().mode_available(mode)) return true;
    profile_runtime::PreserveError preserve;
    try {
        if (!active() || !mode) return false;
        const auto text = read<Address>(reinterpret_cast<Address>(mode));
        if (!text) return false;
        std::array<char, 16> id{};
        for (std::size_t i = 0; i + 1 < id.size(); ++i)
            if (!memory::peek(text + i, id[i]) || !id[i]) break;
        const std::string_view name(id.data());
        const bool known = name == "JamSession" || name == "SpotBattle" || name == "ThrowdownSkate";
        static std::atomic<unsigned> logged{};
        if (known && logged.fetch_add(1) < 3)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown mode {}: backend catalogue absent; reported available offline.", name);
        return known;
    } catch (...) { return false; }
}

// ThrowdownRegistration only shows another player's drop (flag, map icon, join
// prompt) when the throwdowner client system says so: it asks the backend whether
// this user may take part in other players' throwdowns and keeps the answer at
// system +0xfa. Offline that request never gets a reply, so every drop not hosted
// by the local player stayed invisible. There is nobody to restrict offline.
bool others_allowed_hook() {
    if (state().others_allowed()) return true;
    profile_runtime::PreserveError preserve;
    try {
        if (!active()) return false;
        static std::atomic<bool> logged{};
        if (!logged.exchange(true))
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdowns: no backend answer on joining other players' throwdowns; allowed offline.");
        return true;
    } catch (...) { return false; }
}

std::string_view throwdown_event(std::uint32_t hash) {
    switch (hash) {
    case 0x7fac81bb: return "CreateThrowdownQueue";
    case 0xb025c7fc: return "UpdateThrowdownQueueInfo";
    case 0x9c8de5a9: return "HostSetThrowdownParameters";
    case 0x303baecc: return "ForceStartThrowdown";
    case 0xc69c2920: return "ThrowdownAnnouncedToPlayer";
    case 0xf7a6803a: return "RequestThrowdownDestroyGlobal";
    case 0xed07dca8: return "ThrowdownHostExitedDetails";
    case 0x29212a7c: return "RestartThrowdown";
    case 0x525ccfa4: return "ThrowdownInviteAccepted";
    // ServerMPActivityBase.OnLeaveInProgressRequested answers the leaving
    // player alone (clients handle it in OnParticipantLeaveInProgressConfirmation).
    case 0x2226d34c: return "ParticipantLeaveInProgressConfirmation";
    // ServerSkateThrowdown tells each participant (by name) that an attempt was judged;
    // the client refreshes the S.K.A.T.E. letters and set-trick UI on it.
    case 0xb92f774d: return "Skate_AttemptUpdated";
    // PlayerSpawnedEntityManager asks the player placing a second party beacon whether to move
    // the first (the answer sends MoveSpawnedEntityForPlayer).
    case 0xe83bb2d0: return "PushConfirmPlayerSpawnedEntityModal";
    default: return {};
    }
}

// SendNetworkedEvent drops an event addressed to one player (route 1) before
// queueing it when that player's backend EID is empty, which is always the case
// offline. The hosted server has exactly one real player and its broadcast
// route reaches that same local connection, so throwdown events meant for the
// host are re-routed there.
void send_event_hook(Address signature, Address output, const Address* arguments) {
    bool loopback{};
    {
        profile_runtime::PreserveError preserve;
        try {
            if (active() && arguments && arguments[0] && arguments[2]) {
                const auto meta = read<Address>(read<Address>(arguments[0]));
                const auto hash = read<std::uint32_t>(meta);
                const auto route = read<std::uint32_t>(arguments[2]);
                const bool empty_target = route == 1 && arguments[3] && read<char>(read<Address>(arguments[3])) == 0;
                // A virtual coop challenge participant (virtual_player_names.h) has nobody behind it
                // here: its completion events, results and feed updates go nowhere.
                if (route == 1 && arguments[3] && !empty_target) {
                    std::array<char, virtual_eid_prefix.size()> head{};
                    if (memory::peek(read<Address>(arguments[3]), head) &&
                        std::string_view(head.data(), head.size()) == virtual_eid_prefix) {
                        static std::atomic<unsigned> dropped_virtual{};
                        if (dropped_virtual.fetch_add(1) < 32)
                            logging::log(logging::Level::info, logging::Channel::progression,
                                "Coop challenge: event {:#x} to a virtual participant dropped.", hash);
                        return;
                    }
                }
                const auto name = throwdown_event(hash);
                const auto current_realm = realm();
                if (current_realm == client_realm) observe_throwdown_send(hash, read<Address>(arguments[0]), arguments[1]);
                // CreateThrowdownQueue: the local player now hosts a throwdown.
                if (hash == 0x7fac81bb && !state().throwdown_active.exchange(true))
                    logging::log(logging::Level::info, logging::Channel::progression, "Throwdown: local player is hosting one.");
                // AddParticipantToCommunityEvent (the Join button): in one someone else placed,
                // which the Quit action must also leave. RemoveParticipant: left its queue.
                if (current_realm == client_realm && hash == 0xdc10392b && !state().throwdown_joined.exchange(true))
                    logging::log(logging::Level::info, logging::Channel::progression, "Throwdown: local player joined one.");
                if (current_realm == client_realm && hash == 0xaa3f9957) state().throwdown_joined.store(false);
                loopback = !name.empty() && empty_target && current_realm == server_realm && sole_offline_player_id();
                static std::atomic<unsigned> logged{};
                if (loopback && logged.fetch_add(1) < 16)
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdown event {}: single recipient has no backend id offline; delivered to the local player.", name);
                // Research (throwdown lab): which other single-recipient events are dropped here.
                static std::atomic<unsigned> dropped{};
                if (!loopback && empty_target && current_realm == server_realm && dropped.fetch_add(1) < 64)
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdown lab: server event {:#x} to an empty recipient dropped (graph {:#x}).", hash,
                        [] {
                            const auto vm = profile_runtime::executing_expression;
                            Address resource{};
                            std::uint32_t graph{};
                            return vm && memory::read(vm + 0x38, resource) && resource && memory::read(resource + 0x10, graph) ? graph : 0U;
                        }());
            }
        } catch (...) { loopback = false; }
    }
    if (!loopback) return state().send_event(signature, output, arguments);
    const std::uint32_t broadcast = 0;
    const std::array<Address, 4> local{arguments[0], arguments[1], reinterpret_cast<Address>(&broadcast), arguments[3]};
    state().send_event(signature, output, local.data());
}

constexpr std::array<std::uint32_t, 10> populate_parameters_layout{64,48,12,49,4,0,0,327682,720907,2048};
bool populate_parameters_graph(Address vm) {
    if (!vm) return false;
    const auto resource = read<Address>(vm + 0x38), instance = read<Address>(vm + 0x30);
    return instance && read<Address>(instance) == resource && read<std::uint32_t>(resource + 0x10) == 0x091ea871 &&
        read<std::array<std::uint32_t, 10>>(resource + 0x20) == populate_parameters_layout;
}

// ActivityDataService.OnPopulateTDParams asks throwdowns_v1 for the mode
// parameter overrides and waits for its callback. With no such service the
// native returns a null request and never calls back, so the host page never
// sends HostSetThrowdownParameters and a placed throwdown has no data to show.
// Keep a copy of the callable and complete it once the requesting graph has
// stored its result, exactly as an empty backend response would.
Address parameters_hook(Address output, Address callback) {
    const auto result = state().parameters(output, callback);
    profile_runtime::PreserveError preserve;
    try {
        const auto vm = profile_runtime::executing_expression;
        if (!active() || pending_parameters.callback || !callback || !read<Address>(callback) ||
            read<Address>(output) || !populate_parameters_graph(vm)) return result;
        const auto base = state().base;
        const auto services = read<Address>(base + engine::backend_services);
        // A registered throwdowns_v1 container means a real backend will answer.
        if (services && reinterpret_cast<Address (*)(Address)>(base + throwdowns::container_lookup)(services)) return result;
        Address owned{};
        reinterpret_cast<Address (*)(Address*, Address)>(base + engine::delegate_copy)(&owned, callback);
        pending_parameters = {vm, owned};
    } catch (...) {}
    return result;
}

// Authored graphs read developer switches through a native (hash 0xbc999b66,
// bound by a table row) that retail reduced to "return the default". The
// throwdown queue-timer rule asks it for Throwdowns.SinglePlayerQueue and, when
// true, lowers its minimum participant count to one -- the game's own way to
// let a lone host's queue count down and start. Every other name keeps its
// default, exactly as the stub answered.
void debug_variable(const char* const* name, const std::uint32_t* fallback, std::uint32_t* out) {
    *out = *fallback;
    profile_runtime::PreserveError preserve;
    try {
        if (!active() || !name) return;
        const auto text = read<Address>(reinterpret_cast<Address>(name));
        if (!text) return;
        constexpr std::string_view wanted = "Throwdowns.SinglePlayerQueue";
        std::array<char, wanted.size() + 1> value{};
        if (!memory::peek(text, value) || value[wanted.size()] != 0 ||
            std::string_view(value.data(), wanted.size()) != wanted) return;
        *out = 1;
        static std::atomic<bool> logged{};
        if (!logged.exchange(true))
            logging::write(logging::Level::info, logging::Channel::progression,
                "Throwdown queue: single-player queue enabled for local play.");
    } catch (...) {}
}

// GetUIPlayerInfoByPlayerId reads UIPlayerService's PlayerId -> model table,
// which only the online roster fills. Offline it has no entries, so the client
// ThrowdownRegistration retries QueryValidHostPlayer forever after placement
// and the placed throwdown never becomes visible or interactable. Answer for
// the local player only, with the profile record the player card already owns.
std::uint64_t player_info_hook(std::uint32_t player_id) {
    // A relayed player's virtual id: that member's own party record (name, card),
    // ahead of any native answer, which only knows real native players.
    if (const auto relayed = throwdown_relay_player_info(player_id)) return relayed;
    const auto original = state().player_info(player_id);
    if (original && throwdown_lab_player(player_id)) {
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) < 8)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown lab: UI record lookup for lab player {:#x} already answered natively ({:#x}).", player_id, original);
    }
    if (original || !player_id || !active()) return original;
    profile_runtime::PreserveError preserve;
    try {
        if (realm() != client_realm) return 0;
        // Throwdown lab ids (no native player) borrow the local record, as the
        // members' own records will once throwdowns are relayed.
        if (throwdown_lab_player(player_id)) {
            static std::atomic<unsigned> logged{};
            if (logged.fetch_add(1) < 8)
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown lab: UI record lookup for lab player {:#x} answered with the local record.", player_id);
            return profile_runtime::local_player_info_hook();
        }
        if (sole_offline_player_id() != player_id) return 0;
        const auto info = profile_runtime::local_player_info_hook();
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) < 4)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown host lookup: native player {} resolved to local profile model {:#x}.", player_id, info);
        return info;
    } catch (...) { return 0; }
}
}

// ThrowdownRegistration ANDs the two Blaze predicates into whether a placed
// throwdown is shown and interactable. Offline both are false, so the server
// accepted the placement while the client registration stayed hidden. The
// hosted local server has already produced the registration entity, which is
// what those predicates stand for; supply them to these exact graphs only.
bool native_throwdown_ready(unsigned predicate) noexcept {
    profile_runtime::PreserveError preserve;
    try {
        const auto vm = profile_runtime::executing_expression;
        if (!active() || predicate > 1 || !vm) return false;
        const auto resource = read<Address>(vm + 0x38);
        const auto instance = read<Address>(vm + 0x30);
        if (!instance || read<Address>(instance) != resource) return false;
        struct Graph { std::uint32_t hash; std::array<std::uint32_t, 10> layout; };
        constexpr std::array graphs{
            Graph{0xb0c0ef5c, {384,7528,9202,8067,41,0,50,8520164,30081156,17435906}}, // InitWrapper
            Graph{0x95080714, {352,6424,8961,7480,42,0,42,8520117,29687926,17435906}}, // ClientVars.OnGameGroupIdUpdated
            Graph{0x170bc8dd, {336,6280,8819,7096,36,0,41,8520097,29425793,17435906}}, // OnIsLocalPlayerInActivityChanged, OnMPActivityIdleStateChanged
            Graph{0xe7a43930, {336,6312,8838,7170,36,0,42,8520104,29491329,17435906}}, // OnNetworkedEntityComponentDataUnmarshaled
            Graph{0x9c20247e, {336,6312,8838,7170,36,0,42,8520104,29491329,17435906}}, // OnEntitlementsChanged
            // ActivityDataService.OnCheckLogin only waits for this before it
            // requests the throwdown parameters completed below.
            Graph{0x74d32688, {64,32,1,56,4,0,0,327684,11,0}}
        };
        const auto hash = read<std::uint32_t>(resource + 0x10);
        for (std::size_t i = 0; i < graphs.size(); ++i) {
            if (hash != graphs[i].hash || read<std::array<std::uint32_t, 10>>(resource + 0x20) != graphs[i].layout) continue;
            if (realm() != client_realm || !sole_offline_player_id()) return false;
            static std::array<std::atomic<bool>, graphs.size()> logged{};
            if (!logged[i].exchange(true))
                logging::log(logging::Level::info, logging::Channel::progression,
                    "Throwdown registration graph {:#x}: offline readiness supplied.", hash);
            return true;
        }
    } catch (...) {}
    return false;
}

namespace {
// dingo.data.game.throwdowns.v1.ModeParameter, as the live backend serves it:
// mode id, parameter id, then minimum / maximum / default.
struct ModeParameter { const char* mode; const char* parameter; float minimum, maximum, fallback; };
constexpr std::array mode_parameters{
    ModeParameter{"JamSession", "AllowOffBoard", 0, 1, 1},      ModeParameter{"JamSession", "AllowSessionMarkers", 0, 1, 1},
    ModeParameter{"JamSession", "MaxPlayers", 2, 10, 4},        ModeParameter{"JamSession", "Privacy", 0, 1, 0},
    ModeParameter{"JamSession", "ScoreFlips", 0, 1, 1},         ModeParameter{"JamSession", "ScoreGrabs", 0, 1, 1},
    ModeParameter{"JamSession", "ScoreGrinds", 0, 1, 1},        ModeParameter{"JamSession", "ScoreGroundTricks", 0, 1, 1},
    ModeParameter{"JamSession", "ScoreSlams", 0, 1, 0},         ModeParameter{"JamSession", "Timer", 60, 300, 180},
    ModeParameter{"JamSession", "WinCondition", 0, 3, 0},       ModeParameter{"JamSession", "Invite", 0, 2, 0},
    ModeParameter{"SpotBattle", "Rounds", 1, 5, 3},             ModeParameter{"SpotBattle", "TurnDuration", 10, 60, 30},
    ModeParameter{"SpotBattle", "Invite", 0, 2, 0},             ModeParameter{"SpotBattle", "MaxPlayers", 2, 10, 4},
    ModeParameter{"SpotBattle", "Privacy", 0, 1, 0},            ModeParameter{"ThrowdownSkate", "TurnDuration", 10, 60, 30},
    ModeParameter{"ThrowdownSkate", "Invite", 0, 2, 0},         ModeParameter{"ThrowdownSkate", "MaxPlayers", 2, 10, 4},
    ModeParameter{"ThrowdownSkate", "Privacy", 0, 1, 0}};

// Builds the array exactly as the native response flattener
// does: one engine allocation of count * 0x20 + 8 bytes, a {capacity, count}
// header before the first element, and per element two engine strings followed
// by three floats and two flag bytes.
Address build_mode_parameters(Address base, Address& array) {
    constexpr std::uint32_t count = static_cast<std::uint32_t>(mode_parameters.size());
    const auto allocate = reinterpret_cast<Address (*)(Address*, std::size_t, std::size_t, Address)>(base + party::array_allocate);
    const auto assign = reinterpret_cast<void (*)(Address, const char*, std::uint32_t)>(base + throwdowns::string_assign_chars);
    const auto block = allocate(&array, std::size_t{count} * 0x20 + 8, 8, 0);
    if (!block) throw std::runtime_error("Throwdown parameter array allocation failed");
    const auto data = block + 8;
    std::memset(reinterpret_cast<void*>(block), 0, std::size_t{count} * 0x20 + 8);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& record = mode_parameters[i];
        const auto element = data + std::size_t{i} * 0x20;
        assign(element, record.mode, static_cast<std::uint32_t>(std::strlen(record.mode)));
        assign(element + 8, record.parameter, static_cast<std::uint32_t>(std::strlen(record.parameter)));
        // The backend carries these as doubles and the native element keeps
        // them as floats. Written as integers they read back as ~0, which made
        // every queue "full" with its timer expired, so it started instantly.
        *reinterpret_cast<float*>(element + 0x10) = record.minimum;
        *reinterpret_cast<float*>(element + 0x14) = record.maximum;
        *reinterpret_cast<float*>(element + 0x18) = record.fallback;
        // +0x1d is the record's "enabled" flag: ThrowdownDetailsMenuRule.FillContent
        // skips the settings row of any parameter that has it clear (its own
        // debug format prints it as "[Parameter] name: min/max : default (enabled)").
        *reinterpret_cast<std::uint8_t*>(element + 0x1d) = 1;
    }
    *reinterpret_cast<std::uint32_t*>(data - 8) = count;   // capacity
    *reinterpret_cast<std::uint32_t*>(data - 4) = count;   // size
    return data;
}
}

std::uint32_t local_native_player_id() noexcept {
    profile_runtime::PreserveError preserve;
    try { return active() ? sole_offline_player_id() : 0; } catch (...) { return 0; }
}

bool local_throwdown_active() noexcept {
    return state().throwdown_active.load(std::memory_order_acquire) || state().throwdown_joined.load(std::memory_order_acquire);
}
bool local_throwdown_host() noexcept { return state().throwdown_active.load(std::memory_order_acquire); }

void complete_native_throwdown_parameters(Address vm) noexcept {
    pump_throwdown_lab(vm);
    // The throwdown is over once a client throwdown enters its end state or
    // leaves in progress. (Starting the round destroys the registration point
    // through MPThrowdownCoordinator.OnRequestThrowdownDestroy, so neither that
    // graph nor the registration-destroyed event marks the end.)
    if (local_throwdown_active()) {
        profile_runtime::PreserveError preserve;
        try {
            const auto resource = vm ? read<Address>(vm + 0x38) : 0;
            switch (resource ? read<std::uint32_t>(resource + 0x10) : 0) {
            case 0x224de9ab: case 0xd373badf: case 0x2ea80cac: case 0x1d82a056: // Client*Throwdown.EnterEnd
            case 0x91cd7c71:                                                     // ClientThrowdownBase.EnterLeaveInProgress
                if (state().throwdown_active.exchange(false) | state().throwdown_joined.exchange(false))
                    logging::log(logging::Level::info, logging::Channel::progression,
                        "Throwdown: ended (graph {:#x}).", read<std::uint32_t>(resource + 0x10));
                break;
            default: break;
            }
        } catch (...) {}
    }
    if (!pending_parameters.callback || pending_parameters.vm != vm) return;
    profile_runtime::PreserveError preserve;
    auto pending = pending_parameters;
    pending_parameters = {};
    const auto base = state().base;
    try {
        // The placement flow only sends HostSetThrowdownParameters -- which is
        // what makes the server open the queue -- when this list is not empty.
        auto array = reinterpret_cast<Address (*)()>(base + party::empty_array)();
        if ((read<std::uint32_t>(array - 4) & 0x7fffffffU) != 0) throw std::runtime_error("Native empty array differs");
        array = build_mode_parameters(base, array);
        reinterpret_cast<void (*)(Address*, Address*)>(base + engine::delegate_invoke)(&pending.callback, &array);
        reinterpret_cast<void (*)(Address*)>(base + throwdowns::array_release)(&array);
        logging::log(logging::Level::info, logging::Channel::progression,
            "Throwdown parameters: no backend service; delivered {} mode parameters.", mode_parameters.size());
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::progression, "Throwdown parameters: offline completion failed.");
    }
    try { reinterpret_cast<void (*)(Address*)>(base + engine::delegate_destroy)(&pending.callback); } catch (...) {}
}

// UIPlayerService's own PlayerId lookup (the expression native above is an
// inlined copy). Native UI builders call this one: the throwdown leaderboard
// links each row to its player through it, and a row without a
// player shows a blank name and avatar.
std::uint64_t service_player_info_hook(Address service, std::uint32_t player_id) {
    if (const auto relayed = throwdown_relay_player_info(player_id)) {
        profile_runtime::PreserveError preserve;
        try { if (service == read<Address>(state().base + engine::ui_manager)) return relayed; } catch (...) {}
    }
    const auto original = state().service_player_info(service, player_id);
    if (original || !player_id || !active()) return original;
    profile_runtime::PreserveError preserve;
    try {
        if (service != read<Address>(state().base + engine::ui_manager)) return 0;
        if (!throwdown_lab_player(player_id) && sole_offline_player_id() != player_id) return 0;
        return profile_runtime::local_player_info_hook();
    } catch (...) { return 0; }
}

namespace {
struct ThrowdownString {
    const char* id;
    const char* text;
};
// Throwdown text the shipped database lacks: S.K.A.T.E.'s title everywhere it is shown
// (mode select, details, banner, leaderboards, map), and the details pages' mode
// descriptions, in the backend's own words (its throwdowns_v1 mode records). An id the
// database has is left to it.
constexpr std::array throwdown_strings{
    ThrowdownString{"ID_ACTIVITY_SKATE_TITLE", "S.K.A.T.E."},
    ThrowdownString{"ID_ACTIVITY_SPOTBATTLE_DESC",
                    "Earn the [font effect=AttentionOutline color=Attention]Highest Score[/font] in a "
                    "[font effect=AttentionOutline color=Attention]Single Round[/font] to win!"},
    ThrowdownString{"ID_ACTIVITY_TD_DESC", "Do [font effect=AttentionOutline color=Attention]Skate Tricks[/font] to earn points!"},
    ThrowdownString{"ID_THROWDOWN_DESC_SKATE",
                    "Set and copy [font effect=AttentionOutline color=Attention]specific tricks[/font]. "
                    "Don't spell [font effect=AttentionOutline color=Attention]S.K.A.T.E[/font]."},
    ThrowdownString{"ID_ACTIVITY_SKATE_DESC",
                    "Set and copy [font effect=AttentionOutline color=Attention]specific tricks[/font]. "
                    "Don't spell [font effect=AttentionOutline color=Attention]S.K.A.T.E[/font]."},
};
std::uint32_t string_hash(std::string_view id) {
    std::uint32_t hash = 0xffffffffU;
    for (const unsigned char c : id) hash = hash * 33 + c;
    return hash;
}
// The string database the store's text is encoded against, or 0 while none is loaded.
Address string_database(Address base) {
    namespace loc = addr::localization;
    if (!read<Address>(base + loc::database_loaded)) return 0;
    const auto table = read<Address>(base + loc::databases);
    const auto last = read<std::int32_t>(base + loc::database_last);
    for (std::int32_t i = 0; table && i <= last && i < 64; ++i) {
        const auto row = table + 16 * static_cast<Address>(i);
        if (read<std::uint32_t>(row)) continue;
        const auto entry = read<Address>(row + 8);
        return entry ? read<Address>(entry + 0x10) : 0;
    }
    return 0;
}
} // namespace
void apply_throwdown_strings(Address base) noexcept {
    namespace loc = addr::localization;
    static ULONGLONG next{};
    static bool verified{}, refused{}, logged{};
    // The database the strings below were last stored against, and the ones the SDK added
    // (the game has none of its own for them).
    static Address database{};
    static std::set<std::uint32_t> ours;
    const auto now = GetTickCount64();
    if (!base || refused || now < next) return;
    next = now + 2000;
    try {
        if (!verified) {
            const auto matches = [&](Address rva, const auto& prefix) {
                std::array<unsigned char, 19> code{};
                return memory::peek_bytes(base + rva, code.data(), code.size()) &&
                       std::memcmp(code.data(), prefix.data(), code.size()) == 0;
            };
            if (!matches(loc::override_store, loc::override_store_prefix) ||
                !matches(loc::override_set, loc::override_set_prefix) ||
                !matches(loc::string_exists, loc::string_exists_prefix)) {
                refused = true;
                logging::write(logging::Level::warning, logging::Channel::progression,
                    "Throwdowns: the game's string store differs from the supported build; S.K.A.T.E. keeps its raw title.");
                return;
            }
            verified = true;
        }
        // Stored before a database is loaded, the text would be kept empty (and then count as
        // present). A new database (another language) needs the SDK's strings stored again.
        const auto loaded = string_database(base);
        if (!loaded) return;
        const bool changed = loaded != database;
        database = loaded;
        const auto exists = reinterpret_cast<bool (*)(std::uint32_t)>(base + loc::string_exists);
        const auto store = reinterpret_cast<Address (*)()>(base + loc::override_store)();
        if (!store) return;
        const auto set = reinterpret_cast<bool (*)(Address, const char*, const char*, int)>(base + loc::override_set);
        unsigned added{};
        for (const auto& entry : throwdown_strings) {
            const auto hash = string_hash(entry.id);
            if (!(ours.contains(hash) ? changed || !exists(hash) : !exists(hash))) continue;
            if (set(store, entry.id, entry.text, 0)) {
                ours.insert(hash);
                ++added;
            }
        }
        if (added && !logged) {
            logged = true;
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdowns: added {} S.K.A.T.E. strings the game does not ship (its title now reads S.K.A.T.E.).", added);
        }
    } catch (...) {}
}
// ParseInt, reading a grouped number whole (grouped_number.h). Any script may call it.
int (*parse_int_original)(const char* const*){};
int parse_int_hook(const char* const* text) {
    const auto parsed = throwdowns_text::parse_grouped_int(text ? *text : nullptr);
    if (parsed.grouped) {
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
            std::string shown(*text, strnlen(*text, 32));
            for (auto& ch : shown) if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
            logging::log(logging::Level::info, logging::Channel::progression,
                "Scripts: \"{}\" read as {} (the game's own ParseInt stops at the first separator).", shown, parsed.value);
        }
    }
    return parsed.value;
}
void install_parse_int(Address base) noexcept {
    try {
        if (read<std::array<unsigned char, 10>>(base + throwdowns::parse_int) != throwdowns::parse_int_prefix)
            throw std::runtime_error("ParseInt fingerprint differs; grouped scores are read as the game reads them");
        auto* const target = reinterpret_cast<void*>(base + throwdowns::parse_int);
        if (hook_prepare(target, reinterpret_cast<void*>(&parse_int_hook), reinterpret_cast<void**>(&parse_int_original)) != HookOk ||
            hook_enable(target) != HookOk)
            throw std::runtime_error("Cannot replace ParseInt; grouped scores are read as the game reads them");
    } catch (const std::exception& e) { logging::write(logging::Level::warning, logging::Channel::progression, e.what()); }
}
void initialize_native_throwdowns(Address base) noexcept {
    auto& s = state();
    if (s.attempted || !base) return;
    s.attempted = true; s.base = base;
    install_parse_int(base);
    struct Hook { Address rva; std::array<unsigned char, 19> prefix; void* replacement; void** original; };
    const std::array hooks{
        Hook{throwdowns::mode_available, throwdowns::mode_available_prefix, reinterpret_cast<void*>(&mode_available_hook), reinterpret_cast<void**>(&s.mode_available)},
        Hook{throwdowns::player_info, throwdowns::player_info_prefix, reinterpret_cast<void*>(&player_info_hook), reinterpret_cast<void**>(&s.player_info)},
        Hook{throwdowns::service_player_info, throwdowns::service_player_info_prefix, reinterpret_cast<void*>(&service_player_info_hook), reinterpret_cast<void**>(&s.service_player_info)},
        Hook{throwdowns::send_event, throwdowns::send_event_prefix, reinterpret_cast<void*>(&send_event_hook), reinterpret_cast<void**>(&s.send_event)},
        Hook{throwdowns::parameters, throwdowns::parameters_prefix, reinterpret_cast<void*>(&parameters_hook), reinterpret_cast<void**>(&s.parameters)},
        Hook{throwdowns::others_throwdowns_allowed, throwdowns::others_throwdowns_allowed_prefix,
             reinterpret_cast<void*>(&others_allowed_hook), reinterpret_cast<void**>(&s.others_allowed)}
    };
    try {
        const std::array helpers{
            Hook{throwdowns::container_lookup, throwdowns::container_lookup_prefix},
            Hook{engine::delegate_copy, throwdowns::delegate_copy_prefix},
            Hook{engine::delegate_destroy, throwdowns::delegate_destroy_prefix},
            Hook{engine::delegate_invoke, throwdowns::delegate_invoke_prefix},
            Hook{party::empty_array, throwdowns::empty_array_prefix},
            Hook{party::array_allocate, throwdowns::array_allocate_prefix},
            Hook{throwdowns::string_assign_chars, throwdowns::string_assign_chars_prefix},
            Hook{throwdowns::array_release, throwdowns::array_release_prefix}
        };
        for (const auto& helper : helpers) if (read<decltype(helper.prefix)>(base + helper.rva) != helper.prefix)
            throw std::runtime_error("Throwdown parameter helper fingerprint differs");
        for (const auto& hook : hooks) if (read<decltype(hook.prefix)>(base + hook.rva) != hook.prefix)
            throw std::runtime_error("Throwdown adapter fingerprint differs");
        std::size_t prepared{};
        for (const auto& hook : hooks) {
            if (hook_prepare(reinterpret_cast<void*>(base + hook.rva), hook.replacement, hook.original) != HookOk) {
                while (prepared) hook_remove(reinterpret_cast<void*>(base + hooks[--prepared].rva));
                throw std::runtime_error("Cannot prepare throwdown adapters");
            }
            ++prepared;
        }
        for (const auto& hook : hooks) if (hook_enable(reinterpret_cast<void*>(base + hook.rva)) != HookOk)
            throw std::runtime_error("Cannot enable throwdown adapters; restart ReSkate");
        {
            // A table row, not a function: the stub it points at is shared, so
            // rebind this one native instead of hooking the stub.
            constexpr Address row = throwdowns::debug_variable_row, stub = throwdowns::debug_variable_stub;
            DWORD protect{};
            if (read<std::uint32_t>(base + row) == 0xbc999b66 && read<Address>(base + row + 8) == base + stub &&
                VirtualProtect(reinterpret_cast<void*>(base + row + 8), sizeof(Address), PAGE_READWRITE, &protect)) {
                *reinterpret_cast<Address*>(base + row + 8) = reinterpret_cast<Address>(&debug_variable);
                VirtualProtect(reinterpret_cast<void*>(base + row + 8), sizeof(Address), protect, &protect);
            } else logging::write(logging::Level::warning, logging::Channel::progression,
                "Throwdown queue: developer-variable binding differs; single-player queue unavailable.");
        }
        s.installed.store(true, std::memory_order_release);
    } catch (const std::exception& e) { logging::write(logging::Level::warning, logging::Channel::progression, e.what()); }
    if (s.installed.load(std::memory_order_acquire) && !GetEnvironmentVariableW(L"RESKATE_NO_THROWDOWN_LAB", nullptr, 0)) initialize_throwdown_lab(base);
}
}
