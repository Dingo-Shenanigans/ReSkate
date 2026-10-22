#pragma once
// Private CPU observer. A separately selected ACTIVE NativeOnly control may
// issue the existing native-board request once; no motion, body or pose writes.
#include "Engine/Game/BMX/bmx_board_handoff_policy.h"
#include "bmx_native_motion.h"
#include "bmx_rider_pose.h"
#include "bmx_pose_write.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/image_identity.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace dingosdk::bmx::board_handoff_trace {
namespace pose=rider::pose;
namespace policy=board_handoff_policy;
inline constexpr auto capacity=policy::capacity,wall_limit_ms=policy::wall_limit_ms,sample_interval_ms=policy::sample_interval_ms;
inline constexpr double driver_board_dwell_seconds=policy::driver_dwell_seconds;
inline constexpr wchar_t private_root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-chassis-probe";
using Mode=policy::Mode;using Kind=policy::Kind;
struct ClientState {
    std::uint64_t sampled_ms{},motion_updates{},pose_updates{},render_updates{},board_hide_updates{};
    unsigned chassis_allocations{},chassis_mounts{};bool native_control_input_allowed{};
    unsigned parts{};
    bool gameplay{},solo{},focused{},menu{},capture{},active{},pending{},stepping_off{},board_pending{};
    policy::TerminalClientTarget private_terminal;
};
struct Row {
    Kind kind{};std::uint64_t wall_ms{},utc_filetime{},serial{};double native_time{};
    std::array<std::uintptr_t,6> owner{}; // base,client,entity,core,context,rig
    std::uint32_t physics{};NativeMotionLeaseStatus lease{};ClientState sdk{};
    std::uint64_t pose_updates{},render_updates{},board_hide_updates{},publication_revision{};
    bool publication_available{},publication_active{};std::uintptr_t publication_entity{};
    std::uintptr_t component{},holder{},buffer{},render_data{};std::uint32_t bone_count{};
    std::array<float,4> hips_local{};
    std::array<pose::Vec,6> composed{}; // hips,parent,ankleL,ankleR,toeL,toeR; CPU rig model only
    std::array<bool,6> composed_available{};
    std::array<int,6> selected_joint_indices{-1,-1,-1,-1,-1,-1};
    std::array<pose::Bone,6> selected_joint_bones{};std::array<pose::Bone,2> skater_root_prefix{};
    std::array<pose::Bone,16> board{}; // raw CPU publication; no deck-surface semantics
    bool skater_pose{},board_pose{},published_pointer_matches{},same_owner_clock_after{};
    bool native_frame_observed{},offboard{},standable{},motion_supported{};
    std::array<float,16> native_entity_transform{};std::array<float,3> native_rider_root{};
};
static_assert(std::is_trivially_copyable_v<Row>);
struct Store {
    std::atomic<bool> enabled{},armed{},sealed{},exported{},control_arm_requested{};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> contention{};
    std::array<Row,capacity> rows{};std::size_t count{};
    policy::Window window;policy::PublicationDwell board_dwell;policy::TerminalClientRetention terminal_client;
    ClientState sdk{};Mode mode{};bool automated_native_board{},deferred_native_board_arm{};policy::NativeBoardControl native_board_control;std::atomic<const char*> stop{"not_armed"};
    std::uint64_t started{},creation{},arm_request_wall{};
    std::filesystem::path root;DWORD client_thread{};bool attempted{},ever_armed{};
};
static_assert(sizeof(Store)<1024*1024);
inline Store store;
struct ErrorGuard {DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);}};
struct TryGuard {bool held;TryGuard():held(!store.busy.test_and_set(std::memory_order_acquire)){}
    ~TryGuard(){if(held)store.busy.clear(std::memory_order_release);}};
