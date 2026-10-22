#pragma once
#include "bmx_rider_pose.h"
#include "bmx_grip_projection_trace.h"
#include "bmx_native_motion.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Game/BMX/bmx_grip_probe_policy.h"
#include "Engine/Game/BMX/bmx_skeleton_phase_policy.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <type_traits>

namespace dingosdk::bmx::rider::grip_probe {
namespace policy=grip_probe_policy;
inline constexpr std::size_t bone_count=395;
inline constexpr std::size_t rig_count=386;
using Bones=std::array<pose::Bone,bone_count>;
struct Edge {policy::Edge admission;NativeMotionLeaseStatus motion;std::uintptr_t actual_base{};ULONGLONG tick{};DWORD thread{};bool source_pose_owner_matches{};};
inline policy::Owner owner_key(std::uintptr_t client,const NoBailSkater& s) noexcept {return {client,s.entity,s.core,s.context,s.rig};}
inline std::array<std::uintptr_t,5> owner_array(const policy::Owner& s) noexcept {return {s.client,s.entity,s.core,s.context,s.rig};}
inline Edge edge(std::uintptr_t entity,std::uintptr_t core) noexcept {
    Edge out;out.tick=GetTickCount64();out.thread=GetCurrentThreadId();
    out.admission.clock=read_native_clock_try(entity,core,out.admission.serial,out.admission.time);
    out.motion=native_motion_lease_status(true);
    const auto& m=out.motion;auto& a=out.admission;
    a.entity=m.entity;a.core=m.core;a.context=m.context;a.rig=m.rig;a.client=m.client;
    a.epoch=m.epoch_revision;a.lease_revision=m.lease_revision;
    a.lease=m.available&&m.details_available&&m.entity==entity&&m.core==core;
    a.active=m.active;a.epoch_valid=m.epoch_valid;
    // Resolve only the currently published NoBail owner, never a copied lease
    // client/pointer. The client output comes from that same fresh full chain.
    NoBailSkater actual;std::uintptr_t client{};
    a.actual_available=try_no_bail_skater(0,actual,client);
    a.actual_owner=owner_key(client,actual);out.actual_base=actual.base;
    return out;
}
enum class Outcome:unsigned {source_only,solver_rejected,solver_ready,write_admitted,write_failed,written,readback_unavailable,readback_observed};
enum class Unavailable:unsigned {not_attempted,source_or_before_association,fresh_pre_guard,body_read,fresh_post_guard,after_association,guard_exception};
struct Record {
    visual::Frame frame;pose::Result solution;Edge source_before,source,before,after;
    NoBailSkater source_pose_owner;policy::Owner fresh_pre_owner{},fresh_post_owner{};
    std::uintptr_t component{},holder{},buffer{},render_data{};
    std::uint64_t frame_revision{},contact_revision{};
    std::uint32_t caller_rva{},index20{};
    bool index20_available{};
    std::array<pose::Vec,4> targets{};
    std::array<world::Matrix,world::part_count> requested_parts{};
    Bones original{},solved{},readback{};
    Outcome outcome{Outcome::source_only};Unavailable unavailable{Unavailable::not_attempted};
    bool normalized{},requested_parts_available{},fresh_guard_passed{},fresh_post_guard_passed{},readback_available{},write_complete{},write_failed{},source_association_consistent{};
};
static_assert(std::is_trivially_copyable_v<Record>);
static_assert(sizeof(Record)*policy::capacity<8*1024*1024);
// Two bounded raw CPU observations, never pose input. A readable prefix is not
// an allocation-length proof, an identity remap, or rendered matrix semantics.
struct MetadataHeader {
    std::uintptr_t state{},definition{},resource{},pose{},metadata{},definition_ref{},bind{},remap{},parent50{},parent70{};
    std::uint32_t count{},meta08{},cutoff{},parent70_header{};
    std::uint8_t active{};
    std::array<std::uint32_t,16> state50_words{};
    bool operator==(const MetadataHeader&) const=default;
};
struct MetadataSnapshot {
    MetadataHeader header{};
    std::array<std::uint32_t,rig_count> remap_prefix{},parent_prefix{};
    unsigned record_index{};
    bool available{},remap_readable{},parents_readable{};
};
static_assert(std::is_trivially_copyable_v<MetadataSnapshot>);
static_assert(sizeof(MetadataSnapshot)*2<8192);
template<class Read,class T>
inline bool metadata_field(Read& read,std::uintptr_t base,std::size_t offset,T& out) noexcept {
    if(base<0x10000||offset>memory::highest_user_address-sizeof(T)||
       base>memory::highest_user_address-offset-sizeof(T))return false;
    try {return read(base+offset,&out,sizeof(T));}catch(...){return false;}
}
template<class Read>
inline bool metadata_header(Read& read,std::uintptr_t holder,std::uintptr_t buffer,MetadataHeader& out) noexcept {
    MetadataHeader h;
    if(!metadata_field(read,holder,0x78,h.state)||!metadata_field(read,h.state,0x18,h.definition)||
       !metadata_field(read,h.definition,0x1a0,h.resource)||!metadata_field(read,h.resource,0xc,h.count)||h.count!=bone_count||
       !metadata_field(read,h.state,0x48,h.active)||!metadata_field(read,h.state,0xd0,h.pose)||h.pose!=buffer||
       !metadata_field(read,h.state,0x138,h.metadata)||!metadata_field(read,h.state,0x50,h.state50_words)||
       !metadata_field(read,h.metadata,0,h.definition_ref)||!metadata_field(read,h.metadata,8,h.meta08)||
       !metadata_field(read,h.metadata,0x90,h.bind)||!metadata_field(read,h.metadata,0xa0,h.remap)||
       !metadata_field(read,h.metadata,0x140,h.cutoff)||!metadata_field(read,h.definition_ref,0x50,h.parent50)||
       !metadata_field(read,h.definition_ref,0x70,h.parent70)||h.parent70<0x10004||
       !metadata_field(read,h.parent70-4,0,h.parent70_header))return false;
    out=h;return true;
}
template<class Read>
inline bool metadata_snapshot(Read& read,std::uintptr_t holder,std::uintptr_t buffer,MetadataSnapshot& out) noexcept {
    MetadataSnapshot v;
    if(!metadata_header(read,holder,buffer,v.header))return false;
    v.remap_readable=metadata_field(read,v.header.remap,0,v.remap_prefix);
    // Exact u32 selector used by native4892CF0; no inferred array header.
    const auto parents=v.header.meta08==(v.header.parent70_header&0x7fffffffU)?v.header.parent70:v.header.parent50;
    v.parents_readable=metadata_field(read,parents,0,v.parent_prefix);
    out=v;return true;
}
struct Store {
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<bool> capture_done{};
    policy::Window window;
    std::array<Record,policy::capacity> records{};
    std::array<MetadataSnapshot,2> metadata{};
    std::array<int,rig_count> parents{};std::array<pose::Quat,rig_count> bind{};
    std::array<int,3> left_arm{},right_arm{},left_leg{},right_leg{};
    int hips{},head{};std::array<int,4> spine{};std::array<int,2> neck{},shoulders{},toe_base{};
};
struct Shared {
    std::once_flag init;std::atomic<Store*> store{};std::filesystem::path root;
    std::uintptr_t caller_image_base{};bool caller_image_verified{};
    DWORD client_thread{};std::uint64_t process_creation{};bool ever_active{},last_active{},finish_requested{},export_attempted{};
    std::atomic<std::uint64_t> contention{};
};
inline Shared& shared(){static auto* s=new Shared;return *s;}
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
inline bool flag(const wchar_t* name) noexcept {wchar_t v[2]{};return GetEnvironmentVariableW(name,v,2)==1&&v[0]==L'1';}
// Bootstrap only. Reuse the exact supported-image section policy; callbacks
// classify scalar addresses without parsing PE headers or reading native code.
inline bool caller_sections_match(std::uintptr_t base) noexcept {
    namespace image=skeleton_phase_probe::detail;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<IMAGE_SECTION_HEADER,image::image_sections.size()> sections{};
    if(!memory::peek(base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew!=0x80||
       !memory::peek(base+0x80,nt)||nt.Signature!=IMAGE_NT_SIGNATURE||
       nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.FileHeader.NumberOfSections!=sections.size()||
       nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64)||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
       nt.OptionalHeader.SizeOfImage!=supported_build::game_image_size||
       !memory::peek(base+0x80+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+sizeof(IMAGE_OPTIONAL_HEADER64),sections))return false;
    for(std::size_t i=0;i<sections.size();++i) {
        const auto& source=image::image_sections[i];const auto& actual=sections[i];
        if(actual.VirtualAddress!=source.rva||actual.Misc.VirtualSize!=source.size||actual.Characteristics!=source.flags)return false;
    }
    return true;
}
inline std::uint32_t qualified_caller_rva(bool verified,std::uintptr_t image_base,std::uintptr_t actual_base,
    const multiplayer::RenderPoseCallsite& callsite,std::uintptr_t holder,std::uintptr_t render_data) noexcept {
    namespace image=skeleton_phase_probe::detail;
    if(!verified||actual_base!=image_base||holder>UINTPTR_MAX-0xc0||
       callsite.animation_interface!=holder+0xc0||callsite.render_data!=render_data||!render_data||
       !image::image_range(image_base,callsite.return_address,1,image::ImageUse::code_target))return 0;
    const auto rva=callsite.return_address-image_base;
    return rva<=UINT32_MAX?static_cast<std::uint32_t>(rva):0;
}
inline void initialize_client() noexcept {
    try {std::call_once(shared().init,[]{
        if(!flag(L"RESKATE_BMX_GRIP_PROBE")||!flag(L"RESKATE_BMX_RAMP_RIDE")||
           !flag(L"RESKATE_BMX_NATIVE_CHASSIS")||!user_data::development_requested())return;
        const auto root=user_data::override_root();
        if(!root||_wcsicmp(root->filename().c_str(),L"bmx-user-data-ramp-test")!=0)return;
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return;
        const auto creation=(std::uint64_t{created.dwHighDateTime}<<32)|created.dwLowDateTime;
        if(!creation)return;
        shared().root=*root;shared().client_thread=GetCurrentThreadId();
        shared().process_creation=creation;
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        shared().caller_image_verified=supported_build::running_image_matches(base)&&caller_sections_match(base);
        shared().caller_image_base=shared().caller_image_verified?base:0;
        grip_projection_trace::initialize(base,creation,shared().caller_image_verified,*root);
        shared().store.store(new Store,std::memory_order_release);
        if(shared().caller_image_verified)multiplayer::enable_render_pose_callsite_probe();
    });}catch(...){}
}
inline Edge source_edge(const NoBailSkater* expected) noexcept {
    auto* s=shared().store.load(std::memory_order_acquire);
    if(!s||s->capture_done.load(std::memory_order_acquire)||!expected||!expected->core)return {};
    auto out=edge(expected->entity,expected->core);const auto& a=out.admission.actual_owner;
    out.source_pose_owner_matches=out.admission.actual_available&&out.actual_base==expected->base&&a.entity==expected->entity&&
        a.core==expected->core&&a.context==expected->context&&a.rig==expected->rig;
    return out;
}
// Passive raw descriptor lane, borrowed only inside the current render callback.
// The +0x10 output-pointer proof grants no readability or meaning to this lane.
// Optional failure cannot veto a native/SDK pose write or existing readback.
template<class Read>
inline bool read_index20(Read&& read,std::uintptr_t render_data,std::uint32_t& out) noexcept {
    if(render_data<0x10000||render_data>memory::highest_user_address-0x20-sizeof(std::uint32_t))return false;
    try {
        std::uint32_t value{};
        if(!read(render_data+0x20,&value,sizeof(value)))return false;
        out=value;return true;
    }catch(...){return false;}
}
// One try-only recorder at a time. Its destruction records early exits without
// retries, native writes, heap allocation, JSON construction or filesystem work.
struct Token {
    Store* store{};Record* record{};
    int metadata_slot{-1};
    Token()=default;Token(const Token&)=delete;Token& operator=(const Token&)=delete;
    ~Token(){if(store)store->busy.clear(std::memory_order_release);}
    void solution(const pose::Result& result,std::span<const pose::Bone> bones,bool normalized=false) noexcept {
        if(!record)return;record->solution=result;record->normalized=normalized;
        std::memcpy(record->solved.data(),bones.data(),sizeof(Bones));
        record->outcome=result.ready?Outcome::solver_ready:Outcome::solver_rejected;
    }
    template<class Guard,class Read>
    void read_back(Guard&& guard,Read&& read,bool complete) noexcept {
        if(!record)return;auto& r=*record;r.index20_available=false;r.write_complete=complete;r.write_failed=!complete;
        r.outcome=complete?Outcome::written:Outcome::write_failed;
        try {
            r.before=edge(r.frame.entity,r.source.admission.core);
            if(!r.source_association_consistent||!policy::same_admission(r.source.admission,r.before.admission,r.before.admission)){
                r.unavailable=Unavailable::source_or_before_association;r.outcome=Outcome::readback_unavailable;return;
            }
            if(!guard(r.fresh_pre_owner)||r.fresh_pre_owner!=r.source.admission.actual_owner||
               r.fresh_pre_owner!=policy::lease_owner(r.source.admission)){
                r.unavailable=Unavailable::fresh_pre_guard;
                r.outcome=Outcome::readback_unavailable;return;
            }
            r.fresh_guard_passed=true;
            std::uint32_t observed_index20{};
            const bool index20_read=r.caller_rva&&read_index20(read,r.render_data,observed_index20);
            MetadataSnapshot raw_metadata{};MetadataHeader metadata_after{};
            const bool metadata_read=metadata_slot>=0&&r.caller_rva==0x2c20d4f&&metadata_snapshot(read,r.holder,r.buffer,raw_metadata);
            const bool copied=read(r.buffer,r.readback.data(),sizeof(Bones));
            const bool metadata_stable=metadata_read&&metadata_header(read,r.holder,r.buffer,metadata_after)&&raw_metadata.header==metadata_after;
            r.fresh_post_guard_passed=copied&&guard(r.fresh_post_owner)&&r.fresh_post_owner==r.source.admission.actual_owner&&
                r.fresh_post_owner==policy::lease_owner(r.source.admission);
            r.after=edge(r.frame.entity,r.source.admission.core);
            if(!copied||!r.fresh_post_guard_passed||!policy::same_admission(r.source.admission,r.before.admission,r.after.admission)){
                r.unavailable=!copied?Unavailable::body_read:!r.fresh_post_guard_passed?Unavailable::fresh_post_guard:Unavailable::after_association;
                r.outcome=Outcome::readback_unavailable;return;
            }
            r.index20=observed_index20;r.index20_available=index20_read;
            if(metadata_stable&&store){raw_metadata.available=true;raw_metadata.record_index=metadata_slot==0?0U:32U;store->metadata[static_cast<unsigned>(metadata_slot)]=raw_metadata;}
            r.readback_available=true;r.outcome=Outcome::readback_observed;
            if(grip_projection_trace::accepting()&&store&&r.write_complete&&r.solution.ready&&r.requested_parts_available&&r.caller_rva==0x2c20d4f) {
                grip_projection_trace::Source requested;
                requested.base=r.source_pose_owner.base;requested.owner=owner_array(r.after.admission.actual_owner);
                requested.serial=r.after.admission.serial;requested.native_time=r.after.admission.time;requested.tick=r.after.tick;
                requested.revision=r.frame_revision;requested.record=static_cast<unsigned>(&r-store->records.data());
                const std::array<pose::Vec,4> local{geometry::left_grip.center,geometry::right_grip.center,geometry::rear_axle,geometry::front_axle};
                constexpr std::array<unsigned,4> part{5,5,2,3};
                for(unsigned i=0;i<local.size();++i){const auto& m=r.requested_parts[part[i]];
                    for(unsigned axis=0;axis<3;++axis)requested.points[i][axis]=m[12+axis]+m[axis]*local[i][0]+m[4+axis]*local[i][1]+m[8+axis]*local[i][2];}
                grip_projection_trace::publish_source(requested);
            }
        }catch(...){r.unavailable=Unavailable::guard_exception;r.outcome=Outcome::readback_unavailable;}
    }
};
inline void begin(Token& token,const visual::Frame& frame,const pose::Rig& rig,std::span<const pose::Bone> original,
                  const NoBailSkater* source_owner,std::uintptr_t component,std::uintptr_t holder,std::uintptr_t buffer,
                  std::uintptr_t render_data,std::uint64_t revision,std::uint64_t contact_revision,const Edge& source_before) noexcept {
    auto* s=shared().store.load(std::memory_order_acquire);
    if(!s||s->capture_done.load(std::memory_order_acquire)||!render_data||!source_owner||!source_owner->core||frame.rider_mode!=visual::RiderMode::riding||frame.mount_progress<1||
       original.size()!=bone_count||rig.parents.size()!=rig_count||rig.bind.size()!=rig_count)return;
    if(s->busy.test_and_set(std::memory_order_acquire)){shared().contention.fetch_add(1,std::memory_order_relaxed);return;}
    token.store=s;const auto e=source_edge(source_owner);
    const bool selected=s->window.observe(frame.entity,frame.mount_token,e.admission.clock,e.admission.serial,e.admission.time,frame.airborne);
    if(s->window.sealed)s->capture_done.store(true,std::memory_order_release);
    if(!selected)return;
    auto& r=s->records[s->window.count-1];token.record=&r;
    if(s->window.count==1)token.metadata_slot=0;else if(s->window.count==33)token.metadata_slot=1;
    if(token.metadata_slot>=0)s->metadata[static_cast<unsigned>(token.metadata_slot)].record_index=s->window.count-1;
    r.frame=frame;r.source_pose_owner=*source_owner;r.source_before=source_before;r.source=e;r.component=component;r.holder=holder;r.buffer=buffer;r.render_data=render_data;
    r.source_association_consistent=source_before.source_pose_owner_matches&&e.source_pose_owner_matches&&
        policy::same_admission(source_before.admission,e.admission,e.admission);
    r.frame_revision=revision;r.contact_revision=contact_revision;
    r.caller_rva=qualified_caller_rva(shared().caller_image_verified,shared().caller_image_base,e.actual_base,
        multiplayer::render_pose_callsite(),holder,render_data);
    std::memcpy(r.original.data(),original.data(),sizeof(Bones));
    r.targets={pose::palm_target(frame,true),pose::palm_target(frame,false),pose::foot_target(frame,true),pose::foot_target(frame,false)};
    r.requested_parts_available=world::transforms(frame,r.requested_parts);
    if(s->window.count==1){
        std::copy(rig.parents.begin(),rig.parents.end(),s->parents.begin());std::copy(rig.bind.begin(),rig.bind.end(),s->bind.begin());
        s->left_arm=rig.left_arm;s->right_arm=rig.right_arm;s->left_leg=rig.left_leg;s->right_leg=rig.right_leg;
        s->hips=rig.hips;s->head=rig.head;s->spine=rig.spine;s->neck=rig.neck;s->shoulders=rig.shoulders;s->toe_base=rig.toe_base;
    }
}
inline Json edge_json(const Edge& e,bool full=false,bool source=false) {
    const auto& a=e.admission;const auto& m=e.motion;
    Json out{{"tick_ms",e.tick},{"thread_id",e.thread},{"listener_clock_available",a.clock},
        {"listener_serial",a.clock?Json(a.serial):Json(nullptr)},{"listener_time",a.clock?Json(a.time):Json(nullptr)},
        {"lease_status_available",m.available},{"lease_active",m.available?Json(m.active):Json(nullptr)},
        {"lease_details_available",m.details_available},{"epoch_valid",m.details_available?Json(m.epoch_valid):Json(nullptr)},
        {"epoch_revision",m.epoch_revision},{"lease_revision",m.lease_revision},
        {"client",m.client},{"entity",m.entity},{"core",m.core},{"context",m.context},{"rig",m.rig},
        {"submitted_serial",m.serial},{"submitted_time",m.simulation_time},{"submitted_clock_available",m.timing_available},
        {"actual_owner_available",a.actual_available},{"actual_owner",owner_array(a.actual_owner)},
        {"actual_base",e.actual_base}};
    if(source)out["source_pose_owner_matches"]=e.source_pose_owner_matches;
    if(full){out["submitted_transform"]=m.transform;out["submitted_position"]=m.position;out["submitted_seconds"]=m.seconds;
        out["submitted_available"]=m.owner_available;out["submitted_offboard"]=m.offboard;out["submitted_standable"]=m.standable;
        out["submitted_motion_supported"]=m.motion_supported;out["requested_target"]=m.target;out["requested_velocity"]=m.velocity;
        out["requested_rotation"]=m.rotation;out["requested_yaw"]=m.yaw;out["full_rotation"]=m.full_rotation;}
    return out;
}
inline void client_publish(const visual::Frame& frame) noexcept {
    ErrorGuard error;
    initialize_client();auto& state=shared();auto* s=state.store.load(std::memory_order_acquire);
    if(!s||GetCurrentThreadId()!=state.client_thread)return;
    state.last_active=frame.active;if(frame.active)state.ever_active=true;
    if(!frame.active)grip_projection_trace::withdraw();
}
inline void request_finish_client() noexcept {
    auto& state=shared();if(state.store.load(std::memory_order_acquire)&&GetCurrentThreadId()==state.client_thread)
        state.finish_requested=true;
}
// Exclusive creation preserves prior PID-named artifacts even under a race.
// Used only by the post-ride client exporter; never by pose callbacks.
struct NewFile {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit NewFile(const std::filesystem::path& path) noexcept {
        handle=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    ~NewFile(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    explicit operator bool() const noexcept {return handle!=INVALID_HANDLE_VALUE;}
    bool write(const void* bytes,std::size_t size) noexcept {
        if(handle==INVALID_HANDLE_VALUE||size>0xffffffffu)return false;
        DWORD written{};return WriteFile(handle,bytes,static_cast<DWORD>(size),&written,nullptr)&&written==size;
    }
    bool close() noexcept {if(handle==INVALID_HANDLE_VALUE)return false;const auto h=handle;handle=INVALID_HANDLE_VALUE;return CloseHandle(h)!=0;}
};
// Called by the existing client begin-tick only after end_ride requested export
// and withdrew its frame. A busy callback defers to another client tick.
inline void poll_client() noexcept {
    ErrorGuard error;
    auto& state=shared();auto* s=state.store.load(std::memory_order_acquire);
    if(!s||GetCurrentThreadId()!=state.client_thread||!state.finish_requested||state.last_active||
       !state.ever_active||state.export_attempted)return;
    if(s->busy.test_and_set(std::memory_order_acquire))return;
    state.export_attempted=true;s->window.sealed=true;s->capture_done.store(true,std::memory_order_release);
    struct Unlock {Store* p;~Unlock(){p->busy.clear(std::memory_order_release);}} unlock{s};
    try {
        Json rows=Json::array();const auto prefix=std::string("bmx-grip-probe-")+std::to_string(GetCurrentProcessId());
        const auto binary=state.root/(prefix+".bones");const auto index=state.root/(prefix+".json");
        if(!state.process_creation||std::filesystem::exists(binary)||std::filesystem::exists(index))return;
        NewFile data(binary),index_file(index);if(!data||!index_file)return;bool data_ok=true;
        for(unsigned i=0;i<s->window.count;++i){const auto& r=s->records[i];
            const auto start=static_cast<std::uint64_t>(i)*sizeof(Bones)*3;
            for(const auto* b:{&r.original,&r.solved,&r.readback})if(data_ok)data_ok=data.write(b->data(),sizeof(Bones));
            const auto& f=r.frame;
            rows.push_back(Json{{"record",i},{"outcome",static_cast<unsigned>(r.outcome)},
                {"before_source_copy",edge_json(r.source_before,false,true)},{"after_source_copy",edge_json(r.source,true,true)},
                {"source_association_consistent",r.source_association_consistent},{"before_readback",edge_json(r.before)},{"after_readback",edge_json(r.after)},
                {"source_pose_callback_owner",std::array<std::uintptr_t,5>{r.source_pose_owner.base,r.source_pose_owner.entity,r.source_pose_owner.core,r.source_pose_owner.context,r.source_pose_owner.rig}},
                {"fresh_pre_owner",owner_array(r.fresh_pre_owner)},{"fresh_post_owner",owner_array(r.fresh_post_owner)},
                {"component",r.component},{"holder",r.holder},{"buffer",r.buffer},{"borrowed_render_data",r.render_data},
                {"caller_rva",r.caller_rva?Json(r.caller_rva):Json(nullptr)},
                {"index20",r.index20_available?Json(r.index20):Json(nullptr)},
                {"sdk_frame_revision",r.frame_revision},{"sdk_contact_revision",r.contact_revision},
                {"sdk_placement_receipt_revision",r.frame_revision},{"native_prop_geometry_observed",false},
                {"original_byte_offset",start},{"solved_byte_offset",start+sizeof(Bones)},{"readback_byte_offset",start+sizeof(Bones)*2},
                {"normalized_write_candidate",r.normalized},{"fresh_pre_readback_guard_passed",r.fresh_guard_passed},
                {"fresh_post_readback_guard_passed",r.fresh_post_guard_passed},{"readback_unavailable_reason",static_cast<unsigned>(r.unavailable)},
                {"readback_available",r.readback_available},
                {"write_complete",r.write_complete},{"write_failed",r.write_failed},
                {"solver_ready",r.solution.ready},{"solver_hand_error_m",r.solution.hand_error},{"solver_foot_error_m",r.solution.foot_error},
                {"solver_hand_contact",r.solution.hand_contact},{"solver_foot_contact",r.solution.foot_contact},
                {"intended_palms_ankles",r.targets},{"requested_part_matrices_available",r.requested_parts_available},
                {"requested_part_matrices",r.requested_parts},
                {"frame",Json{{"entity",f.entity},{"mount_token",f.mount_token},{"position",f.position},{"yaw",f.yaw},{"pitch",f.pitch},{"roll",f.roll},
                    {"chassis_quaternion",f.chassis_quaternion},{"chassis_quaternion_available",f.chassis_quaternion_available},{"steer",f.steer},
                    {"front_wheel_phase",f.front_wheel_phase},{"rear_wheel_phase",f.rear_wheel_phase},{"crank_phase",f.crank_phase},
                    {"barspin",f.barspin},{"tailwhip",f.tailwhip},{"preload",f.preload},{"front_weight",f.front_weight},{"rear_weight",f.rear_weight},
                    {"airborne",f.airborne},{"barspin_release",f.barspin_release},{"tailwhip_release",f.tailwhip_release},
                    {"movement_id",static_cast<unsigned>(f.movement_id)},{"movement_phase",f.movement_phase},{"movement_progress",f.movement_progress},
                    {"release_left_hand",f.release_left_hand},{"release_right_hand",f.release_right_hand},{"release_left_foot",f.release_left_foot},{"release_right_foot",f.release_right_foot},
                    {"landing_compression",f.landing_compression},{"pedal_effort",f.pedal_effort},{"air_extension_blend",f.air_extension_blend},{"air_extension_available",f.air_extension_available},
                    {"rider_uphill_blend",f.rider_uphill_blend},{"rider_gaze_counterpitch",f.rider_gaze_counterpitch},{"rider_posture_available",f.rider_posture_available},
                    {"movement_bike_pitch",f.movement_bike_pitch},{"movement_bike_roll",f.movement_bike_roll},{"rider_blend",f.rider_blend},
                    {"mount_progress",f.mount_progress},{"mount_from_left",f.mount_from_left}}}});
        }
        const bool closed=data.close();
        Json report{{"schema",2},{"process_id",GetCurrentProcessId()},{"process_creation_filetime",std::to_string(state.process_creation)},
            {"private_opt_in",true},{"capacity",policy::capacity},{"count",s->window.count},
            {"caller_image_base",state.caller_image_base},{"caller_image_verified",state.caller_image_verified},
            {"armed",s->window.armed},{"sealed",s->window.sealed},{"owner_changed",s->window.owner_changed},{"clock_rewound",s->window.clock_rewound},
            {"missing_clock",s->window.missing_clock},{"duplicates",s->window.duplicates},{"serial_gaps",s->window.serial_gaps},{"contention",state.contention.load()},
            {"binary",binary.filename().string()},{"binary_written",data_ok&&closed},{"binary_bytes",s->window.count*sizeof(Bones)*3},
            {"bone_count",bone_count},{"rig_count",rig_count},{"bone_stride",sizeof(pose::Bone)},{"bone_lanes","little_endian_float32_scale4_rotation4_position4"},
            {"window_cue","four_consecutive_observed_listener_serials_with_SDK_Frame_airborne"},
            {"actual_owner_fields",std::array<const char*,5>{"client","entity","core","context","rig"}},
            {"callback_owner_fields",std::array<const char*,5>{"base","entity","core","context","rig"}},
            {"mount_token_boundary","SDK_Frame_presentation_token_often_zero_after_mount_not_native_body_identity"},
            {"boundary","actual_same_address_CPU_pose_buffer_edges_not_atomic_native_or_GPU_completion; SDK_motion_lease_and_part_matrices_are_historical_requests"},
            {"parents",s->parents},{"bind",s->bind},{"left_arm",s->left_arm},{"right_arm",s->right_arm},{"left_leg",s->left_leg},{"right_leg",s->right_leg},
            {"hips",s->hips},{"head",s->head},{"spine",s->spine},{"neck",s->neck},{"shoulders",s->shoulders},{"toe_base",s->toe_base},{"rows",std::move(rows)}};
        const auto text=report.dump();if(text.size()>1024*1024)return;
        (void)index_file.write(text.data(),text.size());
        Json raw=Json::array();
        for(const auto& v:s->metadata){const auto& h=v.header;
            raw.push_back(Json{{"record",v.record_index},{"available",v.available},
                {"state",h.state},{"definition",h.definition},{"resource",h.resource},{"count",h.count},{"active_raw",h.active},
                {"pose",h.pose},{"metadata",h.metadata},{"definition_ref",h.definition_ref},{"bind",h.bind},{"remap",h.remap},
                {"parent50",h.parent50},{"parent70",h.parent70},{"meta08_raw",h.meta08},{"cutoff_raw",h.cutoff},
                {"parent70_header_raw",h.parent70_header},{"state50_raw_words",h.state50_words},
                {"remap_prefix_readable",v.remap_readable},{"selected_parent_prefix_readable",v.parents_readable},
                {"remap_prefix_raw_u32",v.remap_prefix},{"selected_parent_prefix_raw_u32",v.parent_prefix}});
        }
        Json meta{{"schema",1},{"process_id",GetCurrentProcessId()},{"process_creation_filetime",std::to_string(state.process_creation)},
            {"grip_index",index.filename().string()},{"raw_prefix_lanes",rig_count},{"observations",std::move(raw)},
            {"boundary","Two passive raw readable prefixes; no complete allocation extent, identity remap, native getter execution or GPU/world transform semantics proved. Existing CPU owner/clock guards and repeated scalar/header equality required."}};
        const auto metadata_text=meta.dump();
        if(metadata_text.size()<=32*1024){NewFile file(state.root/(prefix+"-metadata.json"));if(file)(void)file.write(metadata_text.data(),metadata_text.size());}
        if(grip_projection_trace::state.initialized.load(std::memory_order_acquire)) {
            const auto projection=grip_projection_trace::export_snapshot().dump();
            if(projection.size()<=2*1024*1024){NewFile file(state.root/(prefix+"-projection.json"));if(file)(void)file.write(projection.data(),projection.size());}
        }
    }catch(...){}
}
}
