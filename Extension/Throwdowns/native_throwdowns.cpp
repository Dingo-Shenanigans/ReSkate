#include "native_throwdowns.h"
#include "native_throwdown_lifetime.h"
#include "native_flag_session.h"
#include "session_marker_actions.h"
#include "throwdown_kit.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/NativeMenu/throwdown_menu.h"
#include "Extension/UI/NativeMenu/native_hud_selection.h"
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
#include "Engine/Game/Build/supported_build.h"
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
namespace kit = dingosdk::throwdowns::kit;
constexpr std::uint32_t client_realm = 0xbf0f9789, server_realm = 0x98be5555;
struct State {
    Address base{};
    bool attempted{};
    std::atomic<bool> installed{};
    bool (*mode_available)(const char* const*){};
    bool (*others_allowed)(){};
    std::uint64_t (*player_info)(std::uint32_t){};
    std::uint64_t (*service_player_info)(Address, std::uint32_t){};
    std::uint64_t (*player_in_activity)(std::uint32_t){};
    void (*send_event)(Address, Address, const Address*){};
    Address (*parameters)(Address, Address){};
    NativeThrowdownLifetime lifetime;
    NativeActivityUiLease custom_ui_presence;
    std::mutex custom_ui_presence_mutex;
    std::string custom_ui_diagnostic;
    std::uint64_t custom_ui_diagnostic_at{};
    std::atomic<Address> menu_trace_address{};
    std::atomic<DWORD> menu_trace_thread{};
    std::atomic<bool> menu_details_presence_reported{};
    Address solo_client_activity{};
    std::uint32_t solo_client_player{}, solo_client_group{};
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
// The MP participant id is not the PlayerId used by native client/UI getters.
// Steam roster entries are UI models and never appear in this native list.
NativeActivityPlayerIds sole_offline_player_ids() {
    const auto base = state().base;
    const auto server = read<Address>(base + engine::game_server);
    if (!server || read<Address>(server) != base + engine::server_vtable) return {};
    const auto context = read<Address>(server + 8);
    const auto manager_offset = read<std::uint32_t>(base + engine::context_player_manager_offset);
    if (!context || manager_offset > 0x1000000) return {};
    const auto manager = read<Address>(context + manager_offset);
    if (!manager || read<Address>(manager) != base + engine::server_player_manager_vtable) return {};
    const auto first = read<Address>(manager + 0xc8), last = read<Address>(manager + 0xd0);
    if (!first || last - first != 8) return {};
    const auto player = read<Address>(first);
    // Registration offsets locate sibling base classes of the same player, so
    // the differences are signed and small.
    const auto parent = static_cast<std::int64_t>(read<std::uint32_t>(base + engine::player_parent_offset));
    const auto identity = static_cast<std::int64_t>(read<std::uint32_t>(base + throwdowns::player_identity_offset)) - parent;
    if (player < 0x100000 || identity < -0x10000 || identity > 0x10000 ||
        read<Address>(player)!=base+engine::server_player_vtable ||
        read<Address>(base+engine::server_player_vtable+0x10)!=base+0x7ff930)return {};
    // Native virtual GetPlayerId is mov eax,[rcx+48]; ret (IDA/Ghidra and
    // both live client/server vtables agree). Read it without a virtual call.
    static const bool id_verified=read<std::array<unsigned char,4>>(base+0x7ff930)==
        std::array<unsigned char,4>{0x8b,0x41,0x48,0xc3};
    if(!id_verified)return {};
    return {read<std::uint32_t>(static_cast<Address>(static_cast<std::int64_t>(player) + identity) + 0x68),
        read<std::uint32_t>(player+0x48)};
}
std::uint32_t sole_offline_player_id() { return sole_offline_player_ids().participant; }

// Same client-player lookup and sibling-base adjustment used by the native
// GetPlayerIsInActivity/GetPlayerMPActivityIdleState functions (834520/834950).
// Never use the skater or the SDK's UI record as a native player pointer.
Address client_activity_variables(std::uint32_t participant,std::uint32_t* menu_player=nullptr) {
    const auto base=state().base;
    const auto ids=sole_offline_player_ids();
    if(!ids.matches_participant(participant))return 0;
    // Native GetPlayerIsInActivity selects the client realm explicitly with
    // GetContextForRealm, not the caller's TLS context. Our UI tick runs AFTER
    // original_tick returns, when that temporary TLS scope may already be gone.
    constexpr std::array<unsigned char,19> context_prefix{
        0x89,0x54,0x24,0x10,0x53,0x48,0x83,0xec,0x30,0x4c,0x8b,0x05,0xe8,0xb7,0x70,0x05,0x48,0x8b,0xd9};
    static const bool verified=read<std::array<unsigned char,19>>(base+0x18b6e40)==context_prefix;
    if(!verified)throw std::runtime_error("Native client context lookup fingerprint differs");
    Address context{};
    reinterpret_cast<Address (*)(Address*,std::uint32_t)>(base+0x18b6e40)(&context,client_realm);
    const auto realm_offset=read<std::uint32_t>(base+engine::context_type_offset);
    if(!context || realm_offset>0x1000000 || read<std::uint32_t>(context+realm_offset)!=client_realm)return 0;
    const auto offset=read<std::uint32_t>(base+engine::context_player_manager_offset);
    if(!context || offset>0x1000000)return 0;
    const auto manager=read<Address>(context+offset);
    if(!manager || read<Address>(manager)!=base+engine::local_player_manager_vtable)return 0;
    const auto first=read<Address>(manager+0x78),last=read<Address>(manager+0x80);
    if(!first || last<first || last-first>64*8 || (last-first)%8)return 0;
    const auto delta=static_cast<std::int64_t>(read<std::uint32_t>(base+0x6f7bae0))-
        static_cast<std::int64_t>(read<std::uint32_t>(base+0x75c3588));
    if(delta < -0x10000 || delta > 0x10000)return 0;
    for(auto at=first;at<last;at+=8) {
        const auto player=read<Address>(at);
        if(!player || read<Address>(player)!=base+engine::local_player_vtable)continue;
        const auto id_function=read<Address>(read<Address>(player)+0x10);
        if(id_function!=base+0x7ff930)continue;
        if(ids.matches_menu_player(read<std::uint32_t>(player+0x48))) {
            if(menu_player)*menu_player=ids.menu_player;
            return static_cast<Address>(static_cast<std::int64_t>(player)+delta);
        }
    }
    return 0;
}
void complete_solo_client_exit() {
    auto& s=state();
    if(!s.solo_client_activity && !s.lifetime.host())return;
    const auto uid=sole_offline_player_id();
    const auto activity=s.lifetime.solo_activity(uid);
    if(activity && activity!=s.solo_client_activity) {
        if(const auto vars=client_activity_variables(uid)) {
            s.solo_client_activity=activity;s.solo_client_player=uid;
            s.solo_client_group=read<std::uint32_t>(vars+0x100);
        }
    }
    const auto ended=s.lifetime.take_completed_solo();
    if(!ended)return;
    // A new queue or a changed group must not inherit this completed event's
    // cleanup. The server's confirmed removal/client EnterEnd arms it once.
    const auto vars=ended==uid && ended==s.solo_client_player && s.solo_client_activity &&
        !s.lifetime.active() && !native_flag::registration_owned()?client_activity_variables(ended):0;
    const auto group=vars?read<std::uint32_t>(vars+0x100):0;
    if(vars && group==s.solo_client_group) {
        const auto in_activity=read<std::uint8_t>(vars+0x104);
        const auto idle=read<std::uint32_t>(vars+0x108);
        if(in_activity || idle) {
            // ClientPlayerVars defaults confirmed by its native constructor
            // (5df4670). Offline AMP never supplies the final activity reset.
            const bool inactive=false;
            const std::uint32_t no_idle=0;
            SIZE_T written{};
            const bool reset_activity=WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(vars+0x104),&inactive,sizeof(inactive),&written) && written==sizeof(inactive);
            const bool reset_idle=WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(vars+0x108),&no_idle,sizeof(no_idle),&written) && written==sizeof(no_idle);
            logging::log(logging::Level::info,logging::Channel::progression,
                "Throwdown: confirmed solo exit for player {:#x}; client activity {} -> {}, idle {} -> {} (reset {}/{}).",
                ended,in_activity,read<std::uint8_t>(vars+0x104),idle,read<std::uint32_t>(vars+0x108),reset_activity,reset_idle);
        }
    } else logging::log(logging::Level::warning,logging::Channel::progression,
        "Throwdown: solo exit for {:#x} did not match the captured client activity; reset skipped.",ended);
    s.solo_client_activity=0;s.solo_client_player=0;s.solo_client_group=0;
}

