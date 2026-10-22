#include "bmx_skeleton_phase_probe.h"
#include "Engine/Game/BMX/bmx_skeleton_phase_policy.h"
#include "Extension/BMX/bmx_native_motion.h"
#include "Extension/BMX/bmx_native_chassis.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <filesystem>
#include <fstream>

namespace dingosdk::bmx::skeleton_phase_probe {
namespace {
using detail::Owner;using detail::Clock;
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
struct Raw4 {bool available{};std::array<std::uint32_t,4> words{};};
struct Raw16 {bool available{};std::array<std::uint32_t,16> words{};};
struct Raw2 {bool available{};std::array<std::uint32_t,2> words{};};
struct TailDispatch {
    bool object_available{},object_bounds{},vtable_available{},vtable_image_range{},
        target_available{},target_image_range{},target_readable{};
    std::uintptr_t object{},vtable{},target{};
};
struct WorldPublication {
    bool available{}, owner_matched{}, entity_context_available{};
    std::uintptr_t client{},client_context{},world{},native_state{};
    std::uint64_t token{};
};
struct Snapshot {
    Owner first{},last{};Clock clock_before{},clock_after{};
    WorldPublication world_before{},world_after{};
    std::int64_t qpc_begin{},qpc_end{};
    bool owner_available{},final_owner_available{},qualified{},root_available{},flags_available{},
        physical_typed{},physical_record_owner{},physical_chain_stable{},physical_flags_available{};
    std::array<std::uint32_t,3> root_words{};
    std::uint32_t context_13cc{},physical_flags{};
    std::uintptr_t collection{},physical_rig{},parts{};
    Raw4 context_830{},context_7c0{},rig_57f0{};
    TailDispatch rig_4630;Raw2 rig_4638; // raw tail inputs; no dereference of4638
    Raw16 rig_4720{},rig_4760{},cached_record_23{},physical_record_23{};
};
struct ProducerRaw {Raw4 rig_5860{},rig_5870{};bool divisor_available{};std::uint32_t divisor{};};
struct ProducerSnapshot {Snapshot frame;ProducerRaw raw;bool component_proved{};};
struct ProducerSlot {
    std::atomic<bool> pre_ready{},post_ready{};
    std::uint64_t invocation{},serial{},mount_token{},frame_revision{};
    DWORD thread{};detail::ProducerKind kind{};bool serial_available{},rendering{};
    std::uintptr_t component{},entity{};std::uint8_t flags{};
    std::array<std::uint32_t,16> selected{};
};
struct ProducerShared {
    std::atomic<bool> enabled{},armed{},sealed{};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    detail::ProducerBudget budget; // all access under try-only busy
    std::array<ProducerSlot,detail::producer_records> slots;
    std::array<ProducerSnapshot,detail::producer_snapshots> snapshots;
    std::atomic<std::uint64_t> contention{},unavailable{},dropped{},bad_cookie{},owner_changed{},sequence{};
};
// Snapshot itself is unchanged: new raw lanes are only in these80 cells, never
// multiplied across the existing1024 skeleton halves. Account for ALL new
// static arrays, metadata, policy state and counters, not only raw payload.
static_assert(sizeof(ProducerShared)<=128*1024);
ProducerShared producer;
struct Slot {
    std::atomic<bool> pre_ready{},post_ready{};
    std::uint64_t invocation{};DWORD thread{};unsigned window{};
    float seconds{};bool incoming{},effective{};
    Snapshot before{},after{};
};
struct Shared {
    std::atomic<bool> enabled{},sealed{},report_requested{};
    std::atomic<unsigned> window{};
    std::array<std::atomic<bool>,3> armed{};
    std::array<std::atomic<std::uint64_t>,3> reserved{},dropped{},unavailable{};
    std::array<Slot,detail::capacity> slots;
    std::atomic_flag callback_busy=ATOMIC_FLAG_INIT;
    Owner pinned; // protected only by try-only callback_busy, never rebound
    std::array<std::uint64_t,3> began{};
    std::atomic<std::uint64_t> contention{},bad_cookie{},owner_changed{};
    bool image_sections_verified{}; // initialized before observer registration, then immutable
    std::uintptr_t base{};std::uint64_t process_creation{};std::filesystem::path root;
    LARGE_INTEGER frequency{};
    bool attempted{}; // bootstrap thread only
};
// Static construction occurs before registration. Callback does not allocate.
Shared shared;
Owner owner(const NoBailSkater& x) noexcept {return {x.base,x.entity,x.core,x.context,x.rig};}
bool opted_flag() noexcept {
    wchar_t flag[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_HANDOFF_PHASE_PROBE",flag,2)==1&&flag[0]==L'1'&&user_data::development_requested();
}
bool opted_root(std::filesystem::path& root) {
    if(!opted_flag())return false;
    const auto requested=user_data::override_root();
    if(!requested||_wcsicmp(requested->filename().c_str(),L"bmx-user-data-chassis-probe")!=0)return false;
    root=*requested;return true;
}
template<class T> bool peek(std::uintptr_t p,std::uintptr_t offset,T& out) noexcept {
    if(p<0x10000||offset>memory::highest_user_address-p)return false;
    return memory::peek(p+offset,out);
}
std::uintptr_t pointer(std::uintptr_t p,std::uintptr_t offset=0) noexcept {
    std::uintptr_t out{};
    return peek(p,offset,out)&&out>=0x10000&&out<=memory::highest_user_address-0x6000&&!(out&7)?out:0;
}
Raw4 raw4(std::uintptr_t p,std::uintptr_t offset) noexcept {Raw4 out;out.available=peek(p,offset,out.words);return out;}
Raw16 raw16(std::uintptr_t p,std::uintptr_t offset) noexcept {Raw16 out;out.available=peek(p,offset,out.words);return out;}
bool source_sections_match(std::uintptr_t base) noexcept {
    // Bootstrap only, after full supported file identity. No section parsing or
    // allocation occurs in the callback. Compare all source bounds/permissions.
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<IMAGE_SECTION_HEADER,detail::image_sections.size()> sections{};
    if(!memory::peek(base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew!=0x80||
       !memory::peek(base+0x80,nt)||nt.Signature!=IMAGE_NT_SIGNATURE||
       nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.FileHeader.NumberOfSections!=sections.size()||
       nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64)||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
       nt.OptionalHeader.SizeOfImage!=supported_build::game_image_size||
       !memory::peek(base+0x80+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+sizeof(IMAGE_OPTIONAL_HEADER64),sections))return false;
    for(std::size_t i=0;i<sections.size();++i) {
        const auto& source=detail::image_sections[i];const auto& actual=sections[i];
        if(actual.VirtualAddress!=source.rva||actual.Misc.VirtualSize!=source.size||actual.Characteristics!=source.flags)return false;
    }
    return true;
}
TailDispatch tail_dispatch(std::uintptr_t rig) noexcept {
    TailDispatch out;
    // Source 47770c0 loads rig+4630, then object vtable, then slot+D8. Never
    // call the observed target or follow its result. Each edge starts afresh.
    out.object_available=peek(rig,0x4630,out.object);
    out.object_bounds=out.object_available&&out.object>=0x10000&&
        out.object<=memory::highest_user_address-sizeof(std::uintptr_t)&&!(out.object&7);
    if(!out.object_bounds)return out;
    out.vtable_available=memory::peek(out.object,out.vtable);
    out.vtable_image_range=out.vtable_available&&shared.image_sections_verified&&
        !(out.vtable&7)&&detail::image_range(shared.base,out.vtable,0xd8+sizeof(std::uintptr_t),detail::ImageUse::vtable_data);
    if(!out.vtable_image_range)return out;
    out.target_available=peek(out.vtable,0xd8,out.target);
    out.target_image_range=out.target_available&&
        detail::image_range(shared.base,out.target,1,detail::ImageUse::code_target);
    std::uint8_t byte{};
    out.target_readable=out.target_image_range&&memory::peek(out.target,byte);
    return out;
}
Clock clock(const Owner& o) noexcept {
    Clock out;if(o.present())out.available=read_native_clock_try(o.entity,o.core,out.serial,out.time);
    return out;
}
WorldPublication world_publication(const Owner& owner) noexcept {
    WorldPublication out;
    // SDK publication is try-only and was initialized on the bootstrap thread.
    // This cannot establish what native physics_world(context) would return now.
    const auto published=native_chassis::released_ownership_status();
    out.available=published.available;
    out.owner_matched=published.available&&published.client&&published.entity==owner.entity&&
        published.core==owner.core&&published.context==owner.context&&published.rig==owner.rig;
    if(!out.owner_matched)return out;
    out.client=published.client;out.token=published.token;out.world=published.world;out.native_state=published.native_state;
    // Read only through the freshly canonical entity; retained SDK client,
    // world/state and body scalars are NEVER dereferenced by this diagnostic.
    out.client_context=pointer(owner.entity,0x20);
    out.entity_context_available=out.client_context!=0;
    return out;
}
Snapshot sample(const NoBailSkeletonEvent& event,ProducerRaw* producer_raw=nullptr) noexcept {
    Snapshot out;LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);out.qpc_begin=qpc.QuadPart;
    // Event publication was resolved freshly by the wrapper. Resolve again at
    // this edge; a stale remembered owner is never used as a fallback.
    NoBailSkater current;
    out.owner_available=event.owner_available&&try_no_bail_skater(event.rig_argument,current)&&owner(current)==owner(event.owner);
    if(out.owner_available) {
        out.first=owner(current);out.clock_before=clock(out.first);
        out.world_before=world_publication(out.first);
        out.flags_available=peek(current.context,0x13cc,out.context_13cc);
        out.context_830=raw4(current.context,0x830);out.context_7c0=raw4(current.context,0x7c0);
        out.rig_57f0=raw4(current.rig,0x57f0);
        if(producer_raw) {
            producer_raw->rig_5860=raw4(current.rig,0x5860);producer_raw->rig_5870=raw4(current.rig,0x5870);
            producer_raw->divisor_available=peek(current.context,0x17ec,producer_raw->divisor);
        }
        out.rig_4630=tail_dispatch(current.rig);
        out.rig_4638.available=peek(current.rig,0x4638,out.rig_4638.words);
        out.rig_4720=raw16(current.rig,0x4720);out.rig_4760=raw16(current.rig,0x4760);
        // Retained 47654d0 source indexing only. Its coordinate basis remains
        // raw; do not label this cache a world transform or add offsets here.
        out.cached_record_23=raw16(current.rig,0x35c0+23*0x40);
        out.collection=pointer(current.entity,0x70);
        std::uintptr_t collection_owner{};std::uint8_t count{},first{},extra{},teleport{1};
        if(peek(out.collection,0,collection_owner)&&collection_owner==current.entity&&
           peek(out.collection,8,count)&&peek(out.collection,9,first)&&peek(out.collection,10,extra)&&
           count<=128&&first<=128&&extra<=32&&peek(current.entity,0x7e0,teleport)&&teleport==0) {
            const auto offset=std::uintptr_t{0x40}+(std::uintptr_t{first}+2*std::uintptr_t{extra})*0x20;
            out.root_available=peek(out.collection,offset,out.root_words);
        }
        out.physical_rig=pointer(current.rig,0x2f10);
        std::uintptr_t vt{};std::uint32_t count_bodies{};
        out.parts=pointer(out.physical_rig,0x20);
        out.physical_typed=peek(out.physical_rig,0,vt)&&
            vt==shared.base+addr::client_source_spawn::rig_physics_vtable&&
            peek(out.parts,0,count_bodies)&&count_bodies==26;
        if(out.physical_typed) {
            const auto record=out.parts+23*std::uintptr_t{0x130};std::uintptr_t record_owner{};
            out.physical_record_owner=peek(record,0x10,record_owner)&&record_owner==out.physical_rig;
            if(out.physical_record_owner) {
                out.physical_record_23=raw16(record,0x20);
                out.physical_flags_available=peek(record,0x60,out.physical_flags);
                std::uintptr_t final_vt{},final_record_owner{};std::uint32_t final_count{};
                out.physical_chain_stable=pointer(current.rig,0x2f10)==out.physical_rig&&
                    peek(out.physical_rig,0,final_vt)&&final_vt==vt&&pointer(out.physical_rig,0x20)==out.parts&&
                    peek(out.parts,0,final_count)&&final_count==26&&peek(record,0x10,final_record_owner)&&final_record_owner==out.physical_rig;
            }
        }
        NoBailSkater last;
        out.final_owner_available=try_no_bail_skater(event.rig_argument,last);
        if(out.final_owner_available)out.last=owner(last);
        out.world_after=world_publication(out.last);
        out.clock_after=clock(out.last);
        out.qualified=detail::snapshot_qualified(out.first,out.last,out.clock_before,out.clock_after);
    }
    QueryPerformanceCounter(&qpc);out.qpc_end=qpc.QuadPart;
    return out;
}
bool producer_component(const Owner& o,std::uintptr_t component) noexcept {
    if(!o.present()||!component)return false;
    std::uintptr_t live{},collection{},entity{},core{};
    return peek(o.entity,0x628,live)&&live==component&&peek(component,0x18,collection)&&
        peek(collection,0,entity)&&entity==o.entity&&peek(component,0x70,core)&&core==o.core;
}
NoBailSkeletonEvent producer_event(std::uintptr_t rig,std::uintptr_t context,std::uintptr_t component) noexcept {
    NoBailSkeletonEvent out;NoBailSkater current;
    out.owner_available=try_no_bail_skater(rig,current)&&(!context||current.context==context)&&
        (!component||producer_component(owner(current),component));
    if(out.owner_available){out.owner=current;out.rig_argument=current.rig;}
    return out;
}
ProducerSnapshot producer_sample(const NoBailSkeletonEvent& event,std::uintptr_t component) noexcept {
    ProducerSnapshot out;out.frame=sample(event,&out.raw);
    // Resolve/check component again after bounded scalar reads. No saved owner
    // is dereferenced when this edge lacks fresh canonical ownership.
    out.component_proved=event.owner_available&&out.frame.final_owner_available&&
        out.frame.first==out.frame.last&&(!component||producer_component(out.frame.last,component));
    if(!out.component_proved)out.frame.qualified=false;
    return out;
}
ProducerCookie producer_before(detail::ProducerKind kind,std::uintptr_t rig,std::uintptr_t context,
    std::uintptr_t component,const std::array<float,16>* selected,std::uint8_t flags,
    bool rendering=false,std::uintptr_t entity=0,std::uint64_t token=0,std::uint64_t revision=0) noexcept {
    // Disabled path has zero diagnostic owner/clock/raw reads.
    if(!producer.enabled.load(std::memory_order_acquire)||!producer.armed.load(std::memory_order_acquire)||
       producer.sealed.load(std::memory_order_acquire))return {};
    ErrorGuard error;
    if(producer.busy.test_and_set(std::memory_order_acquire)){++producer.contention;return {};}
    struct Unlock {~Unlock(){producer.busy.clear(std::memory_order_release);}} unlock;
    const auto event=producer_event(rig,context,component);
    if(!event.owner_available){++producer.unavailable;return {};}
    const auto current=owner(event.owner);
    if(entity&&entity!=current.entity){++producer.unavailable;return {};}
    const auto native_clock=clock(current);
    const auto reserved=producer.budget.reserve(kind,current,native_clock);
    if(!reserved.accepted){
        ++producer.dropped;
        if(producer.budget.sealed){
            producer.sealed.store(true,std::memory_order_release);
            if(producer.budget.pinned.present()&&producer.budget.pinned!=current)++producer.owner_changed;
        }
        return {};
    }
    if(producer.budget.sealed)producer.sealed.store(true,std::memory_order_release);
    auto& slot=producer.slots[reserved.record];
    slot.invocation=producer.sequence.fetch_add(1,std::memory_order_relaxed)+1;
    slot.thread=GetCurrentThreadId();slot.kind=kind;slot.component=component;
    slot.serial_available=native_clock.available&&std::isfinite(native_clock.time)&&native_clock.time>=0;
    slot.serial=native_clock.serial;slot.rendering=rendering;slot.entity=entity;
    slot.mount_token=token;slot.frame_revision=revision;slot.flags=flags;
    if(selected)slot.selected=std::bit_cast<std::array<std::uint32_t,16>>(*selected);
    auto measured=producer_sample(event,component);
    if(measured.frame.final_owner_available&&measured.frame.last.present()&&measured.frame.last!=current) {
        producer.budget.sealed=true;producer.sealed.store(true,std::memory_order_release);++producer.owner_changed;
    }
    producer.snapshots[detail::producer_pre_index(reserved.record)]=measured;
    slot.pre_ready.store(true,std::memory_order_release);
    if(kind==detail::ProducerKind::writer)return {};
    return {reserved.record+1,slot.invocation,slot.thread};
}
void producer_after(ProducerCookie cookie) noexcept {
    if(!cookie||!producer.enabled.load(std::memory_order_acquire))return;
    ErrorGuard error;
    if(producer.busy.test_and_set(std::memory_order_acquire)){++producer.contention;return;}
    struct Unlock {~Unlock(){producer.busy.clear(std::memory_order_release);}} unlock;
    if(cookie.record>32){++producer.bad_cookie;return;}
    auto& slot=producer.slots[cookie.record-1];
    if(!slot.pre_ready.load(std::memory_order_acquire)||slot.post_ready.load(std::memory_order_acquire)||
       !detail::producer_cookie_matches(cookie.record,cookie.invocation,slot.invocation,
           GetCurrentThreadId(),slot.thread)||cookie.thread!=slot.thread){++producer.bad_cookie;return;}
    const auto event=producer_event(0,0,slot.component);
    if(!event.owner_available)++producer.unavailable;
    // A returned call may have lost its owner. Publish an unavailable post,
    // never reconstruct one from the pre owner or read through a retained rig.
    NoBailSkeletonEvent admitted=event;
    if(event.owner_available&&!producer.budget.same_owner(owner(event.owner))) {
        admitted.owner_available=false;producer.sealed.store(true,std::memory_order_release);++producer.owner_changed;
    }
    producer.snapshots[detail::producer_post_index(cookie.record-1)]=producer_sample(admitted,slot.component);
    slot.post_ready.store(true,std::memory_order_release);
}
std::uint64_t capture(const NoBailSkeletonEvent& event,std::uint64_t cookie) noexcept {
    ErrorGuard error;
    // Try-only SDK storage guard. Never spin or wait on native/client work.
    if(shared.callback_busy.test_and_set(std::memory_order_acquire)){shared.contention.fetch_add(1,std::memory_order_relaxed);return 0;}
    struct Busy {~Busy(){shared.callback_busy.clear(std::memory_order_release);}} busy;
    if(event.edge==NoBailSkeletonEdge::original_returned) {
        if(!cookie||cookie>detail::capacity){shared.bad_cookie.fetch_add(1);return 0;}
        auto& slot=shared.slots[cookie-1];
        if(!slot.pre_ready.load(std::memory_order_acquire)||slot.post_ready.load(std::memory_order_relaxed)||
           !detail::cookie_matches(cookie,event.invocation,slot.invocation,GetCurrentThreadId(),slot.thread)) {
            shared.bad_cookie.fetch_add(1);return 0;
        }
        // Complete an already accepted pair even if the window ended meanwhile.
        slot.after=sample(event);slot.post_ready.store(true,std::memory_order_release);return 0;
    }
    if(!shared.enabled.load(std::memory_order_acquire)||shared.sealed.load(std::memory_order_acquire))return 0;
    const auto w=shared.window.load(std::memory_order_acquire);if(w>=3)return 0;
    if(!event.owner_available){shared.unavailable[w].fetch_add(1);return 0;}
    const auto current=owner(event.owner);
    if(!shared.pinned.present())shared.pinned=current;
    if(shared.pinned!=current){shared.owner_changed.fetch_add(1);shared.sealed.store(true,std::memory_order_release);return 0;}
    const auto now=GetTickCount64();if(!shared.began[w])shared.began[w]=now;
    if(now<shared.began[w]||now-shared.began[w]>10000){shared.dropped[w].fetch_add(1);return 0;}
    const auto number=shared.reserved[w].fetch_add(1,std::memory_order_relaxed);
    if(number>=detail::limits[w]){shared.dropped[w].fetch_add(1);return 0;}
    const auto index=detail::offsets[w]+static_cast<std::size_t>(number);auto& slot=shared.slots[index];
    slot.invocation=event.invocation;slot.thread=GetCurrentThreadId();slot.window=w;slot.seconds=event.seconds;
    slot.incoming=event.incoming_third;slot.effective=event.effective_third;slot.before=sample(event);
    slot.pre_ready.store(true,std::memory_order_release);return index+1;
}
Json owner_json(const Owner& o){return Json{{"base",o.base},{"entity",o.entity},{"core",o.core},{"context",o.context},{"rig",o.rig}};}
Json clock_json(const Clock& c){return Json{{"available",c.available},{"pre_original_listener_serial",c.serial},{"native_time",c.available&&std::isfinite(c.time)?Json(c.time):Json(nullptr)}};}
template<std::size_t N> Json words_json(bool available,const std::array<std::uint32_t,N>& words) {
    if(!available)return Json(nullptr);return Json(words);
}
Json raw_json(const Raw4& x){return Json{{"raw_words",words_json(x.available,x.words)},{"xyz_finite",x.available&&detail::finite_words(x.words,0,3)},{"all_finite",x.available&&detail::finite_words(x.words,0,4)}};}
Json raw_json(const Raw16& x){return Json{{"raw_words",words_json(x.available,x.words)},{"translation_xyz_finite",x.available&&detail::finite_words(x.words,12,3)},{"all_finite",x.available&&detail::finite_words(x.words,0,16)}};}
Json world_json(const WorldPublication& w) {
    return Json{{"sdk_available",w.available},{"published_owner_matches",w.owner_matched},
        {"fresh_entity_context_available",w.entity_context_available},{"client",w.client},{"client_context",w.client_context},
        {"world",w.world},{"native_state",w.native_state},{"body_token",w.token},{"canonical_native_world_proved",false}};
}
Json tail_dispatch_json(const TailDispatch& x) {
    return Json{{"object_raw",x.object_available?Json(x.object):Json(nullptr)},{"object_pointer_bounds",x.object_bounds},
        {"vtable_raw",x.vtable_available?Json(x.vtable):Json(nullptr)},{"vtable_in_verified_readable_data_section",x.vtable_image_range},
        {"slot_d8_target_raw",x.target_available?Json(x.target):Json(nullptr)},
        {"target_in_verified_readable_executable_section",x.target_image_range},{"target_first_byte_readable",x.target_readable},
        {"call_performed",false},{"subobject_atomic_snapshot_proved",false},{"completion_contract_proved",false}};
}
Json snapshot_json(const Snapshot& s,bool effective) {
    // Raw-rig floating math is deferred with JSON, never run in the hook.
    bool prediction_known{};
    const auto prediction=detail::copy_guard_prediction(effective,s.flags_available,s.context_13cc,
        s.rig_57f0.available,s.rig_57f0.words,prediction_known);
    return Json{{"qpc_begin",s.qpc_begin},{"qpc_end",s.qpc_end},{"owner_available",s.owner_available},
        {"final_owner_available",s.final_owner_available},{"owner_before",owner_json(s.first)},{"owner_after",owner_json(s.last)},
        {"clock_before",clock_json(s.clock_before)},{"clock_after",clock_json(s.clock_after)},
        {"world_publication_before",world_json(s.world_before)},{"world_publication_after",world_json(s.world_after)},
        {"full_canonical_world_coherence_proved",false},
        {"same_owner_and_listener_clock",s.qualified},{"atomic_native_snapshot",false},
        {"root_words",words_json(s.root_available,s.root_words)},{"context_13cc",s.flags_available?Json(s.context_13cc):Json(nullptr)},
        {"context_13cc_bit8000",s.flags_available?Json((s.context_13cc&0x8000u)!=0):Json(nullptr)},
        {"context_830",raw_json(s.context_830)},{"context_7c0",raw_json(s.context_7c0)},
        {"rig_4630_dispatch_target_evidence",tail_dispatch_json(s.rig_4630)},
        {"rig_4638_raw_pointer_lane_words",words_json(s.rig_4638.available,s.rig_4638.words)},
        {"rig_57f0",raw_json(s.rig_57f0)},{"rig_4720",raw_json(s.rig_4720)},{"rig_4760",raw_json(s.rig_4760)},
        {"cached_35c0_record23_raw_basis_unknown",raw_json(s.cached_record_23)},
        {"physical_rig",s.physical_rig},{"parts",s.parts},{"physical_typed",s.physical_typed},
        {"physical_record_owner",s.physical_record_owner},{"physical_chain_stable",s.physical_chain_stable},
        {"physical_record23_matrix",raw_json(s.physical_record_23)},
        {"physical_flags",s.physical_flags_available?Json(s.physical_flags):Json(nullptr)},
        {"copy_guard_prediction_known",prediction_known},{"copy_guard_prediction",prediction_known?Json(prediction):Json(nullptr)},
        {"copy_branch_execution_proved",false}};
}
}
bool initialize(std::uintptr_t base) noexcept {
    ErrorGuard error;if(shared.attempted)return shared.enabled.load();shared.attempted=true;
    try {
        if(!opted_root(shared.root)||!supported_build::running_image_matches(base))return false;
        shared.image_sections_verified=source_sections_match(base);if(!shared.image_sections_verified)return false;
        shared.base=base;if(!QueryPerformanceFrequency(&shared.frequency)||shared.frequency.QuadPart<=0)return false;
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
        shared.process_creation=(std::uint64_t{created.dwHighDateTime}<<32)|created.dwLowDateTime;
        if(!shared.process_creation)return false;
        // Prime lazy SDK-only stores outside the hook. No native helper invoked.
        (void)native_chassis::released_ownership_status();std::uint64_t serial{};double time{};
        (void)read_native_clock_try(0,0,serial,time);
        shared.armed[0].store(true);shared.window.store(0);
        wchar_t producer_flag[2]{};
        const bool explicit_producer=GetEnvironmentVariableW(L"RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE",producer_flag,2)==1&&producer_flag[0]==L'1';
        producer.enabled.store(explicit_producer,std::memory_order_release);
        shared.enabled.store(true,std::memory_order_release);
        if(!add_no_bail_skeleton_observer(&capture)){shared.enabled.store(false);return false;}
        return true;
    }catch(...){shared.enabled.store(false);return false;}
}
void arm(Window window) noexcept {
    ErrorGuard error;const auto w=static_cast<unsigned>(window);
    if(w==0||w>=3||!shared.enabled.load(std::memory_order_acquire)||shared.sealed.load())return;
    bool no=false;if(shared.armed[w].compare_exchange_strong(no,true)) {
        if(producer.enabled.load(std::memory_order_acquire)) {
            if(window==Window::before_first_mount)producer.armed.store(true,std::memory_order_release);
            else if(window==Window::first_bail)producer.sealed.store(true,std::memory_order_release);
        }
        shared.window.store(w,std::memory_order_release);
    }
}
ProducerCookie before_motion(std::uintptr_t rig,std::uintptr_t context,
    const std::array<float,16>& selected,std::uint8_t flags) noexcept {
    return producer_before(detail::ProducerKind::motion,rig,context,0,&selected,flags);
}
ProducerCookie before_animation(std::uintptr_t component) noexcept {
    return producer_before(detail::ProducerKind::animation,0,0,component,nullptr,0);
}
void after_producer(ProducerCookie cookie) noexcept {producer_after(cookie);}
void before_pose_write(std::uintptr_t component,bool rendering,std::uintptr_t entity,
    std::uint64_t mount_token,std::uint64_t frame_revision) noexcept {
    (void)producer_before(detail::ProducerKind::writer,0,0,component,nullptr,0,rendering,entity,mount_token,frame_revision);
}
static Json producer_snapshot_json(const ProducerSnapshot& snapshot) {
    auto out=snapshot_json(snapshot.frame,false);
    // Third-byte SkeletonResponse copy prediction is irrelevant at these seams.
    out.erase("copy_guard_prediction");out.erase("copy_guard_prediction_known");
    out["raw_5860"]=raw_json(snapshot.raw.rig_5860);out["raw_5870"]=raw_json(snapshot.raw.rig_5870);
    out["raw_context_17ec"]=Json{{"available",snapshot.raw.divisor_available},
        {"raw_word",snapshot.raw.divisor_available?Json(snapshot.raw.divisor):Json(nullptr)},
        {"finite",snapshot.raw.divisor_available&&std::isfinite(std::bit_cast<float>(snapshot.raw.divisor))}};
    out["component_proved"]=snapshot.component_proved;return out;
}
static bool write_producer_report() noexcept {
    if(!producer.enabled.load(std::memory_order_acquire))return true;
    try {
        Json rows=Json::array();std::size_t snapshots{};
        for(std::size_t i=0;i<producer.slots.size();++i) {
            const auto& slot=producer.slots[i];if(!slot.pre_ready.load(std::memory_order_acquire))continue;
            const auto pre=producer.snapshots[detail::producer_pre_index(i)];
            const bool writer=slot.kind==detail::ProducerKind::writer;
            const bool post=!writer&&slot.post_ready.load(std::memory_order_acquire);
            ProducerSnapshot after;if(post)after=producer.snapshots[detail::producer_post_index(i)];
            snapshots+=post?2:1;
            const bool motion=slot.kind==detail::ProducerKind::motion;
            rows.push_back(Json{{"record_index",i},{"slot_in_kind",i%detail::producer_per_kind},
                {"invocation",slot.invocation},{"thread_id",slot.thread},
                {"kind",motion?"motion_original":writer?"before_bmx_pose_write":"local_animation_original"},
                {"listener_serial_at_reservation",slot.serial_available?Json(slot.serial):Json(nullptr)},
                {"component",slot.component},{"rendering",writer?Json(slot.rendering):Json(nullptr)},
                {"frame_entity",slot.entity},{"mount_token",slot.mount_token},{"frame_revision",slot.frame_revision},
                {"flags",motion?Json(slot.flags):Json(nullptr)},
                {"selected_target_words",motion?Json(slot.selected):Json(nullptr)},
                {"before",producer_snapshot_json(pre)},{"post_observed",post},
                {"original_returned",post?Json(true):Json(nullptr)},
                {"after",post?producer_snapshot_json(after):Json(nullptr)},
                {"pair_same_owner_and_listener_clock",post&&detail::pair_qualified(pre.frame.qualified,after.frame.qualified,
                    pre.frame.first,after.frame.first,pre.frame.clock_before,after.frame.clock_after)}});
        }
        // Budget is mutable only under busy; copy scalar export fields try-only.
        // No lock is held during allocation, serialization or filesystem work.
        bool anchor_available{};std::uint64_t anchor{};
        if(producer.busy.test_and_set(std::memory_order_acquire))return false;
        anchor_available=producer.budget.anchor_available;anchor=producer.budget.anchor;
        producer.busy.clear(std::memory_order_release);
        Json report{{"schema_version",1},{"event","bmx_first_mount_producer_probe"},
            {"process_id",GetCurrentProcessId()},{"process_creation_filetime",std::to_string(shared.process_creation)},
            {"read_only_native_state",true},{"explicit_producer_opt_in",true},{"source_sha256",supported_build::game_sha256},
            {"mount_token_continuity_proved",false},
            {"qpc_frequency",shared.frequency.QuadPart},{"armed",producer.armed.load()},{"sealed",producer.sealed.load()},
            {"record_capacity",detail::producer_records},{"snapshot_capacity",detail::producer_snapshots},
            {"epoch_window",detail::producer_steps},{"per_kind_per_epoch",detail::producer_per_step},
            {"anchor_listener_serial",anchor_available?Json(anchor):Json(nullptr)},{"stored_snapshots",snapshots},
            {"static_storage_bytes",sizeof(ProducerShared)},
            {"contention",producer.contention.load()},{"unavailable",producer.unavailable.load()},
            {"dropped",producer.dropped.load()},{"bad_cookies",producer.bad_cookie.load()},{"owner_changes",producer.owner_changed.load()},
            {"completion_boundary","existing_call_return_only_not_native_physics_or_animation_completion"},
            {"clock_boundary","SDK_listener_advances_before_original_and_is_not_a_completion_fence"},
            {"rows",std::move(rows)}};
        const auto text=report.dump(2);const auto tmp=shared.root/L"bmx-first-mount-producer-observation.json.tmp";
        std::ofstream file(tmp,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();
        return file&&MoveFileExW(tmp.c_str(),(shared.root/L"bmx-first-mount-producer-observation.json").c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    }catch(...){return false;}
}
static bool write_report() noexcept {
    ErrorGuard error;
    try {
        std::filesystem::path current;
        if(!shared.enabled.load(std::memory_order_acquire)||!opted_root(current)||current!=shared.root)return false;
        Json rows=Json::array();
        for(const auto& slot:shared.slots) {
            if(!slot.pre_ready.load(std::memory_order_acquire))continue;
            // Copy only a published immutable half. Never copy the whole slot
            // while another callback may still be writing its after member.
            const auto pre=slot.before;const bool post_ready=slot.post_ready.load(std::memory_order_acquire);
            Json row{{"invocation",slot.invocation},{"thread_id",slot.thread},{"window",slot.window},
                {"seconds",std::isfinite(slot.seconds)?Json(slot.seconds):Json(nullptr)},
                {"incoming_third",slot.incoming},{"effective_third",slot.effective},
                {"pair_same_owner_and_listener_clock",post_ready&&detail::pair_qualified(pre.qualified,slot.after.qualified,
                    pre.first,slot.after.first,pre.clock_before,slot.after.clock_after)},
                {"before_original",snapshot_json(pre,slot.effective)},{"post_observed",post_ready},{"original_returned",post_ready?Json(true):Json(nullptr)},
                {"after_original",post_ready?snapshot_json(slot.after,slot.effective):Json(nullptr)}};
            rows.push_back(std::move(row));
        }
        Json counts=Json::array();for(unsigned w=0;w<3;++w)counts.push_back(Json{{"window",w},
            {"limit",detail::limits[w]},{"reserved",shared.reserved[w].load()},{"dropped",shared.dropped[w].load()},
            {"unavailable",shared.unavailable[w].load()}});
        Json report{{"schema_version",1},{"event","bmx_skeleton_response_phase_probe"},{"process_id",GetCurrentProcessId()},
            {"process_creation_filetime",std::to_string(shared.process_creation)},
            {"read_only_native_state",true},{"source_sha256",supported_build::game_sha256},
            {"supported_image_sections_verified",shared.image_sections_verified},
            {"qpc_frequency",shared.frequency.QuadPart},{"sealed",shared.sealed.load()},
            {"owner_changes",shared.owner_changed.load()},{"callback_contention",shared.contention.load()},
            {"bad_cookies",shared.bad_cookie.load()},{"windows",std::move(counts)},
            {"completion_boundary","original_return_only_not_native_physics_or_animation_completion"},
            {"clock_boundary","SDK_listener_advances_before_original_and_is_not_a_completion_fence"},
            {"rows",std::move(rows)}};
        // Caller must use an idle supervisor checkpoint, never the hook/step path.
        const auto text=report.dump(2);const auto tmp=shared.root/L"bmx-skeleton-phase-observation.json.tmp";
        std::ofstream file(tmp,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();
        if(!file)return false;
        // Same-directory atomic replacement; no delete gap or shared report name outside private root.
        if(!MoveFileExW(tmp.c_str(),(shared.root/L"bmx-skeleton-phase-observation.json").c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
        return write_producer_report();
    }catch(...){return false;}
}
bool request_report() noexcept {
    ErrorGuard error;
    if(!shared.enabled.load(std::memory_order_acquire)||!opted_flag())return false;
    shared.report_requested.store(true,std::memory_order_release);return true;
}
void poll(bool idle) noexcept {
    ErrorGuard error;
    if(idle&&shared.report_requested.exchange(false,std::memory_order_acq_rel))(void)write_report();
}

}