inline bool flag(const wchar_t* name) noexcept {wchar_t v[2]{};return GetEnvironmentVariableW(name,v,2)==1&&v[0]==L'1';}
// Missing/empty/0 is passive/default. Every other non-1 value is invalid.
inline int requested_native_board_control() noexcept {
    wchar_t v[4]{};const auto n=GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_BOARD_CONTROL",v,4);
    if(!n||(n==1&&v[0]==L'0'))return 0;
    return n==1&&v[0]==L'1'?1:-1;
}
// Explicit private opt-in; malformed/non-1 values cannot extend readiness.
inline int requested_deferred_native_board_arm() noexcept {
    wchar_t v[4]{};const auto n=GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_BOARD_DEFERRED_ARM",v,4);
    if(!n||(n==1&&v[0]==L'0'))return 0;
    return n==1&&v[0]==L'1'?1:-1;
}
inline Mode requested_mode() noexcept {
    wchar_t v[16]{};const auto n=GetEnvironmentVariableW(L"RESKATE_BMX_BOARD_HANDOFF_MODE",v,16);
    if(n==12&&std::wstring_view(v,n)==L"quick-switch")return Mode::quick_switch;
    if(n==11&&std::wstring_view(v,n)==L"native-only")return Mode::native_only;
    return Mode::disabled;
}
inline bool selected() noexcept {
    return store.enabled.load(std::memory_order_acquire)&&flag(L"RESKATE_BMX_BOARD_HANDOFF_TRACE")&&
        requested_mode()==store.mode&&requested_native_board_control()==int(store.automated_native_board)&&
        requested_deferred_native_board_arm()==int(store.deferred_native_board_arm)&&
        policy::deferred_passive_arm_allowed(store.mode,store.automated_native_board,int(store.deferred_native_board_arm))&&user_data::runtime_development_selected(L"bmx-user-data-chassis-probe",private_root);
}
inline void seal(const char* reason) noexcept {
    store.stop=reason;store.sealed.store(true,std::memory_order_release);
}
// Client only: existing supported-image/profile checks, no callback filesystem queries.
inline bool initialize_client(std::uintptr_t base) noexcept {
    ErrorGuard error;if(store.attempted)return selected();store.attempted=true;
    try {
        if(!flag(L"RESKATE_BMX_BOARD_HANDOFF_TRACE")||!flag(L"RESKATE_BMX_NATIVE_CHASSIS")||
           !user_data::runtime_development_selected(L"bmx-user-data-chassis-probe",private_root)||
           !supported_build::running_image_matches(base))return false;
        const auto mode=requested_mode();if(mode==Mode::disabled)return false;
        const auto active_control=requested_native_board_control();
        if(active_control<0||(active_control&&mode!=Mode::native_only))return false;
        const auto deferred_arm=requested_deferred_native_board_arm();
        if(!policy::deferred_passive_arm_allowed(mode,active_control==1,deferred_arm))return false;
        wchar_t launch[16]{};const auto launch_n=GetEnvironmentVariableW(L"RESKATE_BMX_LAUNCH_MODE",launch,16);
        if(launch_n>=16)return false;
        const bool quick=flag(L"RESKATE_BMX_QUICK_SWITCH_TEST"),automated=flag(L"RESKATE_BMX_AUTOMATED_SWITCH");
        if((mode==Mode::quick_switch&&(!quick||!automated))||(mode==Mode::native_only&&(quick||automated)))return false;
        for(const auto* name:{L"RESKATE_BMX_AUTOMATED_RIDE",L"RESKATE_BMX_RAMP_TEST",L"RESKATE_BMX_RAMP_RIDE",
            L"RESKATE_BMX_CHASSIS_PROBE",L"RESKATE_BMX_CHASSIS_PROBE_PULSE",L"RESKATE_BMX_PROFILE_PERFORMANCE",
            L"RESKATE_BMX_GETUP_PULSE",L"RESKATE_BMX_GRIP_PROBE",L"RESKATE_BMX_IMPACT_RECOVERY",L"RESKATE_BMX_NATIVE_STARTUP_DELAY",
            L"RESKATE_BMX_PLAYER_STARTUP_REQUEST",L"RESKATE_BMX_POSTFALL_REMOUNT_TEST",L"RESKATE_BMX_INTENT_OBSERVER",
            L"RESKATE_BMX_INTENT_ACTIVATION",L"RESKATE_BMX_BINDING_OBSERVER",L"RESKATE_BMX_NATIVE_FALL_OBSERVER",
            L"RESKATE_BMX_TRICK_REHEARSAL",L"RESKATE_BMX_HANDOFF_PHASE_PROBE",L"RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE"}) {
            wchar_t value[4]{};const auto count=GetEnvironmentVariableW(name,value,4);
            if(count&&!(count==1&&value[0]==L'0'))return false;
        }
        for(const auto* name:{L"RESKATE_BMX_POSE_COMPARISON",L"RESKATE_BMX_MOTION_BASIS_COMPARISON"}) {
            wchar_t value[32]{};const auto count=GetEnvironmentVariableW(name,value,32);
            if(count&&(count>=32||std::wstring_view(value,count)!=L"Default"))return false;
        }
        for(const auto* name:{L"RESKATE_BMX_IMPACT_TEST",L"RESKATE_BMX_CATALOG_TEST"}) {
            wchar_t value{};SetLastError(ERROR_SUCCESS);
            const auto copied=GetEnvironmentVariableW(name,&value,1);
            const auto environment_error=GetLastError();
            if(!policy::absent_or_empty_option(copied,environment_error,ERROR_ENVVAR_NOT_FOUND))return false;
        }
        if(policy::select({true,true,true,true,true,quick,automated,
            mode==Mode::quick_switch?L"quick-switch":L"native-only",std::wstring_view(launch,launch_n),active_control==1})!=mode)return false;
        const auto root=user_data::override_root();if(!root||root->native()!=private_root)return false;
        FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
        store.creation=(std::uint64_t{created.dwHighDateTime}<<32)|created.dwLowDateTime;if(!store.creation)return false;
        store.root=*root;store.mode=mode;store.automated_native_board=active_control==1;store.deferred_native_board_arm=deferred_arm==1;store.client_thread=GetCurrentThreadId();store.board_dwell.configure(policy::trace_dwell_seconds);
        store.enabled.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
inline void arm_quick_switch() noexcept {
    ErrorGuard error;if(!selected()||store.mode!=Mode::quick_switch)return;
    TryGuard guard;if(!guard.held||store.ever_armed)return;
    store.started=GetTickCount64();if(!store.window.arm(store.started))return;
    store.ever_armed=true;store.stop="recording";store.armed.store(true,std::memory_order_release);
}
// Console request only. This arms observation; it never mounts a board or bike.
inline bool request_native_control_arm(bool active_control=false) noexcept {
    ErrorGuard error;if(!selected()||store.mode!=Mode::native_only||store.automated_native_board!=active_control)return false;
    TryGuard guard;if(!guard.held||store.ever_armed||store.sealed.load()||store.control_arm_requested.load())return false;
    store.arm_request_wall=GetTickCount64();store.control_arm_requested.store(true,std::memory_order_release);return true;
}
// Initialized NativeOnly sessions stay isolated for this process, including
// passive input comparisons and sealed/revoked observations. Fresh selection
// failure is handled inside the gate; it must never enable BMX fallback.
inline bool native_only_control_process() noexcept {
    return store.enabled.load(std::memory_order_acquire)&&store.mode==Mode::native_only;
}
inline std::array<std::uintptr_t,6> owner_key(const NoBailSkater& o,std::uintptr_t client) noexcept {
    return {o.base,client,o.entity,o.core,o.context,o.rig};
}
inline bool common(Row& r) noexcept {
    NoBailSkater o;std::uintptr_t client{};
    if(!try_no_bail_skater(0,o,client)||!client||
       !read_native_clock_try(o.entity,o.core,r.serial,r.native_time)||!r.serial||!std::isfinite(r.native_time)||
       !memory::peek(o.context+0x1414,r.physics))return false;
    r.owner=owner_key(o,client);r.wall_ms=GetTickCount64();FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);
    r.utc_filetime=(std::uint64_t{utc.dwHighDateTime}<<32)|utc.dwLowDateTime;
    r.lease=native_motion_lease_status(true);return r.lease.available&&r.lease.details_available;
}
inline bool final_owner_clock(const Row& r) noexcept {
    NoBailSkater o;std::uintptr_t client{};std::uint64_t serial{};double time{};std::uint32_t physics{};
    return try_no_bail_skater(0,o,client)&&owner_key(o,client)==r.owner&&
        read_native_clock_try(o.entity,o.core,serial,time)&&serial==r.serial&&time==r.native_time&&
        memory::peek(o.context+0x1414,physics)&&physics==r.physics;
}
inline bool before_read() noexcept {
    const auto decision=store.window.before_read(GetTickCount64(),selected());
    if(policy::failed(decision)){seal(policy::name(decision));return false;}
    return decision==policy::Result::admitted;
}
inline bool admit(Row& r,bool terminal_client=false) noexcept {
    if(!store.armed.load(std::memory_order_acquire)||store.sealed.load(std::memory_order_acquire))return false;
    const auto decision=store.window.observe(r.kind,r.owner,{r.serial,r.native_time},r.wall_ms,selected(),true,terminal_client);
    if(policy::failed(decision)){seal(policy::name(decision));return false;}
    if(decision!=policy::Result::admitted)return false;
    r.sdk=store.sdk;return true;
}
inline void retain(const Row& r) noexcept {
    store.rows[store.count++]=r;store.window.retain(r.kind,{r.serial,r.native_time},r.wall_ms);
}
// Fixed paired native-edge storage is separate from the existing CPU pose ring.
inline constexpr std::size_t native_edge_capacity=512;
struct NativeEdgeSnapshot {
    std::array<std::uintptr_t,6> owner{};std::uint64_t serial{},wall_ms{},utc_filetime{};double native_time{};
    std::array<std::uint32_t,6> context_words{}; //13d4,13d8,13dc,1414,1418,141c; raw only
    std::uintptr_t animation_bundle{},mount_exports{};std::array<std::uint8_t,3> mount_bytes{}; //16c..16e
    bool owner_clock_fenced{},context_available{},pointer_chain_stable{},export_available{};
    const char* issue="unavailable";
};
struct NativeEdgePair {
    NativeEdgeSnapshot before,after;std::uintptr_t argument{};std::uint32_t current{},native_result{};
    NoBailNativePhase entry_phase{};bool original_returned{},same_owner{},same_clock{};
    const char* issue="entry_only";
};
struct NativeEdges {
    std::array<NativeEdgePair,native_edge_capacity> pairs{};std::size_t count{};
    std::atomic<std::uint64_t> contention{};std::uint64_t refused{},capacity_drops{};
};
static_assert(std::is_trivially_copyable_v<NativeEdgeSnapshot>);
static_assert(sizeof(NativeEdges)<256*1024);
inline NativeEdges native_edges;
inline bool native_edge_pointer(std::uintptr_t object,std::uintptr_t offset,std::uintptr_t& value) noexcept {
    constexpr auto highest=memory::highest_user_address;
    return object>=0x10000&&object<=highest-offset-sizeof(value)&&memory::peek(object+offset,value)&&
        value>=0x10000&&value<=highest-0x1000&&(value&7)==0;
}
inline NativeEdgeSnapshot native_edge_snapshot(const NoBailNativeEvent& event) noexcept {
    NativeEdgeSnapshot row;NoBailSkater owner;std::uintptr_t client{};
    if(!try_no_bail_skater(0,owner,client)||!client){row.issue="owner_unavailable";return row;}
    row.owner=owner_key(owner,client);
    const bool selector=event.phase==NoBailNativePhase::selector_before_sdk||event.phase==NoBailNativePhase::selector_after_restore;
    std::uintptr_t argument{};
    if((selector&&(!native_edge_pointer(owner.core,0x440,argument)||argument!=event.argument))||
       (!selector&&owner.core!=event.argument)){row.issue="argument_owner_mismatch";return row;}
    if(!read_native_clock_try(owner.entity,owner.core,row.serial,row.native_time)||!row.serial||!std::isfinite(row.native_time)){
        row.issue="clock_unavailable";return row;}
    row.wall_ms=GetTickCount64();FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);
    row.utc_filetime=(std::uint64_t{utc.dwHighDateTime}<<32)|utc.dwLowDateTime;
    constexpr std::array<std::uintptr_t,6> offsets{0x13d4,0x13d8,0x13dc,0x1414,0x1418,0x141c};
    row.context_available=true;
    for(std::size_t i=0;i<offsets.size();++i)row.context_available=memory::peek(owner.context+offsets[i],row.context_words[i])&&row.context_available;
    const bool exports=native_edge_pointer(owner.core,0x3b8,row.animation_bundle)&&
        native_edge_pointer(row.animation_bundle,0x78,row.mount_exports)&&memory::peek(row.mount_exports+0x16c,row.mount_bytes);
    std::uintptr_t bundle_after{},exports_after{};
    row.pointer_chain_stable=exports&&native_edge_pointer(owner.core,0x3b8,bundle_after)&&bundle_after==row.animation_bundle&&
        native_edge_pointer(bundle_after,0x78,exports_after)&&exports_after==row.mount_exports;
    NoBailSkater final_owner;std::uintptr_t final_client{};std::uint64_t final_serial{};double final_time{};
    std::uintptr_t final_argument{};
    row.owner_clock_fenced=try_no_bail_skater(0,final_owner,final_client)&&owner_key(final_owner,final_client)==row.owner&&
        read_native_clock_try(final_owner.entity,final_owner.core,final_serial,final_time)&&final_serial==row.serial&&final_time==row.native_time&&
        (!selector||(native_edge_pointer(final_owner.core,0x440,final_argument)&&final_argument==event.argument));
    row.context_available=row.context_available&&row.owner_clock_fenced;
    row.export_available=row.pointer_chain_stable&&row.owner_clock_fenced;
    row.issue=!row.owner_clock_fenced?"owner_or_clock_changed":!row.context_available?"context_unavailable":
        !row.export_available?"exports_unavailable_or_changed":"fenced_raw_snapshot_not_graph_consumption";
    return row;
}
inline std::uint64_t native_edge_sample(const NoBailNativeEvent& event,std::uint64_t cookie) noexcept {
    ErrorGuard error;
    if(!store.enabled.load(std::memory_order_acquire)||!store.armed.load(std::memory_order_acquire))return 0;
    if(!cookie&&store.sealed.load(std::memory_order_acquire))return 0; // quiescent closed entry path
    TryGuard guard;if(!guard.held){++native_edges.contention;return 0;}
    const bool entry=event.phase==NoBailNativePhase::selector_before_sdk||event.phase==NoBailNativePhase::publisher_before_original;
    if(!entry) {
        if(!cookie||cookie>native_edges.count){++native_edges.refused;return 0;}
        auto& pair=native_edges.pairs[cookie-1];
        const bool matching=(pair.entry_phase==NoBailNativePhase::selector_before_sdk&&event.phase==NoBailNativePhase::selector_after_restore)||
            (pair.entry_phase==NoBailNativePhase::publisher_before_original&&event.phase==NoBailNativePhase::publisher_original_returned);
        if(!matching||pair.argument!=event.argument||pair.original_returned){++native_edges.refused;return 0;}
        pair.original_returned=true;pair.native_result=event.native_result;
        if(!selected()||store.sealed.load(std::memory_order_acquire)){pair.issue="scope_or_trace_closed_before_exit";return 0;}
        const auto now=GetTickCount64();
        if(now<store.started||now-store.started>=wall_limit_ms){pair.issue="exit_outside_original_trace_deadline";return 0;}
        pair.after=native_edge_snapshot(event);
        pair.same_owner=pair.before.owner_clock_fenced&&pair.after.owner_clock_fenced&&pair.before.owner==pair.after.owner;
        pair.same_clock=pair.same_owner&&pair.before.serial==pair.after.serial&&pair.before.native_time==pair.after.native_time;
        pair.issue=pair.same_clock?"paired_raw_edge_not_graph_consumption":"pair_owner_or_clock_unavailable_or_changed";
        return 0;
    }
    if(cookie||!selected()||store.sealed.load(std::memory_order_acquire))return 0;
    const auto now=GetTickCount64();
    if(now<store.started||now-store.started>=wall_limit_ms){++native_edges.refused;return 0;}
    if(native_edges.count>=native_edge_capacity){++native_edges.capacity_drops;return 0;}
    auto& pair=native_edges.pairs[native_edges.count++];pair.entry_phase=event.phase;pair.argument=event.argument;pair.current=event.current;
    pair.before=native_edge_snapshot(event);return native_edges.count;
}
inline void seal_quick_switch_failure() noexcept {
    ErrorGuard error;if(selected()&&store.mode==Mode::quick_switch&&store.armed.load(std::memory_order_acquire)&&
        !store.sealed.load(std::memory_order_acquire))seal("quick_switch_terminal_failed_partial_evidence");
}
// Client-only serialization. NULL retains unavailable fields; no graph claim.
inline Json native_edges_json(std::size_t byte_budget) {
    const auto snapshot=[](const NativeEdgeSnapshot& r) {
        return Json{{"owner",r.owner},{"serial",r.serial},{"native_time",r.native_time},{"wall_ms",r.wall_ms},
            {"utc_filetime",std::to_string(r.utc_filetime)},{"owner_clock_fenced",r.owner_clock_fenced},
            {"context_words_13d4_13d8_13dc_1414_1418_141c",r.context_available?Json(r.context_words):Json(nullptr)},
            {"animation_bundle",r.pointer_chain_stable?Json(r.animation_bundle):Json(nullptr)},
            {"mount_exports",r.pointer_chain_stable?Json(r.mount_exports):Json(nullptr)},
            {"pointer_chain_stable",r.pointer_chain_stable},{"export_bytes_16c_16d_16e",r.export_available?Json(r.mount_bytes):Json(nullptr)},
            {"issue",r.issue}};
    };
    std::size_t serialized{},used=2048; // fixed header reserve, measured item bytes below
    Json pairs=Json::array();for(std::size_t i=0;i<native_edges.count;++i){const auto& p=native_edges.pairs[i];
        Json item{{"entry_phase",static_cast<unsigned>(p.entry_phase)},{"argument",p.argument},{"current",p.current},
            {"native_result",p.original_returned?Json(p.native_result):Json(nullptr)},{"original_returned",p.original_returned},
            {"same_owner",p.same_owner},{"same_clock",p.same_clock},{"before",snapshot(p.before)},{"after",snapshot(p.after)},{"issue",p.issue}};
        const auto bytes=item.dump().size()+1;
        if(used>byte_budget||bytes>byte_budget-used)break;
        used+=bytes;++serialized;pairs.push_back(std::move(item));
    }
    return Json{{"schema",1},{"capacity",native_edge_capacity},{"count",native_edges.count},{"window_limit_ms",wall_limit_ms},
        {"contention_drops",native_edges.contention.load()},{"refused_edges",native_edges.refused},{"capacity_drops",native_edges.capacity_drops},
        {"serialized_count",serialized},{"serialization_truncated",serialized<native_edges.count},
        {"serialization_byte_budget",byte_budget},{"native_writes",0},{"graph_consumption_proven",false},
        {"boundary","raw words are not atomic; owner, listener-clock and pointer fences only"},{"pairs",std::move(pairs)}};
}
inline void append_native_edges(Json& report) {
    constexpr std::size_t limit=2*1024*1024;
    const auto original_bytes=report.dump().size();
    if(original_bytes+4096<limit)report["native_request_publication"]=native_edges_json(limit-original_bytes-4096);
    else if(original_bytes+128<limit)report["native_request_publication_omitted_due_to_byte_limit"]=true;
    // If even a compact omission marker cannot fit, retain the original CPU
    // report byte-for-byte. Missing edge detail is unavailable measurement.
}

