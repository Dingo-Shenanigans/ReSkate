#include "bmx_intent_observer.h"
#include "bmx_intent_snapshot.h"
#include "bmx_intent_contracts.h"
#include "bmx_intent_pulse.h"
#include "bmx_getup_action.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "bmx_native_chassis.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/UI/Overlay/input_capture.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>

namespace dingosdk::bmx::intent_observer {
namespace {
// Source-exact authored expression adapters: opcode3 ignores their return.
// Begin receives four pointer words; Update receives three pointer words.
using Begin=void (__fastcall*)(std::uintptr_t,std::uintptr_t,std::uintptr_t,std::uintptr_t);
using Update=void (__fastcall*)(std::uintptr_t,std::uintptr_t,std::uintptr_t);
using End=void (__fastcall*)(std::uintptr_t);
constexpr std::uintptr_t begin_rva=0x1038d20,update_rva=0x1039b10,end_rva=0x1039280;
constexpr std::uintptr_t getter_rva=0x11dd4c0,setter_rva=0x11dd7a0,asset_find_rva=0x41b5800,domain_owners_rva=0x7726d10;
constexpr unsigned per_kind=32;
constexpr unsigned rejected_per_kind=16;
constexpr std::uint64_t capture_ms=200000;
struct Row {
    detail::Snapshot snapshot;
    std::array<std::uintptr_t,4> arguments{};
    std::uint64_t observed_ms{};
    DWORD thread_id{};
    detail::OwnerHint owner_hint;
    bool owner_ready_bank{};
};
struct Slot {std::atomic<unsigned> ready{};Row row;};
struct RejectedRow {
    detail::Snapshot snapshot;
    std::array<std::uintptr_t,4> arguments{};
    std::uint64_t observed_ms{};
    DWORD thread_id{};
};
struct RejectedSlot {std::atomic<unsigned> ready{};RejectedRow row;};
constexpr unsigned probe_per_bank=16;
constexpr std::uint64_t probe_arm_ms=250,probe_follow_ms=2000;
struct ProbeRow {
    detail::Snapshot snapshot;
    std::uint64_t observed_ms{},serial{};
    double simulation_time{};
    DWORD thread_id{};
    bool end{},clock_available{},anchor_matches{},after_arm_window{};
};
struct ProbeSlot {std::atomic<unsigned> ready{};ProbeRow row;};
struct Probe {
    SRWLOCK lifecycle=SRWLOCK_INIT;
    std::atomic<bool> initialized{},active{},cancelled{},owner_changed{};
    std::uintptr_t base{};detail::Owner owner;
    std::uint64_t started{};
    std::atomic<std::uint64_t> callbacks{},matches{},ends{};
    std::atomic<std::uint64_t> eligibility_sequence{},eligibility_observed{};
    std::atomic<bool> eligible{};
    std::atomic<unsigned> anchor_ready{};
    std::uintptr_t anchor_node{},anchor_binding{};
    std::array<std::atomic<std::uint64_t>,3> reserved{};
    std::array<ProbeSlot,probe_per_bank*3> slots;
    std::atomic<bool> activation_enabled{};
    detail::Pulse pulse;
    std::atomic<std::uint64_t> asset_lookups{};
    std::atomic<std::uintptr_t> resolved_property{};
    bool riding{};
};
Probe probe;
struct GetupObservation {
    SRWLOCK lifecycle=SRWLOCK_INIT;
    std::atomic<bool> active{},cancelled{};
    std::atomic<std::uint64_t> next_attempt_ms{},deadline_ms{},contention_drops{},cancel_revision{};
    std::uintptr_t base{};detail::Owner owner;
    getup_observation::Window window;
    std::uint64_t eligibility_observed{};
    bool eligible{};
    std::array<std::uint16_t,2> domain_hints{0xffff,0xffff}; // domain keys, never saved native pointers
    std::array<getup_observation::DomainCursor,2> domain_cursors;
    GetupObservationStatus status;
    std::array<GetupObservationStatus,getup_observation::capacity> rows;
};
GetupObservation getup;
struct GetupAction {
    SRWLOCK lifecycle=SRWLOCK_INIT;
    std::atomic<bool> active{},cancelled{};
    std::atomic<std::uint64_t> cancel_revision{};
    std::uintptr_t base{};detail::Owner owner;
    detail::Pulse pulse;
    std::uint64_t eligibility_observed{};
    bool eligible{};
    std::array<std::uint16_t,2> domain_hints{0xffff,0xffff};
    std::array<getup_observation::DomainCursor,2> domain_cursors;
    GetupActionStatus status;
};
GetupAction action;
struct CallbackAudit {
    SRWLOCK lifecycle=SRWLOCK_INIT;
    std::atomic<bool> active{},cancelled{};
    std::atomic<std::uint64_t> deadline{},callbacks{},contention_drops{},cancel_revision{};
    std::uintptr_t base{};detail::Owner owner;
    getup_callback_audit::Window window;
    GetupCallbackAuditStatus status;
    std::array<std::uint16_t,2> hints{0xffff,0xffff};
    unsigned count{};
    std::array<GetupCallbackAuditRow,getup_callback_audit::capacity> rows;
};
CallbackAudit audit;
struct Shared {
    std::atomic<Begin> original_begin{};
    std::atomic<Update> original_update{};
    std::atomic<End> original_end{};
    std::atomic<std::uint64_t> end_forwarded{};
    std::atomic<bool> capturing{};
    std::atomic<bool> requested{};
    std::array<std::atomic<std::uint64_t>,2> forwarded{},filtered{},unreadable{},after_capacity{};
    std::array<std::atomic<std::uint64_t>,4> reserved{};
    std::array<Slot,per_kind*4> slots;
    std::array<std::atomic<std::uint64_t>,2> rejected_reserved{};
    std::array<RejectedSlot,rejected_per_kind*2> rejected;
    std::array<detail::Correlation,per_kind*4> correlations;
    std::array<bool,per_kind*4> correlation_done{};
    std::atomic<std::uint64_t> owner_sequence{},owner_observed{};
    std::atomic<std::uintptr_t> owner_entity{};
    std::uintptr_t base{};
    std::uint64_t started{},last_poll{};
    std::filesystem::path root;
    std::mutex poll_mutex;
    Status status;
    bool attempted{};
    std::atomic<bool> riding_enabled{};
    std::filesystem::path riding_root;
    std::filesystem::path getup_root;
    std::filesystem::path action_root;
    std::filesystem::path audit_root;
};
// No hook callback constructs this object or touches its filesystem/mutex fields.
Shared shared;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free&&std::atomic<std::uintptr_t>::is_always_lock_free);
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
struct ProbeExclusive {
    bool held=TryAcquireSRWLockExclusive(&probe.lifecycle)!=0;
    ~ProbeExclusive(){if(held)ReleaseSRWLockExclusive(&probe.lifecycle);}
};
struct ProbeShared {
    bool held=TryAcquireSRWLockShared(&probe.lifecycle)!=0;
    ~ProbeShared(){if(held)ReleaseSRWLockShared(&probe.lifecycle);}
};
struct GetupExclusive {
    bool held=TryAcquireSRWLockExclusive(&getup.lifecycle)!=0;
    ~GetupExclusive(){if(held)ReleaseSRWLockExclusive(&getup.lifecycle);}
};
struct GetupShared {
    bool held=TryAcquireSRWLockShared(&getup.lifecycle)!=0;
    ~GetupShared(){if(held)ReleaseSRWLockShared(&getup.lifecycle);}
};
struct ActionExclusive {
    bool held=TryAcquireSRWLockExclusive(&action.lifecycle)!=0;
    ~ActionExclusive(){if(held)ReleaseSRWLockExclusive(&action.lifecycle);}
};
struct ActionShared {
    bool held=TryAcquireSRWLockShared(&action.lifecycle)!=0;
    ~ActionShared(){if(held)ReleaseSRWLockShared(&action.lifecycle);}
};
struct AuditExclusive {
    bool held=TryAcquireSRWLockExclusive(&audit.lifecycle)!=0;
    ~AuditExclusive(){if(held)ReleaseSRWLockExclusive(&audit.lifecycle);}
};
struct AuditShared {
    bool held=TryAcquireSRWLockShared(&audit.lifecycle)!=0;
    ~AuditShared(){if(held)ReleaseSRWLockShared(&audit.lifecycle);}
};
bool opted_flag() noexcept {
    wchar_t flag[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_INTENT_OBSERVER",flag,2)==1&&flag[0]==L'1'&&user_data::development_requested();
}
bool activation_flag() noexcept {
    wchar_t flag[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_INTENT_ACTIVATION",flag,2)==1&&flag[0]==L'1';
}
bool opted_root(std::filesystem::path& root){
    if(!opted_flag())return false;
    const auto requested=user_data::override_root();
    if(!requested||_wcsicmp(requested->filename().c_str(),L"bmx-user-data-chassis-probe"))return false;
    root=*requested;return true;
}
bool riding_root(std::filesystem::path& root){
    wchar_t flag[2]{},image[32768]{};
    if(GetEnvironmentVariableW(L"RESKATE_BMX_RIDING_FALLS",flag,2)!=1||flag[0]!=L'1'||
       !user_data::development_requested())return false;
    const auto requested=user_data::override_root();if(!requested)return false;
    const auto length=GetModuleFileNameW(nullptr,image,static_cast<DWORD>(std::size(image)));
    if(!length||length>=std::size(image))return false;
    const auto executable=user_data::detail::canonical(std::filesystem::path(image));
    const auto isolated=user_data::detail::canonical(requested->parent_path()/L"bmx-game");
    if(_wcsicmp(executable.filename().c_str(),L"Skate.exe")||executable.parent_path()!=isolated)return false;
    root=*requested;return true;
}
bool getup_root(std::filesystem::path& root){
    wchar_t flag[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_RIDE_TRACE",flag,2)==1&&flag[0]==L'1'&&riding_root(root);
}
bool action_root(std::filesystem::path& root){
    const auto flag=[](const wchar_t* name) noexcept {
        wchar_t value[2]{};return GetEnvironmentVariableW(name,value,2)==1&&value[0]==L'1';
    };
    wchar_t impact[16]{};const auto n=GetEnvironmentVariableW(L"RESKATE_BMX_IMPACT_TEST",impact,16);
    return flag(L"RESKATE_BMX_GETUP_PULSE")&&flag(L"RESKATE_BMX_AUTOMATED_RIDE")&&
        flag(L"RESKATE_BMX_NATIVE_CHASSIS")&&!flag(L"RESKATE_BMX_IMPACT_RECOVERY")&&
        n&&n<16&&(!wcscmp(impact,L"inverted")||!wcscmp(impact,L"side"))&&getup_root(root)&&
        root.filename()==L"bmx-user-data-chassis-probe";
}
bool read(std::uintptr_t p,void* out,std::size_t n) noexcept {return memory::peek_bytes(p,out,n);}
bool owner_matches(const NoBailSkater& current,const detail::Owner& expected,std::uintptr_t base) noexcept {
    return current.base==base&&current.entity==expected.entity&&current.core==expected.core&&
        current.context==expected.context&&current.rig==expected.rig;
}
// Regular client thread only. Callback validation never copies the allocating
// multiplayer model or performs filesystem/environment work.
void publish_probe_eligibility(std::uint64_t now,bool eligible) noexcept {
    probe.eligibility_sequence.fetch_add(1);
    probe.eligible.store(eligible);probe.eligibility_observed.store(now);
    probe.eligibility_sequence.fetch_add(1);
}
void activate_probe(const detail::Snapshot& sample,std::uint64_t now,bool eligible) noexcept;
void observe_getup(const detail::Snapshot& sample,std::uint64_t now) noexcept;
void observe_getup_action(const detail::Snapshot& sample,std::uint64_t now,getup_callback_audit::Dispatch* evidence) noexcept;
void observe_callback_audit(const detail::Snapshot& sample,std::uint64_t now,const getup_callback_audit::Dispatch&) noexcept;
void observe_probe(const detail::Snapshot& sample,bool end,std::uint64_t now) noexcept {
    if(!probe.active.load(std::memory_order_acquire))return;
    ProbeExclusive lifecycle;if(!lifecycle.held||!probe.active.load(std::memory_order_acquire))return;
    probe.callbacks.fetch_add(1,std::memory_order_relaxed);
    const auto phase=detail::probe_phase(probe.started,now);
    const bool cleanup=probe.activation_enabled&&probe.pulse.pending();
    if(phase==detail::ProbePhase::closed&&!cleanup&&!probe.pulse.busy.test(std::memory_order_acquire)){
        probe.active.store(false,std::memory_order_release);return;
    }
    try {
        const auto& owner=probe.owner;
        const auto eligibility=probe.eligibility_sequence.load();
        const bool eligible=probe.eligible.load();const auto observed=probe.eligibility_observed.load();
        const bool eligible_now=!(eligibility&1)&&probe.eligibility_sequence.load()==eligibility&&eligible&&now>=observed&&now-observed<=250;
        if((!cleanup&&!eligible_now)||
           (!end&&!sample.update_after_coherent)||!detail::control_matches(probe.base,sample,owner,&read))return;
        NoBailSkater current;bool held{};
        if(!inspect_no_bail_owner(owner.client,owner.entity,current,held)||(!cleanup&&held)||!owner_matches(current,owner,probe.base))return;
        const bool after=phase!=detail::ProbePhase::arm;
        unsigned anchor=probe.anchor_ready.load(std::memory_order_acquire);
        if(!after&&!end&&anchor==0){
            unsigned empty{};
            if(probe.anchor_ready.compare_exchange_strong(empty,1,std::memory_order_acq_rel)){
                probe.anchor_node=sample.node;probe.anchor_binding=sample.binding;
                probe.anchor_ready.store(2,std::memory_order_release);
            }
            anchor=probe.anchor_ready.load(std::memory_order_acquire);
        }
        const bool same_anchor=anchor==2&&sample.node==probe.anchor_node&&sample.binding==probe.anchor_binding;
        if((after||end)&&!(anchor==2&&sample.binding==probe.anchor_binding))return;
        NoBailSkater final_owner;bool final_held{};
        if(!inspect_no_bail_owner(owner.client,owner.entity,final_owner,final_held)||(!cleanup&&final_held)||
           !owner_matches(final_owner,owner,probe.base)||!detail::control_matches(probe.base,sample,owner,&read))return;
        probe.matches.fetch_add(1,std::memory_order_relaxed);
        if(!end&&probe.activation_enabled)activate_probe(sample,now,eligible_now);
        if(end&&same_anchor)probe.ends.fetch_add(1,std::memory_order_relaxed);
        const unsigned bank=end?2U:after?1U:0U;
        const auto sequence=probe.reserved[bank].fetch_add(1,std::memory_order_relaxed);
        if(sequence>=probe_per_bank)return;
        ProbeRow row;row.snapshot=sample;row.observed_ms=now;row.thread_id=GetCurrentThreadId();
        row.end=end;row.anchor_matches=same_anchor;row.after_arm_window=after;
        row.clock_available=read_native_clock_try(owner.entity,owner.core,row.serial,row.simulation_time);
        row.clock_available=row.clock_available&&std::isfinite(row.simulation_time);
        if(!row.clock_available){row.serial=0;row.simulation_time=0;}
        auto& slot=probe.slots[bank*probe_per_bank+static_cast<unsigned>(sequence)];
        slot.row=row;slot.ready.store(1,std::memory_order_release);
    }catch(...){}
}
void capture(detail::Kind kind,const std::array<std::uintptr_t,4>& arguments) noexcept {
    const auto key=static_cast<unsigned>(kind);
    shared.forwarded[key].fetch_add(1,std::memory_order_relaxed);
    const auto now=GetTickCount64();
    const bool cleanup=probe.initialized.load(std::memory_order_acquire)&&probe.activation_enabled&&probe.pulse.pending()&&
        !probe.owner_changed.load(std::memory_order_acquire);
    const bool active=probe.active.load(std::memory_order_acquire);
    const bool semantic=kind==detail::Kind::update&&getup.active.load(std::memory_order_acquire)&&
        !getup.cancelled.load(std::memory_order_acquire)&&
        now<getup.deadline_ms.load(std::memory_order_acquire)&&now>=getup.next_attempt_ms.load(std::memory_order_relaxed);
    const bool acting=kind==detail::Kind::update&&action.active.load(std::memory_order_acquire);
    const bool auditing=kind==detail::Kind::update&&audit.active.load(std::memory_order_acquire)&&
        now<audit.deadline.load(std::memory_order_relaxed);
    const bool passive=shared.capturing.load(std::memory_order_relaxed)&&now>=shared.started&&now-shared.started<capture_ms;
    if(!detail::admit_callback(passive,shared.started,now,active||semantic||acting||auditing,cleanup))return;
    if(!semantic&&!acting&&!auditing&&!probe.active.load(std::memory_order_acquire)&&shared.reserved[key].load(std::memory_order_relaxed)>=per_kind&&shared.reserved[key+2].load(std::memory_order_relaxed)>=per_kind){
        // No identity reads after capacity. This is all events, NOT known matching drops.
        shared.after_capacity[key].fetch_add(1,std::memory_order_relaxed);return;
    }
    Row row;row.arguments=arguments;row.observed_ms=now;row.thread_id=GetCurrentThreadId();
    row.snapshot=detail::inspect(shared.base,kind,arguments[0],arguments[1],arguments[2],&read);
    if(kind==detail::Kind::update)observe_probe(row.snapshot,false,now);
    if(semantic)observe_getup(row.snapshot,now);
    getup_callback_audit::Dispatch dispatch;
    if(acting)observe_getup_action(row.snapshot,now,auditing?&dispatch:nullptr);
    if(auditing)observe_callback_audit(row.snapshot,now,dispatch);
    if(!passive)return; // ordinary play never fills passive capture/report banks
    if(!row.snapshot.target){
        (row.snapshot.node_coherent?shared.filtered[key]:shared.unreadable[key]).fetch_add(1,std::memory_order_relaxed);
        const auto sequence=shared.rejected_reserved[key].fetch_add(1,std::memory_order_relaxed);
        if(sequence<rejected_per_kind){
            auto& slot=shared.rejected[key*rejected_per_kind+static_cast<unsigned>(sequence)];
            slot.row={row.snapshot,arguments,now,row.thread_id};
            slot.ready.store(1,std::memory_order_release);
        }
        return;
    }
    // Sequentially consistent hint words/counter give a coherent, nonblocking
    // snapshot. The hint selects storage only; poll revalidates real ownership.
    const auto first=shared.owner_sequence.load();
    row.owner_hint.entity=shared.owner_entity.load();row.owner_hint.observed_ms=shared.owner_observed.load();
    row.owner_hint.coherent=!(first&1)&&shared.owner_sequence.load()==first;
    row.owner_ready_bank=detail::owner_bank(row.snapshot,row.owner_hint,now);
    const auto bank=key+(row.owner_ready_bank?2U:0U);
    const auto sequence=shared.reserved[bank].fetch_add(1,std::memory_order_relaxed);
    if(sequence>=per_kind){shared.after_capacity[key].fetch_add(1,std::memory_order_relaxed);return;}
    auto& slot=shared.slots[bank*per_kind+static_cast<unsigned>(sequence)];
    slot.row=row;slot.ready.store(1,std::memory_order_release);
}
void __fastcall begin_hook(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){
    const auto incoming=GetLastError();const auto original=shared.original_begin.load(std::memory_order_acquire);SetLastError(incoming);
    detail::forward_observed(original,[](auto x,auto y,auto z,auto w) noexcept {capture(detail::Kind::begin,{x,y,z,w});},
        []() noexcept{return GetLastError();},[](DWORD value) noexcept{SetLastError(value);},a,b,c,d);
}
void __fastcall update_hook(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c){
    const auto incoming=GetLastError();const auto original=shared.original_update.load(std::memory_order_acquire);SetLastError(incoming);
    detail::forward_observed(original,[](auto x,auto y,auto z) noexcept {capture(detail::Kind::update,{x,y,z,0});},
        []() noexcept{return GetLastError();},[](DWORD value) noexcept{SetLastError(value);},a,b,c);
}
void __fastcall end_hook(std::uintptr_t a){
    const auto incoming=GetLastError();const auto original=shared.original_end.load(std::memory_order_acquire);SetLastError(incoming);
    detail::forward_observed(original,[](auto event) noexcept {
        shared.end_forwarded.fetch_add(1,std::memory_order_relaxed);
        if(probe.active.load(std::memory_order_acquire))
            observe_probe(detail::inspect(shared.base,detail::Kind::begin,event,0,0,&read),true,GetTickCount64());
    },[]() noexcept{return GetLastError();},[](DWORD value) noexcept{SetLastError(value);},a);
}
template<std::size_t N> bool matches(std::uintptr_t p,const std::array<unsigned char,N>& expected) noexcept {
    std::array<unsigned char,N> actual{};return memory::peek(p,actual)&&actual==expected;
}
bool native_get(std::uintptr_t property,std::uintptr_t binding,std::uint8_t& value) noexcept {
    __try {value=reinterpret_cast<std::uint8_t(__fastcall*)(std::uintptr_t,std::uintptr_t)>(probe.base+getter_rva)(property,binding);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool native_get_at(std::uintptr_t base,std::uintptr_t property,std::uintptr_t binding,std::uint8_t& value) noexcept {
    __try {value=reinterpret_cast<std::uint8_t(__fastcall*)(std::uintptr_t,std::uintptr_t)>(base+getter_rva)(property,binding);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool native_set(std::uintptr_t property,std::uintptr_t binding,std::uint8_t value) noexcept {
    __try {reinterpret_cast<void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint8_t)>(probe.base+setter_rva)(property,binding,value);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool native_set_at(std::uintptr_t base,std::uintptr_t property,std::uintptr_t binding,std::uint8_t value) noexcept {
    __try {reinterpret_cast<void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint8_t)>(base+setter_rva)(property,binding,value);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool native_find(std::uint16_t domain,const char* name,std::uintptr_t& value) noexcept {
    __try {value=game::native_data().find_asset(domain,name);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool pulse_contract(std::uintptr_t property) noexcept {
    std::uintptr_t getter{},setter{};
    const auto asset=detail::inspect_asset(probe.base,property,&read);
    return asset.request_wipeout&&detail::get(probe.base+detail::bool_vtable_rva+0x18,getter,&read)&&
        detail::get(probe.base+detail::bool_vtable_rva+0x20,setter,&read)&&getter==probe.base+getter_rva&&setter==probe.base+setter_rva&&
        matches(getter,contracts::bool_get)&&matches(setter,contracts::bool_set);
}
bool resolve_wipeout(const detail::Snapshot& sample,std::uintptr_t& property) noexcept {
    // Already-current exact target interfaces need no registry lookup.
    if(sample.input.request_wipeout&&pulse_contract(sample.input.property)){property=sample.input.property;return true;}
    if(sample.mirror.request_wipeout&&pulse_contract(sample.mirror.property)){property=sample.mirror.property;return true;}
    if(reinterpret_cast<std::uintptr_t>(game::native_data().find_asset)!=probe.base+asset_find_rva||
       !matches(probe.base+asset_find_rva,contracts::asset_find))return false;
    // Same bounded loaded-domain query used by the SDK's native prop lookup.
    // No native pointer from this search survives the current callback.
    for(std::uint16_t domain=0;domain<=0xbbf;++domain){
        const auto slot=probe.base+domain_owners_rva+std::uintptr_t{domain}*8;
        std::uintptr_t owner{},again{},asset{};
        if(!detail::get(slot,owner,&read)||!owner)continue;
        probe.asset_lookups.fetch_add(1,std::memory_order_relaxed);
        if(!native_find(domain,"animation/dingo/bool.intent.requestwipeout",asset)||!detail::get(slot,again,&read)||owner!=again)return false;
        if(!asset){
            probe.asset_lookups.fetch_add(1,std::memory_order_relaxed);
            if(!native_find(domain,"Animation/Dingo/Bool.Intent.RequestWipeout",asset)||!detail::get(slot,again,&read)||owner!=again)return false;
        }
        if(asset&&detail::span(asset,0x80)&&pulse_contract(asset+0x60)){property=asset+0x60;return true;}
    }
    return false;
}
bool getup_asset_contract(std::uintptr_t base,std::uintptr_t property,const std::array<std::uint8_t,16>& guid) noexcept {
    std::uintptr_t getter{};const auto asset=detail::inspect_asset(base,property,&read);
    return asset.typed&&asset.guid==guid&&detail::get(base+detail::bool_vtable_rva+0x18,getter,&read)&&
        getter==base+getter_rva&&matches(getter,contracts::bool_get);
}
bool resolve_getup_asset(std::uintptr_t base,const detail::Snapshot& sample,const std::array<std::uint8_t,16>& guid,
    const char* lower,const char* upper,std::uint16_t& hint,getup_observation::DomainCursor& cursor,
    std::uint64_t& lookup_calls,std::uintptr_t& property) noexcept {
    for(const auto* asset:{&sample.input,&sample.mirror})
        if(asset->typed&&asset->guid==guid&&getup_asset_contract(base,asset->property,guid)){
            property=asset->property;return true;
        }
    if(reinterpret_cast<std::uintptr_t>(game::native_data().find_asset)!=base+asset_find_rva||
       !matches(base+asset_find_rva,contracts::asset_find))return false;
    const auto query=[&](std::uint16_t domain) noexcept {
        using Result=getup_observation::DomainResult;
        const auto slot=base+domain_owners_rva+std::uintptr_t{domain}*8;
        std::uintptr_t owner{},again{},asset{};
        if(!detail::get(slot,owner,&read)||!owner)return Result::empty;
        for(const auto* name:{lower,upper}) {
            ++lookup_calls;
            if(!native_find(domain,name,asset)||!detail::get(slot,again,&read)||owner!=again)return Result::miss;
            if(asset) {
                if(!detail::span(asset,0x80)||!getup_asset_contract(base,asset+0x60,guid))return Result::miss;
                property=asset+0x60;return Result::found;
            }
        }
        return Result::miss;
    };
    unsigned budget=getup_observation::populated_domain_budget;
    if(hint<getup_observation::domain_count){
        const auto result=query(hint);
        if(result==getup_observation::DomainResult::found)return true;
        if(result==getup_observation::DomainResult::miss)--budget;
    }
    const auto searched_hint=hint;
    return cursor.search(budget,[&](std::uint16_t domain) noexcept {
        if(domain==searched_hint)return getup_observation::DomainResult::empty;
        const auto result=query(domain);if(result==getup_observation::DomainResult::found)hint=domain;return result;
    });
}
void close_getup(const char* issue,bool owner_changed=false) noexcept {
    getup.status.active=false;getup.status.completed=true;getup.status.owner_changed=owner_changed;
    getup.status.issue=issue;getup.active.store(false,std::memory_order_release);
}
void observe_getup(const detail::Snapshot& sample,std::uint64_t now) noexcept {
    if(!getup.active.load(std::memory_order_acquire))return;
    GetupExclusive lock;
    if(!lock.held){getup.contention_drops.fetch_add(1,std::memory_order_relaxed);return;}
    if(!getup.active.load(std::memory_order_acquire))return;
    ++getup.status.callbacks;
    try {
        const auto& owner=getup.owner;
        if(now<getup.window.started_ms||now-getup.window.started_ms>=getup_observation::wall_limit_ms){
            close_getup("Passive get-up observation reached its wall deadline.");return;
        }
        if(!getup.eligible||now<getup.eligibility_observed||now-getup.eligibility_observed>250||
           !sample.update_after_coherent||!detail::control_matches(getup.base,sample,owner,&read))return;
        if(getup.status.entity_handle&&sample.entity_handle!=getup.status.entity_handle){
            close_getup("Passive get-up full rider handle changed.",true);return;
        }
        std::uint64_t serial{};double time{};
        if(!read_native_clock_try(owner.entity,owner.core,serial,time)){
            ++getup.status.failures;getup.status.issue="Passive get-up native clock unavailable.";return;
        }
        const auto decision=getup.window.consider(serial,time,now);
        if(decision==getup_observation::Decision::expired){close_getup("Passive get-up native observation window completed.");return;}
        if(decision==getup_observation::Decision::clock_rejected){
            ++getup.status.failures;close_getup("Passive get-up native clock changed epoch.");return;
        }
        if(decision==getup_observation::Decision::wait){getup.next_attempt_ms.store(now+10,std::memory_order_relaxed);return;}
        const auto qualify=[&]() noexcept {
            if(!getup.active.load(std::memory_order_acquire)||getup.cancelled.load(std::memory_order_acquire))return false;
            const auto fresh=detail::inspect(getup.base,detail::Kind::begin,sample.event_params,0,0,&read);
            if(fresh.node!=sample.node||fresh.root!=sample.root||fresh.data!=sample.data||fresh.binding!=sample.binding||
               fresh.context_wrapper!=sample.context_wrapper||fresh.entity_handle!=sample.entity_handle||
               !detail::control_matches(getup.base,fresh,owner,&read))return false;
            NoBailSkater current;bool held{};std::uint64_t current_serial{};double current_time{};
            return inspect_no_bail_owner(owner.client,owner.entity,current,held)&&owner_matches(current,owner,getup.base)&&
                read_native_clock_try(owner.entity,owner.core,current_serial,current_time)&&current_serial==serial&&current_time==time;
        };
        if(!qualify()){++getup.status.failures;return;}
        auto value=getup.status;value.getup={};value.is_getup_allowed={};
        const auto measure=[&](GetupBoolObservation& result,const std::array<std::uint8_t,16>& guid,
            const char* lower,const char* upper,std::uint16_t& hint,getup_observation::DomainCursor& cursor) noexcept {
            result.guid=guid;std::uintptr_t property{};
            if(!resolve_getup_asset(getup.base,sample,guid,lower,upper,hint,cursor,getup.status.asset_lookup_calls,property)||!qualify())return;
            result.asset_verified=true;std::uint8_t readback{};++getup.status.getter_calls;
            if(!native_get_at(getup.base,property,sample.binding,readback)||readback>1||
               !getup_asset_contract(getup.base,property,guid)||!qualify())return;
            result.getter_available=true;result.value=readback!=0;
            // The proven getter falls back to the asset's default when the
            // current binding has no slot. No new slot-resolver ABI is guessed.
            result.binding_present_known=false;result.binding_present=false;
        };
        measure(value.getup,getup_observation::getup_guid,"animation/dingo/bool.intent.wipeout.getup",
            "Animation/Dingo/Bool.Intent.WipeOut.GetUp",getup.domain_hints[0],getup.domain_cursors[0]);
        measure(value.is_getup_allowed,getup_observation::allowed_guid,"animation/dingo/bool.gp.isgetupallowed",
            "Animation/Dingo/Bool.GP.IsGetUpAllowed",getup.domain_hints[1],getup.domain_cursors[1]);
        if(!qualify()){++getup.status.failures;getup.status.issue="Passive get-up callback changed during reads.";return;}
        if(!getup.window.commit(serial,time,now))return;
        value.available=true;value.active=true;value.completed=false;value.entity_handle=sample.entity_handle;
        value.serial=serial;value.simulation_time=time;value.observed_ms=now;value.samples=getup.window.samples;
        value.callbacks=getup.status.callbacks;value.getter_calls=getup.status.getter_calls;
        value.asset_lookup_calls=getup.status.asset_lookup_calls;value.failures=getup.status.failures;
        if(!value.getup.getter_available||!value.is_getup_allowed.getter_available)++value.failures;
        value.contention_drops=getup.contention_drops.load(std::memory_order_relaxed);
        value.issue=value.getup.getter_available&&value.is_getup_allowed.getter_available?
            "Passive typed get-up values read; binding slot presence unknown.":"Passive get-up asset/getter unavailable; values unmeasured.";
        getup.status=value;getup.rows[value.samples-1]=value;
        getup.next_attempt_ms.store(now+80,std::memory_order_relaxed);
    }catch(...){++getup.status.failures;getup.status.issue="Passive get-up observation failed.";}
}
bool getup_setter_contract(std::uintptr_t base,std::uintptr_t property) noexcept {
    std::uintptr_t setter{};
    return getup_asset_contract(base,property,getup_observation::getup_guid)&&
        detail::get(base+detail::bool_vtable_rva+0x20,setter,&read)&&setter==base+setter_rva&&
        matches(setter,contracts::bool_set);
}
bool released_getup_body(const detail::Owner& owner,const GetupActionRequest& request) noexcept {
    const auto released=native_chassis::released_ownership_status();
    return released.available&&released.ready&&released.detached&&!released.active&&!released.lease_active&&!released.failed&&
        released.client==owner.client&&released.entity==owner.entity&&released.core==owner.core&&
        released.context==owner.context&&released.rig==owner.rig&&released.world==request.world&&
        released.native_state==request.native_state&&released.body_index==request.body_index&&
        released.body_generation==request.body_generation&&released.token==request.body_token;
}
// Existing supported offboard ownership/state fields, read only. No native
// state selector, transition flag, collision flag or position is modified.
bool known_getup_ragdoll(std::uintptr_t base,const detail::Owner& owner) noexcept {
    namespace flight=game::build::v20260929::offboard_flight;
    std::uintptr_t parent{},active{},vtable{},context{},rig{},board{},expected_board{},trick{},offboard{};
    std::uint32_t physics{};std::uint8_t ragdoll{};
    if(!detail::get(owner.context+0x1414,physics,&read)||physics!=504||
       !detail::get(owner.core+0x3b0,parent,&read)||!detail::get(parent,vtable,&read)||
       vtable!=base+flight::offboard_flight_vtable||!detail::get(parent+8,context,&read)||context!=owner.context||
       !detail::get(parent+0x18,rig,&read)||rig!=owner.rig||!detail::get(parent+0x10,board,&read)||
       !detail::get(owner.core+0x430,expected_board,&read)||board!=expected_board||
       !detail::get(parent+0x48,active,&read)||!detail::get(active,vtable,&read)||
       !detail::get(owner.core+0x3b8,trick,&read)||!detail::get(trick+0x78,offboard,&read)||
       !detail::get(offboard+0x18c,ragdoll,&read)||ragdoll!=1)return false;
    bool known=false;
    for(const auto& state:flight::offboard_ragdoll_states)
        if(active==parent+state[0]&&vtable==base+state[1])known=true;
    std::uintptr_t parent_again{},active_again{},vtable_again{},context_again{},rig_again{},board_again{},
        expected_board_again{},trick_again{},offboard_again{};
    std::uint32_t physics_again{};std::uint8_t ragdoll_again{};
    return known&&detail::get(owner.core+0x3b0,parent_again,&read)&&parent_again==parent&&
        detail::get(parent_again+0x48,active_again,&read)&&active_again==active&&
        detail::get(active_again,vtable_again,&read)&&vtable_again==vtable&&
        detail::get(parent_again,vtable_again,&read)&&vtable_again==base+flight::offboard_flight_vtable&&
        detail::get(parent_again+8,context_again,&read)&&context_again==context&&
        detail::get(parent_again+0x18,rig_again,&read)&&rig_again==rig&&
        detail::get(parent_again+0x10,board_again,&read)&&board_again==board&&
        detail::get(owner.core+0x430,expected_board_again,&read)&&expected_board_again==board&&
        detail::get(owner.core+0x3b8,trick_again,&read)&&trick_again==trick&&
        detail::get(trick_again+0x78,offboard_again,&read)&&offboard_again==offboard&&
        detail::get(owner.context+0x1414,physics_again,&read)&&physics_again==physics&&
        detail::get(offboard_again+0x18c,ragdoll_again,&read)&&ragdoll_again==ragdoll;
}
void observe_getup_action(const detail::Snapshot& sample,std::uint64_t now,getup_callback_audit::Dispatch* evidence) noexcept {
    using AuditIssue=getup_callback_audit::DispatchIssue;
    if(!action.active.load(std::memory_order_acquire))return;
    ActionExclusive lock;
    if(!lock.held){if(evidence)evidence->issue=AuditIssue::unavailable;return;}
    if(!action.active.load(std::memory_order_acquire))return;
    struct EvidenceScope {
        getup_callback_audit::Dispatch* out;std::uint64_t serial{};
        explicit EvidenceScope(getup_callback_audit::Dispatch* p) noexcept:out(p) {
            if(!out)return;out->available=true;out->observer_admitted=true;out->generation=action.status.generation;
            out->phase_before=static_cast<unsigned>(action.pulse.phase.load());
            out->preflight_before=action.pulse.preflight_serial.load();out->true_calls_before=action.pulse.true_calls.load();
            out->issue=AuditIssue::not_called;
        }
        ~EvidenceScope() {
            if(!out)return;out->phase_after=static_cast<unsigned>(action.pulse.phase.load());
            out->preflight_after=action.pulse.preflight_serial.load();out->true_calls_after=action.pulse.true_calls.load();
            out->serial_already_reserved=serial&&out->preflight_before==serial;
            out->serial_reserved_this_callback=serial&&out->preflight_before!=serial&&out->preflight_after==serial;
            out->true_entry_this_callback=out->true_calls_after>out->true_calls_before;
        }
        void issue(AuditIssue value) const noexcept {if(out)out->issue=value;}
    } observed{evidence};
    auto& status=action.status;++status.callbacks;
    const bool cleanup=action.pulse.pending();
    if(!cleanup&&(action.cancelled.load(std::memory_order_acquire)||now<status.started_ms||
        now-status.started_ms>=getup_action::start_limit_ms)) {
        observed.issue(AuditIssue::cancelled_or_expired);
        action.active.store(false,std::memory_order_release);return;
    }
    if(!sample.update_after_coherent||!detail::control_matches(action.base,sample,action.owner,&read)){
        observed.issue(AuditIssue::callback_unqualified);return;
    }
    if(sample.entity_handle!=status.entity_handle) {
        observed.issue(AuditIssue::owner_changed);status.owner_changed=true;action.active.store(false,std::memory_order_release);return;
    }
    std::uint64_t serial{};double time{};
    const bool action_clock_available=read_native_clock_try(action.owner.entity,action.owner.core,serial,time);
    if(evidence) {evidence->clock_available=action_clock_available;evidence->serial=serial;evidence->native_time=time;}
    if(!action_clock_available||!getup_action::epoch({status.requested_serial,status.requested_native_time},{serial,time})) {
        observed.issue(AuditIssue::clock_unavailable);status.issue="Lab GetUp current native clock unavailable or request epoch changed.";return;
    }
    observed.serial=serial;
    std::uintptr_t resolved{};
    const auto common=[&]() noexcept {
        const auto fresh=detail::inspect(action.base,detail::Kind::begin,sample.event_params,0,0,&read);
        if(fresh.node!=sample.node||fresh.root!=sample.root||fresh.data!=sample.data||fresh.binding!=sample.binding||
           fresh.context_wrapper!=sample.context_wrapper||fresh.entity_handle!=status.entity_handle||
           !detail::control_matches(action.base,fresh,action.owner,&read))return false;
        NoBailSkater owner;bool held{};std::uint64_t after{};double after_time{};
        return inspect_no_bail_owner(action.owner.client,action.owner.entity,owner,held)&&
            owner_matches(owner,action.owner,action.base)&&
            read_native_clock_try(action.owner.entity,action.owner.core,after,after_time)&&
            getup_action::exact({serial,time},{after,after_time});
    };
    const auto qualify=[&](bool resetting) noexcept {
        if(status.owner_changed||!common()||(resolved&&!getup_setter_contract(action.base,resolved))) {
            observed.issue(AuditIssue::transaction_changed);
            status.issue="Lab GetUp callback owner, full handle, clock or Bool setter contract changed.";return false;
        }
        if(resetting)return true; // Cleanup is independent of native recovery and start permission.
        NoBailSkater owner;bool held{};
        const auto motion=native_motion_lease_status();const auto current_ms=GetTickCount64();
        const getup_action::Permission permission{action.eligible,action.cancelled.load(std::memory_order_acquire),
            inspect_no_bail_owner(action.owner.client,action.owner.entity,owner,held)&&owner_matches(owner,action.owner,action.base),
            !held,known_getup_ragdoll(action.base,action.owner),motion.available&&!motion.active,
            !overlay::detail::bmx_input_capture_active(),!overlay::detail::bmx_interaction_capture_active(),
            released_getup_body(action.owner,status.request),status.started_ms,current_ms,action.eligibility_observed};
        if(!getup_action::may_start(permission)) {
            observed.issue(AuditIssue::permission_declined);
            status.issue=!permission.body_released?"Lab GetUp released SDK body ownership unavailable or differs.":
                !permission.known_ragdoll?"Lab GetUp supported native ragdoll is no longer freshly verified.":
                !permission.unprotected?"Lab GetUp independent NoBail protection is active.":
                !permission.motion_released||!permission.bmx_released||!permission.interaction_released?
                    "Lab GetUp SDK motion/input release unavailable.":"Lab GetUp client permission cancelled, stale or expired.";
            return false;
        }
        std::uintptr_t allowed{};std::uint8_t value{};
        if(!resolve_getup_asset(action.base,sample,getup_observation::allowed_guid,"animation/dingo/bool.gp.isgetupallowed",
            "Animation/Dingo/Bool.GP.IsGetUpAllowed",action.domain_hints[1],action.domain_cursors[1],status.asset_lookup_calls,allowed)||
           !common()) {observed.issue(AuditIssue::allowed_asset_unavailable);status.issue="Lab GetUp Allowed asset or canonical callback unavailable.";return false;}
        ++status.getter_calls;
        const bool allowed_read=native_get_at(action.base,allowed,sample.binding,value);
        if(!allowed_read||value!=1||
           !getup_asset_contract(action.base,allowed,getup_observation::allowed_guid)||!common()) {
            observed.issue(!allowed_read||value>1?AuditIssue::allowed_getter_unavailable:
                value==0?AuditIssue::allowed_false:AuditIssue::allowed_changed);
            status.issue="Lab GetUp fresh native IsGetUpAllowed is false, unavailable or changed.";return false;
        }
        return true;
    };
    if(evidence)evidence->called=true;
    observed.issue(AuditIssue::called);
    action.pulse.dispatch({sample.binding,sample.entity_handle,serial},status.started_ms,now,action.eligible,
        action.cancelled.load(std::memory_order_acquire),status.owner_changed,
        [&](std::uintptr_t& property) noexcept {
            if(!resolve_getup_asset(action.base,sample,getup_observation::getup_guid,"animation/dingo/bool.intent.wipeout.getup",
                "Animation/Dingo/Bool.Intent.WipeOut.GetUp",action.domain_hints[0],action.domain_cursors[0],status.asset_lookup_calls,property)||
                !getup_setter_contract(action.base,property)) {
                observed.issue(AuditIssue::getup_asset_unavailable);
                status.issue="Lab GetUp exact typed GetUp asset/setter unavailable.";return false;
            }
            resolved=property;return true;
        },qualify,
        [&](auto property,auto binding,std::uint8_t& value) noexcept {return native_get_at(action.base,property,binding,value);},
        [&](auto property,auto binding,std::uint8_t value) noexcept {
            // The final permission/getter qualification shares the setter's
            // native clock. A previous passive sample never authorizes entry.
            if(!qualify(value==0))return false;
            if(value) {status.allowed_before_action=true;status.allowed_serial=serial;status.allowed_native_time=time;}
            if(evidence)evidence->native_set_called=true;
            return native_set_at(action.base,property,binding,value);
        });
    const auto phase=action.pulse.phase.load(std::memory_order_acquire);
    if(phase==detail::PulsePhase::reset||phase==detail::PulsePhase::preexisting_true||phase==detail::PulsePhase::owner_lost)
        action.active.store(false,std::memory_order_release);
}
bool audit_owner_matches(std::uintptr_t base,const detail::Owner& owner) noexcept {
    namespace engine=game::build::v20260929::engine;
    const getup_callback_audit::Types types{engine::client_vtable,engine::skater_entity_vtable,
        engine::context_player_manager_offset,engine::local_player_manager_vtable,engine::local_player_vtable,
        engine::skater_component_vtable,game::build::v20260929::no_bail::bail_core_vtable};
    return getup_callback_audit::owner_matches(base,owner,types,&read);
}
// Audit lookup is hint-only: at most two exact name queries for each asset.
// No domain scan and no native pointer persisted from an earlier callback.
bool resolve_audit_asset(const detail::Snapshot& sample,const std::array<std::uint8_t,16>& guid,
    const char* lower,const char* upper,std::uint16_t hint,std::uintptr_t& property) noexcept {
    for(const auto* value:{&sample.input,&sample.mirror})
        if(value->typed&&value->guid==guid&&getup_asset_contract(audit.base,value->property,guid)) {
            property=value->property;return true;
        }
    if(hint>=getup_observation::domain_count||reinterpret_cast<std::uintptr_t>(game::native_data().find_asset)!=audit.base+asset_find_rva||
        !matches(audit.base+asset_find_rva,contracts::asset_find))return false;
    const auto slot=audit.base+domain_owners_rva+std::uintptr_t{hint}*8;std::uintptr_t owner{},again{},asset{};
    if(!detail::get(slot,owner,&read)||!owner)return false;
    for(const auto* name:{lower,upper}) {
        if(!audit.window.live(GetTickCount64()))return false;
        ++audit.status.asset_lookup_calls;
        if(!native_find(hint,name,asset)||!detail::get(slot,again,&read)||again!=owner)return false;
        if(asset) {
            if(!detail::span(asset,0x80)||!getup_asset_contract(audit.base,asset+0x60,guid))return false;
            property=asset+0x60;return true;
        }
    }
    return false;
}
void observe_callback_audit(const detail::Snapshot& sample,std::uint64_t now,const getup_callback_audit::Dispatch& dispatch) noexcept {
    using Issue=getup_callback_audit::Issue;
    audit.callbacks.fetch_add(1,std::memory_order_relaxed);
    AuditExclusive lock;
    if(!lock.held){audit.contention_drops.fetch_add(1,std::memory_order_relaxed);return;}
    // capture's timestamp predates the action observer; never use it to
    // authorize optional finder/getter work after the audit's real deadline.
    now=GetTickCount64();
    if(!audit.active.load(std::memory_order_acquire)||audit.cancelled.load(std::memory_order_acquire)||!audit.window.live(now)) {
        ++audit.status.rejected;return;
    }
    if(!sample.update_after_coherent||!detail::control_matches(audit.base,sample,audit.owner,&read)) {
        ++audit.status.rejected;return;
    }
    ++audit.status.candidates;
    if(audit.count>=audit.rows.size()){++audit.status.overflow_drops;return;}
    auto& row=audit.rows[audit.count++];row={};row.ordinal=audit.status.candidates;row.observed_ms=now;row.dispatch=dispatch;
    row.entity_handle=sample.entity_handle;
    row.event_params=sample.event_params;row.node=sample.node;row.root=sample.root;row.data=sample.data;
    row.binding=sample.binding;row.context_wrapper=sample.context_wrapper;row.update_after=sample.update_after;
    row.data_guid=sample.data_guid;row.data_guid_available=sample.data_coherent;
    row.input_guid=sample.input.guid;row.input_guid_available=sample.input.coherent;
    row.mirror_guid=sample.mirror.guid;row.mirror_guid_available=sample.mirror.coherent;
    row.getup.guid=getup_observation::getup_guid;row.is_getup_allowed.guid=getup_observation::allowed_guid;
    if(sample.entity_handle!=audit.status.entity_handle) {
        row.issue=Issue::owner_changed;audit.status.owner_changed=true;audit.active.store(false,std::memory_order_release);return;
    }
    if(!(row.clock_available=read_native_clock_try(audit.owner.entity,audit.owner.core,row.serial,row.native_time))) {
        row.issue=Issue::clock_unavailable;return;
    }
    if(!getup_action::epoch({audit.status.requested_serial,audit.status.requested_native_time},{row.serial,row.native_time})) {
        row.issue=Issue::clock_changed;return;
    }
    const auto common=[&]() noexcept {
        if(audit.cancelled.load(std::memory_order_acquire)||!audit.window.live(GetTickCount64()))return false;
        const auto current=detail::inspect(audit.base,detail::Kind::begin,sample.event_params,0,0,&read);
        if(current.node!=sample.node||current.root!=sample.root||current.data!=sample.data||
           current.data_guid!=sample.data_guid||current.binding!=sample.binding||current.context_wrapper!=sample.context_wrapper||
           current.entity_handle!=audit.status.entity_handle||!detail::control_matches(audit.base,current,audit.owner,&read))return false;
        std::uint64_t serial{};double time{};
        return audit_owner_matches(audit.base,audit.owner)&&read_native_clock_try(audit.owner.entity,audit.owner.core,serial,time)&&
            getup_action::exact({row.serial,row.native_time},{serial,time});
    };
    if(!(row.owner_verified=common())) {row.issue=Issue::node_changed;return;}
    row.issue=Issue::measured;
    const auto measure=[&](GetupBoolObservation& value,const std::array<std::uint8_t,16>& guid,
        const char* lower,const char* upper,std::uint16_t hint) noexcept {
        std::uintptr_t property{};
        if(!resolve_audit_asset(sample,guid,lower,upper,hint,property)||!common()){row.issue=Issue::asset_unavailable;return;}
        value.asset_verified=true;std::uint8_t result{};
        if(!audit.window.live(GetTickCount64())) {row.issue=Issue::window_closed;return;}
        ++audit.status.getter_calls;
        if(!native_get_at(audit.base,property,sample.binding,result)){row.issue=Issue::getter_unavailable;return;}
        if(result>1){row.issue=Issue::non_bool_value;return;}
        if(!getup_asset_contract(audit.base,property,guid)||!common()){row.issue=Issue::node_changed;return;}
        value.getter_available=true;value.value=result!=0; // slot presence deliberately remains unknown
    };
    measure(row.getup,getup_observation::getup_guid,"animation/dingo/bool.intent.wipeout.getup",
        "Animation/Dingo/Bool.Intent.WipeOut.GetUp",audit.hints[0]);
    measure(row.is_getup_allowed,getup_observation::allowed_guid,"animation/dingo/bool.gp.isgetupallowed",
        "Animation/Dingo/Bool.GP.IsGetUpAllowed",audit.hints[1]);
    if(!common()) {row.owner_verified=false;row.getup.getter_available=false;row.is_getup_allowed.getter_available=false;row.issue=Issue::node_changed;}
}
void activate_probe(const detail::Snapshot& sample,std::uint64_t now,bool eligible) noexcept {
    std::uint64_t serial{};double time{};
    if(!read_native_clock_try(probe.owner.entity,probe.owner.core,serial,time)||!std::isfinite(time))return;
    std::uintptr_t resolved{};
    const auto qualify=[&](bool cleanup) noexcept {
        if(probe.owner_changed.load(std::memory_order_acquire)||(!cleanup&&probe.cancelled.load(std::memory_order_acquire))||
           !detail::control_matches(probe.base,sample,probe.owner,&read))return false;
        if(!cleanup){
            const auto current_ms=GetTickCount64();
            const auto sequence=probe.eligibility_sequence.load();
            const bool allowed=probe.eligible.load();const auto observed=probe.eligibility_observed.load();
            if(current_ms<probe.started||current_ms-probe.started>=probe_arm_ms||(sequence&1)||!allowed||
               current_ms<observed||current_ms-observed>250||probe.eligibility_sequence.load()!=sequence)return false;
        }
        // Re-root through the current callback argument again, rather than
        // treating a previously inspected node or binding as a lifetime lease.
        const auto fresh=detail::inspect(probe.base,detail::Kind::begin,sample.event_params,0,0,&read);
        if(fresh.node!=sample.node||fresh.root!=sample.root||fresh.data!=sample.data||fresh.binding!=sample.binding||
           fresh.context_wrapper!=sample.context_wrapper||fresh.entity_handle!=sample.entity_handle||
           !detail::control_matches(probe.base,fresh,probe.owner,&read))return false;
        std::uint64_t current_serial{};double current_time{};
        if(!read_native_clock_try(probe.owner.entity,probe.owner.core,current_serial,current_time)||
           current_serial!=serial||!std::isfinite(current_time))return false;
        NoBailSkater current;bool held{};
        return inspect_no_bail_owner(probe.owner.client,probe.owner.entity,current,held)&&
            (cleanup||!held)&&owner_matches(current,probe.owner,probe.base)&&(!resolved||pulse_contract(resolved));
    };
    probe.pulse.dispatch({sample.binding,sample.entity_handle,serial},probe.started,now,eligible,
        probe.cancelled.load(std::memory_order_acquire),probe.owner_changed.load(std::memory_order_acquire),
        [&](std::uintptr_t& property) noexcept {
            if(!resolve_wipeout(sample,property))return false;
            resolved=property;probe.resolved_property.store(property);return true;
        },qualify,
        [](auto property,auto binding,std::uint8_t& value) noexcept {return native_get(property,binding,value);},
        [](auto property,auto binding,std::uint8_t value) noexcept {return native_set(property,binding,value);});
}
Json guid_json(const std::array<std::uint8_t,16>& guid){
    Json out=Json::array();for(const auto value:guid)out.push_back(value);return out;
}
Json asset_json(const detail::Asset& a){
    return Json{{"property",a.property},{"asset",a.asset},{"guid_bytes",guid_json(a.guid)},
        {"readable",a.readable},{"coherent",a.coherent},{"typed",a.typed},{"request_wipeout",a.request_wipeout}};
}
}
// Caller owns poll_mutex; native callbacks never acquire it.
bool ensure_hooks_locked(std::uintptr_t base) noexcept {
    if(shared.attempted)return shared.status.initialized&&shared.base==base;shared.attempted=true;
    try {
        if(!supported_build::running_image_matches(base)||!detail::span(base,0x1039c71)){
            shared.status.issue="unsupported_image";return false;
        }
        if(!matches(base+begin_rva,contracts::begin)||!matches(base+update_rva,contracts::update)||!matches(base+end_rva,contracts::end)){
            shared.status.issue="adapter_contract_rejected";return false;
        }
        const auto init=hook_initialize();
        if(init!=HookOk&&init!=HookAlreadyInitialized){shared.status.issue="hook_initialize_failed";return false;}
        auto* begin_target=reinterpret_cast<void*>(base+begin_rva);auto* update_target=reinterpret_cast<void*>(base+update_rva);
        auto* end_target=reinterpret_cast<void*>(base+end_rva);
        void* begin_relay{};void* update_relay{};void* end_relay{};
        if(hook_prepare(begin_target,reinterpret_cast<void*>(&begin_hook),&begin_relay)!=HookOk||!begin_relay){
            shared.status.issue="begin_prepare_failed";return false;
        }
        if(hook_prepare(update_target,reinterpret_cast<void*>(&update_hook),&update_relay)!=HookOk||!update_relay){
            (void)hook_remove(begin_target);shared.status.issue="update_prepare_failed";return false;
        }
        if(hook_prepare(end_target,reinterpret_cast<void*>(&end_hook),&end_relay)!=HookOk||!end_relay){
            (void)hook_remove(begin_target);(void)hook_remove(update_target);shared.status.issue="end_prepare_failed";return false;
        }
        shared.base=base;shared.started=GetTickCount64();
        shared.original_begin.store(reinterpret_cast<Begin>(begin_relay),std::memory_order_release);
        shared.original_update.store(reinterpret_cast<Update>(update_relay),std::memory_order_release);
        shared.original_end.store(reinterpret_cast<End>(end_relay),std::memory_order_release);
        if(hook_enable(begin_target)!=HookOk||hook_enable(update_target)!=HookOk||hook_enable(end_target)!=HookOk){
            shared.capturing.store(false,std::memory_order_release);(void)hook_disable(begin_target);(void)hook_disable(update_target);
            (void)hook_disable(end_target);
            // A callback could already be using a published relay. Keep all
            // prepared permanent relays for that in-flight forwarding path.
            shared.status.issue="adapter_enable_failed";return false;
        }
        shared.status.initialized=true;shared.status.issue="adapters_ready";return true;
    }catch(...){shared.capturing.store(false,std::memory_order_release);shared.status.issue="initialize_failed";return false;}
}
bool initialize(std::uintptr_t base) noexcept {
    ErrorGuard error;std::lock_guard lock(shared.poll_mutex);
    try {
        std::filesystem::path root;if(!opted_root(root))return false;
        if(!ensure_hooks_locked(base))return false;
        if(shared.status.requested)return true;
        shared.root=root;shared.status.requested=true;
        shared.requested.store(true,std::memory_order_release);
        shared.capturing.store(true,std::memory_order_release);shared.status.capturing=true;
        shared.status.issue="capturing_read_only";return true;
    }catch(...){return false;}
}
bool enable_riding_falls(std::uintptr_t base) noexcept {
    ErrorGuard error;std::lock_guard lock(shared.poll_mutex);
    try {
        std::filesystem::path root;const auto mp=multiplayer::model();
        if(!riding_root(root)||mp.active||mp.lobby_joining||!ensure_hooks_locked(base))return false;
        shared.riding_root=root;shared.riding_enabled.store(true,std::memory_order_release);
        shared.requested.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
bool requested() noexcept {return shared.requested.load(std::memory_order_acquire);}
bool arm_bail_probe(std::uintptr_t base,const NativeFrame& frame) noexcept {
    ErrorGuard error;
    try {
        if(!frame.available||!frame.offboard)return false;
        // Hook installation must never happen while the bail-state mutex or a
        // native callback lifecycle lock is held.
        if(!shared.riding_enabled.load(std::memory_order_acquire)) (void)enable_riding_falls(base);
        std::lock_guard lock(shared.poll_mutex);std::filesystem::path current_root;
        const auto now=GetTickCount64();
        const bool diagnostic=shared.status.requested&&opted_root(current_root)&&current_root==shared.root;
        const bool riding=!diagnostic&&shared.riding_enabled.load(std::memory_order_acquire)&&
            riding_root(current_root)&&current_root==shared.riding_root;
        const auto mp=multiplayer::model();
        if(!shared.status.initialized||base!=shared.base||(!diagnostic&&!riding)||mp.active||mp.lobby_joining)return false;
        const detail::Owner owner{frame.client,frame.entity,frame.core,frame.context,frame.rig,true};
        NoBailSkater current;bool held{};
        if(!inspect_no_bail_owner(owner.client,owner.entity,current,held)||held||!owner_matches(current,owner,base))return false;
        ProbeExclusive lifecycle;if(!lifecycle.held)return false;
        const bool finished=!probe.initialized.load(std::memory_order_acquire)||
            probe.cancelled.load(std::memory_order_acquire)||probe.owner_changed.load(std::memory_order_acquire)||
            detail::probe_phase(probe.started,now)==detail::ProbePhase::closed;
        if(!probe.pulse.rearm(finished))return false;
        probe.active.store(false,std::memory_order_release);
        probe.base=base;probe.owner=owner;probe.started=now;probe.riding=riding;
        probe.activation_enabled=riding||activation_flag();
        probe.callbacks.store(0);probe.matches.store(0);probe.ends.store(0);probe.owner_changed.store(false);
        probe.anchor_ready.store(0);probe.anchor_node=probe.anchor_binding=0;
        probe.asset_lookups.store(0);probe.resolved_property.store(0);
        for(auto& reservation:probe.reserved)reservation.store(0);
        for(auto& slot:probe.slots)slot.ready.store(0);
        probe.cancelled.store(false,std::memory_order_relaxed);
        publish_probe_eligibility(now,true);
        probe.initialized.store(true,std::memory_order_release);
        probe.active.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
void cancel_bail_probe() noexcept {
    probe.cancelled.store(true,std::memory_order_release);
    // The call may already be entering. Keep its fresh-callback cleanup route
    // available; cancellation removes permission to start, not reset duty.
    if(!probe.activation_enabled)probe.active.store(false,std::memory_order_release);
}
BailProbeStatus bail_probe_status() noexcept {
    ErrorGuard error;BailProbeStatus out;
    ProbeShared lifecycle;if(!lifecycle.held){out.available=false;out.issue="Native intent lifecycle busy; retain prior status.";return out;}
    if(!probe.initialized.load(std::memory_order_acquire))return out;
    out.armed=true;out.started_ms=probe.started;out.generation=probe.pulse.generation.load();
    out.callbacks=probe.callbacks.load(std::memory_order_relaxed);out.matches=probe.matches.load(std::memory_order_relaxed);
    out.ends=probe.ends.load(std::memory_order_relaxed);out.context_verified=out.matches!=0;
    out.owner_changed=probe.owner_changed.load(std::memory_order_acquire);
    const auto& pulse=probe.pulse;
    out.activation_enabled=probe.activation_enabled;
    out.intent_written=pulse.written.load();out.intent_reset=pulse.reset_false.load()&&pulse.phase.load()==detail::PulsePhase::reset;
    out.owner_expired=out.owner_changed&&pulse.true_calls.load()!=0&&!out.intent_reset;
    out.reset_pending=pulse.pending()&&!out.owner_changed;
    out.prior_false=pulse.prior_available.load()&&pulse.prior_false.load();out.true_returned=pulse.true_returned.load();
    out.true_readback=pulse.true_readback_available.load()&&pulse.true_readback.load();
    out.reset_readback=pulse.reset_readback_available.load()&&pulse.reset_false.load();out.native_cleared=pulse.native_cleared.load();
    out.true_serial=pulse.true_serial.load();out.reset_serial=pulse.reset_serial.load();
    out.true_calls=pulse.true_calls.load();out.reset_calls=pulse.reset_calls.load();out.getter_calls=pulse.getters.load();
    out.asset_lookup_calls=probe.asset_lookups.load();
    out.completed=!out.reset_pending&&!pulse.busy.test(std::memory_order_acquire)&&
        (probe.cancelled.load(std::memory_order_acquire)||out.owner_changed||
         detail::probe_phase(probe.started,GetTickCount64())==detail::ProbePhase::closed);
    if(out.completed)probe.active.store(false,std::memory_order_release);
    out.issue=out.owner_expired?"Intent reset unavailable after canonical owner replacement.":out.owner_changed?"Native intent probe owner changed.":
        out.reset_pending?"Native intent pulse entered; fresh next-clock reset pending.":out.intent_reset?
        "Native RequestWipeout pulse reset verified; observing native fall.":
        pulse.phase.load()==detail::PulsePhase::preexisting_true?"Native RequestWipeout was already true; SDK did not overwrite it.":
        probe.cancelled.load(std::memory_order_acquire)?
        "Native intent probe cancelled.":out.context_verified?
        (out.activation_enabled?"Fresh native Bool binding captured; typed pulse prerequisites unavailable.":"Fresh native Bool binding captured (no write)."):out.completed?
        "No canonical Bool callback captured around impact (no write).":"Awaiting fresh canonical Bool callback (no write).";
    return out;
}
bool begin_getup_observation(std::uintptr_t base,const NativeFrame& frame) noexcept {
    ErrorGuard error;
    try {
        if(!frame.available||!frame.offboard||!frame.timing_available)return false;
        if(!shared.riding_enabled.load(std::memory_order_acquire)&&!enable_riding_falls(base))return false;
        std::lock_guard poll_lock(shared.poll_mutex);std::filesystem::path root;
        const auto mp=multiplayer::model();
        if(!getup_root(root)||root!=shared.riding_root||mp.active||mp.lobby_joining||
           !shared.status.initialized||shared.base!=base)return false;
        const detail::Owner owner{frame.client,frame.entity,frame.core,frame.context,frame.rig,true};
        NoBailSkater current;bool held{};std::uint64_t serial{};double time{};
        if(!inspect_no_bail_owner(owner.client,owner.entity,current,held)||held||!owner_matches(current,owner,base)||
           !read_native_clock_try(owner.entity,owner.core,serial,time)||serial!=frame.serial||time!=frame.simulation_time)return false;
        GetupExclusive lock;if(!lock.held)return false;
        const auto cancel_revision=getup.cancel_revision.load(std::memory_order_acquire);
        if(getup.status.generation&&getup.owner.entity==owner.entity&&getup.owner.core==owner.core&&
           serial<=getup.window.start_serial)return false;
        const auto now=GetTickCount64();getup_observation::Window window;
        if(!window.begin(serial,time,now))return false;
        const auto generation=getup.status.generation+1;if(!generation)return false;
        getup.active.store(false,std::memory_order_release);
        getup.base=base;getup.owner=owner;getup.window=window;getup.domain_hints={0xffff,0xffff};getup.domain_cursors={};
        getup.status={};getup.status.active=true;getup.status.generation=generation;getup.status.entity=owner.entity;
        getup.status.getup.guid=getup_observation::getup_guid;getup.status.is_getup_allowed.guid=getup_observation::allowed_guid;
        getup.status.issue="Awaiting a fresh canonical Bool callback for passive get-up observation.";
        getup.cancelled.store(false,std::memory_order_release);
        getup.contention_drops.store(0,std::memory_order_relaxed);getup.next_attempt_ms.store(0,std::memory_order_relaxed);
        getup.eligible=true;getup.eligibility_observed=now;shared.getup_root=root;
        getup.deadline_ms.store(now+getup_observation::wall_limit_ms,std::memory_order_release);
        if(getup.cancel_revision.load(std::memory_order_acquire)!=cancel_revision){
            getup.cancelled.store(true,std::memory_order_release);close_getup("Passive get-up begin cancelled concurrently.");return false;
        }
        getup.active.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
GetupObservationStatus getup_observation_status() noexcept {
    ErrorGuard error;GetupShared lock;
    if(!lock.held){GetupObservationStatus out;out.available=false;out.issue="Passive get-up lifecycle busy; no new snapshot.";return out;}
    auto out=getup.status;out.contention_drops=getup.contention_drops.load(std::memory_order_relaxed);
    if(out.generation&&getup.cancelled.load(std::memory_order_acquire)){
        out.active=false;out.completed=true;out.issue="Passive get-up observation cancelled.";return out;
    }
    if(out.active&&GetTickCount64()>=getup.deadline_ms.load(std::memory_order_acquire)){
        out.active=false;out.completed=true;out.issue="Passive get-up observation reached its wall deadline.";
    }
    return out;
}
GetupObservationTrace getup_observation_trace() noexcept {
    ErrorGuard error;GetupObservationTrace out;GetupShared lock;
    if(!lock.held){out.available=false;out.status.available=false;out.status.issue="Passive get-up lifecycle busy; no trace copy.";return out;}
    out.status=getup.status;out.status.contention_drops=getup.contention_drops.load(std::memory_order_relaxed);
    if(out.status.generation&&getup.cancelled.load(std::memory_order_acquire)){
        out.status.active=false;out.status.completed=true;out.status.issue="Passive get-up observation cancelled.";
    }else if(out.status.active&&GetTickCount64()>=getup.deadline_ms.load(std::memory_order_acquire)){
        out.status.active=false;out.status.completed=true;out.status.issue="Passive get-up observation reached its wall deadline.";
    }
    if(getup.window.samples>out.samples.size()){out.available=false;out.status.available=false;return out;}
    for(unsigned i=0;i<getup.window.samples;++i){
        const auto& row=getup.rows[i];
        if(row.generation!=out.status.generation||row.entity!=out.status.entity||row.entity_handle!=out.status.entity_handle||
           row.serial<getup.window.start_serial||row.simulation_time<getup.window.start_time){
            out.available=false;out.status.available=false;out.count=0;out.status.issue="Passive get-up trace generation/epoch differs.";return out;
        }
        out.samples[out.count++]=row;
    }
    return out;
}
void cancel_getup_observation() noexcept {
    ErrorGuard error;getup.cancel_revision.fetch_add(1,std::memory_order_acq_rel);
    getup.cancelled.store(true,std::memory_order_release);getup.active.store(false,std::memory_order_release);
    GetupExclusive lock;if(!lock.held)return;
    if(getup.status.generation)close_getup("Passive get-up observation cancelled.");
}
bool arm_getup_action(std::uintptr_t base,const NativeFrame& frame,const GetupActionRequest& request) noexcept {
    ErrorGuard error;
    try {
        if(!frame.available||!frame.offboard||frame.standable||!frame.timing_available||!request.fall_request_id||
           !request.fall_generation||!detail::span(request.world,8)||!detail::span(request.native_state,8)||
           request.body_index>=1048576||!request.body_token)return false;
        std::lock_guard poll_lock(shared.poll_mutex);std::filesystem::path root;const auto mp=multiplayer::model();
        if(!action_root(root)||root!=shared.riding_root||mp.active||mp.lobby_joining||
           !shared.status.initialized||base!=shared.base)return false;
        const detail::Owner owner{frame.client,frame.entity,frame.core,frame.context,frame.rig,true};
        NoBailSkater current;bool held{};std::uint64_t serial{};double time{};
        const auto motion=native_motion_lease_status();
        if(!inspect_no_bail_owner(owner.client,owner.entity,current,held)||held||!owner_matches(current,owner,base)||
           !known_getup_ragdoll(base,owner)||!motion.available||motion.active||
           !released_getup_body(owner,request)||
           overlay::detail::bmx_input_capture_active()||overlay::detail::bmx_interaction_capture_active()||
           !read_native_clock_try(owner.entity,owner.core,serial,time)||
           !getup_action::exact({frame.serial,frame.simulation_time},{serial,time}))return false;
        // The passive observer has already captured a full native handle.
        // Re-resolve it now; an entity pointer alone is not an epoch identity.
        GetupShared passive;if(!passive.held||!getup.status.generation||!getup.status.entity_handle||
            getup.owner.client!=owner.client||getup.owner.entity!=owner.entity||getup.owner.core!=owner.core||
            getup.owner.context!=owner.context||getup.owner.rig!=owner.rig)return false;
        const auto handle=detail::inspect_handle(base,getup.status.entity_handle,&read);
        if(!handle.coherent||handle.entity!=owner.entity)return false;
        ActionExclusive lock;if(!lock.held||action.status.armed)return false;
        const auto revision=action.cancel_revision.load(std::memory_order_acquire);
        if(!action.pulse.rearm(true))return false;
        action.base=base;action.owner=owner;action.status={};action.status.armed=true;
        auto& status=action.status;status.generation=action.pulse.generation.load();status.request=request;
        status.client=owner.client;status.entity=owner.entity;status.core=owner.core;status.context=owner.context;status.rig=owner.rig;
        status.entity_handle=handle.value;status.requested_serial=serial;status.requested_native_time=time;
        status.started_ms=GetTickCount64();status.issue="Lab GetUp action armed; awaiting a fresh qualified callback.";
        action.domain_hints=getup.domain_hints;action.domain_cursors={};
        action.eligible=true;action.eligibility_observed=status.started_ms;shared.action_root=root;
        action.cancelled.store(false,std::memory_order_release);
        if(action.cancel_revision.load(std::memory_order_acquire)!=revision) {
            action.cancelled.store(true,std::memory_order_release);status.issue="Lab GetUp action arm cancelled concurrently.";return false;
        }
        action.active.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
GetupActionStatus getup_action_status() noexcept {
    ErrorGuard error;ActionShared lock;
    if(!lock.held) {GetupActionStatus out;out.available=false;out.issue="Lab GetUp lifecycle busy; retain prior status.";return out;}
    auto out=action.status;if(!out.armed)return out;
    const auto& pulse=action.pulse;const auto phase=pulse.phase.load(std::memory_order_acquire);
    out.cancelled=action.cancelled.load(std::memory_order_acquire);
    const auto now=GetTickCount64();
    out.prior_false=pulse.prior_available.load()&&pulse.prior_false.load();out.true_returned=pulse.true_returned.load();
    out.true_readback=pulse.true_readback_available.load()&&pulse.true_readback.load();out.intent_written=pulse.written.load();
    out.intent_reset=phase==detail::PulsePhase::reset&&pulse.reset_false.load();
    out.reset_readback=pulse.reset_readback_available.load()&&pulse.reset_false.load();
    out.reset_pending=pulse.pending();out.native_cleared=pulse.native_cleared.load();
    out.true_serial=pulse.true_serial.load();out.reset_serial=pulse.reset_serial.load();
    out.true_calls=pulse.true_calls.load();out.reset_calls=pulse.reset_calls.load();out.getter_calls+=pulse.getters.load();
    out.expired=!out.true_calls&&(now<out.started_ms||now-out.started_ms>=getup_action::start_limit_ms);
    out.owner_expired=out.owner_changed&&out.true_calls&&!out.intent_reset;
    out.completed=!out.reset_pending&&!pulse.busy.test(std::memory_order_acquire)&&
        (out.intent_reset||phase==detail::PulsePhase::preexisting_true||out.cancelled||out.expired||out.owner_changed);
    out.issue=out.owner_expired?"Lab GetUp reset unavailable after canonical owner replacement.":
        out.owner_changed?"Lab GetUp canonical owner changed; no further native calls.":
        out.reset_pending?"Lab GetUp entered; fresh next-clock reset pending.":
        out.intent_reset?"Lab GetUp pulse reset verified; native recovery remains an independent observation.":
        phase==detail::PulsePhase::preexisting_true?"Native GetUp was already true; SDK left it untouched.":
        out.cancelled?"Lab GetUp start cancelled.":out.issue;
    return out;
}
void cancel_getup_action() noexcept {
    ErrorGuard error;action.cancel_revision.fetch_add(1,std::memory_order_acq_rel);
    action.cancelled.store(true,std::memory_order_release);
    // Do not remove the callback admission: an entered/faulted call still owes
    // a fresh logical reset even after normal ride or diagnostic shutdown.
}
bool begin_getup_callback_audit(std::uintptr_t base,const NativeFrame& frame,const GetupActionRequest& request) noexcept {
    ErrorGuard error;
    try {
        if(!frame.available||!frame.offboard||!frame.timing_available||!request.fall_request_id||!request.fall_generation)return false;
        std::lock_guard poll_lock(shared.poll_mutex);std::filesystem::path root;const auto mp=multiplayer::model();
        if(!action_root(root)||root!=shared.riding_root||mp.active||mp.lobby_joining||
           !shared.status.initialized||base!=shared.base)return false;
        const detail::Owner owner{frame.client,frame.entity,frame.core,frame.context,frame.rig,true};
        std::uint64_t serial{};double time{};
        if(!audit_owner_matches(base,owner)||!read_native_clock_try(owner.entity,owner.core,serial,time)||
            !getup_action::exact({frame.serial,frame.simulation_time},{serial,time}))return false;
        GetupShared passive;if(!passive.held||!getup.status.generation||!getup.status.entity_handle||
            getup.owner.client!=owner.client||getup.owner.entity!=owner.entity||getup.owner.core!=owner.core||
            getup.owner.context!=owner.context||getup.owner.rig!=owner.rig)return false;
        const auto handle=detail::inspect_handle(base,getup.status.entity_handle,&read);
        if(!handle.coherent||handle.entity!=owner.entity)return false;
        AuditExclusive lock;if(!lock.held||audit.window.armed)return false;
        const auto revision=audit.cancel_revision.load(std::memory_order_acquire);const auto now=GetTickCount64();
        if(!audit.window.begin(now))return false;
        audit.base=base;audit.owner=owner;audit.hints=getup.domain_hints;audit.count=0;
        audit.status={};audit.status.armed=true;audit.status.active=true;audit.status.generation=1;
        audit.status.client=owner.client;audit.status.entity=owner.entity;audit.status.core=owner.core;
        audit.status.context=owner.context;audit.status.rig=owner.rig;audit.status.entity_handle=handle.value;
        audit.status.requested_serial=serial;audit.status.requested_native_time=time;audit.status.started_ms=now;
        audit.status.deadline_ms=audit.window.deadline;audit.status.request=request;
        audit.callbacks.store(0);audit.contention_drops.store(0);audit.cancelled.store(false,std::memory_order_release);
        shared.audit_root=root;audit.deadline.store(audit.window.deadline,std::memory_order_release);
        if(audit.cancel_revision.load(std::memory_order_acquire)!=revision) {
            audit.cancelled.store(true,std::memory_order_release);return false;
        }
        audit.active.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
bool copy_getup_callback_audit_trace(GetupCallbackAuditTrace& out) noexcept {
    ErrorGuard error;out.available=true;out.status={};out.count=0;AuditShared lock;
    if(!lock.held){out.available=false;out.status.available=false;return false;}
    out.status=audit.status;out.status.callbacks=audit.callbacks.load(std::memory_order_relaxed);
    out.status.contention_drops=audit.contention_drops.load(std::memory_order_relaxed);
    out.status.cancelled=audit.cancelled.load(std::memory_order_acquire);
    out.status.active=out.status.armed&&audit.active.load(std::memory_order_acquire)&&!out.status.cancelled&&audit.window.live(GetTickCount64());
    out.status.completed=out.status.armed&&!out.status.active;
    if(audit.count>out.samples.size()){out.available=false;out.status.available=false;return false;}
    out.count=audit.count;
    for(unsigned i=0;i<out.count;++i)out.samples[i]=audit.rows[i];
    return true;
}
void cancel_getup_callback_audit() noexcept {
    ErrorGuard error;audit.cancel_revision.fetch_add(1,std::memory_order_acq_rel);
    audit.cancelled.store(true,std::memory_order_release);audit.active.store(false,std::memory_order_release);
}
Status poll(const NativeFrame& canonical,bool force_write) noexcept {
    ErrorGuard error;std::lock_guard lock(shared.poll_mutex);
    try {
        const auto now=GetTickCount64();
        const detail::Owner owner{canonical.client,canonical.entity,canonical.core,canonical.context,canonical.rig,canonical.available};
        if(audit.active.load(std::memory_order_acquire)) {
            std::filesystem::path current;const auto mp=multiplayer::model();
            const bool eligible=action_root(current)&&current==shared.audit_root&&!mp.active&&!mp.lobby_joining;
            AuditExclusive audit_lock;
            if(audit_lock.held&&audit.active.load(std::memory_order_acquire)) {
                const bool changed=owner.available&&(owner.client!=audit.owner.client||owner.entity!=audit.owner.entity||
                    owner.core!=audit.owner.core||owner.context!=audit.owner.context||owner.rig!=audit.owner.rig);
                if(changed)audit.status.owner_changed=true;
                if(!eligible)audit.cancelled.store(true,std::memory_order_release);
                if(!eligible||changed||!audit.window.live(now))audit.active.store(false,std::memory_order_release);
            }
        }
        if(action.active.load(std::memory_order_acquire)) {
            std::filesystem::path current;const auto mp=multiplayer::model();
            const bool eligible=action_root(current)&&current==shared.action_root&&!mp.active&&!mp.lobby_joining;
            ActionExclusive action_lock;
            if(action_lock.held&&action.active.load(std::memory_order_acquire)) {
                action.eligible=eligible&&owner.available;action.eligibility_observed=now;
                if(!eligible)action.cancelled.store(true,std::memory_order_release);
                if(owner.available&&(owner.client!=action.owner.client||owner.entity!=action.owner.entity||owner.core!=action.owner.core||
                    owner.context!=action.owner.context||owner.rig!=action.owner.rig)) {
                    action.status.owner_changed=true;action.active.store(false,std::memory_order_release);
                }else if(!action.pulse.pending()&&(action.cancelled.load(std::memory_order_acquire)||
                    now<action.status.started_ms||now-action.status.started_ms>=getup_action::start_limit_ms))
                    action.active.store(false,std::memory_order_release);
            }
        }
        if(getup.active.load(std::memory_order_acquire)){
            std::filesystem::path current;const auto mp=dingosdk::multiplayer::model();
            const bool eligible=getup_root(current)&&current==shared.getup_root&&!mp.active&&!mp.lobby_joining;
            GetupExclusive semantic_lock;
            if(semantic_lock.held&&getup.active.load(std::memory_order_acquire)){
                getup.eligible=eligible&&owner.available;getup.eligibility_observed=now;
                if(!eligible)close_getup("Passive get-up exact development eligibility ended.");
                else if(owner.available&&(owner.client!=getup.owner.client||owner.entity!=getup.owner.entity||
                    owner.core!=getup.owner.core||owner.context!=getup.owner.context||owner.rig!=getup.owner.rig))
                    close_getup("Passive get-up canonical rider changed.",true);
                else if(now<getup.window.started_ms||now-getup.window.started_ms>=getup_observation::wall_limit_ms)
                    close_getup("Passive get-up observation reached its wall deadline.");
            }
        }
        if(probe.active.load(std::memory_order_acquire)){
            const auto mp=dingosdk::multiplayer::model();std::filesystem::path current;
            const bool riding_ok=shared.riding_enabled.load(std::memory_order_acquire)&&riding_root(current)&&current==shared.riding_root;
            const bool diagnostic_ok=opted_root(current)&&current==shared.root;
            ProbeExclusive lifecycle;
            if(lifecycle.held&&probe.active.load(std::memory_order_acquire)){
                const bool eligible=(probe.riding?riding_ok:(diagnostic_ok&&(!probe.activation_enabled||activation_flag())))&&
                    !mp.active&&!mp.lobby_joining;
                publish_probe_eligibility(now,eligible);
                if(!eligible)probe.cancelled.store(true,std::memory_order_release);
                if(owner.available&&(owner.client!=probe.owner.client||owner.entity!=probe.owner.entity||owner.core!=probe.owner.core||
                   owner.context!=probe.owner.context||owner.rig!=probe.owner.rig)){
                    probe.owner_changed.store(true,std::memory_order_release);probe.active.store(false,std::memory_order_release);
                }
                if(!probe.pulse.pending()&&(probe.cancelled.load(std::memory_order_acquire)||
                   detail::probe_phase(probe.started,now)==detail::ProbePhase::closed))probe.active.store(false,std::memory_order_release);
            }
        }
        if(!shared.status.requested)return shared.status;
        if(!opted_flag()){
            shared.capturing.store(false,std::memory_order_release);shared.status.capturing=false;
            shared.status.issue="opt_in_or_profile_changed";return shared.status;
        }
        shared.owner_sequence.fetch_add(1);
        shared.owner_entity.store(owner.available?owner.entity:0);shared.owner_observed.store(now);
        shared.owner_sequence.fetch_add(1);
        // Process only newly published or still-young unmatched rows before the
        // file timer, so a1Hz report interval cannot miss a250ms owner window.
        for(unsigned i=0;i<shared.slots.size();++i){
            const auto& slot=shared.slots[i];if(shared.correlation_done[i]||!slot.ready.load(std::memory_order_acquire))continue;
            detail::advance_correlation(shared.correlations[i],shared.correlation_done[i],shared.base,
                slot.row.snapshot,slot.row.observed_ms,now,owner,&read);
        }
        if(!force_write&&shared.last_poll&&now>=shared.last_poll&&now-shared.last_poll<1000)return shared.status;
        shared.last_poll=now;std::filesystem::path current;
        if(!opted_root(current)||current!=shared.root){
            shared.capturing.store(false,std::memory_order_release);shared.status.capturing=false;
            shared.status.issue="opt_in_or_profile_changed";return shared.status;
        }
        if(now<shared.started||now-shared.started>=capture_ms)shared.capturing.store(false,std::memory_order_release);
        shared.status.capturing=shared.capturing.load(std::memory_order_acquire);
        shared.status.begin_forwarded=shared.forwarded[0].load(std::memory_order_relaxed);
        shared.status.update_forwarded=shared.forwarded[1].load(std::memory_order_relaxed);
        shared.status.filtered=shared.filtered[0].load(std::memory_order_relaxed)+shared.filtered[1].load(std::memory_order_relaxed);
        shared.status.unreadable=shared.unreadable[0].load(std::memory_order_relaxed)+shared.unreadable[1].load(std::memory_order_relaxed);
        shared.status.after_capacity=shared.after_capacity[0].load(std::memory_order_relaxed)+shared.after_capacity[1].load(std::memory_order_relaxed);
        Json rows=Json::array();std::uint64_t count{},matched{};
        for(unsigned i=0;i<shared.slots.size();++i){
            const auto& slot=shared.slots[i];if(!slot.ready.load(std::memory_order_acquire))continue;
            const auto& r=slot.row;const auto& s=r.snapshot;const auto& c=shared.correlations[i];++count;if(c.entity_matches)++matched;
            rows.push_back(Json{{"kind",s.kind==detail::Kind::begin?"begin":"update"},{"observed_ms",r.observed_ms},
                {"elapsed_ms",r.observed_ms-shared.started},{"thread_id",r.thread_id},
                {"owner_ready_bank",r.owner_ready_bank},{"owner_hint_entity",r.owner_hint.entity},
                {"owner_hint_observed_ms",r.owner_hint.observed_ms},{"owner_hint_coherent",r.owner_hint.coherent},
                {"arguments",Json::array({r.arguments[0],r.arguments[1],r.arguments[2],r.arguments[3]})},
                {"event_params",s.event_params},{"node",s.node},{"root",s.root},{"authored_data",s.data},
                {"authored_guid_bytes",guid_json(s.data_guid)},{"authored_wipeout_guid",s.authored_wipeout},
                {"context_wrapper",s.context_wrapper},{"binding",s.binding},{"begin_context_argument",s.begin_context_argument},
                {"begin_context_readable",s.begin_context_readable},{"entity_handle",s.entity_handle},{"root_handle",s.root_handle},
                {"node_coherent",s.node_coherent},{"root_coherent",s.root_coherent},{"data_coherent",s.data_coherent},
                {"reset_on_end",s.reset[0]},{"reset_to_default",s.reset[1]},
                {"update_values_observed_after",Json::array({s.update_after[0],s.update_after[1]})},{"update_after_coherent",s.update_after_coherent},
                {"input",asset_json(s.input)},{"mirror",asset_json(s.mirror)},
                {"handle_manager",s.handle.manager},{"handle_table",s.handle.table},{"handle_slot",s.handle.slot},
                {"handle_mask",s.handle.mask},{"handle_index",s.handle.index},{"handle_entity",s.handle.entity},{"handle_coherent",s.handle.coherent},
                {"canonical_correlation_available",c.available},{"canonical_age_ms",c.age_ms},
                {"full_handle_still_maps",c.full_handle_still_maps},{"canonical_entity_match",c.entity_matches},
                {"canonical_client",c.canonical.client},{"canonical_entity",c.canonical.entity},{"canonical_core",c.canonical.core},
                {"canonical_context",c.canonical.context},{"canonical_rig",c.canonical.rig}});
        }
        Json rejected=Json::array();
        for(unsigned i=0;i<shared.rejected.size();++i){
            const auto& slot=shared.rejected[i];if(!slot.ready.load(std::memory_order_acquire))continue;
            const auto& r=slot.row;const auto& s=r.snapshot;const auto& h=s.header;
            const bool relative=h.vtable_readable&&h.vtable>=shared.base;
            rejected.push_back(Json{{"kind",i<rejected_per_kind?"begin":"update"},{"observed_ms",r.observed_ms},
                {"elapsed_ms",r.observed_ms-shared.started},{"thread_id",r.thread_id},
                {"arguments",Json::array({r.arguments[0],r.arguments[1],r.arguments[2],r.arguments[3]})},
                {"event_params",r.arguments[0]},{"event_readable",h.event_readable},{"node",s.node},
                {"node_span_valid",h.node_span_valid},{"node_readable_120",h.node_readable_120},
                {"vtable_readable",h.vtable_readable},{"vtable",h.vtable},
                {"vtable_relative_to_base_available",relative},{"vtable_minus_image_base",relative?h.vtable-shared.base:0},
                {"expected_node_vtable_rva",detail::node_vtable_rva},{"node_type_matches",h.node_type_matches},
                {"raw_data_word",h.data_word},{"raw_root_word",h.root_word},
                {"data_vtable_readable",h.data_vtable_readable},{"data_vtable",h.data_vtable},
                {"expected_data_vtable_rva",detail::data_vtable_rva},{"data_coherent",s.data_coherent},
                {"authored_guid_bytes",guid_json(s.data_guid)},{"authored_wipeout_guid",s.authored_wipeout},
                {"input",asset_json(s.input)},{"mirror",asset_json(s.mirror)},
                {"node_rereadable_120",h.node_rereadable_120},{"node_equal",h.node_equal},
                {"event_rereadable",h.event_rereadable},{"event_equal",h.event_equal},
                {"stage",detail::stage_name(h.stage)}});
        }
        const auto probe_status=bail_probe_status();Json probe_rows=Json::array();
        for(const auto& slot:probe.slots){
            if(!slot.ready.load(std::memory_order_acquire))continue;
            const auto& r=slot.row;const auto& s=r.snapshot;
            probe_rows.push_back(Json{{"kind",r.end?"end":"update"},{"observed_ms",r.observed_ms},
                {"elapsed_from_arm_ms",r.observed_ms-probe.started},{"thread_id",r.thread_id},
                {"node",s.node},{"root",s.root},{"binding",s.binding},{"context_wrapper",s.context_wrapper},
                {"entity_handle",s.entity_handle},{"handle_entity",s.handle.entity},{"root_coherent",s.root_coherent},
                {"authored_guid_bytes",guid_json(s.data_guid)},{"input",asset_json(s.input)},{"mirror",asset_json(s.mirror)},
                {"update_after_coherent",s.update_after_coherent},
                {"update_values_observed_after",Json::array({s.update_after[0],s.update_after[1]})},
                {"reset_on_end",s.reset[0]},{"reset_to_default",s.reset[1]},
                {"clock_available",r.clock_available},{"native_serial",r.serial},{"native_simulation_time",r.simulation_time},
                {"anchor_matches",r.anchor_matches},{"after_arm_window",r.after_arm_window}});
        }
        const Json probe_report{{"status_available",probe_status.available},{"generation",probe_status.generation},
            {"armed",probe_status.armed},{"completed",probe_status.completed},
            {"context_verified",probe_status.context_verified},{"owner_changed",probe_status.owner_changed},
            {"issue",probe_status.issue},{"no_write",!probe_status.activation_enabled},{"activation_enabled",probe_status.activation_enabled},
            {"reset_contract_proven",probe_status.activation_enabled},
            {"intent_written",probe_status.intent_written},{"intent_reset",probe_status.intent_reset},
            {"intent_reset_pending",probe_status.reset_pending},{"intent_prior_false",probe_status.prior_false},
            {"intent_true_returned",probe_status.true_returned},{"intent_true_readback",probe_status.true_readback},
            {"intent_reset_readback",probe_status.reset_readback},{"intent_native_cleared",probe_status.native_cleared},
            {"intent_owner_expired",probe_status.owner_expired},{"intent_true_serial",probe_status.true_serial},
            {"intent_reset_serial",probe_status.reset_serial},{"intent_true_calls",probe_status.true_calls},
            {"intent_reset_calls",probe_status.reset_calls},{"intent_getter_calls",probe_status.getter_calls},
            {"pulse_stage",static_cast<unsigned>(probe.pulse.stage.load())},{"resolved_property",probe.resolved_property.load()},
            {"getter_rva",getter_rva},{"setter_rva",setter_rva},{"asset_find_rva",asset_find_rva},
            {"asset_lookup_calls",probe_status.asset_lookup_calls},
            {"intent_native_calls",probe_status.true_calls+probe_status.reset_calls},
            {"started_ms",probe_status.started_ms},{"callbacks",probe_status.callbacks},{"matches",probe_status.matches},
            {"anchor_end_callbacks",probe_status.ends},{"arm_limit_ms",probe_arm_ms},{"follow_limit_ms",probe_follow_ms},
            {"capacity_per_bank",probe_per_bank},{"bank_count",3},
            {"owner_client",probe_status.armed?probe.owner.client:0},{"owner_entity",probe_status.armed?probe.owner.entity:0},
            {"owner_core",probe_status.armed?probe.owner.core:0},{"owner_context",probe_status.armed?probe.owner.context:0},
            {"owner_rig",probe_status.armed?probe.owner.rig:0},{"rows",std::move(probe_rows)}};
        shared.status.samples=count;
        Json report{{"event","bmx_intent_observation"},{"process_id",GetCurrentProcessId()},
            {"read_only",!probe_status.activation_enabled},{"activation_implemented",true},{"initialized",shared.status.initialized},
            {"capturing",shared.status.capturing},{"completed",!shared.status.capturing},{"issue",shared.status.issue},
            {"source_sha256","fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9"},
            {"image_base",shared.base},{"begin_rva",begin_rva},{"update_rva",update_rva},
            {"capacity_per_kind_per_owner_bank",per_kind},{"bank_count",4},{"capture_wall_limit_ms",capture_ms},
            {"begin_forwarded",shared.status.begin_forwarded},{"update_forwarded",shared.status.update_forwarded},
            {"end_forwarded",shared.end_forwarded.load(std::memory_order_relaxed)},{"end_rva",end_rva},
            {"samples",count},{"filtered",shared.status.filtered},{"unreadable",shared.status.unreadable},
            {"events_after_capacity",shared.status.after_capacity},{"canonical_matches",matched},{"rows",std::move(rows)},
            {"rejected_capacity_per_kind",rejected_per_kind},{"rejected_header_samples",std::move(rejected)},
            {"impact_probe",probe_report}};
        std::ofstream file(shared.root/L"bmx-intent-observation.json",std::ios::binary|std::ios::trunc);
        const auto text=report.dump(2);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();
        if(!file){shared.status.issue="report_write_failed";return shared.status;}
        shared.status.written=true;
    }catch(...){shared.status.issue="report_failed";}
    return shared.status;
}
}
