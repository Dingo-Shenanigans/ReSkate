#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include "Extension/Skater/FirstPerson/Upstream/src/glance_controls.h"
#include "Extension/Skater/FirstPerson/Upstream/src/camera_policy.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace jishsfirstpersonmod {
void log(const std::string&) noexcept {}
std::filesystem::path module_directory(){return std::filesystem::current_path()/"board-test-data";}
std::filesystem::path data_directory(){return module_directory();}
}
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() { try {
    using namespace jishsfirstpersonmod;
    const BoardLookSettings defaults;
    Config policy;policy.board.enabled=true;ComfortSnapshot owner;owner.state_valid=owner.animation_valid=true;
    CameraMovementSample movement;movement.mode=movement.custom_mode=2;movement.non_bail=true;
    check(board_free_look_allowed(policy,owner,movement),"normal board free look rejected");
    movement.body_flip_camera_owned=true;
    check(!board_free_look_allowed(policy,owner,movement),"body-flip camera ownership must restore trick input");
    movement.body_flip_camera_owned=false;movement.bail_recovery=true;
    check(!board_free_look_allowed(policy,owner,movement),"bail must release on-board capture");
    movement.bail_recovery=false;movement.mode=movement.custom_mode=1;
    check(!board_free_look_allowed(policy,owner,movement),"off-board camera entered board free look");
    check(!defaults.enabled && defaults.modifier==0x100 && defaults.deadzone==.10f && defaults.yaw_speed==330 && defaults.pitch_speed==270 && defaults.yaw_limit==85 && defaults.pitch_limit==60 &&
        defaults.continue_auto_center && defaults.look_smoothing==12 && defaults.keep_native_input && !defaults.center_on_stick && defaults.center_on_modifier &&
        defaults.center_smoothing==35 && defaults.center_pitch==0,"requested fresh defaults changed");

    BoardLookSettings settings;settings.enabled=true;settings.look_smoothing=0;settings.center_on_stick=false;settings.center_pitch=12;
    BoardLookState state;
    first_person_vendor::ControllerInput input{true,0,1};
    auto aim=state.update(settings,{},true,input,0);
    check(aim.active && !aim.capture && aim.pitch==12,"resting pitch or normal skating capture incorrect");
    input.buttons=settings.modifier;
    aim=state.update(settings,{1,0},true,input,.01);
    check(aim.capture && aim.heading==0,"held trick stick was not captured pending neutral");
    state.update(settings,{},true,input,.02);
    aim=state.update(settings,{1,1},true,input,.12);
    check(aim.capture && aim.heading<0 && aim.pitch>12,"look directions incorrect");
    const auto looked=aim;
    input.buttons=0;
    aim=state.update(settings,{1,1},true,input,.13);
    check(!aim.capture && std::abs(aim.heading)<std::abs(looked.heading) && aim.pitch<looked.pitch,"release failed to restore input or start centering");
    settings.center_on_modifier=false;
    const auto retained=state.update(settings,{1,1},true,input,.14);
    aim=state.update(settings,{-1,-1},true,input,.24);
    check(!aim.capture && aim.heading==retained.heading && aim.pitch==retained.pitch,"native trick input changed retained look");
    settings.center_pitch=20;
    aim=state.update(settings,{},true,input,.25);
    check(std::abs(aim.pitch-retained.pitch-8)<.001f,"live center pitch change ignored");
    check(!state.update(settings,{},false,input,.26).active,"interruption kept camera ownership");
    input.buttons=settings.modifier;
    aim=state.update(settings,{},true,input,.27);
    check(!aim.capture && aim.pitch==20,"held modifier rearmed after interruption");
    input.buttons=0;state.update(settings,{},true,input,.28);
    input.buttons=settings.modifier;state.update(settings,{},true,input,.29);
    aim=state.update(settings,{},true,{true,settings.modifier,2},.30);
    check(!aim.capture,"controller switch retained modifier ownership");
    settings.center_on_modifier=true;settings.center_smoothing=0;
    input.buttons=0;state.update(settings,{},true,input,.31);
    input.buttons=settings.modifier;state.update(settings,{},true,input,.32);
    state.update(settings,{1,1},true,input,.42);
    input.buttons=0;aim=state.update(settings,{},true,input,.43);
    check(aim.heading==0 && aim.pitch==20 && !aim.capture,"zero smoothing did not center immediately");
    // Every release combination has independent behavior; modifier release wins.
    for(bool stick_center:{false,true}) for(bool modifier_center:{false,true}) {
        BoardLookSettings options;options.enabled=true;options.look_smoothing=0;options.center_on_stick=stick_center;
        options.center_on_modifier=modifier_center;options.center_smoothing=0;
        BoardLookState camera;first_person_vendor::ControllerInput pad{true,0,1};
        camera.update(options,{},true,pad,0);pad.buttons=options.modifier;
        camera.update(options,{},true,pad,.01);
        auto moved=camera.update(options,{1,0},true,pad,.11);
        check(moved.heading<0,"manual look did not move");
        auto neutral=camera.update(options,{},true,pad,.12);
        check((neutral.heading==0)==stick_center,"stick-release toggle not independent");
        camera.update(options,{1,0},true,pad,.22);
        pad.buttons=0;auto released=camera.update(options,{1,0},true,pad,.23);
        check(!released.capture && ((released.heading==0)==modifier_center),"modifier center did not take precedence with deflected stick");
    }
    BoardLookSettings options;options.enabled=true;options.keep_native_input=true;
    options.center_on_modifier=false;options.center_on_stick=false;options.look_smoothing=0;options.yaw_limit=25;options.pitch_limit=15;options.center_pitch=10;
    BoardLookState limited;first_person_vendor::ControllerInput pad{true,0,1};
    limited.update(options,{},true,pad,0);pad.buttons=options.modifier;
    limited.update(options,{.4f,-.7f},true,pad,.01);
    limited.update(options,{},true,pad,.02);
    for(int i=1;i<=20;++i) aim=limited.update(options,{1,1},true,pad,.02+i*.1);
    check(aim.heading==-25 && aim.pitch==25,"camera exceeded configured horizontal/vertical limits");
    check(limited.native_axes(options).x==.4f && limited.native_axes(options).y==-.7f,"native stick hold changed with camera movement");
    options.keep_native_input=false;check(limited.native_axes(options).x==0 && limited.native_axes(options).y==0,"disabled native hold did not neutralize");
    options.yaw_limit=5;options.pitch_limit=3;
    aim=limited.update(options,{},true,pad,2.03);
    check(aim.heading==-5 && aim.pitch==13,"live limit reduction ignored");
    BoardLookState smooth,instant;options={};options.enabled=true;options.center_on_modifier=false;options.center_on_stick=false;options.look_smoothing=0;
    pad.buttons=0;smooth.update(options,{},true,pad,0);instant.update(options,{},true,pad,0);
    pad.buttons=options.modifier;smooth.update(options,{},true,pad,.01);instant.update(options,{},true,pad,.01);
    const auto immediate=instant.update(options,{1,0},true,pad,.11);
    options.look_smoothing=80;const auto dolly=smooth.update(options,{1,0},true,pad,.11);
    check(std::abs(dolly.heading)>0 && std::abs(dolly.heading)<std::abs(immediate.heading),"free-look smoothing did not ease movement");
    pad.buttons=0;const auto stopped=smooth.update(options,{1,0},true,pad,.12);
    check(stopped.heading==dolly.heading && !stopped.capture,"camera continued dragging after release with auto-center off");
    // A modifier-release return keeps its trajectory through re-press, even
    // while a held native trick position is waiting for neutral camera arming.
    for(bool stick_center:{false,true}) {
        BoardLookSettings keep;keep.enabled=true;keep.look_smoothing=0;keep.center_smoothing=65;
        keep.center_pitch=14;keep.center_on_stick=stick_center;
        BoardLookState camera,reference;first_person_vendor::ControllerInput pad{true,0,1};
        for(auto* value:{&camera,&reference}) {
            value->update(keep,{},true,pad,0);pad.buttons=keep.modifier;
            value->update(keep,{},true,pad,.01);value->update(keep,{1,1},true,pad,.11);
            pad.buttons=0;value->update(keep,{1,1},true,pad,.12);
        }
        for(int i=13;i<=16;++i) {
            pad.buttons=keep.modifier;
            const StickAxes axes=i<15 ? StickAxes{.7f,.5f}:StickAxes{.01f,0};
            auto returning=camera.update(keep,axes,true,pad,i*.01);
            pad.buttons=0;auto expected=reference.update(keep,axes,true,pad,i*.01);
            check(std::abs(returning.heading-expected.heading)<.00001f && std::abs(returning.pitch-expected.pitch)<.00001f,
                "modifier re-press or deadzone/native input disturbed auto-center trajectory");
        }
        pad.buttons=keep.modifier;auto interrupted=camera.update(keep,{1,1},true,pad,.17);
        pad.buttons=0;auto uninterrupted=reference.update(keep,{},true,pad,.17);
        check(interrupted.manual && interrupted.heading<uninterrupted.heading && interrupted.pitch>uninterrupted.pitch,
            "accepted camera movement failed to interrupt persistent return");
        // Repeated modifier edges do not reset damping. A held modifier can finish.
        camera.update(keep,{},true,pad,.18);
        for(int i=19;i<=800;++i) {
            pad.buttons=(i<35 && i%2) || i>=35 ? keep.modifier:0;
            aim=camera.update(keep,{},true,pad,i*.01);
        }
        check(aim.heading==0 && aim.pitch==14,"persistent auto-center did not reach exact configured center");
    }
    {
        BoardLookSettings old;old.enabled=true;old.continue_auto_center=false;old.center_on_stick=false;old.look_smoothing=0;
        BoardLookState camera;first_person_vendor::ControllerInput pad{true,0,1};
        camera.update(old,{},true,pad,0);pad.buttons=old.modifier;camera.update(old,{},true,pad,.01);
        camera.update(old,{1,0},true,pad,.11);pad.buttons=0;
        auto returning=camera.update(old,{},true,pad,.12);pad.buttons=old.modifier;
        auto paused=camera.update(old,{},true,pad,.13);
        check(paused.heading==returning.heading,"disabled option changed interruptible modifier re-press behavior");
    }
    // Shared LB can remain down while each D-pad chord rearms, including after menus.
    first_person_vendor::FirstPersonLookControls glances{{{0x101,30},{0x102,40},{0x104,60},{0x108,75}}};
    for(bool toggle:{false,true}) {
        GlanceControlsState glance;first_person_vendor::ControllerInput lb{true,0x100,1};
        glance.update(glances,lb,false,toggle,0x100);
        lb.buttons=0x104;check(glance.update(glances,lb,false,toggle,0x100)[1]==-60,"held LB blocked shared D-pad glance");
        lb.buttons=0x100;glance.update(glances,lb,false,toggle,0x100);
        lb.buttons=0x108;check(glance.update(glances,lb,false,toggle,0x100)[1]==75,"shared LB failed to switch glance direction");
        glance.update(glances,lb,true,toggle,0x100);
        check(glance.update(glances,lb,false,toggle,0x100)==std::array<float,2>{},"menu exit accepted held D-pad");
        lb.buttons=0x100;glance.update(glances,lb,false,toggle,0x100);
        lb.buttons=0x101;check(glance.update(glances,lb,false,toggle,0x100)[0]==30,"shared LB did not rearm after D-pad release");
    }
    Config config;config.board=settings;config.board.modifier=0x300;
    config.board.yaw_speed=240;config.board.pitch_speed=180;config.board.deadzone=.22f;
    config.board.continue_auto_center=false;config.board.invert_y=true;config.board.center_on_modifier=false;config.board.center_smoothing=75;
    config.board.center_on_stick=true;config.board.keep_native_input=true;
    config.board.yaw_limit=95;config.board.pitch_limit=40;config.board.look_smoothing=65;
    auto path=module_directory()/"settings.json";
    write_config(path,config);const auto loaded=read_config(path);
    check(loaded.board.enabled && loaded.board.modifier==0x300 && loaded.board.invert_y && !loaded.board.center_on_modifier &&
        loaded.board.yaw_speed==240 && loaded.board.pitch_speed==180 && loaded.board.deadzone==.22f &&
        loaded.board.center_pitch==20 && loaded.board.center_smoothing==75,"settings round trip lost on-board preferences");
    check(!loaded.board.continue_auto_center && loaded.board.center_on_stick && loaded.board.keep_native_input && loaded.board.yaw_limit==95 &&
        loaded.board.pitch_limit==40 && loaded.board.look_smoothing==65,"new preferences failed round trip");
    SettingsStore store(path,{});Command command;command.kind=CommandKind::board_settings;command.board=config.board;
    check(store.submit(command,1) && store.snapshot().config.board.enabled,"on-board settings command rejected");
    command.kind=CommandKind::binding;command.action=5;command.mask=0x100;
    check(store.submit(command,2) && store.snapshot().config.board.modifier==0x100,"modifier recorder command rejected");
    command.kind=CommandKind::apply_preset;command.preset=config;
    check(store.submit(command,3) && store.snapshot().config.board.modifier==0x300,"preset lost on-board preferences");
    const auto same_board=[](const BoardLookSettings& a,const BoardLookSettings& b) {
        return a.enabled==b.enabled &&  a.center_on_modifier==b.center_on_modifier &&
            a.center_on_stick==b.center_on_stick && a.continue_auto_center==b.continue_auto_center &&
            a.keep_native_input==b.keep_native_input && a.invert_y==b.invert_y && a.yaw_speed==b.yaw_speed &&
            a.pitch_speed==b.pitch_speed && a.deadzone==b.deadzone && a.center_smoothing==b.center_smoothing &&
            a.center_pitch==b.center_pitch && a.look_smoothing==b.look_smoothing && a.yaw_limit==b.yaw_limit && a.pitch_limit==b.pitch_limit;
    };
    PresetLibrary profiles(module_directory()/"profiles");
    Config global;global.menu_key=VK_F8;global.menu_combo=0x300;
    SettingsStore profile_store(module_directory()/"profile-current.json",global);
    for(bool keep:{false,true}) {
        config.board.continue_auto_center=keep;config.menu_key=VK_F7;config.menu_combo=0x30;
        const std::string name=keep ? "Persistent return":"Interruptible return";
        profiles.save(name,config);const auto saved=profiles.load(name);
        check(same_board(saved.board,config.board),"personal profile lost board preferences");
        Command apply;apply.kind=CommandKind::apply_preset;apply.preset=saved;
        check(profile_store.submit(apply,5),"personal board profile failed to apply");
        const auto active=profile_store.snapshot().config;
        check(same_board(active.board,config.board),"applied personal profile lost board preferences");
        check(active.menu_key==VK_F8 && active.menu_combo==0x300,"camera profile changed global menu access");
    }
    command.kind=CommandKind::board_settings;command.board.center_pitch=86;
    check(!store.submit(command,4),"invalid pitch accepted");
    std::cout<<"On-board modifier, neutral arming, centering, retention, lifecycle and persistence passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;} }
