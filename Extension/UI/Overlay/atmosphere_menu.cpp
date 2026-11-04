#include "skate_menu_internal.h"
#include "atmosphere_color.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::overlay::menu {
namespace {
bool send(SkateMenu& menu,const CallbacksV3& callbacks,const std::string& command) {
    std::array<char,512> result{};
    const bool queued=callbacks.queue_console_command &&
        callbacks.queue_console_command(callbacks.user,command.c_str(),result.data(),result.size());
    result.back()=0;
    feedback(menu,result[0] ? result.data() : queued ? "" : "Change unavailable.");
    return queued;
}
void control_row(SkateMenu& menu,const WorldControlsModel& model,const CallbacksV3& callbacks,unsigned i) {
    const auto& c=dingosdk::atmosphere_controls[i];const auto& reading=model.atmosphere[i];
    const auto choice=model.choices.atmosphere.find(c.key);
    const std::optional<AtmosphereValue> saved=choice==model.choices.atmosphere.end() ? std::nullopt : std::optional(choice->second);
    auto& state=menu.atmosphere_edit[i];
    if (state.waiting && (saved==state.pending || ImGui::GetTime()>=state.deadline ||
        model.status=="World controls could not be saved.")) state.waiting=false;
    const auto shown=state.waiting ? state.pending : saved;
    if (!state.editing && !state.dirty) {
        state.value=shown.value_or(reading.value.value_or(AtmosphereValue{}));
        if (!reading.value && !shown) {
            if (c.type==AtmosphereType::enumeration) state.value.number[0]=atmosphere_options[c.option_start].value;
            if (c.type==AtmosphereType::integer) state.value.number[0]=c.minimum;
        }
    }
    const auto submit=[&](std::optional<AtmosphereValue> v) {
        if (v && !valid_atmosphere_value(c,*v)) { feedback(menu,"Value is outside the supported range.");return; }
        const std::string value=v ? atmosphere_value_text(c,*v) : "default";
        if (send(menu,callbacks,std::string(c.key)+" \""+value+"\"")) {
            // Compare acknowledgement against the numbers actually sent.
            if (v) parse_atmosphere_value(c,value,*v);
            state.waiting=true;state.pending=std::move(v);state.deadline=ImGui::GetTime()+2.0;
        }
    };
    // A row for the control (a lane of a vector or colour each), changed with left / right; saved
    // once a run of steps ends, as a slider is once let go.
    ImGui::PushID(c.key);
    std::string hint=c.key;
    if (reading.varies) hint+=". Values vary across this level's environments";
    if (saved && !reading.applied) hint+=". The saved value is waiting to apply";
    hint+=".";
    bool immediate=false,changed=false,editing=false,deactivated=false;
    if (c.type==AtmosphereType::texture) {
        const auto& names=model.textures[i];
        int at=-1;
        for (std::size_t j=0;j<names.size();++j) if (names[j]==state.value.texture) at=static_cast<int>(j);
        if (option_row(menu,c.label,names.empty() ? "No authored texture for this slot is loaded in this level." : hint.c_str(),at,names,!names.empty()) && at>=0) {
            state.value.texture=names[static_cast<std::size_t>(at)];changed=immediate=true;
        }
    } else if (c.type==AtmosphereType::toggle) {
        bool enabled=state.value.number[0]!=0;
        if (toggle_row(menu,c.label,hint.c_str(),enabled)) { state.value.number[0]=enabled ? 1 : 0;changed=immediate=true; }
    } else if (c.type==AtmosphereType::enumeration) {
        std::vector<std::string> names;
        int at=-1;
        for (unsigned j=0;j<c.option_count;++j) {
            const auto& option=atmosphere_options[c.option_start+j];
            names.emplace_back(option.label);
            if (state.value.number[0]==option.value) at=static_cast<int>(j);
        }
        if (option_row(menu,c.label,hint.c_str(),at,names) && at>=0) {
            state.value.number[0]=atmosphere_options[c.option_start+static_cast<unsigned>(at)].value;changed=immediate=true;
        }
    } else {
        ImGui::BeginGroup();
        if (c.type==AtmosphereType::color) {
            auto color=atmosphere_color_to_ui(state.value);
            static constexpr const char* channels[]{" red"," green"," blue"};
            for (int k=0;k<3;++k) {
                field(menu,(std::string(c.label)+channels[k]).c_str(),(hint+" 0 to 1; the game gets it times 2500.").c_str());
                ImGui::PushID(k);
                set_console_step(0.02);
                if (slider_float("##lane",&color[static_cast<std::size_t>(k)],0.0f,1.0f,"%.2f",ImGuiSliderFlags_AlwaysClamp)) {
                    state.value=atmosphere_color_from_ui(color,state.value);changed=true;
                }
                ImGui::PopID();
            }
        } else {
            static constexpr const char* axes[]{" X"," Y"," Z"," W"};
            for (unsigned lane=0;lane<c.lanes;++lane) {
                field(menu,(std::string(c.label)+(c.lanes>1 ? axes[std::min(lane,3u)] : "")).c_str(),hint.c_str());
                ImGui::PushID(static_cast<int>(lane));
                double value=state.value.number[lane];
                const double range=c.maximum-c.minimum;
                set_console_step(c.type==AtmosphereType::integer ? 1.0 : range>0 && range<=1e4 ? range/100 : std::max(std::abs(value)*0.05,0.01));
                if (slider_double("##lane",&value,c.minimum,c.maximum,c.type==AtmosphereType::integer ? "%.0f" : "%.4g",ImGuiSliderFlags_AlwaysClamp)) {
                    state.value.number[lane]=value;changed=true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndGroup();
        editing=ImGui::IsItemActive();
        deactivated=ImGui::IsItemDeactivatedAfterEdit();
    }
    state.dirty |= changed;
    state.editing=editing;
    if (state.dirty && (immediate || deactivated || !state.editing)) {
        submit(state.value);
        state.dirty=false;
    }
    ImGui::PopID();
}
}
void atmosphere_menu(SkateMenu& menu,const Model& model,const CallbacksV3& callbacks) {
    const auto& controls=model.world_controls;
    // One group at a time (fog, sky or wind), its controls as rows under their section names.
    static int kind=0;
    static const std::vector<std::string> groups{atmosphere_groups.begin(),atmosphere_groups.end()};
    const bool can=controls.environment_available && callbacks.queue_console_command;
    begin_card(menu,"atmosphere","FOG, SKY AND WIND","Changes apply at once and save by themselves");
    option_row(menu,"Showing","Left / right: the fog, the sky or the wind.",kind,groups);
    const auto group=static_cast<unsigned>(std::clamp(kind,0,2));
    std::vector<unsigned> rows;
    std::vector<std::string_view> sections;
    for (unsigned i=0;i<dingosdk::atmosphere_controls.size();++i) {
        const auto& c=dingosdk::atmosphere_controls[i];
        if (atmosphere_kind(c.property)!=group) continue;
        rows.push_back(i);
        if (std::find(sections.begin(),sections.end(),c.section)==sections.end()) sections.push_back(c.section);
    }
    const int pressed=button_row(menu,"atmosphere-reset",{
        {"RESTORE THESE DEFAULTS",can,false,"This group back to the level's own settings."},
        {"RESTORE ALL DEFAULTS",can,false,"Fog, sky and wind back to the level's own settings."}});
    if (pressed==0 && send(menu,callbacks,std::string(atmosphere_groups[group])+".reset"))
        for (const auto index:rows) { auto& e=menu.atmosphere_edit[index];e.waiting=true;e.pending.reset();e.deadline=ImGui::GetTime()+2.0; }
    if (pressed==1 && send(menu,callbacks,"environment reset_environment"))
        for (auto& e:menu.atmosphere_edit) { e.waiting=true;e.pending.reset();e.deadline=ImGui::GetTime()+2.0; }
    if (!controls.environments[group]) note(("Waiting for this level's "+std::string(atmosphere_groups[group])+" component.").c_str());
    const auto& choices=controls.choices;
    if (group==0 ? choices.fog>=0 || choices.fog_distance>=0 : group==1 ? choices.sky_brightness>=0 || choices.clouds>=0
                 : choices.wind_strength>=0 || choices.wind_direction>=0)
        warn("Console modifiers are active. Restore defaults clears these too.");
    ImGui::BeginDisabled(!can);
    for (const auto section_name:sections) {
        if (section_name!=atmosphere_groups[group]) section(menu,std::string(section_name).c_str());
        if (group==1 && (section_name=="Physical atmosphere" || section_name=="Primary light" || section_name=="Secondary light"))
            note("Used when Sky Type is Physical.");
        if (group==1 && section_name=="Primary light")
            note("Turn off Follow Outdoor Light for a custom rotation, and Takes Color From Outdoor Light for a custom colour.");
        for (const auto i:rows) if (dingosdk::atmosphere_controls[i].section==section_name) control_row(menu,controls,callbacks,i);
    }
    ImGui::EndDisabled();
    if (!controls.status.empty()) note(controls.status.c_str());
    end_card();
}
}