inline void client_sample(const ClientState& sdk) noexcept {
    ErrorGuard error;if(!store.enabled.load(std::memory_order_acquire)||store.sealed.load(std::memory_order_acquire))return;
    TryGuard guard;if(!guard.held){++store.contention;return;}
    if(!selected()){store.window.fail(policy::Result::configuration_changed);seal("private_configuration_changed");return;}
    store.sdk=sdk;Row row;row.kind=Kind::client;
    if(!store.armed.load(std::memory_order_acquire)&&store.mode==Mode::quick_switch)return;
    if(!store.armed.load()&&!store.control_arm_requested.load(std::memory_order_acquire))return;
    if(!store.armed.load()&&policy::native_only_prearm_expired(store.arm_request_wall,GetTickCount64(),
        store.mode,store.automated_native_board,store.deferred_native_board_arm)){
        seal("native_only_arm_readiness_timeout");return;}
    if(store.armed.load()&&!before_read())return;
    if(!common(row)){store.window.fail(policy::Result::read_failure);seal("client_owner_clock_or_read_unavailable");return;}
    NativeFrame native;
    if(!store.armed.load()) {
        native=read_native_frame(row.owner[0],row.owner[1]);
        if(!native.available||!native.timing_available||native.entity!=row.owner[2]||native.core!=row.owner[3]||
           native.context!=row.owner[4]||native.rig!=row.owner[5]||native.serial!=row.serial||native.simulation_time!=row.native_time||!final_owner_clock(row)){
            seal("native_only_arm_owner_clock_or_frame_unavailable");return;}
        if(!policy::native_control_standing(row.physics,native.offboard,native.standable,native.motion_supported)){
            if(store.automated_native_board)seal("native_board_control_arm_not_standing");return;}
    }
    if(!store.armed.load()&&store.mode==Mode::native_only&&store.control_arm_requested.load()&&row.physics==504&&sdk.gameplay&&sdk.solo&&sdk.focused&&!sdk.menu&&
       !sdk.active&&!sdk.pending&&!sdk.stepping_off&&!sdk.capture&&sdk.parts==0&&!row.lease.active&&
       policy::native_control_untouched(sdk.motion_updates,sdk.pose_updates,sdk.render_updates)) {
        store.started=row.wall_ms;if(!store.window.arm(row.wall_ms))return;
        store.ever_armed=true;store.stop="recording";store.control_arm_requested.store(false);store.armed.store(true,std::memory_order_release);
    }
    bool existing_terminal=false;
    if(sdk.private_terminal.requested&&!store.terminal_client.attempted){
        for(std::size_t i=0;i<store.count;++i){const auto& prior=store.rows[i];
            if(prior.kind==Kind::client&&prior.owner==sdk.private_terminal.owner&&prior.serial==sdk.private_terminal.clock.serial&&
               prior.native_time==sdk.private_terminal.clock.time&&prior.physics==100&&prior.same_owner_clock_after){existing_terminal=true;break;}}
    }
    const bool terminal_gates=sdk.sampled_ms&&row.wall_ms>=sdk.sampled_ms&&row.wall_ms-sdk.sampled_ms<=250&&
        sdk.gameplay&&sdk.solo&&sdk.focused&&!sdk.menu&&sdk.native_control_input_allowed&&!sdk.active&&!sdk.pending&&
        !sdk.stepping_off&&!sdk.board_pending&&!sdk.capture&&sdk.parts==0&&!row.lease.active;
    const auto terminal=store.terminal_client.observe(sdk.private_terminal,store.mode,store.armed.load(),store.sealed.load(),
        terminal_gates,row.owner,{row.serial,row.native_time},row.physics,row.wall_ms,existing_terminal);
    if(terminal==policy::TerminalClientAction::refused){seal("private_terminal_client_snapshot_or_guard_mismatch");return;}
    const bool force_terminal=terminal==policy::TerminalClientAction::force;
    if(!admit(row,force_terminal)){if(force_terminal)seal("private_terminal_client_admission_refused");return;}
    if(!sdk.sampled_ms||row.wall_ms<sdk.sampled_ms||row.wall_ms-sdk.sampled_ms>250){seal("existing_input_sample_unavailable_or_stale");return;}
    if(!sdk.gameplay||!sdk.solo||!sdk.focused||sdk.menu){seal("client_focus_menu_solo_or_gameplay_changed");return;}
    if(!native.available)native=read_native_frame(row.owner[0],row.owner[1]);
    if(!native.available||!native.timing_available||native.entity!=row.owner[2]||native.core!=row.owner[3]||
       native.context!=row.owner[4]||native.rig!=row.owner[5]||native.serial!=row.serial||native.simulation_time!=row.native_time){
        seal("client_guarded_native_frame_unavailable_or_changed");return;}
    // Unsupported504 remains evidence. It never becomes an allowed mount state.
    row.native_frame_observed=true;row.offboard=native.offboard;row.standable=native.standable;row.motion_supported=native.motion_supported;
    row.native_entity_transform=native.transform;row.native_rider_root=native.position;
    if(!final_owner_clock(row)){seal("client_owner_clock_changed_during_read");return;}
    row.same_owner_clock_after=true;retain(row);
    if(force_terminal)store.terminal_client.complete();
    if(store.automated_native_board) {
        const bool untouched=sdk.native_control_input_allowed&&!sdk.active&&!sdk.pending&&!sdk.stepping_off&&!sdk.board_pending&&
            !sdk.capture&&sdk.parts==0&&!row.lease.active&&!sdk.board_hide_updates&&!sdk.chassis_allocations&&!sdk.chassis_mounts&&
            policy::native_control_untouched(sdk.motion_updates,sdk.pose_updates,sdk.render_updates);
        const auto action=store.native_board_control.observe(row.owner,{row.serial,row.native_time},row.wall_ms,row.physics,
            native.offboard,native.standable,native.motion_supported,untouched);
        if(action==policy::NativeBoardAction::request) {
            // ACTIVE, software selector request exactly once after an armed,
            // retained untouched standing baseline and later fresh owner clock.
            if(!store.native_board_control.complete_request(request_bmx_board_mount(row.owner[1],row.owner[2])))
                seal(store.native_board_control.issue);
        }else if(action==policy::NativeBoardAction::failed)seal(store.native_board_control.issue);
        if(store.sealed.load(std::memory_order_acquire))return;
    }
    if(store.board_dwell.observe(row.kind,row.physics==100,{row.serial,row.native_time},row.wall_ms))
        seal("observed_board100_dwell_complete_not_a_pose_pass");
}
inline bool finite_bone(const pose::Bone& b,bool zero_scale=false) noexcept {
    float norm=0;for(float q:b.rotation){if(!std::isfinite(q))return false;norm+=q*q;}
    if(norm<.9f||norm>1.1f)return false;
    for(unsigned a=0;a<3;++a)if(!std::isfinite(b.position[a])||std::abs(b.position[a])>1e6f||
       !std::isfinite(b.scale[a])||b.scale[a]<(zero_scale?0.f:.0001f)||b.scale[a]>20)return false;
    return true;
}
// Reuse the existing compose convention with fixed scratch storage; no solve or pose write.
inline bool summarize_skater(Row& row,const pose::Rig& rig,const std::array<pose::Bone,395>& bones) noexcept {
    if(rig.parents.size()!=386||rig.hips<0||rig.hips>=386)return false;
    std::array<pose::Transform,386> composed{};
    for(std::size_t i=0;i<386;++i){
        const auto& b=bones[i];if(!finite_bone(b)||rig.parents[i]<-1||rig.parents[i]>=static_cast<int>(i))return false;
        pose::Transform t{{b.position[0],b.position[1],b.position[2]},b.rotation,(b.scale[0]+b.scale[1]+b.scale[2])/3};
        if(rig.parents[i]>=0){const auto& p=composed[static_cast<std::size_t>(rig.parents[i])];
            t.position=pose::add(p.position,pose::turn(p.rotation,pose::mul(t.position,p.scale)));
            t.rotation=pose::normal(pose::multiply(p.rotation,t.rotation));t.scale*=p.scale;}
        composed[i]=t;
    }
    row.hips_local=bones[static_cast<std::size_t>(rig.hips)].position;
    const std::array<int,6> indices{rig.hips,rig.parents[static_cast<std::size_t>(rig.hips)],rig.left_leg[2],rig.right_leg[2],rig.toe_base[0],rig.toe_base[1]};
    row.selected_joint_indices=indices;row.skater_root_prefix={bones[0],bones[1]};
    for(unsigned i=0;i<indices.size();++i)if(indices[i]>=0&&indices[i]<386){row.composed[i]=composed[static_cast<std::size_t>(indices[i])].position;
        row.composed_available[i]=true;row.selected_joint_bones[i]=bones[static_cast<std::size_t>(indices[i])];}
    row.skater_pose=true;return true;
}
inline void published(std::uintptr_t animation_interface,std::uintptr_t render_data,const pose::Rig* rig,
    std::uint64_t updates,std::uint64_t renders,std::uint64_t board_hides,std::uint64_t revision,
    bool publication_available,bool publication_active,std::uintptr_t publication_entity) noexcept {
    ErrorGuard error;if(!store.enabled.load(std::memory_order_acquire)||!store.armed.load(std::memory_order_acquire)||store.sealed.load(std::memory_order_acquire))return;
    TryGuard guard;if(!guard.held){++store.contention;return;}
    if(!selected()){store.window.fail(policy::Result::configuration_changed);seal("private_configuration_changed");return;}
    if(!before_read())return;
    Row row;if(!common(row)){store.window.fail(policy::Result::read_failure);seal("render_owner_clock_or_read_unavailable");return;}
    try {
        const auto read=[](std::uintptr_t p,void* out,std::size_t n){return memory::peek_bytes(p,out,n);};
        row.component=multiplayer::read_native_skater_component(read,row.owner[0],row.owner[2]);
        // This holder lane is already used/guarded by rider::on_render and apply_rider_pose.
        if(!memory::peek(row.component+0xa0,row.holder)||!row.holder){seal("skater_holder_unavailable");return;}
        if(animation_interface==row.holder+0xc0)row.kind=Kind::skater_published;
        else {
            const auto board=multiplayer::read_native_board(read,row.owner[0],row.owner[2]);
            if(!board.holder||animation_interface!=board.holder+0xc0)return; // unrelated publication
            const auto visual=multiplayer::read_native_board_visual(read,row.owner[0],board.entity);
            if(visual.holder!=board.holder||!visual.initialized||!visual.enabled){seal("board_visual_not_ready");return;}
            row.kind=Kind::board_published;row.component=board.component;row.holder=board.holder;
        }
        if(!admit(row))return;
        const auto bound=row.kind==Kind::skater_published?395U:16U;
        const auto layout=multiplayer::read_native_pose_layout(read,row.owner[0],row.holder,bound);
        row.buffer=layout.buffer;row.bone_count=layout.count;row.render_data=render_data;
        if(!row.buffer||row.bone_count!=bound||!rider::pose_write::published_buffer_matches(read,render_data,row.buffer)){seal("published_layout_or_pointer_unavailable");return;}
        row.published_pointer_matches=true;
        if(row.kind==Kind::skater_published){
            std::array<pose::Bone,395> bones{};
            if(!rig||!read(row.buffer,bones.data(),sizeof(bones))||!summarize_skater(row,*rig,bones)){seal("skater_pose_read_or_known_rig_unavailable");return;}
        }else {
            if(!read(row.buffer,row.board.data(),sizeof(row.board))){seal("board_pose_read_unavailable");return;}
            for(const auto& b:row.board)if(!finite_bone(b,true)){seal("board_pose_invalid");return;}
            row.board_pose=true;
        }
        const auto after=multiplayer::read_native_pose_layout(read,row.owner[0],row.holder,bound);
        if(!final_owner_clock(row)||after.buffer!=row.buffer||after.count!=row.bone_count||
           !rider::pose_write::published_buffer_matches(read,render_data,row.buffer)){seal("publication_owner_clock_or_layout_changed");return;}
        row.same_owner_clock_after=true;row.pose_updates=updates;row.render_updates=renders;row.publication_revision=revision;row.board_hide_updates=board_hides;
        row.publication_available=publication_available;row.publication_active=publication_active;row.publication_entity=publication_entity;retain(row);
        if(store.board_dwell.observe(row.kind,row.physics==100,{row.serial,row.native_time},row.wall_ms))
            seal("observed_board100_dwell_complete_not_a_pose_pass");
    }catch(...){seal("existing_pose_reader_failed");}
}
inline void poll_export(bool idle) noexcept {
    ErrorGuard error;if(!idle||GetCurrentThreadId()!=store.client_thread||!store.sealed.load(std::memory_order_acquire)||
        store.exported.exchange(true,std::memory_order_acq_rel))return;
    TryGuard guard;if(!guard.held){store.exported.store(false);return;}
    try {
        const auto fresh=user_data::override_root();if(!selected()||!fresh||fresh->native()!=store.root.native())return;
        Json rows=Json::array();for(std::size_t i=0;i<store.count;++i){const auto& r=store.rows[i];
            Json composed=Json::array();for(unsigned n=0;n<6;++n)composed.push_back(r.composed_available[n]?Json(r.composed[n]):Json(nullptr));
            const auto bone_json=[](const pose::Bone& b){return Json{{"scale",b.scale},{"rotation",b.rotation},{"position",b.position}};};
            Json selected=Json::array(),root_prefix=Json::array();
            if(r.skater_pose){for(unsigned n=0;n<6;++n)selected.push_back(r.composed_available[n]?bone_json(r.selected_joint_bones[n]):Json(nullptr));
                for(const auto& b:r.skater_root_prefix)root_prefix.push_back(bone_json(b));}
            Json board=Json::array();if(r.board_pose)for(const auto& b:r.board)board.push_back(Json{{"scale",b.scale},{"rotation",b.rotation},{"position",b.position}});
            rows.push_back(Json{{"kind",static_cast<unsigned>(r.kind)},{"wall_ms",r.wall_ms},{"utc_filetime",std::to_string(r.utc_filetime)},
                {"owner",r.owner},{"serial",r.serial},{"native_time",r.native_time},{"physics",r.physics},
                {"offboard",r.native_frame_observed?Json(r.offboard):Json(nullptr)},
                {"standable",r.native_frame_observed?Json(r.standable):Json(nullptr)},
                {"motion_supported",r.native_frame_observed?Json(r.motion_supported):Json(nullptr)},
                {"native_rider_root",r.native_frame_observed?Json(r.native_rider_root):Json(nullptr)},
                {"native_entity_transform",r.native_frame_observed?Json(r.native_entity_transform):Json(nullptr)},
                {"same_owner_clock_after",r.same_owner_clock_after},{"lease_active",r.lease.active},{"lease_epoch",r.lease.epoch_revision},
                {"lease_revision",r.lease.lease_revision},{"lease_epoch_valid",r.lease.epoch_valid},
                {"lease_owner",std::array<std::uintptr_t,5>{r.lease.client,r.lease.entity,r.lease.core,r.lease.context,r.lease.rig}},
                {"lease_clock_serial",r.lease.serial},{"lease_native_time",r.lease.timing_available?Json(r.lease.simulation_time):Json(nullptr)},
                {"lease_owner_matches_current",r.lease.owner_available&&r.lease.client==r.owner[1]&&r.lease.entity==r.owner[2]&&r.lease.core==r.owner[3]&&r.lease.context==r.owner[4]&&r.lease.rig==r.owner[5]},
                {"lease_clock_matches_current",r.lease.timing_available&&r.lease.serial==r.serial&&r.lease.simulation_time==r.native_time},
                {"lease_native_transform_snapshot",r.lease.transform},{"lease_rotation_requested",r.lease.rotation},
                {"full_rotation_requested",r.lease.full_rotation},{"basis_policy_requested",nullptr},{"lease_target_requested",r.lease.target},
                {"lease_velocity_requested",r.lease.velocity},{"lease_yaw_requested",r.lease.yaw},
                {"capture_observed_ms",r.sdk.sampled_ms},{"capture",r.sdk.capture},{"bmx_active",r.sdk.active},{"parts",r.sdk.parts},
                {"board_pending",r.sdk.board_pending},{"stepping_off",r.sdk.stepping_off},
                {"client_motion_updates",r.sdk.motion_updates},{"client_pose_updates",r.sdk.pose_updates},{"client_render_updates",r.sdk.render_updates},
                {"client_board_hide_updates",r.sdk.board_hide_updates},{"chassis_allocations",r.sdk.chassis_allocations},{"chassis_mounts",r.sdk.chassis_mounts},
                {"native_control_input_allowed",r.sdk.native_control_input_allowed},
                {"publication_pose_updates",r.kind!=Kind::client?Json(r.pose_updates):Json(nullptr)},
                {"publication_render_updates",r.kind!=Kind::client?Json(r.render_updates):Json(nullptr)},
                {"publication_revision",r.publication_available?Json(r.publication_revision):Json(nullptr)},
                {"sdk_pose_publication_active",r.publication_available?Json(r.publication_active):Json(nullptr)},
                {"sdk_pose_publication_entity",r.publication_available?Json(r.publication_entity):Json(nullptr)},
                {"custom_board_hide_updates",r.kind!=Kind::client?Json(r.board_hide_updates):Json(nullptr)},
                {"holder",r.holder},{"buffer",r.buffer},{"render_data",r.render_data},{"bone_count",r.bone_count},
                {"published_pointer_matches",r.published_pointer_matches},{"hips_local",r.skater_pose?Json(r.hips_local):Json(nullptr)},
                {"selected_joint_indices",r.skater_pose?Json(r.selected_joint_indices):Json(nullptr)},
                {"raw_selected_joint_bones",std::move(selected)},{"raw_skater_root_prefix",std::move(root_prefix)},
                {"cpu_composed_hips_parent_ankles_toes",std::move(composed)},{"actual_shoe_sole",nullptr},{"actual_deck_surface",nullptr},
                {"raw_board_pose",std::move(board)}});
        }
        Json report{{"private_terminal_client_retention",Json{{"requested",store.sdk.private_terminal.requested},
                {"attempted",store.terminal_client.attempted},{"retained",store.terminal_client.retained},{"issue",store.terminal_client.issue},
                {"owner",store.sdk.private_terminal.owner},{"serial",store.sdk.private_terminal.clock.serial},
                {"native_time",store.sdk.private_terminal.clock.time},{"sampled_ms",store.sdk.private_terminal.sampled_ms},
                {"deadline_ms",store.sdk.private_terminal.deadline_ms}}},
            {"schema",1},{"mode",static_cast<unsigned>(store.mode)},{"process_id",GetCurrentProcessId()},
            {"creation_filetime",std::to_string(store.creation)},{"private_profile","bmx-user-data-chassis-probe"},
            {"private_root",store.root.string()},{"capacity",capacity},{"count",store.count},
            {"window_limit_ms",wall_limit_ms},{"per_kind_minimum_wall_gap_ms",sample_interval_ms},{"stop",store.stop.load(std::memory_order_acquire)},
            {"contention_drops",store.contention.load()},{"rows",std::move(rows)},
            {"prearm_readiness",Json{{"deferred_passive_opt_in",store.deferred_native_board_arm},
                {"limit_ms",policy::native_only_prearm_limit_ms(store.mode,store.automated_native_board,store.deferred_native_board_arm)},
                {"default_limit_ms",policy::prepare_limit_ms},{"applies_to_native_only",store.mode==Mode::native_only},
                {"observation_window_unchanged",true}}},
            {"active_control",Json{{"enabled",store.automated_native_board},{"kind","existing_native_board_mount_request"},
                {"ordinary_Y_or_hardware_test",false},{"attempts",store.native_board_control.attempted?1:0},
                {"accepted",store.native_board_control.accepted},{"failed",store.native_board_control.failed},
                {"issue",store.native_board_control.issue},{"baseline_retained",store.native_board_control.baseline_ready},
                {"baseline_owner",store.native_board_control.owner},{"baseline_serial",store.native_board_control.baseline.serial},
                {"baseline_native_time",store.native_board_control.baseline.time},{"baseline_wall_ms",store.native_board_control.baseline_wall},
                {"request_serial",store.native_board_control.requested.serial},{"request_native_time",store.native_board_control.requested.time},
                {"request_wall_ms",store.native_board_control.requested_wall}}},
            {"boundary","same owner and listener clock CPU evidence; not atomic native/GPU completion; ankle/ToeBase are not measured shoe soles; raw board bones are not a measured deck surface"}};
        constexpr std::size_t byte_limit=2*1024*1024;
        const auto original_bytes=report.dump().size();if(original_bytes>byte_limit)return;
        // Native edge diagnostics may never evict the existing CPU evidence.
        // Leave a bounded field/header reserve and serialize only what fits.
        append_native_edges(report);
        const auto text=report.dump();if(text.size()>byte_limit)return;
        const auto path=*fresh/(L"bmx-board-handoff-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(store.creation)+L".json");
        const auto temporary=std::filesystem::path(path.native()+L".tmp");
        const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;DWORD written{};
        const bool complete=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();CloseHandle(file);
        if(complete)(void)MoveFileW(temporary.c_str(),path.c_str()); // no replace; failed export stays diagnostic-only
    }catch(...){}
}
} // namespace dingosdk::bmx::board_handoff_trace