// While a plugin match blocks markers (kit::match().markers) the session marker conditions read
// false and the pause menu routes to the activity, whose Quit the match's plugin hears.
void complete_match_controls(Address vm) {
    if(!kit::match().markers || !vm || realm()!=client_realm)return;
    const auto resource=read<Address>(vm+0x38),instance=read<Address>(vm+0x30);
    if(!resource || !instance || read<Address>(instance)!=resource)return;
    const auto hash=read<std::uint32_t>(resource+0x10);
    const auto layout=read<std::array<std::uint32_t,10>>(resource+0x20);
    unsigned output_offset{};
    bool result=false,matched=false;
    if(hash==0x32c0b460 && layout==std::array<std::uint32_t,10>{64,32,10,52,4,0,0,3,0,0}) {
        output_offset=16;matched=true; // SessionMarkerReturn condition
    } else if((hash==0x870491b0 || hash==0xa5d0ee41) && layout==std::array<std::uint32_t,10>{48,64,68,95,3,0,0,8,720896,0}) {
        output_offset=8;matched=true; // Marker set/setting rumble conditions
    } else if(hash==0x3a5e5108 && layout==std::array<std::uint32_t,10>{64,8,37,113,4,0,0,9,786432,0}) {
        result=true;matched=true; // Native UI activity pause/marker routing
    } else if(hash==0x6998ec4b && layout==std::array<std::uint32_t,10>{48,288,98,211,3,0,3,327693,1572864,16777472}) {
        // QuitChallenge is the original pause modal's confirmed Quit Match
        // action. Its native picker may have retired already; still end the match.
        native_flag::quit_match();
    }
    if(!matched)return;
    const auto arguments=read<Address>(instance+((layout[0]+15U)&~15U)+8);
    const auto output=arguments?read<Address>(arguments+output_offset):0;
    SIZE_T written{};
    if(output)WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(output),&result,sizeof(result),&written);
}

