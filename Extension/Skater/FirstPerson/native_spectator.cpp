#include "integration.h"
#include "spectator_state.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Upstream/src/settings.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <atomic>
namespace dingosdk::first_person_integration {
namespace {
SpectatorState spectator;
first_person::FrameSpring spring;
std::atomic<std::uint64_t> active_target{},sampled{};
std::atomic<bool> toggle_pending{};
std::atomic<DWORD> render_thread{};
struct RenderFrame {
    std::uintptr_t base{},client{},entity{},component{},holder{},world{};
    double time{};float fov{};
    first_person::Matrix pose{};
    first_person::Settings settings;
};
RenderFrame render_frame;
void refine_render(std::uintptr_t animation_interface) noexcept {
    const auto thread=render_thread.load(std::memory_order_acquire);
    if(!thread || thread!=GetCurrentThreadId())return;
    client_source::detail::SourceLastError preserve_error;
    try {
        using namespace client_source::detail;
        const auto& frame=render_frame;
        const auto elapsed=source_flight_now()-frame.time;
        if(!spectator.head || elapsed<0 || elapsed>.1 || animation_interface!=frame.holder+0xc0)return;
        SourceReader reader;
        source_require(reader.pointer(frame.client,8)==frame.world && reader.pointer(frame.entity,0x628)==frame.component &&
            reader.pointer(frame.component,0xa0)==frame.holder,"Spectated render owner changed.");reader.verify();
        auto pose=frame.pose;
        const auto origin=first_person_spectator_head(frame.base,frame.entity,pose);
        pose=spring.sample(pose,frame.settings,origin);
        std::string detail;
        if(!update_party_camera(frame.base,frame.client,true,true,&pose,detail,frame.fov,true)) {
            first_person_restore_spectator_head();render_thread.store(0,std::memory_order_release);
        }
    } catch(...) {
        client_source::detail::first_person_restore_spectator_head();render_thread.store(0,std::memory_order_release);
    }
}
}
bool toggle_spectator() noexcept {
    const auto now=GetTickCount64(),last=sampled.load();
    if(!active_target.load() || now<last || now-last>250 || !jishsfirstpersonmod::menu_snapshot().config.spectator_first_person)return false;
    toggle_pending.store(true);return true;
}
bool spectator_camera(std::uintptr_t base,std::uintptr_t client,std::uint64_t id,std::uint64_t epoch,
    std::uintptr_t entity,first_person::Matrix& pose,float& fov,bool ready) noexcept {
    using namespace client_source::detail;
    try {
        render_thread.store(0,std::memory_order_release);
        multiplayer::set_final_render_pose_listener(&refine_render);
        const auto config=jishsfirstpersonmod::menu_snapshot().config;
        std::uintptr_t world{};
        const bool valid=ready && first_person_read(client+8,&world,sizeof(world)) && world;
        if(spectator.observe(config.spectator_first_person && valid,id,epoch,world)) {
            spring.reset();toggle_pending.store(false);first_person_restore_spectator_head();
        }
        active_target.store(spectator.id);sampled.store(GetTickCount64());
        if(toggle_pending.exchange(false)) {spectator.toggle();spring.reset();}
        if(!spectator.head) {first_person_restore_spectator_head();return false;}
        auto head=pose;
        const auto origin=first_person_spectator_head(base,entity,head);
        const auto& native=source_state().trial.debug;
        spring.begin(source_flight_now());pose=spring.sample(head,native.first_person_settings,origin);
        if(native.first_person_fov>0) fov=native.first_person_fov;
        SourceReader reader;
        const auto component=reader.pointer(entity,0x628),holder=reader.pointer(component,0xa0);reader.verify();
        render_frame={base,client,entity,component,holder,world,spring.frame_time,fov,pose,native.first_person_settings};
        render_thread.store(GetCurrentThreadId(),std::memory_order_release);
        return true;
    } catch(...) {
        first_person_restore_spectator_head();spring.reset();return false;
    }
}
}
