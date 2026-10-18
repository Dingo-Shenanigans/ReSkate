#include "controls_context.h"
#include "Extension/Skater/free_flight.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Upstream/src/camera_movement.h"
#include "Upstream/src/getup_contract.h"
#include "Upstream/src/bail_recovery_contract.h"
#include "Upstream/src/comfort_policy.h"
namespace dingosdk::first_person_integration {
ControlsContext read_controls_context(std::uintptr_t base,std::uintptr_t client) noexcept {
    using namespace client_source::detail;
    namespace pc=jishsfirstpersonmod::player_state_contract;
    ControlsContext result;result.game_base=base;
    try {
        // Reuse the native model. Unknown/menu states release all gameplay filtering.
        const auto ui=multiplayer::sample_game_ui_state(base,true);
        if(!ui.available || ui.in_menu) return {};
        SourceReader r;
        source_require(r.pointer(client)==base+addr::engine::client_vtable,"Client changed");
        const auto world=r.value<std::uint32_t>(client,0xc4);
        source_require(world==13 || world==21,"Outside gameplay");
        overlay::DebugModel skater;(void)debug_skater(base,client,skater);
        const auto entity=skater.skater_identity,component=r.pointer(entity,pc::component_offset);
        source_require(r.pointer(component)==base+addr::engine::skater_component_vtable,"Skater changed");
        result.teleport=r.value<std::uint8_t>(entity,0x7e0)!=0;
        result.world_context=r.pointer(client,8);result.tick=GetTickCount64();r.verify();result.telemetry_valid=true;
        // Observe a teleport without borrowing camera/input ownership during it.
        if(result.teleport) return result;
        const auto core=r.pointer(component,pc::core_offset),rig=r.pointer(core,0x438),context=r.pointer(core,pc::physics_context_offset);
        source_require(r.pointer(core)==base+pc::core_vtable && r.pointer(rig)==context && r.pointer(rig,0x4630)==core,"Physics owner changed");
        const auto parent=r.pointer(core,pc::movement_parent_offset);
        const bool foot=r.pointer(parent)==base+pc::offboard_vtable;
        const auto physics=r.value<std::uint32_t>(context,pc::physics_state_offset);
        const auto output=r.pointer(r.pointer(core,pc::animation_owner),pc::animation_output);
        const auto wipeout=r.value<std::uint8_t>(output,pc::wipeout_request_offset);
        const auto bail=r.value<std::uint8_t>(output,pc::animation_wipeout_offset);
        const auto recovery=r.value<std::uint8_t>(output,pc::recovery_offset);
        source_require(wipeout<=1 && bail<=1 && recovery<=1,"Animation flags invalid");
        const bool signals_bail=wipeout || bail || physics==pc::wipeout;
        bool non_bail=!signals_bail && !recovery;
        result.bail=!non_bail;result.owner={core,entity,context,rig};
        const bool mounting=(r.value<std::uint32_t>(context,0x13d4)&0x80)!=0;
        std::uintptr_t active{},type{};
        if(foot) {
            source_require(r.pointer(parent,8)==context && r.pointer(parent,0x18)==rig && r.pointer(parent,0x10)==r.pointer(core,0x430),"Foot owner changed");
            active=r.pointer(parent,pc::active_substate_offset);type=r.pointer(active);
        }
        source_require(r.raw(rig+0x4720,result.body.data(),sizeof(result.body)),"Body pose unavailable");
        source_require(!non_bail || valid_flight_transform(result.body),"Body pose invalid");r.verify();
        const int mode=jishsfirstpersonmod::camera_movement_mode(true,foot,parent,active,type,base,physics);
        namespace recovery_contract=jishsfirstpersonmod::bail_recovery_contract;
        namespace getup=jishsfirstpersonmod::getup_contract;
        static std::uintptr_t verified_base{};
        static bool recovery_layout{},getup_layout{};
        if(verified_base!=base) {
            const auto read=[](std::uintptr_t address,void* output,std::size_t size) {return first_person_read(address,output,size);};
            recovery_layout=recovery_contract::verified(read,base);getup_layout=getup::verified(read,base);verified_base=base;
        }
        const auto flags=(foot && (recovery_layout || getup_layout)) ? r.value<std::uint32_t>(context,recovery_contract::control_flags_offset):0u;
        const bool ground=foot && active==parent+pc::normal_foot_states[0].offset && type==base+pc::normal_foot_states[0].vtable;
        result.normal_foot=ground;
        result.recovery_verified=recovery_layout && getup_layout;
        result.getting_up=getup_layout && getup::requested(parent,active,type,base,flags);
        result.recovery_controllable=recovery_layout && recovery_contract::controllable(ground,flags,
            ground && r.value<std::uint8_t>(active,recovery_contract::ground_override_offset)!=0,mounting,signals_bail);
        result.wipeout_request=wipeout!=0;result.physics=physics;
        static jishsfirstpersonmod::BailLatch bail_latch;
        const auto latched=bail_latch.update({core,true,pc::riding(physics).value_or(false),ground,true,signals_bail,recovery!=0});
        result.bail=latched.value_or(!non_bail);non_bail=!result.bail;
        r.verify();
        // Special jump/climbing camera overrides are intentionally excluded.
        const bool falling=foot && active==parent+pc::normal_foot_states[2].offset;
        result.verified=true;result.on_foot=foot;result.bail=!non_bail;result.mounting=mounting;
        result.movement_state={physics,parent,active,type};
        result.eligible=mode!=0 && non_bail && !mounting && !falling;
        result.turn_allowed=foot && result.eligible;
        result.motion_ready=source_state().motion_original.load(std::memory_order_acquire)!=nullptr;
        result.owner={core,entity,context,rig};result.time=source_flight_now();result.tick=GetTickCount64();
        return result;
    } catch(...) { result.verified=false;return result; }
}
}