void complete_custom_details_presence(Address vm) {
    // This is the authored ActivitiesDetailsScreen -> MenusClosed condition,
    // not its OnEnd action. Let the native condition execute all its reads and
    // preserve its selected-menu/navigation/layer close reasons (local 0x80).
    // Its other term is !activity_present, local 0x81. Native presence means
    // DynamicPhaseHandler.PhaseSetEntities has entries when the dynamic manager
    // exists; otherwise it uses RuleActionManager.ActiveRuleActionHandles.
    // A custom match supplies that activity ownership only for its own Start
    // request. No fake native event, phase entity, camera or Page is created.
    auto& s=state();
    const auto selection=s.menu_trace_address.load(std::memory_order_acquire);
    if(!selection || s.menu_trace_thread.load(std::memory_order_relaxed)!=GetCurrentThreadId() || !vm)return;
    const auto resource=read<Address>(vm+0x38),instance=read<Address>(vm+0x30);
    constexpr std::array<std::uint32_t,10> layout{48,296,140,332,3,0,3,655385,2293760,0};
    if(!resource || !instance || read<Address>(instance)!=resource ||
       read<std::uint32_t>(resource+0x10)!=0x05edc9f8 ||
       read<std::array<std::uint32_t,10>>(resource+0x20)!=layout)return;
    if(!active() || realm()!=client_realm || !kit::match().in_activity)return;
    const auto pages=instance+48;
    const auto constants=read<Address>(pages),arguments=read<Address>(pages+8),locals=read<Address>(pages+16);
    if(!constants || !arguments || !locals ||
       read<std::uint32_t>(constants+8)!=0xaf4c67d7 ||
       read<std::uint32_t>(constants+0x28)!=0xcc4776b5 ||
       read<std::uint32_t>(constants+0x48)!=0xd71ee0a1)return;
    const auto output=read<Address>(arguments);
    if(!output || read<std::int32_t>(selection)!=1)return;
    std::array<std::uint8_t,140> values{};
    if(!memory::peek_bytes(locals,values.data(),values.size()))return;
    const auto original=read<std::uint8_t>(output);
    // The exact final native copy must agree with its saved result. Reject an
    // expired/nested frame or changed graph instead of borrowing its pointers.
    if(values[0x8b]!=original || values[0x89]!=1 || values[0x8a]!=1 ||
       !custom_details_presence_needed(true,values[0x64],original,values[0x80],values[0x81]))return;
    {
        std::lock_guard presence_lock(s.custom_ui_presence_mutex);
        if(!s.custom_ui_presence.owned())return;
    }
    if(s.lifetime.active() || s.lifetime.has_activity())return;
    const std::uint8_t keep_native_details=0;
    SIZE_T written{};
    if(WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(output),&keep_native_details,
            sizeof(keep_native_details),&written) && written==sizeof(keep_native_details) &&
       !s.menu_details_presence_reported.exchange(true,std::memory_order_relaxed))
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: native Details recognized custom activity owner; graph={:#x}, close_requested={}, native_activity_present={}, native_exit={} -> 0; native Back/camera/input unchanged.",
            0x05edc9f8,values[0x80],values[0x81],original);
}

