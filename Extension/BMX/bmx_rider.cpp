#include "bmx_rider.h"
#include "bmx_coupled_pose_debug.h"
#include "bmx_board_visibility.h"
#include "bmx_rider_pose.h"
#include "bmx_pose_write.h"
#include "bmx_skeleton_phase_probe.h"
#include "bmx_grip_probe.h"
#include "bmx_board_handoff_trace.h"
#include "bmx_native_chassis.h"
#include "Engine/Game/BMX/bmx_pose_comparison_policy.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_skeleton.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/skeleton_asset.h"
#include "Engine/Vfs/game_bundles.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/no_bail.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dingosdk::bmx::rider {
namespace {
using Bone=pose::Bone;
static_assert(sizeof(Bone)==pose_write::bone_stride &&
    offsetof(Bone,scale)==pose_write::scale_offset && offsetof(Bone,rotation)==pose_write::rotation_offset &&
    offsetof(Bone,position)==pose_write::position_offset);
struct Layout { frostbite::Skeleton skeleton; pose::Rig rig; };
struct Shared {
    std::mutex mutex; visual::Frame frame; std::chrono::steady_clock::time_point at;
    std::atomic<std::shared_ptr<const Layout>> layout; std::once_flag preparation;
    std::atomic<bool> registered{};
    std::atomic<bool> board_registered{};
    std::atomic<std::uint64_t> updates{};
    std::atomic<std::uint64_t> render_updates{};
    std::atomic<std::uint64_t> suppressed_animation_applies{},suppressed_render_applies{};
    std::atomic<std::uint64_t> comparison_animation_applies{},comparison_render_applies{};
    std::atomic<std::uint64_t> stale_parent_rejections{};
    std::atomic<std::uint64_t> unpublished_buffer_rejections{};
    std::atomic<float> max_riding_hips_local_norm{};
    std::atomic<float> hand_error{}, foot_error{};
    std::atomic<std::uint64_t> posture_rejections{};
    std::atomic<std::uint64_t> board_updates{};
    std::atomic<bool> profiling{};
    PoseMetrics render_metrics; // mutex protected; only completed rider render writes
    pose::EntryContacts mount_contacts; // token/entity scoped world contacts
    pose::SpringBinding spring_binding; // calibrated only from a completed neutral riding pose
    coupled_debug::Store diagnostic;
    native_render_phase::Receipt spring_presentation; // only a completed actual render write
    std::uint64_t frame_revision{},contact_revision{};
    pose_write::PlacementGate placement; // mutex protected, separate from logical mount contacts
};
Shared& shared() { static auto* value=new Shared;return *value; }
struct LastError { DWORD value=GetLastError(); ~LastError(){SetLastError(value);} };
// Immutable first-use selection from the inherited child-process environment.
// No hot toggle, native synchronization claim or broader pose-write permission.
const pose_comparison::Decision& comparison() noexcept {
    static const auto decision=[]() noexcept {
        LastError error;
        try {
            wchar_t value[32]{};
            const auto length=GetEnvironmentVariableW(L"RESKATE_BMX_POSE_COMPARISON",value,32);
            if(!length)return pose_comparison::select(L"Default",{});
            if(length>=32)return pose_comparison::Decision{pose_comparison::Mode::Default,false,true};
            const auto flag=[](const wchar_t* name) noexcept {
                wchar_t flag_value[2]{};return GetEnvironmentVariableW(name,flag_value,2)==1&&flag_value[0]==L'1';
            };
            const bool development=user_data::development_requested();
            const auto root=development?user_data::override_root():std::optional<std::filesystem::path>{};
            wchar_t impact[16]{};const auto impact_length=GetEnvironmentVariableW(L"RESKATE_BMX_IMPACT_TEST",impact,16);
            const pose_comparison::Scope scope{development,
                root&&_wcsicmp(root->filename().c_str(),L"bmx-user-data-chassis-probe")==0,
                flag(L"RESKATE_BMX_POSTFALL_REMOUNT_TEST"),flag(L"RESKATE_BMX_HANDOFF_PHASE_PROBE"),
                flag(L"RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE"),
                impact_length==4&&std::wstring_view(impact,impact_length)==L"side",
                flag(L"RESKATE_BMX_GETUP_PULSE")&&!flag(L"RESKATE_BMX_IMPACT_RECOVERY"),flag(L"RESKATE_BMX_AUTOMATED_RIDE"),flag(L"RESKATE_BMX_NATIVE_CHASSIS")};
            return pose_comparison::select(std::wstring_view(value,length),scope);
        }catch(...){return pose_comparison::Decision{pose_comparison::Mode::Default,false,true};}
    }();
    return decision;
}
// Explicit inherited opt-in, selected once. The normal pose path remains off.
// This changes candidate targets only; all placement/pose/owner/write gates below remain mandatory.
bool observed_posture_selected() noexcept {
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    static const bool selected=[]() noexcept {LastError error;try {
        wchar_t value[2]{},native[2]{};
        return GetEnvironmentVariableW(L"RESKATE_BMX_OBSERVED_POSTURE",value,2)==1&&value[0]==L'1'&&
            user_data::development_requested()&&user_data::override_root().has_value()&&
            GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_CHASSIS",native,2)==1&&native[0]==L'1';
    }catch(...){return false;}}();return selected;
#else
    return false;
#endif
}
// Cached only from the client publisher, never selected or initialized in a hook.
bool coupled_debug_selected() noexcept {
#if defined(DINGOSDK_BMX_NATIVE_COUPLED) && defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    static const bool value=[]() noexcept {LastError error;try {
        constexpr wchar_t exact[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-coupled-test";
        wchar_t ride[2]{},trial[2]{};
        return user_data::runtime_development_selected(L"bmx-user-data-coupled-test",exact)&&
            GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_RIDE",ride,2)==1&&ride[0]==L'1'&&
            GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_CONTROLLER_TRIAL",trial,2)==1&&trial[0]==L'1';
    }catch(...){return false;}}();return value;
#else
    return false;
#endif
}
void apply_rider_pose(std::uintptr_t component, bool rendering, std::uintptr_t render_data = 0, const NoBailSkater* evidence_owner = nullptr) noexcept {
    LastError error;
    try {
        coupled_debug::Trace trace(shared().diagnostic,rendering&&shared().diagnostic.active.load());
        trace.at(coupled_debug::Stage::layout);const auto layout=shared().layout.load();if(!layout)return;visual::Frame frame;
        pose::EntryContacts entry;pose::SpringBinding spring_binding;std::uint64_t frame_revision{},contact_revision{};
        trace.at(coupled_debug::Stage::publication);
        {std::lock_guard lock(shared().mutex);if(!shared().placement.ready(shared().frame_revision)||!shared().frame.active||
            std::chrono::steady_clock::now()-shared().at>std::chrono::milliseconds(200))return;
            frame=shared().frame;entry=shared().mount_contacts;spring_binding=shared().spring_binding;frame_revision=shared().frame_revision;contact_revision=shared().contact_revision;}
        if(frame.rider_mode==visual::RiderMode::none)return;
        static profiler::Site animation_site{"hooks/BMX/rider pose/animation"},render_site{"hooks/BMX/rider pose/render"};
        const profiler::Scope pose_scope{rendering?render_site:animation_site};
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uintptr_t collection{},owner{},vtable{},cached{},holder{};
        trace.at(coupled_debug::Stage::owner);
        if(!frame.entity||!memory::peek(component,vtable)||vtable!=base+addr::engine::skater_component_vtable||
           !memory::peek(component+0x18,collection)||!memory::peek(collection,owner)||owner!=frame.entity||
           !memory::peek(owner+0x628,cached)||cached!=component||!memory::peek(component+0xa0,holder))return;
        const auto read=[](std::uintptr_t p,void* out,std::size_t size){return memory::peek_bytes(p,out,size);};
        const auto pose=multiplayer::read_native_pose_layout(read,base,holder,512);
        // The source render skeleton names the leading386 joints of the395-joint standard output.
        trace.at(coupled_debug::Stage::pose_layout);
        if(!pose.buffer||pose.count!=395||layout->skeleton.parents.size()!=386)return;
        trace.at(coupled_debug::Stage::published_buffer);
        if(rendering&&!pose_write::published_buffer_matches(read,render_data,pose.buffer)) {
            shared().unpublished_buffer_rejections.fetch_add(1,std::memory_order_relaxed);return;
        }
        const auto evidence_before=rendering?grip_probe::source_edge(evidence_owner):grip_probe::Edge{};
        trace.at(coupled_debug::Stage::bones);
        std::vector<Bone> bones(pose.count);if(!read(pose.buffer,bones.data(),bones.size()*sizeof(Bone)))return;
        const auto original=bones;
        trace.capture(original,layout->rig,frame,entry,spring_binding,observed_posture_selected());
        grip_probe::Token evidence;
        if(rendering)grip_probe::begin(evidence,frame,layout->rig,original,evidence_owner,component,holder,pose.buffer,render_data,frame_revision,contact_revision,evidence_before);
        const bool had_spring_binding=spring_binding.root.ready;
        native_render_phase::Receipt presentation;
        const auto spring_lease_current=[&]() noexcept {
            if(!frame.spring_avatar.requested)return true;
            const auto& epoch=frame.spring_avatar.actual.epoch;
            if(!avatar_binding::valid(frame.spring_avatar.actual)||epoch.owner_world[1]!=owner||epoch.owner_world[2]==0)return false;
            std::uint64_t serial{};double time{};
            const bool clock_available=read_native_clock_try(owner,epoch.owner_world[2],serial,time);
            const auto lease=native_motion_lease_status(true);trace.clock(clock_available,serial,time,lease,epoch);
            const auto admitted=native_render_phase::qualify(frame.spring_avatar,lease,{clock_available,serial,time},rendering);
            if(!admitted.accepted)return false;
            if(had_spring_binding) {
                const auto target=avatar_binding::sample(spring_binding.root,frame.spring_avatar.actual);
                if(!avatar_binding::motion_lease_matches(frame.spring_avatar,target,lease))return false;
                presentation=admitted;return true;
            }
            // Entry's final lease may precede calibration. It must match the
            // actual mounted root witness and exact current owner/clock/revision.
            if(!avatar_binding::close(lease.target,frame.spring_avatar.capture.native.position,.003f))return false;
            presentation=admitted;return true;
        };
        trace.at(coupled_debug::Stage::lease_before);if(!spring_lease_current())return;
        // Native parent-local quaternions compose through AITrajectory into world
        // space (the same contract used by client_first_person.cpp).
        pose::Result solved;
        {
            DINGO_PROFILE_ZONE("hooks/BMX/rider pose/solve");
            trace.at(coupled_debug::Stage::solve);
            solved=pose::solve(bones,layout->rig,frame,&entry,observed_posture_selected(),frame.spring_avatar.requested?&spring_binding:nullptr);
            trace.solved(solved);
        }
        evidence.solution(solved,bones);
        if(solved.posture_requested&&!solved.ready)shared().posture_rejections.fetch_add(1,std::memory_order_relaxed);
        trace.at(coupled_debug::Stage::solved);if(!solved.ready)return;
        trace.at(coupled_debug::Stage::lease_recheck);if(!spring_lease_current())return;
        const auto hips=static_cast<std::size_t>(layout->rig.hips);
        // Recheck the local entity immediately before writing. Native animation supplies a fresh
        // pose next frame, so disable/respawn never restores an old pose through cached pointers.
        std::lock_guard lock(shared().mutex);
        DINGO_PROFILE_ZONE("hooks/BMX/rider pose/write");
        trace.at(coupled_debug::Stage::publication_recheck);
        if(!shared().placement.ready(frame_revision)||!shared().frame.active||shared().frame.entity!=owner||shared().frame.rider_mode!=frame.rider_mode||
           shared().frame.mount_token!=frame.mount_token||
           shared().frame_revision!=frame_revision||shared().contact_revision!=contact_revision||
           std::chrono::steady_clock::now()-shared().at>std::chrono::milliseconds(200))return;
        std::uintptr_t live_owner{},live_component{},live_holder{},core{},core_type{},context{},entity_type{};std::uint32_t state{};
        trace.at(coupled_debug::Stage::owner_recheck);
        if(!memory::peek(collection,live_owner)||live_owner!=owner||!memory::peek(owner,entity_type)||entity_type!=base+addr::engine::skater_entity_vtable||
           !memory::peek(owner+0x628,live_component)||live_component!=component||!memory::peek(component+0xa0,live_holder)||live_holder!=holder||
           !memory::peek(component+0x70,core)||!memory::peek(core,core_type)||core_type!=base+addr::no_bail::bail_core_vtable||
           !memory::peek(core+0x3c0,context)||!memory::peek(context+0x1414,state)||state!=addr::no_bail::offboard_physics_state)return;
        const auto live_pose=multiplayer::read_native_pose_layout(read,base,live_holder,512);
        trace.at(coupled_debug::Stage::pose_recheck);
        if(live_pose.buffer!=pose.buffer||live_pose.count!=pose.count)return;
        if(rendering&&!pose_write::published_buffer_matches(read,render_data,pose.buffer)) {
            shared().unpublished_buffer_rejections.fetch_add(1,std::memory_order_relaxed);return;
        }
        NoBailSkater current;
        if(!no_bail_skater(current)||current.base!=base||current.entity!=owner||current.core!=core||current.context!=context)return;
        // Only changed XYZ/quaternion lanes are written; scale and metadata W lanes
        // remain native-owned. Other bones are never copied back over concurrent work.
        const auto changed=pose::changed(layout->rig);
        for(int index:changed){
            auto& q=bones[static_cast<std::size_t>(index)].rotation;if(q==original[static_cast<std::size_t>(index)].rotation)continue;
            float norm=0;for(float v:q){if(!std::isfinite(v))return;norm+=v*v;}if(norm<.5f||norm>1.5f)return;
            for(auto& v:q)v/=std::sqrt(norm);
        }
        // A pose buffer can stay live while native animation advances its
        // trajectory. Refuse the entire riding solution before any lane write
        // when a source ancestor no longer matches the snapshot we solved.
        trace.at(coupled_debug::Stage::lease_recheck);if(!spring_lease_current())return;
        trace.at(coupled_debug::Stage::ancestors);
        if(frame.rider_mode==visual::RiderMode::riding&&
           !pose_write::ancestors_unchanged<Bone>(read,pose.buffer,pose.count,original,layout->rig.parents,layout->rig.hips)) {
            shared().stale_parent_rejections.fetch_add(1,std::memory_order_relaxed);return;
        }
        if(frame.mount_token&&(shared().mount_contacts.token!=frame.mount_token||shared().mount_contacts.entity!=frame.entity)) {
            shared().mount_contacts=entry;++shared().contact_revision;
        }
        // Passive prefix only; noexcept diagnostics cannot grant/veto any write.
        bool writes_pose=bones[hips].position!=original[hips].position;
        for(int index:changed)writes_pose=writes_pose||bones[static_cast<std::size_t>(index)].rotation!=original[static_cast<std::size_t>(index)].rotation;
        if(writes_pose)skeleton_phase_probe::before_pose_write(component,rendering,owner,frame.mount_token,frame_revision);
        evidence.solution(solved,bones,true);
        if(evidence.record)evidence.record->outcome=grip_probe::Outcome::write_admitted;
        const auto readback_guard=[&](grip_probe_policy::Owner& observed) {
            observed={};
            if(!spring_lease_current())return false;
            // Private observation only. Reuse the existing known owner/layout/
            // published-buffer guards, with no speculative offset or getter.
            if(!shared().placement.ready(frame_revision)||!shared().frame.active||shared().frame.entity!=owner||
               shared().frame.mount_token!=frame.mount_token||shared().frame.rider_mode!=frame.rider_mode||
               shared().frame_revision!=frame_revision||shared().contact_revision!=contact_revision||
               std::chrono::steady_clock::now()-shared().at>std::chrono::milliseconds(200))return false;
            NoBailSkater fresh;std::uintptr_t observed_client{};
            if(!try_no_bail_skater(0,fresh,observed_client))return false;
            observed=grip_probe::owner_key(observed_client,fresh);
            if(!evidence.record||observed!=evidence.record->source.admission.actual_owner||
               observed!=grip_probe_policy::lease_owner(evidence.record->source.admission)||
               fresh.base!=base||fresh.base!=evidence.record->source_pose_owner.base||fresh.entity!=current.entity||fresh.core!=current.core||
               fresh.context!=current.context||fresh.rig!=current.rig)return false;
            std::uintptr_t c{},o{},v{},p{},h{},k{},kt{},ctx{};std::uint32_t st{};
            if(!memory::peek(component,v)||v!=base+addr::engine::skater_component_vtable||
               !memory::peek(component+0x18,c)||c!=collection||!memory::peek(c,o)||o!=owner||
               !memory::peek(owner,v)||v!=base+addr::engine::skater_entity_vtable||!memory::peek(owner+0x628,p)||p!=component||
               !memory::peek(component+0xa0,h)||h!=holder||!memory::peek(component+0x70,k)||k!=core||
               !memory::peek(k,kt)||kt!=base+addr::no_bail::bail_core_vtable||!memory::peek(k+0x3c0,ctx)||ctx!=context||
               !memory::peek(ctx+0x1414,st)||st!=addr::no_bail::offboard_physics_state)return false;
            const auto refreshed=multiplayer::read_native_pose_layout(read,base,h,512);
            return refreshed.buffer==pose.buffer&&refreshed.count==pose.count&&
                pose_write::published_buffer_matches(read,render_data,pose.buffer);
        };
        trace.at(coupled_debug::Stage::write);
        if(bones[hips].position!=original[hips].position)
            if(!spring_lease_current()||!pose_write::write(pose.buffer,pose.count,hips,pose_write::Lane::position,bones[hips].position.data())){evidence.read_back(readback_guard,read,false);return;}
        for(int index:changed){
            const auto& q=bones[static_cast<std::size_t>(index)].rotation;if(q==original[static_cast<std::size_t>(index)].rotation)continue;
            if(!spring_lease_current()||!pose_write::write(pose.buffer,pose.count,static_cast<std::size_t>(index),pose_write::Lane::rotation,q.data())){evidence.read_back(readback_guard,read,false);return;}
        }
        evidence.read_back(readback_guard,read,true);
        if(rendering&&frame.spring_avatar.requested&&spring_lease_current()) {
            spring_binding.completed_pose_write=true;
            shared().spring_binding=spring_binding;
            shared().spring_presentation=presentation;
        }
        if(frame.rider_mode==visual::RiderMode::riding) {
            const auto& position=bones[hips].position;
            const float norm=std::hypot(position[0],position[1],position[2]);
            auto maximum=shared().max_riding_hips_local_norm.load(std::memory_order_relaxed);
            if(std::isfinite(norm))while(norm>maximum&&
                !shared().max_riding_hips_local_norm.compare_exchange_weak(maximum,norm,std::memory_order_relaxed)){}
        }
        trace.at(coupled_debug::Stage::completed);
        shared().updates.fetch_add(1,std::memory_order_relaxed);
        if(rendering){
            const auto count=shared().render_updates.fetch_add(1,std::memory_order_relaxed)+1;
            shared().render_metrics={solved.hand_error,solved.foot_error,owner,GetTickCount64(),count,true,solved.hand_contact,solved.foot_contact,
                frame.movement_id,frame.movement_phase,frame.movement_progress,frame.rider_mode,frame_revision,
                solved.posture_attempts,solved.posture_scale,solved.posture_fit_adjustment,solved.posture_gaze,
                solved.posture_requested,solved.posture_fallback,
                solved.observed_requested,solved.observed_applied,solved.observed_strength};
        }
        shared().hand_error.store(solved.hand_error,std::memory_order_relaxed);
        shared().foot_error.store(solved.foot_error,std::memory_order_relaxed);
    }catch(...){}
}
void on_animation(std::uintptr_t component) noexcept {
    LastError error;
    if(!pose_comparison::dispatch(comparison(),false,[&]() noexcept {
        shared().comparison_animation_applies.fetch_add(1,std::memory_order_relaxed);apply_rider_pose(component,false);}))
        shared().suppressed_animation_applies.fetch_add(1,std::memory_order_relaxed);
}
void on_render(std::uintptr_t animation_interface, std::uintptr_t render_data) noexcept {
    LastError error;
    const board_handoff_policy::ObserverExit passive_publication{[=] {
        if(!board_handoff_trace::store.enabled.load(std::memory_order_acquire)||
           !board_handoff_trace::store.armed.load(std::memory_order_acquire)||
           board_handoff_trace::store.sealed.load(std::memory_order_acquire))return;
        const auto layout=shared().layout.load();std::uint64_t revision{};bool publication_available{},publication_active{};std::uintptr_t publication_entity{};
        {std::unique_lock lock(shared().mutex,std::try_to_lock);if(lock.owns_lock()) {
            revision=shared().frame_revision;publication_available=true;publication_active=shared().frame.active;publication_entity=shared().frame.entity;}}
        board_handoff_trace::published(animation_interface,render_data,layout?&layout->rig:nullptr,
            shared().updates.load(),shared().render_updates.load(),shared().board_updates.load(),revision,publication_available,publication_active,publication_entity);
    },[]() noexcept {board_handoff_trace::seal("passive_publication_exception");}};
    try {
        if(shared().diagnostic.active.load())shared().diagnostic.callbacks.fetch_add(1);
        if(animation_interface<=0xc0)return;
        visual::Frame frame;std::uint64_t frame_revision{};
        {std::lock_guard lock(shared().mutex);if(!shared().placement.ready(shared().frame_revision)||!shared().frame.active||
            std::chrono::steady_clock::now()-shared().at>std::chrono::milliseconds(200))return;
            frame=shared().frame;frame_revision=shared().frame_revision;}
        DINGO_PROFILE_ZONE("hooks/BMX/render callback");
        NoBailSkater owner;
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uint32_t state{};
        if(!frame.entity||!no_bail_skater(owner)||owner.base!=base||owner.entity!=frame.entity||
           !memory::peek(owner.context+0x1414,state)||state!=addr::no_bail::offboard_physics_state)return;
        const auto read=[](std::uintptr_t p,void* out,std::size_t size){return memory::peek_bytes(p,out,size);};
        std::uintptr_t component{},holder{};
        if(memory::peek(owner.entity+0x628,component)&&memory::peek(component+0xa0,holder)&&holder&&animation_interface==holder+0xc0){
            if(shared().diagnostic.active.load())shared().diagnostic.matching_dispatches.fetch_add(1);
            // Reapply at the actual local rider render handoff, after native
            // animation publication. Animation and render share the same guarded pose.
            // Suppress only this custom rider apply; board rendering below and
            // native originals/observers remain outside the private comparison.
            if(!pose_comparison::dispatch(comparison(),true,[&]() noexcept {
                shared().comparison_render_applies.fetch_add(1,std::memory_order_relaxed);apply_rider_pose(component,true,render_data,&owner);}))
                shared().suppressed_render_applies.fetch_add(1,std::memory_order_relaxed);
            return;
        }
        const auto board=multiplayer::read_native_board(read,base,owner.entity);
        if(!board.holder||animation_interface!=board.holder+0xc0)return;
        const auto visual=multiplayer::read_native_board_visual(read,base,board.entity);
        if(visual.holder!=board.holder||!visual.initialized||!visual.enabled)return;
        const auto pose=multiplayer::read_native_pose_layout(read,base,board.holder,64);
        // Board Break independently identifies this exact supported skateboard rig as16 bones.
        if(!pose.buffer||pose.count!=16)return;
        if(!pose_write::published_buffer_matches(read,render_data,pose.buffer))return;
        std::array<Bone,16> bones{};if(!read(pose.buffer,bones.data(),sizeof(bones)))return;
        for(const auto& bone:bones){
            float norm=0;for(float value:bone.rotation){if(!std::isfinite(value))return;norm+=value*value;}
            if(norm<.9f||norm>1.1f)return;
            for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(bone.position[axis])||std::abs(bone.position[axis])>1e6f||
                !std::isfinite(bone.scale[axis])||bone.scale[axis]<0||bone.scale[axis]>20)return;
        }
        // Resolve the current local owner and this board again immediately before rendering.
        // No cached scale is restored: the next native animation output owns that pose.
        std::lock_guard lock(shared().mutex);
        if(!shared().placement.ready(frame_revision)||!shared().frame.active||shared().frame.entity!=owner.entity||
           std::chrono::steady_clock::now()-shared().at>std::chrono::milliseconds(200))return;
        NoBailSkater current;
        if(!no_bail_skater(current)||current.base!=owner.base||current.entity!=owner.entity||current.core!=owner.core||
           current.context!=owner.context||!memory::peek(current.context+0x1414,state)||state!=addr::no_bail::offboard_physics_state)return;
        const auto live=multiplayer::read_native_board(read,base,current.entity);
        if(live.entity!=board.entity||live.component!=board.component||live.holder!=board.holder)return;
        const auto live_pose=multiplayer::read_native_pose_layout(read,base,live.holder,64);
        if(live_pose.buffer!=pose.buffer||live_pose.count!=pose.count)return;
        if(!pose_write::published_buffer_matches(read,render_data,pose.buffer))return;
        // The native pose may retain trajectory/root scales across animation
        // refreshes. Hide only render descendants; never poison either root.
        const auto native_bones=bones;
        if(!apply_mounted_board_hide<Bone>(bones,true))return;
        for(std::size_t i=first_hidden_board_bone;i<bones.size();++i){
            if(std::equal(bones[i].scale.begin(),bones[i].scale.begin()+3,native_bones[i].scale.begin()))continue;
            if(!pose_write::write(pose.buffer,pose.count,i,pose_write::Lane::board_scale,bones[i].scale.data()))return;
        }
        shared().board_updates.fetch_add(1,std::memory_order_relaxed);
    }catch(...){}
}
const frostbite::ebx::Value& field(const frostbite::ebx::Object& object, const char* name) {
    const auto* found=object.find(name);if(!found)throw std::runtime_error("Missing rider bind field");return found->value;
}
const frostbite::ebx::Object& object(const frostbite::ebx::Value& value) {
    const auto* found=std::get_if<std::shared_ptr<frostbite::ebx::Object>>(&value.data);
    if(!found||!*found)throw std::runtime_error("Invalid rider bind object");return **found;
}
float number(const frostbite::ebx::Value& value) {
    if(const auto* found=std::get_if<double>(&value.data))return static_cast<float>(*found);
    if(const auto* found=std::get_if<std::int64_t>(&value.data))return static_cast<float>(*found);
    throw std::runtime_error("Invalid rider bind number");
}
pose::Vec vector(const frostbite::ebx::Value& value) {
    const auto& v=object(value);return {number(field(v,"x")),number(field(v,"y")),number(field(v,"z"))};
}
void prepare(){
    std::thread([]{
        try{
            std::vector<wchar_t> name(32768);const auto size=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));if(!size||size>=name.size())return;
            const vfs::GameData data(std::filesystem::path(std::wstring(name.data(),size)).parent_path());
            namespace skel=addr::skater_skeleton;
            const auto toc=data.read_toc(skel::skeleton_toc);const auto bundle=data.read_bundle(toc,skel::skeleton_bundle);if(!bundle)return;
            std::size_t index{};if(!bundle->find(frostbite::AssetKind::ebx,skel::skeleton_asset,&index))return;
            const auto* payload=bundle->payload(frostbite::AssetKind::ebx,index);if(!payload)return;
            const auto document=frostbite::ebx::read_document(data.read(*payload));if(!document.root()||!document.root()->object)return;
            auto layout=std::make_shared<Layout>();layout->skeleton=frostbite::read_skeleton(*document.root()->object);
            const auto joint=[&](const char* n){const auto it=std::find(layout->skeleton.names.begin(),layout->skeleton.names.end(),n);if(it==layout->skeleton.names.end())throw std::runtime_error("Missing rider joint");return static_cast<int>(it-layout->skeleton.names.begin());};
            auto& rig=layout->rig;rig.parents=layout->skeleton.parents;
            rig.hips=joint("Hips");rig.head=joint("Head");
            rig.spine={joint("Spine"),joint("Spine1"),joint("Spine2"),joint("Spine3")};
            rig.shoulders={joint("LeftShoulder"),joint("RightShoulder")};rig.neck={joint("Neck"),joint("Neck1")};
            rig.left_arm={joint("LeftArm"),joint("LeftForeArm"),joint("LeftHand")};rig.right_arm={joint("RightArm"),joint("RightForeArm"),joint("RightHand")};
            rig.left_leg={joint("LeftUpLeg"),joint("LeftLeg"),joint("LeftFoot")};rig.right_leg={joint("RightUpLeg"),joint("RightLeg"),joint("RightFoot")};
            rig.toe_base={joint("LeftToeBase"),joint("RightToeBase")};
            const auto fingers=[&](const char* side){
                std::array<pose::Finger,5> result{};
                constexpr std::array<const char*,5> names{"Index","Middle","Ring","Pinky","Thumb"};
                for(std::size_t i=0;i<result.size();++i){
                    const std::string prefix=std::string(side)+"Hand"+names[i];
                    for(unsigned j=0;j<3;++j)result[i].joints[j]=joint((prefix+std::to_string(j+1)).c_str());
                    if(i!=4)result[i].metacarpal=joint((std::string(side)+"InHand"+names[i]).c_str());
                }
                return result;
            };
            rig.left_fingers=fingers("Left");rig.right_fingers=fingers("Right");
            if(layout->skeleton.names.size()!=386||rig.hips!=7)return;
            for(std::size_t i=0;i<layout->skeleton.parents.size();++i)if(layout->skeleton.parents[i]>=static_cast<int>(i))return;
            const auto* bind=std::get_if<frostbite::ebx::Value::Array>(&field(*document.root()->object,"LocalPose").data);
            if(!bind||bind->size()!=rig.parents.size())return;
            for(const auto& value:*bind){
                const auto& transform=object(value);const auto x=vector(field(transform,"right")),y=vector(field(transform,"up")),z=vector(field(transform,"forward"));
                if(std::abs(pose::length(x)-1)>.01f||std::abs(pose::length(y)-1)>.01f||std::abs(pose::length(z)-1)>.01f)return;
                rig.bind.push_back(pose::orientation(x,y,z));
            }
            shared().layout.store(std::move(layout));
        }catch(...){}
    }).detach();
}
}
void finish_private_grip_probe() noexcept {grip_probe::request_finish_client();}
bool available() noexcept {return shared().registered.load()&&static_cast<bool>(shared().layout.load());}
std::uint64_t updates() noexcept {return shared().updates.load(std::memory_order_relaxed);}
std::uint64_t render_updates() noexcept {return shared().render_updates.load(std::memory_order_relaxed);}
PoseMetrics metrics() noexcept {
    try {std::lock_guard lock(shared().mutex);return shared().render_metrics;}
    catch(...){return {};}
}
bool profiling_active() noexcept { return shared().profiling.load(std::memory_order_relaxed); }
CoupledAvatarBindingStatus coupled_avatar_binding_status() noexcept {
    std::lock_guard lock(shared().mutex);const auto& b=shared().spring_binding;
    return {b.root,b.pose_epoch,b.proximal_ready,b.completed_pose_write,b.last_pose_applied,b.fit_adjustment,b.hand_error,b.foot_error,shared().spring_presentation};
}
Json coupled_pose_diagnostic() noexcept {
    try {auto& d=shared().diagnostic;auto exits=Json::object();for(unsigned i=0;i<coupled_debug::names.size();++i)exits[coupled_debug::names[i]]=d.exits[i].load();
        const auto saved=d.fixture.load();return Json{{"schema",1},{"layout_ready_at_export",bool(shared().layout.load())},
            {"animation_listener_ready_at_export",shared().registered.load()},{"render_listener_ready_at_export",shared().board_registered.load()},
            {"render_callbacks",d.callbacks.load()},{"matching_rider_dispatches",d.matching_dispatches.load()},
            {"last_stage",coupled_debug::names[d.last.load()]},{"callback_exits",std::move(exits)},
            {"counter_sampling","independently sampled atomics; fixture is the same-callback witness"},{"clock_available",d.clock_available.load()},{"actual_serial",d.actual_serial.load()},{"actual_time",d.actual_time.load()},
            {"published_serial",d.published_serial.load()},{"published_time",d.published_time.load()},
            {"fixture_claimed",d.claimed.load()},{"capture_failed",d.capture_failed.load()},{"first_endpoint_fixture",saved?coupled_debug::fixture_json(*saved):Json(nullptr)}};
    }catch(...){return Json{{"issue","bounded_coupled_pose_diagnostic_export_failed"}};}
}
std::string status() {
    LastError error;const auto& selected=comparison();
    const auto contact=metrics();std::uint64_t revision{};bool admitted{};
    {std::lock_guard lock(shared().mutex);revision=shared().frame_revision;admitted=shared().placement.ready(revision);}
    return std::string("pose listener=")+(shared().registered.load()?"ready":"waiting")+
        ", source skeleton="+(shared().layout.load()?"validated":"waiting")+", successful pose updates="+std::to_string(updates())+
        ", rider render updates="+std::to_string(shared().render_updates.load())+
        ", private rider comparison="+pose_comparison::name(selected.mode)+
        ", comparison scope="+(selected.qualified_scope?"true":"false")+", comparison refused="+(selected.refused?"true":"false")+
        ", suppressed animation applies="+std::to_string(shared().suppressed_animation_applies.load(std::memory_order_relaxed))+
        ", suppressed render applies="+std::to_string(shared().suppressed_render_applies.load(std::memory_order_relaxed))+
        ", animation apply dispatches="+std::to_string(shared().comparison_animation_applies.load(std::memory_order_relaxed))+
        ", render apply dispatches="+std::to_string(shared().comparison_render_applies.load(std::memory_order_relaxed))+
        ", prop placement admission="+(admitted?"ready":"waiting")+", staged revision="+std::to_string(revision)+
        ", completed render revision="+std::to_string(contact.publication_revision)+
        ", stale parent rejections="+std::to_string(shared().stale_parent_rejections.load(std::memory_order_relaxed))+
        ", unpublished render buffer rejections="+std::to_string(shared().unpublished_buffer_rejections.load(std::memory_order_relaxed))+
        ", max completed riding hips local XYZ norm (native local units)="+
            std::to_string(shared().max_riding_hips_local_norm.load(std::memory_order_relaxed))+
        ", palm/ankle target error m="+std::to_string(shared().hand_error.load())+"/"+std::to_string(shared().foot_error.load())+
        ", hand/foot contact="+std::to_string(contact.hand_contact)+"/"+std::to_string(contact.foot_contact)+
        ", observed posture selected="+std::to_string(observed_posture_selected())+
        ", observed posture requested/applied/strength="+std::to_string(contact.observed_requested)+"/"+
            std::to_string(contact.observed_applied)+"/"+std::to_string(contact.observed_strength)+
        ", uphill posture rejected solves="+std::to_string(shared().posture_rejections.load(std::memory_order_relaxed))+
        ", uphill posture requested/fallback="+std::to_string(contact.posture_requested)+"/"+std::to_string(contact.posture_fallback)+
        ", posture attempts/scale="+std::to_string(contact.posture_attempts)+"/"+std::to_string(contact.posture_scale)+
        ", posture pelvis reach fit m/gaze counterpitch rad="+std::to_string(contact.posture_fit_adjustment)+"/"+std::to_string(contact.posture_gaze)+
        ", local board render hides="+std::to_string(shared().board_updates.load(std::memory_order_relaxed));
}
PrivateHandoffPublication private_handoff_publication() noexcept {
    std::lock_guard lock(shared().mutex);
    return {!shared().frame.active,shared().updates.load(),shared().render_updates.load(),
        shared().board_updates.load(),shared().frame_revision};
}
void begin_client_tick() noexcept {
    grip_probe::poll_client();
    // Do not clear mount_contacts or the last completed render measurement:
    // callbacks from an earlier solve will fail their final revision check.
    std::lock_guard lock(shared().mutex);shared().frame_revision=shared().placement.begin_tick();
}
visual::Publication pending_placement() noexcept {
    std::lock_guard lock(shared().mutex);visual::Publication out;out.revision=shared().frame_revision;
    if(shared().placement.has_candidate()&&out.revision&&
       std::chrono::steady_clock::now()-shared().at<=std::chrono::milliseconds(200)) {
        out.frame=shared().frame;out.available=true;
    }
    return out;
}
bool complete_placement(std::uint64_t revision,std::uintptr_t entity,bool all_parts_placed) noexcept {
    std::lock_guard lock(shared().mutex);
    // A late receipt may neither admit nor revoke a newer publication.
    if(!revision||revision!=shared().frame_revision)return false;
    const bool fresh=std::chrono::steady_clock::now()-shared().at<=std::chrono::milliseconds(200);
    const bool admitted=shared().placement.complete(revision,entity,all_parts_placed,fresh);
    if(!admitted)shared().render_metrics={};
    return admitted;
}
void publish(const visual::Frame& frame) noexcept {
    // Initialization/export runs on the client publisher, never inside a render hook.
    grip_probe::client_publish(frame);
    shared().diagnostic.active.store(coupled_debug_selected()&&frame.active&&frame.spring_avatar.requested);
    shared().profiling.store(frame.active&&frame.entity!=0,std::memory_order_relaxed);
    {std::lock_guard lock(shared().mutex);
        if(!frame.active||!frame.spring_avatar.requested||frame.rider_mode!=visual::RiderMode::riding||
           frame.entity!=shared().frame.entity||frame.mount_progress<1||
           (shared().spring_binding.root.ready&&!avatar_binding::same_identity(shared().spring_binding.root.bind_epoch,frame.spring_avatar.actual.epoch)))
            {shared().spring_binding={};shared().spring_presentation={};}
        if(!frame.active||!frame.entity||shared().frame.entity!=frame.entity||
           frame.rider_mode!=shared().frame.rider_mode||frame.rider_mode==visual::RiderMode::none)shared().render_metrics={};
        if(!frame.active||frame.entity!=shared().frame.entity||frame.mount_token!=shared().frame.mount_token||
           frame.rider_mode!=visual::RiderMode::riding||frame.mount_progress>=1) {
            shared().mount_contacts={};++shared().contact_revision;
        }
        shared().frame_revision=shared().placement.publish(frame.active,frame.entity);
        shared().frame=frame;shared().at=std::chrono::steady_clock::now();}
    if(!frame.active||!frame.entity)return;
    try {
        std::call_once(shared().preparation,prepare);
        if(!shared().registered.load()){
            std::string detail;
            if(multiplayer::install_entity_hooks(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),detail)&&multiplayer::add_animation_evaluated_listener(on_animation))shared().registered.store(true);
        }
        if(shared().registered.load()&&!shared().board_registered.load()&&multiplayer::add_render_pose_data_listener(on_render))shared().board_registered.store(true);
    }catch(...){}
}

