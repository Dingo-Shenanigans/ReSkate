#include "Extension/Skater/FirstPerson/Upstream/src/mod.h"
#include <fstream>
#include "Engine/Core/Config/profile_config.h"
#include <map>
#include <set>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <limits>

namespace jishsfirstpersonmod {
Config read_config(const std::filesystem::path& path) {
    const auto json=dingosdk::profile_config::read(path);
    if(!json || !json->is_object() || !json->contains("options"))throw std::runtime_error("Cannot read First Person settings.");
    std::ostringstream values;values<<std::setprecision(std::numeric_limits<double>::max_digits10);
    for(const auto& [key,value]:json->at("options").items())if(value.is_number())values<<key<<'='<<value.get<double>()<<'\n';
    return read_config_text(values.str());
}
Config read_config_text(const std::string& text) {
    Config cfg;
    std::istringstream input(text);
    std::map<std::string,std::string> values;
    std::string line;
    const auto trim = [](std::string s) {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return std::string{};
        return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
    };
    while (std::getline(input,line)) {
        line = trim(line.substr(0,line.find_first_of(";#")));
        if (line.empty() || line == "[FirstPerson]") continue;
        const auto split = line.find('=');
        if (split == std::string::npos || !values.emplace(trim(line.substr(0,split)),trim(line.substr(split+1))).second)
            throw std::runtime_error("invalid or duplicate First Person configuration setting");
    }
    const auto number = [&](const std::string& name, float fallback) {
        const auto it = values.find(name);
        if (it == values.end()) return fallback;
        std::size_t used{};
        const auto result = std::stof(it->second, &used);
        if (used != it->second.size() || !std::isfinite(result)) throw std::runtime_error(name + " must be finite numeric text");
        values.erase(it); return result;
    };
    const auto combo = [&](const std::string& name) {
        const auto it = values.find(name);
        if (it == values.end()) return std::uint32_t{};
        if (it->second.empty() || it->second[0]=='-') throw std::runtime_error(name + " must be a nonnegative mask");
        std::size_t used{};
        const auto mask = std::stoull(it->second,&used,0);
        if (used != it->second.size() || mask > first_person_vendor::controller_button_mask ||
            !first_person_vendor::valid_controller_combo(static_cast<std::uint32_t>(mask)))
            throw std::runtime_error(name + " contains unsupported controller buttons");
        values.erase(it); return static_cast<std::uint32_t>(mask);
    };
    cfg.menu_combo=combo("menu_combo");
    cfg.toggle = combo("first_person");
    std::set<std::uint32_t> occupied;
    // Native reservations and activation can become runtime conflicts.
    // Keep their saved values; only duplicate glance rows are malformed.
    unsigned reserved_index{};
    for (const auto name : {"noclip", "forward_velocity", "up_velocity"}) {
        const auto mask = combo(name);
        cfg.reserved[reserved_index++]=mask;
    }
    for (unsigned i=0;i<4;++i) {
        const std::string name = first_person_vendor::look_action_keys[i];
        cfg.look[i].combo = combo(name);
        if (cfg.look[i].combo && !occupied.insert(cfg.look[i].combo).second) throw std::runtime_error("duplicate look/controller binding");
        cfg.look[i].angle = number(name+"_angle",cfg.look[i].angle);
        if (cfg.look[i].angle < 1 || cfg.look[i].angle > first_person_vendor::look_angle_limit(i)) throw std::runtime_error(name + " angle out of range");
    }
    cfg.fov = number("fov",0);
    if (cfg.fov != 0 && (cfg.fov < 40 || cfg.fov > 120)) throw std::runtime_error("FOV must be zero or 40..120");
    const auto enabled = number("spring",0);
    if (enabled != 0 && enabled != 1) throw std::runtime_error("spring must be 0 or 1");
    cfg.spring.enabled = enabled != 0;
    const char* offsets[]{"offset_x","offset_y","offset_z"};
    const char* rotations[]{"pitch","yaw","roll"};
    for (unsigned i=0;i<3;++i) {
        cfg.spring.offset[i] = number(offsets[i],0);
        cfg.spring.rotation[i] = number(rotations[i],0);
    }
    cfg.spring.up = number("strength_up",50); cfg.spring.down = number("strength_down",50);
    cfg.spring.left = number("strength_left",50); cfg.spring.right = number("strength_right",50);
    const auto boolean=[&](const char* key,bool fallback) {
        const auto value=number(key,fallback ? 1.0f:0.0f);
        if(value!=0 && value!=1) throw std::runtime_error(std::string(key)+" must be 0 or 1");
        return value!=0;
    };
    cfg.camera.stabilize=boolean("native_stabilize",true);
    cfg.camera.follow_flips=boolean("native_follow_flips",true);
    cfg.camera.camera_cycle=boolean("native_camera_cycle",false);
    cfg.camera.smoothing=number("native_smoothing",60);cfg.camera.head_pitch=number("native_head_pitch",86);
    cfg.camera.head_roll=number("native_head_roll",0);cfg.camera.bob=number("native_bob",25);
    cfg.look_only=boolean("look_only",false);
    cfg.link_smoothing=boolean("link_smoothing",true);
    cfg.look_smoothing_initialized=boolean("look_smoothing_initialized",false);
    cfg.look_spring.enabled=boolean("look_spring",false);
    cfg.look_spring.up=number("look_strength_up",50); cfg.look_spring.down=number("look_strength_down",50);
    cfg.look_spring.left=number("look_strength_left",50); cfg.look_spring.right=number("look_strength_right",50);
    cfg.third_person_bailing=boolean("third_person_bailing",false);
    cfg.third_person_recovery=boolean("third_person_recovery",false);
    cfg.bail_camera_collision=boolean("bail_camera_collision",true);
    cfg.spectator_first_person=boolean("spectator_first_person",false);
    cfg.cycle_restore_pending=boolean("cycle_restore_pending",false);
    cfg.cycle_restore_enabled=boolean("cycle_restore_enabled",false);
    cfg.on_board_only=boolean("on_board_only",false);
    cfg.disable_spring_on_foot=boolean("disable_spring_on_foot",false);
    cfg.glances_enabled=boolean("glances_enabled",true);
    cfg.toggle_glances=boolean("toggle_glances",false);
    cfg.remember_marker_glance=boolean("remember_marker_glance",false);
    cfg.board.enabled=boolean("board_look",false);
    cfg.board.center_on_modifier=boolean("board_center_on_modifier",true);
    cfg.board.center_on_stick=boolean("board_center_on_stick",false);
    cfg.board.continue_auto_center=boolean("board_continue_auto_center",true);
    cfg.board.keep_native_input=boolean("board_keep_native_input",true);
    cfg.board.look_smoothing=number("board_look_smoothing",12);
    cfg.board.yaw_limit=number("board_yaw_limit",85);
    cfg.board.pitch_limit=number("board_pitch_limit",60);
    cfg.board.invert_y=boolean("board_invert_y",false);
    cfg.board.modifier=values.contains("board_modifier") ? combo("board_modifier"):0x100;
    cfg.board.yaw_speed=number("board_yaw_speed",330);
    cfg.board.pitch_speed=number("board_pitch_speed",270);
    cfg.board.deadzone=number("board_deadzone",.10f);
    cfg.board.center_smoothing=number("board_center_smoothing",35);
    cfg.board.center_pitch=number("board_center_pitch",0);
    cfg.stick.enabled=boolean("stick_look",false);
    cfg.stick.turn_character=boolean("stick_turn_character",false);
    cfg.stick.invert_y=boolean("stick_invert_y",false);
    cfg.stick.yaw_speed=number("stick_yaw_speed",120);
    cfg.stick.pitch_speed=number("stick_pitch_speed",90);
    cfg.stick.deadzone=number("stick_deadzone",.15f);
    const float menu_key=number("menu_key",VK_INSERT);
    if(!std::isfinite(menu_key) || menu_key!=std::floor(menu_key) || menu_key<0 || menu_key>255) throw std::runtime_error("Invalid menu key.");
    cfg.menu_key=static_cast<unsigned>(menu_key);
    cfg.stick.smooth_transitions=boolean("smooth_transitions",true);
    cfg.stick.mount_smoothing=number("mount_smoothing",70);
    cfg.stick.dismount_smoothing=number("dismount_smoothing",35);
    for(float strength:{cfg.stick.mount_smoothing,cfg.stick.dismount_smoothing})
        if(!std::isfinite(strength) || strength<0 || strength>100) throw std::runtime_error("Transition smoothing must be 0ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“100 percent.");
    if(!std::isfinite(cfg.stick.yaw_speed) || cfg.stick.yaw_speed<1 || cfg.stick.yaw_speed>720 ||
       !std::isfinite(cfg.stick.pitch_speed) || cfg.stick.pitch_speed<1 || cfg.stick.pitch_speed>720 ||
       !std::isfinite(cfg.stick.deadzone) || cfg.stick.deadzone<.01f || cfg.stick.deadzone>.5f)
        throw std::runtime_error("Stick look settings outside supported ranges");
    if(!first_person_vendor::first_person::valid(cfg.look_spring)) throw std::runtime_error("Look smoothing outside supported ranges");
    if(!cfg.link_smoothing && !cfg.look_smoothing_initialized) {
        cfg.look_spring=cfg.spring; cfg.look_spring.offset={}; cfg.look_spring.rotation={}; cfg.look_smoothing_initialized=true;
    }
    if (!first_person_vendor::first_person::valid(cfg.spring)) throw std::runtime_error("spring settings outside custom build's supported ranges");
    cfg.stick_smoothing_initialized=boolean("stick_smoothing_initialized",false);
    cfg.stick_spring.enabled=boolean("stick_spring",false);
    cfg.stick_spring.up=number("stick_strength_up",50); cfg.stick_spring.down=number("stick_strength_down",50);
    cfg.stick_spring.left=number("stick_strength_left",50); cfg.stick_spring.right=number("stick_strength_right",50);
    if(!cfg.stick_smoothing_initialized) {
        initialize_stick_smoothing(cfg);
    }
    if(!first_person_vendor::first_person::valid(cfg.stick_spring)) throw std::runtime_error("Stick smoothing outside supported ranges");
    // Like Trainer, read the settings this build owns; unrelated fields do not block loading.
    validate_config(cfg);
    return cfg;
}

void validate_config(const Config& cfg) {
    if(!first_person_vendor::valid_controller_combo(cfg.menu_combo)) throw std::runtime_error("Unsupported menu controller buttons.");
    if(!first_person_vendor::valid_controller_combo(cfg.board.modifier) ||
       !std::isfinite(cfg.board.yaw_speed) || cfg.board.yaw_speed<1 || cfg.board.yaw_speed>720 ||
       !std::isfinite(cfg.board.pitch_speed) || cfg.board.pitch_speed<1 || cfg.board.pitch_speed>720 ||
       !std::isfinite(cfg.board.deadzone) || cfg.board.deadzone<.01f || cfg.board.deadzone>.5f ||
       !std::isfinite(cfg.board.center_smoothing) || cfg.board.center_smoothing<0 || cfg.board.center_smoothing>100 ||
       !std::isfinite(cfg.board.look_smoothing) || cfg.board.look_smoothing<0 || cfg.board.look_smoothing>100 ||
       !std::isfinite(cfg.board.yaw_limit) || cfg.board.yaw_limit<0 || cfg.board.yaw_limit>180 ||
       !std::isfinite(cfg.board.pitch_limit) || cfg.board.pitch_limit<0 || cfg.board.pitch_limit>85 ||
       !std::isfinite(cfg.board.center_pitch) || cfg.board.center_pitch< -85 || cfg.board.center_pitch>85)
        throw std::runtime_error("On-board stick look settings outside supported ranges.");
    if(cfg.menu_key>255 || (cfg.menu_key && (cfg.menu_key<=VK_XBUTTON2 || cfg.menu_key==VK_ESCAPE || cfg.menu_key==VK_SHIFT || cfg.menu_key==VK_CONTROL || cfg.menu_key==VK_MENU || (cfg.menu_key>=VK_LSHIFT && cfg.menu_key<=VK_RMENU)))) throw std::runtime_error("Choose a keyboard key other than Escape or a modifier.");
    for(float strength:{cfg.stick.mount_smoothing,cfg.stick.dismount_smoothing})
        if(!std::isfinite(strength) || strength<0 || strength>100) throw std::runtime_error("Transition smoothing must be 0ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“100 percent.");
    if(!std::isfinite(cfg.stick.yaw_speed) || cfg.stick.yaw_speed<1 || cfg.stick.yaw_speed>720 ||
       !std::isfinite(cfg.stick.pitch_speed) || cfg.stick.pitch_speed<1 || cfg.stick.pitch_speed>720 ||
       !std::isfinite(cfg.stick.deadzone) || cfg.stick.deadzone<.01f || cfg.stick.deadzone>.5f)
        throw std::runtime_error("Stick look settings outside supported ranges.");
    if(!first_person_vendor::first_person::valid(cfg.stick_spring)) throw std::runtime_error("Stick smoothing outside supported ranges");
    std::set<std::uint32_t> used;
    const auto reserve=[&](std::uint32_t mask) {
        if(!first_person_vendor::valid_controller_combo(mask)) throw std::runtime_error("Unsupported controller buttons.");
        if(mask && !used.insert(mask).second) throw std::runtime_error("That combo is already assigned to another control.");
    };
    if(!first_person_vendor::valid_controller_combo(cfg.toggle)) throw std::runtime_error("Unsupported controller buttons.");
    for(auto mask:cfg.reserved) if(!first_person_vendor::valid_controller_combo(mask)) throw std::runtime_error("Unsupported controller buttons.");
    for(unsigned i=0;i<4;++i) {
        reserve(cfg.look[i].combo);
        const auto angle=cfg.look[i].angle;
        if(!std::isfinite(angle) || angle<1 || angle>first_person_vendor::look_angle_limit(i)) throw std::runtime_error("Look angle is outside its supported range.");
    }
    if(!std::isfinite(cfg.fov) || (cfg.fov!=0 && (cfg.fov<40 || cfg.fov>120))) throw std::runtime_error("FOV must be the game default or 40ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“120 degrees.");
    for(float value:{cfg.camera.smoothing,cfg.camera.head_pitch,cfg.camera.head_roll,cfg.camera.bob})
        if(!std::isfinite(value) || value<0 || value>100)throw std::runtime_error("Native camera percentage is outside its supported range.");
    if(!first_person_vendor::first_person::valid(cfg.spring)) throw std::runtime_error("Camera spring settings are outside their supported ranges.");
    if(!first_person_vendor::first_person::valid(cfg.look_spring)) throw std::runtime_error("Look smoothing is outside its supported range.");
}
std::string config_text(const Config& cfg,bool include_shared) {
    validate_config(cfg);std::ostringstream out,result;
        out<<std::setprecision(std::numeric_limits<float>::max_digits10);
        out<<"[FirstPerson]\nfirst_person="<<cfg.toggle<<'\n';
        for(unsigned i=0;i<4;++i) out<<first_person_vendor::look_action_keys[i]<<'='<<cfg.look[i].combo<<'\n'
            <<first_person_vendor::look_action_keys[i]<<"_angle="<<cfg.look[i].angle<<'\n';
        out<<"noclip="<<cfg.reserved[0]<<"\nforward_velocity="<<cfg.reserved[1]<<"\nup_velocity="<<cfg.reserved[2]
            <<"\nfov="<<cfg.fov<<"\nspring="<<cfg.spring.enabled;
        const char* offset[]{"offset_x","offset_y","offset_z"}; const char* rotation[]{"pitch","yaw","roll"};
        for(unsigned i=0;i<3;++i) out<<'\n'<<offset[i]<<'='<<cfg.spring.offset[i]<<'\n'<<rotation[i]<<'='<<cfg.spring.rotation[i];
        out<<"\nstrength_up="<<cfg.spring.up<<"\nstrength_down="<<cfg.spring.down
            <<"\nstrength_left="<<cfg.spring.left<<"\nstrength_right="<<cfg.spring.right<<'\n';
        out<<"native_stabilize="<<cfg.camera.stabilize<<"\nnative_follow_flips="<<cfg.camera.follow_flips
            <<"\nnative_camera_cycle="<<cfg.camera.camera_cycle<<"\nnative_smoothing="<<cfg.camera.smoothing
            <<"\nnative_head_pitch="<<cfg.camera.head_pitch<<"\nnative_head_roll="<<cfg.camera.head_roll<<"\nnative_bob="<<cfg.camera.bob<<'\n';
        out<<"glances_enabled="<<cfg.glances_enabled<<'\n';
        out<<"look_only="<<cfg.look_only<<"\ntoggle_glances="<<cfg.toggle_glances<<"\nremember_marker_glance="<<cfg.remember_marker_glance<<'\n';
        out<<"link_smoothing="<<cfg.link_smoothing<<"\nlook_smoothing_initialized="<<cfg.look_smoothing_initialized
            <<"\nlook_spring="<<cfg.look_spring.enabled<<"\nlook_strength_up="<<cfg.look_spring.up
            <<"\nlook_strength_down="<<cfg.look_spring.down<<"\nlook_strength_left="<<cfg.look_spring.left
            <<"\nlook_strength_right="<<cfg.look_spring.right<<"\nthird_person_bailing="<<cfg.third_person_bailing
            <<"\ncycle_restore_pending="<<cfg.cycle_restore_pending<<"\ncycle_restore_enabled="<<cfg.cycle_restore_enabled
            <<"\non_board_only="<<cfg.on_board_only
            <<"\nbail_camera_collision="<<cfg.bail_camera_collision
            <<"\nthird_person_recovery="<<cfg.third_person_recovery
            <<"\nspectator_first_person="<<cfg.spectator_first_person
            <<"\ndisable_spring_on_foot="<<cfg.disable_spring_on_foot<<'\n';
        auto normalized=cfg; initialize_stick_smoothing(normalized);
        const auto& stick_spring=normalized.stick_spring;
        out<<"stick_smoothing_initialized=1\nstick_spring="<<stick_spring.enabled
            <<"\nstick_strength_up="<<stick_spring.up<<"\nstick_strength_down="<<stick_spring.down
            <<"\nstick_strength_left="<<stick_spring.left<<"\nstick_strength_right="<<stick_spring.right<<'\n';
        out<<"stick_look="<<cfg.stick.enabled<<"\nstick_turn_character="<<cfg.stick.turn_character
            <<"\nstick_invert_y="<<cfg.stick.invert_y<<"\nstick_yaw_speed="<<cfg.stick.yaw_speed
            <<"\nstick_pitch_speed="<<cfg.stick.pitch_speed<<"\nstick_deadzone="<<cfg.stick.deadzone<<'\n';
        out<<"smooth_transitions="<<cfg.stick.smooth_transitions<<"\nmount_smoothing="<<cfg.stick.mount_smoothing
            <<"\ndismount_smoothing="<<cfg.stick.dismount_smoothing<<'\n';
        out<<"board_look="<<cfg.board.enabled<<"\nboard_modifier="<<cfg.board.modifier
            <<"\nboard_center_on_modifier="<<cfg.board.center_on_modifier
            <<"\nboard_center_on_stick="<<cfg.board.center_on_stick
            <<"\nboard_continue_auto_center="<<cfg.board.continue_auto_center
            <<"\nboard_keep_native_input="<<cfg.board.keep_native_input
            <<"\nboard_look_smoothing="<<cfg.board.look_smoothing
            <<"\nboard_yaw_limit="<<cfg.board.yaw_limit<<"\nboard_pitch_limit="<<cfg.board.pitch_limit<<"\nboard_invert_y="<<cfg.board.invert_y
            <<"\nboard_yaw_speed="<<cfg.board.yaw_speed<<"\nboard_pitch_speed="<<cfg.board.pitch_speed
            <<"\nboard_deadzone="<<cfg.board.deadzone<<"\nboard_center_smoothing="<<cfg.board.center_smoothing
            <<"\nboard_center_pitch="<<cfg.board.center_pitch<<'\n';
        out<<"menu_combo="<<cfg.menu_combo<<'\n';
        out<<"menu_key="<<cfg.menu_key<<'\n';
        const std::set<std::string> shared{"first_person","noclip","forward_velocity","up_velocity","fov","spring","offset_x","offset_y","offset_z","pitch","yaw","roll","strength_up","strength_down","strength_left","strength_right","on_board_only","menu_key","menu_combo","look_up","look_down","look_left","look_right","board_modifier","native_stabilize","native_follow_flips","native_camera_cycle","native_smoothing","native_head_pitch","native_head_roll","native_bob"};
        std::istringstream lines(out.str()); std::string line;
        while(std::getline(lines,line)) {
            const auto key=line.substr(0,line.find('='));
            if(include_shared || !shared.contains(key)) result<<line<<'\n';
        }
    return result.str();
}
void write_config(const std::filesystem::path& path,const Config& cfg,bool include_shared) {
    dingosdk::Json json=dingosdk::Json::object();json["schema"]=1;dingosdk::Json options=dingosdk::Json::object();
    std::istringstream input(config_text(cfg,include_shared));std::string line;
    while(std::getline(input,line)) {const auto equal=line.find('=');if(equal!=std::string::npos)options[line.substr(0,equal)]=std::stod(line.substr(equal+1));}
    json["options"]=std::move(options);
    if(!dingosdk::profile_config::write(path,json))throw std::runtime_error("Could not save First Person settings.");
}
}
