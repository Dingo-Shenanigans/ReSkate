#include "skate_menu_internal.h"
#include "Extension/Skater/FirstPerson/integration.h"
#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include "Extension/Skater/FirstPerson/Upstream/src/bail_collision.h"
#include <algorithm>
#include <cstdio>
namespace dingosdk::overlay::menu {
namespace fpmod = jishsfirstpersonmod;
namespace {
void submit(SkateMenu& menu, const fpmod::Command& command) {
    fpmod::submit_command(command);
    menu.feedback=fpmod::menu_snapshot().feedback;
}
void option(SkateMenu& menu,const char* label,const char* hint,bool value,fpmod::CommandKind kind) {
    if(toggle_row(menu,label,hint,value)) {
        fpmod::Command command;command.kind=kind;command.enabled=value;submit(menu,command);
    }
}
}
bool first_person_slider(const char* id,float& value,float low,float high,const char* format,float reset) {
    ImGui::PushID(id);
    const float width=ImGui::CalcTextSize("Reset").x+ImGui::GetStyle().FramePadding.x*2;
    ImGui::SetNextItemWidth(std::max(px(90),ImGui::GetContentRegionAvail().x-width-ImGui::GetStyle().ItemSpacing.x));
    bool changed=slider_float("##value",&value,low,high,format,ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    if(ImGui::Button("Reset",ImVec2(width,0))) {value=reset;changed=true;}
    ImGui::PopID();return changed;
}
void first_person_main_controls(SkateMenu& menu,const Model&,const CallbacksV3&) {
    if(!first_person_integration::ready()) {
        if(first_person_integration::failed()) warn("Additional first-person settings could not initialize. See the ReSkate log.");
        else note("Additional first-person settings are waiting for initialization.");
        return;
    }
    const auto snapshot=fpmod::menu_snapshot();const auto& config=snapshot.config;
    const auto selected_name=[&]() -> std::string {
        const auto index=menu.first_person_preset;
        return index>0 && index<=static_cast<int>(menu.first_person_presets.size()) ? menu.first_person_presets[index-1]:std::string{};
    };
    const auto refresh=[&](const std::string& selection) {
        menu.first_person_presets=fpmod::personal_presets();menu.first_person_presets_loaded=true;
        const auto found=std::find(menu.first_person_presets.begin(),menu.first_person_presets.end(),selection);
        menu.first_person_preset=found==menu.first_person_presets.end() ? 0:static_cast<int>(found-menu.first_person_presets.begin())+1;
    };
    if(!menu.first_person_presets_loaded) refresh(selected_name());
    field(menu,"Profile");
    const auto selected=selected_name();
    if(ImGui::BeginCombo("##first-person-profile",selected.empty() ? "Recommended":selected.c_str())) {
        if(ImGui::Selectable("Recommended",selected.empty())) menu.first_person_preset=0;
        for(std::size_t i=0;i<menu.first_person_presets.size();++i) {
            const auto& name=menu.first_person_presets[i];
            if(ImGui::Selectable(name.c_str(),name==selected)) {
                menu.first_person_preset=static_cast<int>(i+1);
                std::snprintf(menu.first_person_preset_name.data(),menu.first_person_preset_name.size(),"%s",name.c_str());
            }
        }
        ImGui::EndCombo();
    }
    if(ImGui::Button("Load")) {
        const auto name=selected_name();
        if(name.empty()) fpmod::load_recommended_preset();else fpmod::load_personal_preset(name);
        menu.feedback=fpmod::menu_snapshot().feedback;
    }
    ImGui::SameLine();
    if(ImGui::Button("Refresh")) refresh(selected_name());
    ImGui::SameLine();
    if(ImGui::Button("Reset to defaults")) {
        fpmod::Command command;command.kind=fpmod::CommandKind::apply_preset;command.preset={};submit(menu,command);
    }
    field(menu,"Name");
    const float save_width=ImGui::CalcTextSize("Save").x+ImGui::GetStyle().FramePadding.x*2;
    ImGui::SetNextItemWidth(std::max(px(90),ImGui::GetContentRegionAvail().x-save_width-ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputText("##first-person-profile-name",menu.first_person_preset_name.data(),menu.first_person_preset_name.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!menu.first_person_preset_name[0]);
    if(ImGui::Button("Save",ImVec2(save_width,0))) {
        if(fpmod::save_personal_preset(menu.first_person_preset_name.data())) refresh(menu.first_person_preset_name.data());
        menu.feedback=fpmod::menu_snapshot().feedback;
    }
    ImGui::EndDisabled();
    const auto deleting=selected_name();
    ImGui::BeginDisabled(deleting.empty());
    if(ImGui::Button("Delete profile")) {
        if(fpmod::delete_personal_preset(deleting))refresh({});
        menu.feedback=fpmod::menu_snapshot().feedback;
    }
    ImGui::EndDisabled();
    if(snapshot.unsaved || snapshot.save_failed) {
        if(snapshot.save_failed) warn(snapshot.save_error.c_str());
        if(ImGui::Button("Save settings now")) {fpmod::Command request;request.kind=fpmod::CommandKind::retry_save;submit(menu,request);}
    }
    auto stick=config.stick;
    if(toggle_row(menu,"Off-board right-stick look","Enable first-person look while walking.",stick.enabled)) {
        fpmod::Command command;command.kind=fpmod::CommandKind::stick_settings;command.stick=stick;submit(menu,command);
    }
    auto board=config.board;
    if(toggle_row(menu,"On-board right-stick free look","Hold the First Person modifier configured in Settings > Controls > Action Binds.",board.enabled)) {
        fpmod::Command command;command.kind=fpmod::CommandKind::board_settings;command.board=board;submit(menu,command);
    }
    option(menu,"Directional glances","Enable the glance actions configured in Settings > Controls > Action Binds.",config.glances_enabled,fpmod::CommandKind::glances_enabled);
}
void first_person_view_controls(SkateMenu& menu) {
    if(!first_person_integration::ready()) return;
    const auto config=fpmod::menu_snapshot().config;
    option(menu,"First-person spectator view","During party spectating and throwdown turns, use your First Person action to switch between follow and head view. Each target starts in follow view.",config.spectator_first_person,fpmod::CommandKind::spectator_first_person);
    bool bail=config.third_person_bailing;
    if(toggle_row(menu,"Third person while bailing","Return to first person when native control resumes.",bail)) {
        fpmod::Command command;command.kind=fpmod::CommandKind::comfort_options;command.third_person_bailing=bail;command.on_board_only=config.on_board_only;submit(menu,command);
    }
    ImGui::BeginDisabled(config.third_person_bailing);
    option(menu,"Third person while getting up","Keep the fall in first person, then use the normal camera while getting up.",config.third_person_recovery,fpmod::CommandKind::third_person_recovery);
    ImGui::EndDisabled();
    if(config.third_person_camera_switching()) note("The camera cycle pauses while automatic bail/get-up switching is enabled. Your saved camera-cycle choice is retained.");
    ImGui::BeginDisabled(config.third_person_bailing);
    option(menu,"Camera collision during bails","Sweep the first-person camera against surfaces during bails and recovery.",config.bail_camera_collision,fpmod::CommandKind::bail_collision);
    ImGui::EndDisabled();
    if(config.bail_camera_collision && !fpmod::bail_collision_available()) note("Bail camera collision is waiting for a supported gameplay context. See the ReSkate log if it remains unavailable.");
}
void first_person_foot_spring_control(SkateMenu& menu) {
    if(!first_person_integration::ready()) return;
    const auto config=fpmod::menu_snapshot().config;
    option(menu,"Disable Spring on foot","Position and rotation offsets still apply. Spring returns on the board.",config.disable_spring_on_foot,fpmod::CommandKind::spring_on_foot);
}
void first_person_glance_controls(SkateMenu& menu) {
    if(!first_person_integration::ready()) return;
    const auto config=fpmod::menu_snapshot().config;
    if(!config.glances_enabled) note("Directional glances are disabled. Tuning values remain editable.");
    ImGui::Spacing();ImGui::PushFont(menu.bold);ImGui::TextUnformatted("Directional glances");ImGui::PopFont();
    option(menu,"Toggle glances","Press a direction again to center; another direction replaces it.",config.toggle_glances,fpmod::CommandKind::toggle_glances);
    ImGui::BeginDisabled(!config.toggle_glances);
    option(menu,"Remember glance on return to marker","Restore a toggled glance after returning to a marker. Menus, bails, changed controls and leaving first person clear it; other teleports may also retain it.",config.remember_marker_glance,fpmod::CommandKind::remember_marker_glance);
    ImGui::EndDisabled();
    const char* labels[]{"Look up angle","Look down angle","Look left angle","Look right angle"};
    for(unsigned i=0;i<4;++i) {
        ImGui::PushID(static_cast<int>(i));field(menu,labels[i]);float angle=config.look[i].angle;
        if(first_person_slider("glance-angle",angle,1,i<2 ? 90.f:180.f,"%.0f degrees",fpmod::Config{}.look[i].angle)) {
            fpmod::Command command;command.kind=fpmod::CommandKind::angle;command.action=i+1;command.value=angle;submit(menu,command);
        }
        ImGui::PopID();
    }
    option(menu,"Link look smoothing to camera Spring","Use the existing Spring strengths to smooth directional glances.",config.link_smoothing,fpmod::CommandKind::link_smoothing);
    if(!config.link_smoothing) {
        auto settings=config.look_spring;
        bool changed=toggle_row(menu,"Smooth directional glances","Smooth the glance separately from the camera Spring.",settings.enabled);
        const auto strength=[&](const char* label,float& value) {
            field(menu,label);ImGui::PushID(label);
            changed=first_person_slider("glance-strength",value,0,100,"%.0f%%",50) || changed;
            ImGui::PopID();
        };
        ImGui::BeginDisabled(!settings.enabled);
        strength("Glance up",settings.up);strength("Glance down",settings.down);
        strength("Glance left",settings.left);strength("Glance right",settings.right);
        ImGui::EndDisabled();
        if(changed) {fpmod::Command command;command.kind=fpmod::CommandKind::look_settings;command.spring=settings;submit(menu,command);}
    }
}
}
