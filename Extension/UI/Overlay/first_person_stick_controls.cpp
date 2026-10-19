#include "skate_menu_internal.h"
#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
namespace dingosdk::overlay::menu {
namespace fpmod=jishsfirstpersonmod;
namespace {
bool slider(SkateMenu& menu,const char* label,float& value,float low,float high,const char* format,float reset) {
    field(menu,label);ImGui::PushID(label);
    const bool changed=first_person_slider("stick-value",value,low,high,format,reset);
    ImGui::PopID();return changed;
}
bool deadzone(SkateMenu& menu,float& value,float reset) {
    float percent=value*100;
    if(!slider(menu,"Stick deadzone",percent,1,50,"%.0f%%",reset*100)) return false;
    value=percent*.01f;return true;
}
void send(SkateMenu& menu,const fpmod::Command& command) {
    fpmod::submit_command(command);menu.feedback=fpmod::menu_snapshot().feedback;
}
}
void first_person_stick_controls(SkateMenu& menu) {
    const auto snapshot=fpmod::menu_snapshot();const auto& config=snapshot.config;
    ImGui::PushID("first-person-right-stick");
    auto stick=config.stick;
    if(ImGui::TreeNodeEx("Foot/board transitions",ImGuiTreeNodeFlags_SpanAvailWidth)) {
    bool changed=false;
    changed=toggle_row(menu,"Smooth foot/board transitions","Blend camera direction across mount and dismount while retaining the native head position in character-lock mode.",stick.smooth_transitions) || changed;
    ImGui::BeginDisabled(!stick.smooth_transitions);
    changed=slider(menu,"Getting on board smoothing",stick.mount_smoothing,0,100,"%.0f%%",fpmod::StickSettings{}.mount_smoothing) || changed;
    changed=slider(menu,"Getting off board smoothing",stick.dismount_smoothing,0,100,"%.0f%%",fpmod::StickSettings{}.dismount_smoothing) || changed;
    ImGui::EndDisabled();
    if(changed) {fpmod::Command command;command.kind=fpmod::CommandKind::stick_settings;command.stick=stick;send(menu,command);}
    ImGui::TreePop();
    }
    if(ImGui::TreeNodeEx("Off-board right-stick look",ImGuiTreeNodeFlags_SpanAvailWidth)) {
    bool changed=false;
    if(!stick.enabled) note("Off-board look is disabled. Tuning values remain editable.");
    field(menu,"Look mode");int mode=stick.turn_character ? 1:0;
    if(choice(menu,"foot-look-mode",mode,{"Camera only","Camera and character"},!config.look_only)) {stick.turn_character=mode==1;changed=true;}
    changed=slider(menu,"Horizontal sensitivity",stick.yaw_speed,1,720,"%.0f deg/s",fpmod::StickSettings{}.yaw_speed) || changed;
    changed=slider(menu,"Vertical sensitivity",stick.pitch_speed,1,720,"%.0f deg/s",fpmod::StickSettings{}.pitch_speed) || changed;
    changed=deadzone(menu,stick.deadzone,fpmod::StickSettings{}.deadzone) || changed;
    changed=toggle_row(menu,"Invert Y","Reverse vertical right-stick look.",stick.invert_y) || changed;
    if(changed) {fpmod::Command command;command.kind=fpmod::CommandKind::stick_settings;command.stick=stick;send(menu,command);}
    auto smoothing=config.stick_spring;
    bool smooth_changed=toggle_row(menu,"Smooth on-foot right-stick look","Smooth right-stick look separately from camera Spring and button glances.",smoothing.enabled);
    ImGui::BeginDisabled(!smoothing.enabled);
    smooth_changed=slider(menu,"Stick up smoothing",smoothing.up,0,100,"%.0f%%",50) || smooth_changed;
    smooth_changed=slider(menu,"Stick down smoothing",smoothing.down,0,100,"%.0f%%",50) || smooth_changed;
    smooth_changed=slider(menu,"Stick left smoothing",smoothing.left,0,100,"%.0f%%",50) || smooth_changed;
    smooth_changed=slider(menu,"Stick right smoothing",smoothing.right,0,100,"%.0f%%",50) || smooth_changed;
    ImGui::EndDisabled();
    if(smooth_changed) {fpmod::Command command;command.kind=fpmod::CommandKind::stick_smoothing;command.spring=smoothing;send(menu,command);}
    note("Center the stick after entering first person or changing look mode. Look pauses during mounting, jumps, bails, recovery and game menus.");
    ImGui::TreePop();
    }
    ImGui::PushID("on-board");
    if(ImGui::TreeNodeEx("On-board right-stick free look",ImGuiTreeNodeFlags_SpanAvailWidth)) {
    auto board=config.board;
    bool board_changed=false;
    if(!board.enabled) note("On-board free look is disabled. Tuning values remain editable.");
    board_changed=slider(menu,"Horizontal sensitivity",board.yaw_speed,1,720,"%.0f deg/s",fpmod::BoardLookSettings{}.yaw_speed) || board_changed;
    board_changed=slider(menu,"Vertical sensitivity",board.pitch_speed,1,720,"%.0f deg/s",fpmod::BoardLookSettings{}.pitch_speed) || board_changed;
    board_changed=deadzone(menu,board.deadzone,fpmod::BoardLookSettings{}.deadzone) || board_changed;
    board_changed=toggle_row(menu,"Invert Y","Reverse vertical right-stick look.",board.invert_y) || board_changed;
    board_changed=slider(menu,"Horizontal look limit",board.yaw_limit,0,180,"%.0f degrees",fpmod::BoardLookSettings{}.yaw_limit) || board_changed;
    board_changed=slider(menu,"Vertical look limit",board.pitch_limit,0,85,"%.0f degrees",fpmod::BoardLookSettings{}.pitch_limit) || board_changed;
    board_changed=slider(menu,"On Board Free Look Smoothing",board.look_smoothing,0,100,"%.0f%%",fpmod::BoardLookSettings{}.look_smoothing) || board_changed;
    board_changed=toggle_row(menu,"Modifier Key Keeps Right Stick Input Held","Keep the native skating stick position from when the modifier was pressed. Center the stick to arm camera movement; release the modifier to restore the current physical stick immediately.",board.keep_native_input) || board_changed;
    board_changed=toggle_row(menu,"Auto Center On Right Stick Let Go","Return to forward and the center pitch while the modifier remains held.",board.center_on_stick) || board_changed;
    board_changed=toggle_row(menu,"Auto Center On Modifier Key Let Go","Modifier release starts a return even with the stick deflected; this takes priority over right-stick auto-center.",board.center_on_modifier) || board_changed;
    board_changed=toggle_row(menu,"Continue Auto Center Until New Look Input","Re-pressing the modifier keeps a modifier-release return moving. Only newly armed right-stick camera movement interrupts it.",board.continue_auto_center) || board_changed;
    ImGui::BeginDisabled(!board.center_on_modifier && !board.center_on_stick);
    board_changed=slider(menu,"Auto-center smoothing",board.center_smoothing,0,100,"%.0f%%",fpmod::BoardLookSettings{}.center_smoothing) || board_changed;
    ImGui::EndDisabled();
    board_changed=slider(menu,"Center pitch angle",board.center_pitch,-85,85,"%.0f degrees",fpmod::BoardLookSettings{}.center_pitch) || board_changed;
    if(board_changed) {fpmod::Command command;command.kind=fpmod::CommandKind::board_settings;command.board=board;send(menu,command);}
    ImGui::TreePop();
    }
    ImGui::PopID();ImGui::PopID();
}
}
