#include "bmx_native_motion.h"
#include "Engine/Game/BMX/bmx_owner_observation.h"
#include "Engine/Game/BMX/bmx_pose_comparison_policy.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/offboard_flight.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "Engine/Game/BMX/chassis_transform.h"
#include <cmath>
#include <mutex>

namespace dingosdk::bmx {
namespace {
using namespace client_source::detail;
struct Lease {
    NativeFrame owner;
    std::array<float, 3> velocity{}, target{};
    float yaw{};
    ULONGLONG expires{};
    std::array<float, 4> rotation{0,0,0,1};
    bool full_rotation{};
    std::uint64_t revision{};
    motion_target::Basis basis{motion_target::Basis::legacy_yaw};
};
struct Shared {
    std::mutex mutex;
    Lease lease;
    owner_observation::MotionEpoch epoch; // existing mutex guards lease and revision
    std::atomic<bool> failed{};
    std::atomic<std::uint64_t> updates{};
    std::atomic<std::uint64_t> selected_full_rotation_targets{},preserved_supplied_bases{};
    std::atomic<std::uint64_t> step_off_target_calls{},step_off_basis_word_matches{},step_off_basis_word_mismatches{};
    std::string issue;
    bool observer{}; // acquired only on the verified client thread
};
Shared& shared() { static auto* value = new Shared; return *value; }
const pose_comparison::MotionBasisDecision& motion_basis_comparison() noexcept {
    static const auto decision=[]() noexcept {
        SourceLastError error;
        try {
            wchar_t requested[32]{};const auto length=GetEnvironmentVariableW(L"RESKATE_BMX_MOTION_BASIS_COMPARISON",requested,32);
            if(!length||std::wstring_view(requested,length<32?length:0)==L"Default")return pose_comparison::MotionBasisDecision{};
            if(length>=32)return pose_comparison::MotionBasisDecision{pose_comparison::MotionBasis::Default,false,true};
            const auto flag=[](const wchar_t* name) noexcept {wchar_t flag_value[2]{};
                return GetEnvironmentVariableW(name,flag_value,2)==1&&flag_value[0]==L'1';};
            const bool development=user_data::development_requested();
            const auto root=development?user_data::override_root():std::optional<std::filesystem::path>{};
            wchar_t impact[16]{},pose[32]{};
            const auto impact_length=GetEnvironmentVariableW(L"RESKATE_BMX_IMPACT_TEST",impact,16);
            const auto pose_length=GetEnvironmentVariableW(L"RESKATE_BMX_POSE_COMPARISON",pose,32);
            const pose_comparison::Scope scope{development,
                root&&_wcsicmp(root->filename().c_str(),L"bmx-user-data-chassis-probe")==0,
                flag(L"RESKATE_BMX_POSTFALL_REMOUNT_TEST"),flag(L"RESKATE_BMX_HANDOFF_PHASE_PROBE"),
                flag(L"RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE"),
                impact_length==4&&std::wstring_view(impact,impact_length)==L"side",
                flag(L"RESKATE_BMX_GETUP_PULSE")&&!flag(L"RESKATE_BMX_IMPACT_RECOVERY"),
                flag(L"RESKATE_BMX_AUTOMATED_RIDE"),flag(L"RESKATE_BMX_NATIVE_CHASSIS")};
            const auto pose_choice=pose_comparison::select(pose_length<32?std::wstring_view(pose,pose_length):L"invalid",scope);
            return pose_comparison::select_motion_basis(std::wstring_view(requested,length),pose_choice,scope);
        }catch(...){return pose_comparison::MotionBasisDecision{pose_comparison::MotionBasis::Default,false,true};}
    }();
    return decision;
}
void motion_failed(const char* issue) noexcept {
    auto& s = shared();
    s.failed.store(true);
    try {
        std::lock_guard lock(s.mutex);
        s.issue = issue;
    } catch (...) { }
}
struct TimeFeed {
    std::mutex mutex;
    std::uintptr_t entity{}, core{};
    std::uint64_t serial{};
    double seconds{};
    ULONGLONG observed{};
};
TimeFeed& time_feed() { static auto* value = new TimeFeed; return *value; }
void physics_step(const NoBailSkater& skater, float seconds, bool) noexcept {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > .1f) return;
    auto& feed = time_feed();
    std::lock_guard lock(feed.mutex);
    if (feed.entity != skater.entity || feed.core != skater.core) {
        feed.entity = skater.entity; feed.core = skater.core; feed.serial = 0; feed.seconds = 0;
    }
    ++feed.serial;
    feed.seconds += seconds;
    feed.observed = GetTickCount64();
}
bool current(Lease& result) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.lease.expires || GetTickCount64() >= s.lease.expires || s.failed.load()) return false;
    result = s.lease;
    return true;
}
bool revision_current(const Lease& lease) {
    auto& s=shared();std::lock_guard lock(s.mutex);
    return s.epoch.valid(lease.revision)&&s.lease.expires&&GetTickCount64()<s.lease.expires&&
        s.lease.revision==lease.revision&&!s.failed.load();
}
bool finite3(const std::array<float, 3>& values, float limit) {
    return std::all_of(values.begin(), values.end(), [=](float f) { return std::isfinite(f) && std::abs(f) <= limit; });
}
template<std::size_t N> bool finite_passive_values(const std::array<float,N>& values,
    std::size_t begin=0,std::size_t count=N) noexcept {
    if(begin>N||count>N-begin)return false;
    for(std::size_t i=begin;i<begin+count;++i)
        if(!std::isfinite(values[i])||std::abs(values[i])>1000000)return false;
    return true;
}
// Bounded passive watch storage; this diagnostic does not add the generic
// SourceReader's per-observation vector allocation to the fall sampler.
class PassiveRiderReader {
public:
    template<class T> bool optional_value(std::uintptr_t address,std::uintptr_t offset,T& result) {
        static_assert(sizeof(T)<=16);
        if(!source_range(address,1)||offset>source_highest-address)return false;
        address+=offset;T observed{};
        if(!source_range(address,sizeof(T))||count_>=watches_.size()||
            !memory::peek_bytes(address,&observed,sizeof(observed)))return false;
        auto& watch=watches_[count_++];watch.address=address;watch.size=sizeof(observed);
        std::memcpy(watch.bytes.data(),&observed,sizeof(observed));result=observed;return true;
    }
    template<class T> T value(std::uintptr_t address,std::uintptr_t offset=0) {
        T result{};
        source_require(optional_value(address,offset,result),"Passive rider read unavailable.");
        return result;
    }
    std::uintptr_t pointer(std::uintptr_t address,std::uintptr_t offset=0) {return value<std::uintptr_t>(address,offset);}
    bool optional_float4(std::uintptr_t address,std::uintptr_t offset,std::array<float,4>& values,
        std::array<std::uint32_t,4>& words) {
        if(!optional_value(address,offset,words))return false;
        std::memcpy(values.data(),words.data(),sizeof(words));return true;
    }
    bool optional_matrix(std::uintptr_t address,std::uintptr_t offset,std::array<float,16>& result,
        std::array<std::uint32_t,16>& words,bool& translation_read) {
        bool complete=true;translation_read=false;
        for(std::size_t row=0;row<4;++row) {
            std::array<std::uint32_t,4> observed{};
            const bool read_ok=optional_value(address,offset+row*16,observed);
            complete=complete&&read_ok;
            if(read_ok) {
                std::copy(observed.begin(),observed.end(),words.begin()+row*4);
                std::memcpy(result.data()+row*4,observed.data(),sizeof(observed));
            }
            if(row==3)translation_read=read_ok;
        }
        return complete;
    }
    void verify() const {
        for(std::size_t i=0;i<count_;++i) {
            const auto& watch=watches_[i];std::array<unsigned char,16> after{};
            source_require(memory::peek_bytes(watch.address,after.data(),watch.size)&&
                std::memcmp(after.data(),watch.bytes.data(),watch.size)==0,"Passive rider state changed during read.");
        }
    }
private:
    // At most 61 watches for ordinary + handoff lanes, including all four
    // selected PhysicsRig owner/position/flags records and root-layout guards.
    std::array<SourceWatch,96> watches_{};std::size_t count_{};
};
unsigned offboard_motion_state(SourceReader& reader, std::uintptr_t base, std::uintptr_t core) {
    const auto parent = reader.pointer(core, 0x3b0);
    source_require(reader.pointer(parent) == base + addr::offboard_flight::offboard_flight_vtable,
                   "BMX offboard state ownership changed.");
    const auto active = reader.pointer(parent, 0x48);
    const auto& states = addr::offboard_flight::offboard_flight_states;
    for (unsigned i = 0; i < states.size(); ++i)
        if (active == parent + states[i].offset && reader.pointer(active) == base + states[i].vtable_rva) return i + 1;
    return 0;
}
NoclipBodies validate(const Lease& lease) {
    auto& state = source_state();
    source_require(state.initialized.load(std::memory_order_acquire) &&
        state.velocity_guard_active.load(std::memory_order_acquire), "BMX native hooks are unavailable.");
    SourceReader reader;
    const auto phase = reader.value<std::uint32_t>(lease.owner.client, 0xc4);
    source_require(phase == 13 || phase == 21, "BMX interrupted by a loading transition.");
    const auto& debug = state.trial.debug;
    source_require(!debug.noclip && !debug.park_editor && !debug.camera_owned && !debug.first_person,
                   "BMX motion lost exclusive ownership.");
    const auto bodies = debug_noclip_bodies(state.trial.base, lease.owner.client, lease.owner.entity);
    source_require(bodies.offboard && bodies.core == lease.owner.core && bodies.context == lease.owner.context &&
                   bodies.rig_wrapper == lease.owner.rig, "BMX rider physics changed.");
    source_require(offboard_motion_state(reader, state.trial.base, bodies.core) != 0,
                   "BMX interrupted by a native animation or ragdoll state.");
    source_require(reader.value<std::uint8_t>(lease.owner.entity, 0x7e0) == 0, "BMX interrupted by teleport.");
    source_require(finite3(lease.target, 1000000) && finite3(lease.velocity, 30), "BMX motion is out of bounds.");
    std::array<float, 4> checked_rotation{};
    source_require(!lease.full_rotation || chassis::unit_quaternion(lease.rotation, checked_rotation),
                   "BMX chassis rider orientation is invalid.");
    float distance2{};
    for (unsigned axis = 0; axis != 3; ++axis) {
        const float delta = lease.target[axis] - bodies.root[axis];
        distance2 += delta * delta;
    }
    // A loading/respawn/foreign teleport cannot drag the old bike across the world.
    source_require(std::isfinite(distance2) && distance2 < 4.0f, "BMX rider moved away from the bike.");
    reader.verify();
    return bodies;
}
std::array<float, 3> velocity_for(const Lease& lease, const NoclipBodies& bodies) {
    auto result = lease.velocity;
    for (unsigned axis = 0; axis != 3; ++axis) {
        // Bounded spring correction follows the physical rider. It never writes a transform
        // or integrates a target beyond the current native root.
        result[axis] += std::clamp((lease.target[axis] - bodies.root[axis]) * 8.0f, -2.0f, 2.0f);
    }
    return result;
}
bool guarded_copy(std::uintptr_t address, const void* source, std::size_t bytes) noexcept {
    __try { std::memcpy(reinterpret_cast<void*>(address), source, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> void write_body(std::uintptr_t address, const T& value) {
    source_require(guarded_copy(address, &value, sizeof(value)), "BMX body write failed.");
    T check{};
    source_require(memory::peek(address, check) && check == value, "BMX body write verification failed.");
}
}

bool enable_native_motion(std::uintptr_t base) noexcept {
    try {
        (void)motion_basis_comparison(); // immutable private selection before any motion lease/hook use
        initialize_client_source_spawn(base);
        if (!start_no_bail(base)) return false;
        if (!shared().observer) {
            if (!add_no_bail_step_listener(&physics_step)) return false;
            shared().observer = true;
        }
        const bool ready = start_client_noclip_velocity(base);
        if (ready) {
            std::lock_guard lock(shared().mutex);
            shared().issue.clear();
            shared().failed.store(false);
        }
        return ready;
    } catch (...) { return false; }
}
bool read_native_clock(std::uintptr_t entity, std::uintptr_t core,
                       std::uint64_t& serial, double& simulation_time) noexcept {
    try {
        auto& feed = time_feed();
        std::lock_guard lock(feed.mutex);
        if (!entity || !core || feed.entity != entity || feed.core != core ||
            GetTickCount64() - feed.observed >= 250 || !std::isfinite(feed.seconds)) return false;
        serial = feed.serial;
        simulation_time = feed.seconds;
        return true;
    } catch (...) { return false; }
}
bool read_native_clock_try(std::uintptr_t entity,std::uintptr_t core,
                           std::uint64_t& serial,double& simulation_time) noexcept {
    try {
        auto& feed=time_feed();std::unique_lock lock(feed.mutex,std::try_to_lock);
        if(!lock.owns_lock()||!entity||!core||feed.entity!=entity||feed.core!=core||
           GetTickCount64()-feed.observed>=250||!std::isfinite(feed.seconds))return false;
        serial=feed.serial;simulation_time=feed.seconds;return true;
    }catch(...) {return false;}
}
NativeFrame read_native_frame(std::uintptr_t base, std::uintptr_t client) noexcept {
    NativeFrame frame;
    frame.client = client;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load() || state.trial.base != base || state.busy.test_and_set(std::memory_order_acquire)) {
        frame.issue = "Native BMX movement is busy or unavailable.";
        return frame;
    }
    SourceBusyScope scope{state.busy};
    const char* stage = "local player";
    try {
        overlay::DebugModel skater;
        frame.transform = debug_skater(base, client, skater);
        frame.entity = skater.skater_identity;
        stage = "skater physics";
        const auto bodies = debug_noclip_bodies(base, client, frame.entity);
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(frame.entity, 0x7e0) == 0, "Wait for the teleport to finish.");
        reader.verify();
        const auto& debug = state.trial.debug;
        source_require(!debug.noclip && !debug.park_editor && !debug.camera_owned && !debug.first_person,
                       "Close Freecam, Noclip, First Person and Park Editor before mounting BMX.");
        frame.core = bodies.core;
        frame.context = bodies.context;
        frame.rig = bodies.rig_wrapper;
        frame.position = bodies.root;
        frame.seconds = bodies.seconds;
        {
            auto& feed = time_feed();
            std::lock_guard lock(feed.mutex);
            frame.timing_available = feed.entity == frame.entity && feed.core == frame.core && GetTickCount64() - feed.observed < 250;
            frame.serial = feed.serial; frame.simulation_time = feed.seconds;
        }
        frame.offboard = bodies.offboard;
        if (bodies.offboard) {
            const auto motion_state = offboard_motion_state(reader, base, bodies.core);
            frame.standable = motion_state == 1;
            frame.motion_supported = motion_state != 0;
        }
        reader.verify();
        frame.available = true;
    } catch (const SourceGuard& issue) { frame.issue = std::string(stage) + ": " + issue.message; }
      catch (...) { frame.issue = "Native BMX rider observation failed."; }
    return frame;
}
NativeRiderObservation observe_native_rider(std::uintptr_t base,const NativeFrame& frame,
    bool nonblocking_clock,bool handoff_details) noexcept {
    NativeRiderObservation result;
    SourceLastError error;
    if(!frame.available||!frame.timing_available)return result;
    try {
        const auto clock=nonblocking_clock?&read_native_clock_try:&read_native_clock;
        std::uint64_t before_serial{},after_serial{};double before_time{},after_time{};
        if(!clock(frame.entity,frame.core,before_serial,before_time)||
           before_serial!=frame.serial||before_time!=frame.simulation_time)return result;
        NoBailSkater owner;bool protected_owner{};
        if(!inspect_no_bail_owner(frame.client,frame.entity,owner,protected_owner)||owner.base!=base||
           owner.core!=frame.core||owner.context!=frame.context||owner.rig!=frame.rig)return result;
        PassiveRiderReader reader;
        result.physics_state=reader.value<std::uint32_t>(frame.context,0x1414);
        result.offboard_flags=reader.value<std::uint32_t>(frame.context,0x9c0);
        result.impact_flags=reader.value<std::uint32_t>(frame.context,0x13c4);
        result.animation_request_mask=reader.value<std::uint32_t>(frame.context,0x13d4);
        const auto parent=reader.pointer(frame.core,0x3b0);
        const bool current_offboard=frame.offboard&&result.physics_state==504&&
            reader.pointer(parent)==base+addr::offboard_flight::offboard_flight_vtable&&
            reader.pointer(parent,8)==frame.context&&reader.pointer(parent,0x18)==frame.rig&&
            reader.pointer(parent,0x10)==reader.pointer(frame.core,0x430);
        if(current_offboard) {
            const auto active=reader.pointer(parent,0x48);
            const auto pending=reader.value<std::uintptr_t>(parent,0x50);
            if(active>=parent&&active-parent<=0xd50) {
                const auto vtable=reader.pointer(active);
                if(vtable>=base&&vtable-base<0x09144000) {
                    result.state_available=true;result.active_offset=active-parent;
                    result.active_vtable_rva=vtable-base;
                    if(pending>=parent&&pending-parent<=0xd50) {
                        result.pending_offset_available=true;result.pending_offset=pending-parent;
                    }
                    const auto locked=reader.value<std::uint8_t>(parent,0xcf0);
                    result.transition_lock_available=locked<=1;result.transition_lock=locked;
                    const auto& moving=addr::offboard_flight::offboard_flight_states;
                    for(unsigned i=0;i<moving.size();++i)
                        if(active==parent+moving[i].offset&&vtable==base+moving[i].vtable_rva)result.state_kind=i+1;
                    const auto& ragdolls=addr::offboard_flight::offboard_ragdoll_states;
                    for(unsigned i=0;i<ragdolls.size();++i)
                        if(active==parent+ragdolls[i][0]&&vtable==base+ragdolls[i][1])result.state_kind=i+4;
                    if(result.state_kind) {
                        result.motion_velocity=reader.value<std::array<float,3>>(active,0x10);
                        result.velocity_available=finite3(result.motion_velocity,1000);
                    }
                    if(handoff_details&&result.state_kind>=4&&result.state_kind<=6) {
                        result.active_160_raw_available=reader.optional_float4(active,0x160,result.active_160,
                            result.active_160_words);
                        result.active_160_available=result.active_160_raw_available&&finite_passive_values(result.active_160);
                        result.active_160_xyz_available=result.active_160_raw_available&&finite_passive_values(result.active_160,0,3);
                    }
                }
            }
        }
        if(current_offboard) {
            const auto trick=reader.pointer(frame.core,0x3b8),offboard=reader.pointer(trick,0x78);
            result.ragdoll_byte=reader.value<std::uint8_t>(offboard,0x18c);
            result.ragdoll_available=result.ragdoll_byte<=1;
        }
        if(handoff_details) {
            // Repeat debug_skater's owned root-layout proof with fixed storage;
            // no generic SourceReader allocation or native helper is involved.
            const auto collection=reader.pointer(frame.entity,0x70);
            source_require(reader.pointer(collection)==frame.entity,"Passive handoff root owner changed.");
            const auto count=reader.value<std::uint8_t>(collection,8);
            const auto first=reader.value<std::uint8_t>(collection,9);
            const auto extra=reader.value<std::uint8_t>(collection,10);
            source_require(count<=128&&first<=128&&extra<=32,"Passive handoff root layout changed.");
            const auto root_offset=std::uintptr_t{0x40}+(std::uintptr_t{first}+2*std::uintptr_t{extra})*0x20;
            source_require(reader.value<std::array<float,3>>(collection,root_offset)==frame.position&&
                reader.value<std::uint8_t>(frame.entity,0x7e0)==0,"Passive handoff rider root changed.");
            result.context_830_raw_available=reader.optional_float4(frame.context,0x830,result.context_830,
                result.context_830_words);
            result.context_830_available=result.context_830_raw_available&&finite_passive_values(result.context_830);
            result.context_830_xyz_available=result.context_830_raw_available&&finite_passive_values(result.context_830,0,3);
            // FollowRagdoll may replace +830 with this candidate when raw
            // offboard_flags mask0x4 is set; no ground/contact meaning is assigned.
            result.context_7c0_raw_available=reader.optional_float4(frame.context,0x7c0,result.context_7c0,
                result.context_7c0_words);
            result.context_7c0_available=result.context_7c0_raw_available&&finite_passive_values(result.context_7c0);
            result.context_7c0_xyz_available=result.context_7c0_raw_available&&finite_passive_values(result.context_7c0,0,3);
            bool translation_read{};
            result.rig_4720_raw_available=reader.optional_matrix(frame.rig,0x4720,result.rig_4720,
                result.rig_4720_words,translation_read);
            result.rig_4720_available=result.rig_4720_raw_available&&finite_passive_values(result.rig_4720);
            result.rig_4720_translation_available=translation_read&&finite_passive_values(result.rig_4720,12,3);
            result.rig_4760_raw_available=reader.optional_matrix(frame.rig,0x4760,result.rig_4760,
                result.rig_4760_words,translation_read);
            result.rig_4760_available=result.rig_4760_raw_available&&finite_passive_values(result.rig_4760);
            result.rig_4760_translation_available=translation_read&&finite_passive_values(result.rig_4760,12,3);
            // Existing debug_noclip_bodies contract: typed PhysicalRig at
            // wrapper+2f10, 26 records at +20, stride130 and owner at +10.
            std::uintptr_t physical_vtable{};std::uint32_t body_count{};
            result.physical_rig_available=reader.optional_value(frame.rig,0x2f10,result.physical_rig)&&
                source_object(result.physical_rig)&&reader.optional_value(result.physical_rig,0,physical_vtable)&&
                physical_vtable==base+spawn::rig_physics_vtable&&
                reader.optional_value(result.physical_rig,0x20,result.physical_rig_parts)&&
                source_object(result.physical_rig_parts)&&reader.optional_value(result.physical_rig_parts,0,body_count)&&
                body_count==26;
            constexpr std::array<std::uint32_t,4> indices{0,1,12,23};
            for(std::size_t i=0;i<indices.size();++i) {
                auto& body=result.physical_rig_bodies[i];body.index=indices[i];
                if(!result.physical_rig_available)continue;
                const auto address=result.physical_rig_parts+std::uintptr_t{body.index}*0x130;
                std::uintptr_t body_owner{};
                if(!reader.optional_value(address,0x10,body_owner)||body_owner!=result.physical_rig)continue;
                body.position_raw_available=reader.optional_float4(address,0x50,body.position,body.position_words);
                body.position_xyz_available=body.position_raw_available&&finite_passive_values(body.position,0,3);
                body.flags_available=reader.optional_value(address,0x60,body.flags);
                body.available=body.position_raw_available&&finite_passive_values(body.position)&&body.flags_available;
            }
        }
        reader.verify();
        NoBailSkater final_owner;bool final_protection{};
        if(!inspect_no_bail_owner(frame.client,frame.entity,final_owner,final_protection)||
           final_owner.base!=owner.base||final_owner.entity!=owner.entity||final_owner.core!=owner.core||
           final_owner.context!=owner.context||final_owner.rig!=owner.rig||final_protection!=protected_owner||
           !clock(frame.entity,frame.core,after_serial,after_time)||
           after_serial!=before_serial||after_time!=before_time)return {};
        result.protected_owner=protected_owner;result.available=true;
    }catch(...) { return {}; }
    return result;
}
NativeMotionLeaseStatus native_motion_lease_status(bool details) noexcept {
    try {
        auto& state=shared();std::unique_lock lock(state.mutex,std::try_to_lock);
        if(!lock.owns_lock())return {};
        NativeMotionLeaseStatus out;out.available=true;
        out.active=state.lease.expires&&GetTickCount64()<state.lease.expires&&!state.failed.load();
        if(details){
            const auto& l=state.lease;const auto& f=l.owner;
            out.details_available=true;out.lease_revision=l.revision;out.epoch_revision=state.epoch.value();out.epoch_valid=state.epoch.valid(l.revision);
            out.client=f.client;out.entity=f.entity;out.core=f.core;out.context=f.context;out.rig=f.rig;
            out.transform=f.transform;out.position=f.position;out.seconds=f.seconds;out.serial=f.serial;out.simulation_time=f.simulation_time;
            out.timing_available=f.timing_available;out.owner_available=f.available;out.offboard=f.offboard;out.standable=f.standable;out.motion_supported=f.motion_supported;
            out.velocity=l.velocity;out.target=l.target;out.rotation=l.rotation;out.yaw=l.yaw;out.full_rotation=l.full_rotation;
        }
        return out;
    }catch(...) {return {};}
}
bool submit_native_motion(const NativeFrame& frame, const std::array<float, 3>& velocity,
                          const std::array<float, 3>& target, float yaw) noexcept {
    if (!frame.available || !frame.offboard || !finite3(velocity, 28) || !finite3(target, 1000000) || !std::isfinite(yaw)) return false;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.failed.load()) return false;
    s.lease = {frame, velocity, target, yaw, GetTickCount64() + 200};
    s.lease.revision=s.epoch.value();
    return true;
}
bool submit_native_step_off_motion(const NativeFrame& frame,const std::array<float,3>& velocity,
                                   const std::array<float,3>& target,float yaw) noexcept {
    if(!frame.available||!frame.offboard||!finite3(velocity,28)||!finite3(target,1000000)||!std::isfinite(yaw))return false;
    auto& s=shared();
    std::lock_guard lock(s.mutex);
    if(s.failed.load())return false;
    s.lease={frame,velocity,target,yaw,GetTickCount64()+200};
    s.lease.basis=motion_target::Basis::native_step_off;
    s.lease.revision=s.epoch.value();
    return true;
}
bool submit_native_chassis_motion(const NativeFrame& frame, const std::array<float, 3>& velocity,
                                 const std::array<float, 3>& target,
                                 const std::array<float, 4>& rotation) noexcept {
    std::array<float, 4> checked{};
    if (!frame.available || !frame.offboard || !finite3(velocity, 28) || !finite3(target, 1000000) ||
        !chassis::unit_quaternion(rotation, checked)) return false;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.failed.load()) return false;
    s.lease = {frame, velocity, target, 0, GetTickCount64() + 200, checked, true};
    s.lease.revision=s.epoch.value();
    return true;
}
void release_native_motion() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.lease.expires = 0;
    if(!s.epoch.revoke())s.failed.store(true);
}
bool native_motion_healthy() noexcept { return !shared().failed.load(); }
std::string native_motion_issue() {
    std::lock_guard lock(shared().mutex);
    return shared().issue;
}
std::uint64_t native_motion_updates() noexcept { return shared().updates.load(); }
NativeMotionBasisStatus native_motion_basis_status() noexcept {
    SourceLastError error;const auto& choice=motion_basis_comparison();const auto& s=shared();
    return {pose_comparison::name(choice.mode),choice.qualified_scope,choice.refused,
        s.selected_full_rotation_targets.load(std::memory_order_relaxed),s.preserved_supplied_bases.load(std::memory_order_relaxed),
        s.step_off_target_calls.load(std::memory_order_relaxed),s.step_off_basis_word_matches.load(std::memory_order_relaxed),
        s.step_off_basis_word_mismatches.load(std::memory_order_relaxed)};
}
void apply_native_velocity(std::uintptr_t core) noexcept {
    Lease lease;
    if (!current(lease) || lease.owner.core != core) return;
    SourceLastError error;
    DINGO_PROFILE_ZONE("hooks/BMX/rider velocity");
    auto& state = source_state();
    if (state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    if(!revision_current(lease))return;
    try {
        const auto bodies = validate(lease);
        if (bodies.seconds <= 0) return;
        const auto velocity = velocity_for(lease, bodies);
        SourceReader reader;
        std::array<std::uint32_t, 32> flags{};
        for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
            source_require(source_writable(bodies.parts[i] + 0x60, 0x20), "BMX body is not writable.");
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        source_require(sync_offboard_flight_velocity(state.trial.base, core, bodies.context, bodies.rig_wrapper, velocity),
                       "BMX offboard motion ownership changed.");
        for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
            write_body(bodies.parts[i] + 0x70, velocity);
            write_body(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
        shared().updates.fetch_add(1);
    } catch (const SourceGuard& issue) { motion_failed(issue.message); }
      catch (...) { motion_failed("BMX native body update failed."); }
}
bool native_motion_target(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float, 16>* supplied, std::array<float, 16>& target) noexcept {
    Lease lease;
    if (!current(lease) || lease.owner.rig != rig || lease.owner.context != context) return false;
    SourceLastError error;
    DINGO_PROFILE_ZONE("hooks/BMX/rider target");
    auto& state = source_state();
    if (state.busy.test_and_set(std::memory_order_acquire)) return false;
    SourceBusyScope scope{state.busy};
    if(!revision_current(lease))return false;
    try {
        const auto bodies = validate(lease);
        if (bodies.seconds <= 0) return false;
        SourceReader reader;
        source_require(reader.raw(reinterpret_cast<std::uintptr_t>(supplied), target.data(), sizeof(target)) &&
                       valid_flight_transform(target), "Invalid BMX native motion target.");
        const auto velocity = velocity_for(lease, bodies);
        const auto supplied_basis=target; // exact already-guarded incoming target; no extra native read
        if (lease.full_rotation) {
            std::array<float, 4> rotation{};
            source_require(chassis::unit_quaternion(lease.rotation, rotation), "Invalid measured chassis rotation.");
            target = {}; target[15] = 1;
            // Keep the on-foot physics rig in its native supplied orientation.
            // Applying chassis pitch/roll here displaced physical skeleton parts
            // by 17 m in the matched 180/181 probe. The bike and custom rider pose
            // still use the full measured chassis rotation in their own paths.
            for (unsigned column = 0; column != 3; ++column) {
                for (unsigned row = 0; row != 3; ++row)
                    target[column * 4 + row] = supplied_basis[column * 4 + row];
            }
        } else {
            target=motion_target::yaw_basis(supplied_basis,lease.yaw,lease.basis);
        }
        const bool preserved_basis=pose_comparison::preserve_motion_basis(motion_basis_comparison(),lease.full_rotation,supplied_basis,target);
        for (unsigned axis = 0; axis != 3; ++axis) target[12 + axis] = bodies.root[axis] + velocity[axis] * bodies.seconds;
        source_require(valid_flight_transform(target), "Invalid BMX target bounds.");
        reader.verify();
        source_require(sync_offboard_flight_velocity(state.trial.base, bodies.core, context, rig, velocity),
                       "BMX offboard motion ownership changed.");
        if(!lease.full_rotation&&lease.basis==motion_target::Basis::native_step_off) {
            motion_target::count_bounded(shared().step_off_target_calls);
            // Compare only the nine exact source words already read/validated
            // above, after final translation. Never read native memory again.
            motion_target::count_bounded(motion_target::same_basis_words(supplied_basis,target)?
                shared().step_off_basis_word_matches:shared().step_off_basis_word_mismatches);
        }
        if(lease.full_rotation)shared().selected_full_rotation_targets.fetch_add(1,std::memory_order_relaxed);
        if(preserved_basis)shared().preserved_supplied_bases.fetch_add(1,std::memory_order_relaxed);
        return true;
    } catch (const SourceGuard& issue) { motion_failed(issue.message); return false; }
      catch (...) { motion_failed("BMX native motion target failed."); return false; }
}
}
