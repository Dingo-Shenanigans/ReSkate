#include "Extension/Skater/FirstPerson/native_stick_filter.h"
#include "Extension/Skater/free_flight.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk;
using namespace dingosdk::first_person_integration;
namespace fpmod=jishsfirstpersonmod;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
constexpr first_person::Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
int main() { try {
    fpmod::Config config;config.board.enabled=true;config.board.look_smoothing=0;
    config.board.center_smoothing=0;config.board.center_pitch=12;
    ControlsContext context;context.verified=context.eligible=true;context.owner={1,2,3,4};context.body=identity;
    overlay::GameplayController controller;controller.input.available=true;controller.input.device=1;
    RightStickRuntime runtime;
    const auto step=[&](double time,std::uint32_t buttons,short x=0,short y=0) {
        context.time=time;context.tick=1000+static_cast<std::uint64_t>(time*1000);
        controller.input.buttons=buttons;controller.pad.buttons=static_cast<std::uint16_t>(buttons);
        controller.pad.right_x=x;controller.pad.right_y=y;runtime.step(config,context,controller);
    };
    step(0,0);check(runtime.aim().pitch==12,"Configured resting pitch lost");
    step(.01,0x100,20000,-15000);
    check(runtime.capture().x==20000 && runtime.capture().y==-15000,"Native skating position was not latched");
    step(.02,0x100);step(.12,0x100,32767);
    check(runtime.aim().heading<0 && runtime.aim().capture,"On-board camera did not move");
    const auto captured=runtime.capture();overlay::GamepadValues pad{0x1100,44,55,123,456,-8000,9000};
    const auto raw=pad;check(captured.apply(0,pad,context.tick),"Capture missing on connected source");
    check(pad.right_x==20000 && pad.right_y==-15000 && pad.left_x==123 && pad.left_y==456 &&
        pad.buttons==raw.buttons && pad.left_trigger==44 && pad.right_trigger==55,"Capture changed unrelated controls");
    auto other=raw;check(!captured.apply(1,other,context.tick) && equal_pad(other,raw),"Capture leaked to another XInput slot");
    auto released=raw;released.buttons=0x1000;const auto release_raw=released;
    check(!captured.apply(0,released,context.tick) && equal_pad(released,release_raw),"Modifier release did not pass physical axes immediately");
    auto expired=raw;check(!captured.apply(0,expired,context.tick+101) && equal_pad(expired,raw),"Stale capture survived");
    PacketSequence packets;const auto a=packets.update(17,raw,pad),b=packets.update(17,raw,pad);
    check(a!=17 && a==b,"Filtered packet did not change/stabilize");
    check(packets.update(17,release_raw,release_raw)!=b,"Release without a new hardware packet was hidden");
    step(.13,0,-8000,9000);check(!runtime.capture().modifier && runtime.aim().heading==0 && runtime.aim().pitch==12,"Modifier auto-center priority failed");
    context.verified=false;runtime.step(config,context,controller);
    check(!runtime.capture().modifier && runtime.compose(identity,context.tick)==identity,"Menu/unknown owner failed to release camera/input");
    context.verified=context.eligible=true;config.board.keep_native_input=false;
    step(.2,0);step(.21,0x100,32767,32767);check(runtime.capture().x==0 && runtime.capture().y==0,"Neutral capture setting ignored");
    context.owner[1]=99;step(.22,0x100);check(!runtime.aim().capture,"Owner change reused a held modifier");
    // The native adapter must not reset a modifier-release return on recapture.
    RightStickRuntime returning,reference;config.board.center_smoothing=65;config.board.keep_native_input=true;
    const auto feed=[&](RightStickRuntime& state,double time,unsigned buttons,short x) {
        context.time=time;context.tick=10000+static_cast<std::uint64_t>(time*1000);
        controller.input.buttons=buttons;controller.pad.right_x=x;controller.pad.right_y=0;
        state.step(config,context,controller);
    };
    for(auto* state:{&returning,&reference}) {
        feed(*state,0,0,0);feed(*state,.01,0x100,0);feed(*state,.11,0x100,32767);feed(*state,.12,0,32767);
    }
    feed(returning,.13,0x100,32767);feed(reference,.13,0,32767);
    check(std::abs(returning.aim().heading-reference.aim().heading)<.0001f && !returning.aim().manual,
        "Modifier recapture interrupted auto-center through the native adapter");
    feed(returning,.14,0x100,0);feed(returning,.15,0x100,32767);
    check(returning.aim().manual,"New armed camera movement did not interrupt auto-center");
    // Off-board camera-only and camera/character use the same native motion path.
    runtime.reset();context.on_foot=true;context.turn_allowed=context.motion_ready=true;
    config.stick.enabled=true;config.stick.turn_character=true;config.stick_spring.enabled=false;
    step(.3,0);(void)runtime.compose(identity,context.tick);step(.31,0);step(.41,0,32767);
    check(runtime.aim().active && runtime.capture().modifier==~0u,"Off-board look did not capture native turn input");
    auto pose=runtime.compose(identity,context.tick);check(valid_flight_transform(pose) && pose!=identity,"Off-board camera pose invalid/unchanged");
    auto body=identity;check(runtime.motion(4,3,body,context.tick) && body!=identity && valid_flight_transform(body),"Character-facing handoff failed");
    auto foreign=identity;check(!runtime.motion(5,3,foreign,context.tick) && foreign==identity,"Character turn leaked to another rig");
    check(!runtime.motion(4,3,foreign,context.tick+101),"Expired character turn survived");
    config.stick.turn_character=false;step(.42,0);body=identity;
    check(!runtime.motion(4,3,body,context.tick),"Camera-only mode still turned the character");
    context.eligible=false;step(.43,0);check(!runtime.capture().modifier,"Bail/mounting eligibility did not release native input");
    // Recovery must retain the same away-from-body recenter request as dismount.
    runtime.reset();config.stick.turn_character=true;context.on_foot=true;context.eligible=true;context.bail=false;
    context.body=identity;step(.5,0);(void)runtime.compose(identity,context.tick);step(.51,0);
    context.bail=true;context.eligible=false;context.telemetry_valid=true;context.verified=false;step(.52,0);
    check(!runtime.capture().modifier && !runtime.turning(),"Bail retained input/character control");
    context.bail=false;context.eligible=context.verified=true;step(.53,0,32767);
    check(!runtime.aim().active,"Recovery recentered before neutral stick");
    step(.54,0);
    check(runtime.aim().active && std::abs(std::abs(runtime.aim().heading)-180)<.001f && std::abs(runtime.aim().body_heading)<.001f,
        "Bail recovery did not face away while preserving character lock");
    // The live adapter must actually route each configured handoff strength.
    const auto transition_pose=[&](float strength,bool foot,bool turn){
        RightStickRuntime state;auto c=context;c.eligible=true;c.bail=false;c.mounting=false;c.on_foot=!foot;c.time=1;c.tick=3000;
        auto cfg=config;cfg.stick.enabled=cfg.stick.smooth_transitions=true;cfg.stick.turn_character=turn;
        cfg.stick.mount_smoothing=cfg.stick.dismount_smoothing=strength;
        controller.pad.right_x=controller.pad.right_y=0;
        state.step(cfg,c,controller);(void)state.transition(identity,cfg,1);
        auto destination=fpmod::aim_camera(identity,{false,true,90,0});destination[12]=2;
        c.on_foot=foot;c.time=1.016;c.tick+=16;state.step(cfg,c,controller);
        (void)state.transition(destination,cfg,1.016);
        return state.transition(destination,cfg,1.032);
    };
    for(bool foot:{false,true}) for(bool turn:{false,true}){
        const auto instant=transition_pose(0,foot,turn),slow=transition_pose(90,foot,turn);
        check(instant[12]==2 && slow[12]==2,"Camera-only/character-lock transition moved native head position");
        check(std::abs(instant[0]-slow[0])>.01f,"Configured mount/dismount smoothing was not used");
    }
    // Ordinary locomotion/animation changes must never become a camera handoff.
    for(bool foot:{false,true}) for(bool enabled:{false,true}) {
        RightStickRuntime steady;auto cfg=config;cfg.stick.enabled=enabled;cfg.board.enabled=enabled;
        cfg.spring.enabled=cfg.stick.smooth_transitions=true;cfg.spring.up=cfg.spring.down=cfg.spring.left=cfg.spring.right=90;
        auto c=context;c.verified=c.eligible=true;c.bail=c.mounting=false;c.on_foot=foot;c.owner={1,2,3,4};
        c.time=4;c.tick=4000;c.movement_state={100,10,20,30};
        controller.input.buttons=0;controller.pad.right_x=controller.pad.right_y=0;
        steady.step(cfg,c,controller);(void)steady.transition(identity,cfg,4);
        for(unsigned frame=1;frame<10;++frame) {
            auto target=fpmod::aim_camera(identity,{false,true,float(frame*8),float(frame)});target[12]=frame*.02f;
            c.time=4+frame*.016;c.tick=4000+frame*16;c.movement_state={100+frame,10,20+frame,30+frame};
            steady.step(cfg,c,controller);
            check(steady.transition(target,cfg,c.time)==target,"Ordinary movement acquired extra camera smoothing");
        }
    }
    // An interrupted pose must not reuse the pre-teleport right-stick heading.
    {
        RightStickRuntime marker;auto cfg=config;cfg.stick.enabled=true;cfg.stick.turn_character=false;
        auto c=context;c.verified=c.eligible=true;c.bail=false;c.on_foot=true;c.owner={1,2,3,4};c.time=5;c.tick=5000;
        controller.input.buttons=0;controller.pad.right_x=controller.pad.right_y=0;
        marker.step(cfg,c,controller);(void)marker.compose(identity,c.tick);
        c.verified=false;c.teleport=true;c.time=5.016;c.tick+=16;marker.step(cfg,c,controller);
        c.teleport=false;c.verified=true;c.time=5.032;c.tick+=16;marker.step(cfg,c,controller);
        const auto destination=fpmod::aim_camera(identity,{false,true,75,0});(void)marker.compose(destination,c.tick);
        c.time=5.048;c.tick+=16;marker.step(cfg,c,controller);
        check(std::abs(marker.aim().heading-75)<.001f,"Marker return retained previous look heading");
    }
    // Native Sony parser/layout and an independently calculated Bluetooth CRC.
    StickCapture sony;sony.modifier=0x100;sony.sony=true;sony.published=5000;
    for(auto kind:{PlayStationPad::dualshock4,PlayStationPad::dualsense}) {
        std::array<std::uint8_t,64> report{};report.fill(0x55);report[0]=1;
        const unsigned buttons=kind==PlayStationPad::dualsense ? 8:5;
        report[buttons]=0x28;report[buttons+1]=1;
        const auto before=report;
        check(filter_sony_stick(sony,kind,report,5000),"Native Sony USB capture failed");
        for(unsigned i=0;i<report.size();++i) if(i!=3 && i!=4) check(report[i]==before[i],"Sony USB edit changed another field");
        check(report[3]==128 && report[4]==128,"Sony neutral stick encoding wrong");
        report[buttons+1]=0;const auto let_go=report;
        check(!filter_sony_stick(sony,kind,report,5000) && report==let_go,"Sony modifier release was filtered");
    }
    std::array<std::uint8_t,78> bluetooth;
    for(unsigned i=0;i<bluetooth.size();++i) bluetooth[i]=static_cast<std::uint8_t>(i*37+11);
    bluetooth[0]=0x31;bluetooth[1]=0xab;for(unsigned i=2;i<6;++i)bluetooth[i]=128;
    bluetooth[9]=0x28;bluetooth[10]=1;
    check(filter_sony_stick(sony,PlayStationPad::dualsense,bluetooth,5000),"Sony Bluetooth capture failed");
    const auto once=bluetooth;
    check(filter_sony_stick(sony,PlayStationPad::dualsense,bluetooth,5000) && bluetooth==once,
        "Repeated Sony completion changed already filtered input");
    const auto actual=std::uint32_t(bluetooth[74])|(std::uint32_t(bluetooth[75])<<8)|(std::uint32_t(bluetooth[76])<<16)|(std::uint32_t(bluetooth[77])<<24);
    check(actual==3132569669u,"Sony Bluetooth checksum differs from independent fixture");
    std::cout<<"Native right-stick runtime, input hand-back and report checks passed\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what();return 1;} }