// Private passive preparation only; no active Frame, lease, solver or native request.
void poll_private_board_handoff(std::uintptr_t base,bool idle,const board_handoff_trace::ClientState& state) noexcept {
    LastError error;
    if(!board_handoff_trace::initialize_client(base)) {
        if(board_handoff_trace::store.enabled.load(std::memory_order_acquire))
            board_handoff_trace::seal("private_configuration_changed");
        remove_no_bail_native_observer(&board_handoff_trace::native_edge_sample);
        return;
    }
    static bool attempted{},ready{};static ULONGLONG preparation_started{};
    try {
        if(!attempted){
            attempted=true;
            if(!enable_native_motion(base))board_handoff_trace::seal("existing_clock_observer_unavailable");
            else {
                preparation_started=GetTickCount64();
                std::call_once(shared().preparation,prepare);
            }
        }
        if(!ready&&!board_handoff_trace::store.sealed.load(std::memory_order_acquire)) {
            const auto preparation=board_handoff_policy::preparation(bool(shared().layout.load()),preparation_started,GetTickCount64());
            if(preparation==board_handoff_policy::Preparation::ready) {
                std::string detail;
                ready=multiplayer::install_entity_hooks(base,detail)&&multiplayer::add_render_pose_data_listener(on_render);
                if(ready)shared().board_registered.store(true);
                else board_handoff_trace::seal("existing_pose_listener_unavailable");
            } else if(preparation==board_handoff_policy::Preparation::timed_out)
                board_handoff_trace::seal("async_known_layout_preparation_timeout");
        }
        if(ready) {
            auto observed=state;observed.board_hide_updates=shared().board_updates.load(std::memory_order_relaxed);
            const auto chassis=native_chassis::status();observed.chassis_allocations=chassis.allocation_attempts;observed.chassis_mounts=chassis.mounts;
            board_handoff_trace::client_sample(observed);
            if(board_handoff_trace::store.armed.load(std::memory_order_acquire)&&!board_handoff_trace::store.sealed.load(std::memory_order_acquire)) {
                if(!add_no_bail_native_observer(&board_handoff_trace::native_edge_sample))board_handoff_trace::seal("native_edge_observer_slot_unavailable");
            }else remove_no_bail_native_observer(&board_handoff_trace::native_edge_sample);
        }
    }catch(...){board_handoff_trace::seal("passive_preparation_failed");}
    board_handoff_trace::poll_export(idle);
}
}