std::uint64_t player_in_activity_hook(std::uint32_t uid) {
    const auto original=state().player_in_activity(uid);
    if(native_activity_flag(original) || !kit::match().in_activity)return original;
    profile_runtime::PreserveError preserve;
    try {
        // The original pause/marker UI must treat the local match participant
        // as playing an activity even after its temporary flag queue retires.
        // The query adapter remains independent of the scoped client UI lease.
        if(active() && realm()==client_realm) {
            const auto ids=sole_offline_player_ids();
            if(ids.matches_menu_player(uid) && client_activity_variables(ids.participant))return 1;
        }
    } catch(...) {}
    return original;
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
                if (current_realm == client_realm) prepare_native_solo_throwdown_leave(hash);
                if (current_realm == client_realm) observe_throwdown_send(hash, read<Address>(arguments[0]), arguments[1]);
                // Keep the native registration and waiting HUD. Only its Start
                // action is diverted to the plugin mode; ordinary Throwdowns are unchanged.
                if(current_realm==client_realm && consume_flag_throwdown_send(hash,read<Address>(arguments[0]),arguments[1])) return;
                // CreateThrowdownQueue: the local player now hosts a throwdown.
                if (current_realm == client_realm && hash == 0x7fac81bb && state().lifetime.create(sole_offline_player_id()))
                    logging::log(logging::Level::info, logging::Channel::progression, "Throwdown: local player is hosting one.");
                // AddParticipantToCommunityEvent (the Join button): in one someone else placed,
                // which the Quit action must also leave. RemoveParticipant: left its queue.
                if (current_realm == client_realm && hash == 0xdc10392b && state().lifetime.join(sole_offline_player_id()))
                    logging::log(logging::Level::info, logging::Channel::progression, "Throwdown: local player joined one.");
                if (current_realm == client_realm && hash == 0xaa3f9957) state().lifetime.leave_queue();
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
        *out = native_flag::registration_owned() ? 0U : 1U;
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
    ModeParameter{"JamSession", "MaxPlayers", 1, 10, 4},        ModeParameter{"JamSession", "Privacy", 0, 1, 0},
    ModeParameter{"JamSession", "ScoreFlips", 0, 1, 1},         ModeParameter{"JamSession", "ScoreGrabs", 0, 1, 1},
    ModeParameter{"JamSession", "ScoreGrinds", 0, 1, 1},        ModeParameter{"JamSession", "ScoreGroundTricks", 0, 1, 1},
    ModeParameter{"JamSession", "ScoreSlams", 0, 1, 0},         ModeParameter{"JamSession", "Timer", 60, 300, 180},
    ModeParameter{"JamSession", "WinCondition", 0, 3, 0},       ModeParameter{"JamSession", "Invite", 0, 2, 0},
    ModeParameter{"SpotBattle", "Rounds", 1, 5, 3},             ModeParameter{"SpotBattle", "TurnDuration", 10, 60, 30},
    ModeParameter{"SpotBattle", "Invite", 0, 2, 0},             ModeParameter{"SpotBattle", "MaxPlayers", 1, 10, 4},
    ModeParameter{"SpotBattle", "Privacy", 0, 1, 0},            ModeParameter{"ThrowdownSkate", "TurnDuration", 10, 60, 30},
    ModeParameter{"ThrowdownSkate", "Invite", 0, 2, 0},         ModeParameter{"ThrowdownSkate", "MaxPlayers", 1, 10, 4},
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
bool consume_match_marker_expression(Address vm,std::uint32_t pc) noexcept {
    if(pc || !vm || !kit::match().markers)return false;
    profile_runtime::PreserveError preserve;
    try {
        if(!active() || realm()!=client_realm)return false;
        const auto resource=read<Address>(vm+0x38),instance=read<Address>(vm+0x30);
        return resource && instance && read<Address>(instance)==resource &&
            session_marker_action(read<std::uint32_t>(resource+0x10),
                read<std::array<std::uint32_t,10>>(resource+0x20),pc);
    } catch(...) {return false;}
}

bool local_throwdown_active() noexcept {
    return state().lifetime.active();
}
bool local_throwdown_event_active() noexcept {
    try {return state().lifetime.has_activity();} catch(...) {return true;}
}
void set_custom_activity_menu_request(Address address) noexcept {
    auto& s=state();
    s.menu_trace_address.store(0,std::memory_order_release);
    s.menu_details_presence_reported.store(false,std::memory_order_relaxed);
    std::int32_t value{};
    if(!address || !memory::peek(address,value))return;
    s.menu_trace_thread.store(GetCurrentThreadId(),std::memory_order_relaxed);
    s.menu_trace_address.store(address,std::memory_order_release);
}
void sync_custom_activity_ui_presence(bool participant) noexcept {
    profile_runtime::PreserveError preserve;
    auto& s=state();
    try {
        if(!active())return;
        std::lock_guard presence_lock(s.custom_ui_presence_mutex);
        if(!participant && !s.custom_ui_presence.owned() && s.custom_ui_diagnostic.empty())return;
        const auto caller_realm=realm();
        const auto uid=sole_offline_player_id();
        std::uint32_t menu_player{};
        const auto vars=uid?client_activity_variables(uid,&menu_player):0;
        const auto group=vars?read<std::uint32_t>(vars+0x100):0;
        const auto current=vars?read<std::uint8_t>(vars+0x104)!=0:false;
        const bool native_flag=vars && s.player_in_activity && native_activity_flag(s.player_in_activity(menu_player));
        const bool queued=s.lifetime.active(),native_activity=s.lifetime.has_activity();
        const bool was_owned=s.custom_ui_presence.owned();
        const auto diagnostic=[&](const char* result) {
            if(!participant && !was_owned && s.custom_ui_diagnostic.empty())return;
            const auto message=std::format(
                "wanted={}, caller_realm={:#x}, participant={}, menu_player={}, variables={:#x}, group={}, flag={}, getter={}, idle={}, queue={}, native_activity={}, owned={}, result={}",
                participant,caller_realm,uid,menu_player,vars,group,current,native_flag,
                vars?read<std::uint32_t>(vars+0x108):0,queued,native_activity,s.custom_ui_presence.owned(),result);
            const auto now=GetTickCount64();
            if(message!=s.custom_ui_diagnostic || ((participant || was_owned) && now>=s.custom_ui_diagnostic_at+1000)) {
                logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: activity UI handoff: {}.",message);
                s.custom_ui_diagnostic=message;s.custom_ui_diagnostic_at=now;
            }
        };
        // Verify the existing getter and reflected flag identify the same
        // player before acquiring. No idle/group/backend values are changed.
        if(participant && (!vars || !s.player_in_activity || native_flag!=current)) {
            diagnostic(!uid?"no sole native player":!vars?"no verified native client player":
                !s.player_in_activity?"native getter unavailable":"native getter disagrees with client player");
            return;
        }
        bool write_failed{};
        s.custom_ui_presence.update(participant && !queued,vars,uid,group,current,
            native_activity,[&](bool value) {
                SIZE_T written{};
                const std::uint8_t flag=value?1:0;
                const bool applied=WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(vars+0x104),
                    &flag,sizeof(flag),&written) && written==sizeof(flag);
                if(applied && (native_activity_flag(s.player_in_activity(menu_player))!=value)) {
                    // A same-id object from another context cannot be the
                    // native UI owner. Undo this one-byte attempt immediately.
                    const std::uint8_t original=current?1:0;
                    WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(vars+0x104),&original,sizeof(original),&written);
                    write_failed=true;
                    return false;
                }
                if(applied)logging::log(logging::Level::info,logging::Channel::ui,
                    "Throwdowns: client activity UI presence {} -> {} for player {}, group {}, variables {:#x}; native idle/backend unchanged.",
                    current,value,uid,group,vars);
                if(!applied)write_failed=true;
                return applied;
            });
        diagnostic(write_failed?"write/getter verification failed":queued?"native queue owns activity":
            native_activity?"native event owns activity":s.custom_ui_presence.owned()?"scoped client flag owned":
            participant && current?"client flag already native":participant?"client flag not acquired":"released");
        if(!participant && !s.custom_ui_presence.owned())s.custom_ui_diagnostic.clear();
    } catch(const std::exception& error) {
        static std::uint64_t next_log{};const auto now=GetTickCount64();
        if(now>=next_log) {next_log=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,
            "Throwdowns: client activity UI presence unavailable: {}",error.what());}
    } catch(...) {}
}
bool local_throwdown_host() noexcept { return state().lifetime.host(); }
void native_throwdown_player_limit(std::uint32_t limit) noexcept {
    try { state().lifetime.player_limit(limit); } catch (...) {}
}
Address native_solo_throwdown_activity(std::uint32_t player) noexcept {
    try { return state().lifetime.solo_activity(player); } catch (...) { return 0; }
}
bool native_solo_throwdown_menu_cleanup_pending() noexcept {
    try { return !native_flag::registration_owned() && state().lifetime.menu_cleanup_pending(); } catch (...) { return false; }
}
void native_solo_throwdown_menu_cleaned() noexcept {
    try { state().lifetime.menu_cleaned(); } catch (...) {}
}
void release_native_flag_placeholder() noexcept {
    try { state().lifetime.clear(); } catch (...) {}
}
void native_throwdown_activity_started(Address activity, std::uint32_t player) noexcept {
    try {
        if (state().lifetime.activity_started(activity, player))
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown: local player {:#x} entered native activity {:#x}.", player, activity);
    } catch (...) {}
}
void native_throwdown_participant_left(Address activity, std::uint32_t player) noexcept {
    try {
        if (state().lifetime.participant_left(activity, player))
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdown: ended after player {:#x} left native activity {:#x}; ownership cleared.", player, activity);
    } catch (...) {}
}
void native_throwdown_level_left() noexcept {
    retire_native_queue_hud();
    set_custom_activity_menu_request(0);
    sync_custom_activity_ui_presence(false);
    try {
        state().solo_client_activity=0;state().solo_client_player=0;state().solo_client_group=0;
        if (state().lifetime.clear())
            logging::write(logging::Level::info, logging::Channel::progression,
                "Throwdown: ownership cleared before leaving the level.");
    } catch (...) {}
}

void complete_native_throwdown_parameters(Address vm) noexcept {
    pump_throwdown_lab(vm);
    {
        profile_runtime::PreserveError preserve;
        try {
            if(active() && realm()==client_realm)complete_solo_client_exit();
            complete_match_controls(vm);
            complete_custom_details_presence(vm);
            const auto join_resource=vm?read<Address>(vm+0x38):0;
            if(join_resource && read<std::uint32_t>(join_resource+0x10)==0xebb4ab69 && active() && realm()==client_realm)
                observe_native_custom_join_details(vm);
            // Replace presentation at the native publication boundary, rather
            // than waiting for the next throttled UI/camera confirmation tick.
            // Real activities, other realms/threads and unarmed requests keep
            // their own contents. The native outer Page is never replaced.
            auto& s=state();const auto selected=s.menu_trace_address.load(std::memory_order_acquire);
            if(selected && s.menu_trace_thread.load(std::memory_order_relaxed)==GetCurrentThreadId()) {
                const auto resource=vm?read<Address>(vm+0x38):0,instance=vm?read<Address>(vm+0x30):0;
                if(resource && instance && read<Address>(instance)==resource &&
                   menu_data::native_activity_page_publish_graph(read<std::uint32_t>(resource+0x10),
                       read<std::array<std::uint32_t,10>>(resource+0x20)) &&
                   active() && realm()==client_realm && read<std::int32_t>(selected)==1)
                    prepare_native_throwdown_match_panel();
            }
        } catch(...) {}
    }
    try {
        const auto resource=vm?read<Address>(vm+0x38):0;
        if(resource && read<std::uint32_t>(resource+0x10)==0x627382a6 && active() && realm()==client_realm)
            observe_native_flag_interaction(vm);
    } catch(...) {}
    const bool flag_queue=native_flag::registration_owned();
    const bool solo_queue=!flag_queue && native_solo_throwdown_waiting();
    if(flag_queue || solo_queue) {
        profile_runtime::PreserveError preserve;
        try {
            const auto resource=vm?read<Address>(vm+0x38):0;
            const auto instance=vm?read<Address>(vm+0x30):0;
            const auto hash=resource?read<std::uint32_t>(resource+0x10):0;
            // Authored predicate outputs (EBX GraphPortRegister page 1): the
            // retail queue normally locks the Toolbox while waiting. A plugin
            // mode's registration may reopen setup, and may start when its
            // waiting card says so (can_start). A stock one-player host queue
            // may start alone; other queues retain their authored predicates.
            constexpr std::array<std::uint32_t,10> disabled_layout{64,104,44,177,3,0,2,14,720896,66560};
            constexpr std::array<std::uint32_t,10> start_layout{144,1360,1565,1445,18,0,7,2293833,5832773,17434624};
            if(instance && read<Address>(instance)==resource && realm()==client_realm &&
                ((flag_queue && hash==0xc3999b06 && read<std::array<std::uint32_t,10>>(resource+0x20)==disabled_layout) ||
                 (hash==0x1a0c8c31 && read<std::array<std::uint32_t,10>>(resource+0x20)==start_layout))) {
                const auto frame=read<std::uint32_t>(resource+0x20);
                const auto arguments=read<Address>(instance+((frame+15U)&~15U)+8);
                if(arguments) {
                    bool result=false;
                    if(hash==0x1a0c8c31) {
                        if(solo_queue)result=true;
                        else if(native_flag::registration_waiting()) {
                            const auto docs=kit::documents_for(native_flag::mode());
                            result=docs && docs->waiting && docs->waiting->can_start;
                        }
                    }
                    // Both graphs return through opcode 0x32's borrowed
                    // output pointer on page 1, not their local scratch page.
                    const auto output=read<Address>(arguments+(hash==0xc3999b06?8:96));
                    SIZE_T written{};
                    if(output) WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(output),&result,sizeof(result),&written);
                }
            }
        } catch(...) {}
    }
    if(native_flag::placement_active()) {
        profile_runtime::PreserveError preserve;
        try {
            const auto resource=vm?read<Address>(vm+0x38):0;
            if(resource && realm()==client_realm) {
                const auto hash=read<std::uint32_t>(resource+0x10);
                native_flag::observe_graph(hash);
            }
        } catch(...) {}
    }
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
                if (realm() == client_realm && state().lifetime.ended())
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
        Hook{throwdowns::player_in_activity,throwdowns::player_in_activity_prefix,reinterpret_cast<void*>(&player_in_activity_hook),reinterpret_cast<void**>(&s.player_in_activity)},
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
