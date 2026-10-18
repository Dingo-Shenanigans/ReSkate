#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include <fstream>
#include "Engine/Core/Config/profile_config.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace jishsfirstpersonmod {
void log(const std::string&) noexcept {}
std::filesystem::path module_directory() { return std::filesystem::current_path()/"native-settings-test"; }
std::filesystem::path data_directory() { return module_directory(); }
}
using namespace jishsfirstpersonmod;
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) { try {
    if(argc>1) {
        const auto actual=read_config(argv[1]);
        std::cout<<"External settings loaded: off-board sensitivity "<<actual.stick.yaw_speed<<" / "<<actual.stick.pitch_speed<<"\n";
    }
    const auto directory=module_directory()/std::to_wstring(GetCurrentProcessId());
    // Native catalogs may contain fields this build does not own. Keep recognized values.
    const auto extra=read_config_text("stick_yaw_speed=390\nstick_pitch_speed=363\nunused_option=1\n");
    check(extra.stick.yaw_speed==390 && extra.stick.pitch_speed==363,"Unrecognized field blocked or reset recognized preferences");
    Config original;original.board.enabled=true;original.board.continue_auto_center=false;
    original.board.yaw_speed=401;original.board.pitch_speed=381;original.disable_spring_on_foot=true;
    original.spectator_first_person=true;original.third_person_bailing=true;original.third_person_recovery=true;
    original.bail_camera_collision=false;
    original.glances_enabled=false;original.remember_marker_glance=true;original.stick.smooth_transitions=false;
    original.stick.mount_smoothing=63;original.stick.dismount_smoothing=42;
    SettingsStore store(directory/"settings.json",original);
    first_person_vendor::first_person::Settings shared;shared.enabled=true;
    shared.offset={.1f,.2f,.3f};shared.rotation={4,5,6};shared.up=21;shared.down=22;shared.left=23;shared.right=24;
    const NativeCameraOptions native{false,false,true,17,31,42,53};
    store.refresh_shared(100,shared,true,native);
    const auto observed=store.snapshot(1);
    check(!observed.unsaved,"Reading native preferences dirtied add-on settings");
    store.refresh_shared(100,shared,true,native);
    check(store.snapshot(1).revision==observed.revision,"Unchanged native values republished");
    Command command;command.kind=CommandKind::angle;command.action=1;command.value=47;
    check(store.submit(command,1),"Glance command rejected");store.flush_due(1000);
    const auto saved=store.snapshot(1000);check(!saved.unsaved && !saved.save_failed,"Live save failed");
    const auto document=dingosdk::profile_config::read(directory/"settings.json");
    for(const char* key:{"fov","spring","offset_x","offset_y","offset_z","pitch","yaw","roll","strength_up","on_board_only","menu_key","first_person","look_up","look_down","look_left","look_right","board_modifier","native_stabilize","native_follow_flips","native_camera_cycle","native_smoothing","native_head_pitch","native_head_roll","native_bob"})
        check(document && !document->at("options").contains(key),"Add-on settings duplicated a native setting");
    for(const char* key:{"lock_body_flips","lock_camera_mount_jump","lock_camera_hippy_jump","look_during_parkour","glance_emote_coordination"})
        check(!document->at("options").contains(key),"Excluded feature was serialized");
    const auto live=read_config(directory/"settings.json");
    check(live.spectator_first_person && live.third_person_bailing && live.third_person_recovery && !live.bail_camera_collision,"Accepted feature preferences did not round trip");
    check(live.look[0].angle==47 && live.board.yaw_speed==401 && live.board.pitch_speed==381 &&
        live.board.enabled && !live.board.continue_auto_center && live.disable_spring_on_foot,"Add-on settings did not round trip");
    check(!live.glances_enabled && live.remember_marker_glance && !live.stick.smooth_transitions && live.stick.mount_smoothing==63 && live.stick.dismount_smoothing==42,"Live preferences lost marker/transition settings");
    PresetLibrary library(directory/"presets");library.save("Snapshot",saved.config);
    const auto profile=library.load("Snapshot");
    check(profile.camera==native,"Profile lost native stabilization/head-motion/camera-cycle values");
    check(profile.spectator_first_person && profile.third_person_bailing && profile.third_person_recovery && !profile.bail_camera_collision,"Accepted features missing from named profile");
    check(profile.fov==100 && profile.spring.enabled && profile.on_board_only && profile.spring.offset==shared.offset &&
        profile.spring.rotation==shared.rotation && profile.spring.up==21 && profile.spring.down==22 &&
        profile.spring.left==23 && profile.spring.right==24,"Profile lost shared camera snapshot");
    check(profile.board.enabled && profile.board.yaw_speed==401 && !profile.board.continue_auto_center &&
        profile.look[0].angle==47 && profile.disable_spring_on_foot,"Profile lost add-on settings");
    check(!profile.glances_enabled && profile.remember_marker_glance && !profile.stick.smooth_transitions && profile.stick.mount_smoothing==63 && profile.stick.dismount_smoothing==42,"Profile lost marker/transition settings");
    command.kind=CommandKind::apply_preset;command.preset=profile;
    check(store.submit(command,2000) && store.snapshot(2000).config.camera==native,"Profile load failed to apply native camera snapshot");
    command.preset={};check(store.submit(command,2100),"Defaults request rejected");
    check(store.snapshot(2100).config.camera==NativeCameraOptions{} && !store.snapshot(2100).config.board.enabled,"Reset did not restore defaults");
    std::cout<<"Native ownership and profile round trips passed\n";return 0;
} catch(const std::exception& e) { std::cerr<<e.what();return 1; } }
