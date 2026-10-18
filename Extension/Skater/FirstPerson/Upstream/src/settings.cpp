#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include "Engine/Core/Config/profile_config.h"
#include <sstream>
#include <iomanip>
#include <set>
#include <limits>
#include <cctype>
#include <fstream>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cwctype>

namespace jishsfirstpersonmod {
SettingsStore::SettingsStore(std::filesystem::path path,Config initial):path_(std::move(path)) {
    initialize_stick_smoothing(initial); validate_config(initial); model_.config=initial; model_.revision=1;update_binding_conflicts();
}
bool SettingsStore::submit(const Command& command,std::uint64_t now) {
    std::lock_guard lock(mutex_);
    try {
        if(command.kind==CommandKind::set_enabled) {
            enabled_=command.enabled; model_.requested_enabled=command.enabled; model_.feedback="Waiting for the camera update.";saving_feedback_=false;feedback_until_=0; return true;
        }
        if(command.kind==CommandKind::retry_save) { model_.unsaved=true; due_=now; model_.feedback="Saving settingsÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦"; return true; }
        auto next=model_.config;
        bool immediate{};
        switch(command.kind) {
        case CommandKind::spectator_first_person: next.spectator_first_person=command.enabled;immediate=true;break;
        case CommandKind::third_person_recovery: next.third_person_recovery=command.enabled;immediate=true;break;
        case CommandKind::look_only:
            next.look_only=command.enabled;enabled_.reset();model_.requested_enabled.reset();immediate=true;break;
        case CommandKind::spring_on_foot: next.disable_spring_on_foot=command.enabled;immediate=true;break;
        case CommandKind::menu_key:
            if(command.action!=VK_INSERT && native_key_overlap(command.action,next.native)) throw std::runtime_error("That panel key is already used by ReSkate.");
            next.menu_key=command.action;immediate=true;break;
        case CommandKind::apply_preset:
            next=command.preset; initialize_stick_smoothing(next);next.cycle_restore_pending=model_.config.cycle_restore_pending;next.cycle_restore_enabled=model_.config.cycle_restore_enabled;next.native=model_.config.native;next.reserved=model_.config.reserved;next.menu_key=model_.config.menu_key;next.menu_combo=model_.config.menu_combo;next.look_only=model_.config.look_only;
            validate_config(next);
            immediate=true;break;
        case CommandKind::board_settings: next.board=command.board;break;
        case CommandKind::stick_settings: next.stick=command.stick;break;
        case CommandKind::glances_enabled: next.glances_enabled=command.enabled;immediate=true;break;
        case CommandKind::toggle_glances: next.toggle_glances=command.enabled;immediate=true;break;
        case CommandKind::remember_marker_glance: next.remember_marker_glance=command.enabled;immediate=true;break;
        case CommandKind::bail_collision: next.bail_camera_collision=command.enabled;immediate=true;break;
        case CommandKind::binding:
            if(command.action>6) throw std::runtime_error("Unknown controller action.");
            if(command.action==6) next.menu_combo=command.mask;
            else if(command.action==5) next.board.modifier=command.mask;
            else if(command.action==0) next.toggle=command.mask; else next.look[command.action-1].combo=command.mask;
            immediate=true; break;
        case CommandKind::angle:
            if(command.action<1 || command.action>4) throw std::runtime_error("Unknown look direction.");
            next.look[command.action-1].angle=command.value; break;
        case CommandKind::camera_settings: next.spring=command.spring; next.fov=command.value; break;
        case CommandKind::link_smoothing:
            if(!command.enabled && !next.look_smoothing_initialized) {
                next.look_spring=next.spring; next.look_spring.offset={}; next.look_spring.rotation={};
                next.look_smoothing_initialized=true;
            }
            next.link_smoothing=command.enabled; immediate=true; break;
        case CommandKind::stick_smoothing:
            next.stick_spring=command.spring; next.stick_spring.offset={}; next.stick_spring.rotation={};
            next.stick_smoothing_initialized=true; break;
        case CommandKind::look_settings:
            next.look_spring=command.spring; next.look_spring.offset={}; next.look_spring.rotation={};
            next.look_smoothing_initialized=true; break;
        case CommandKind::cycle_restore:
            next.cycle_restore_pending=command.action!=0;next.cycle_restore_enabled=command.enabled;immediate=true;break;
        case CommandKind::comfort_options:
            next.third_person_bailing=command.third_person_bailing; next.on_board_only=command.on_board_only;
            immediate=true; break;
        default: throw std::runtime_error("Unknown menu request.");
        }
        // Existing conflicts from a stock-side edit must not prevent fixing a
        // different row. Validate ranges globally and new chords individually.
        auto ranges=next; ranges.toggle=0; ranges.reserved={}; for(auto& look:ranges.look) look.combo=0;
        validate_config(ranges);
        if(command.kind==CommandKind::binding) {
            if(!first_person_vendor::valid_controller_combo(command.mask)) throw std::runtime_error("Unsupported controller buttons.");
            if(command.mask) {
                if(native_binding_overlap(command.mask,next.native)) throw std::runtime_error("Those buttons are already used by ReSkate.");
                if(!next.native.active) for(auto reserved:next.reserved) if(reserved==command.mask) throw std::runtime_error("Those buttons are already used for Noclip or a boost.");
                if(command.action!=0 && next.toggle==command.mask) throw std::runtime_error("Those buttons are already used for First Person.");
                for(unsigned i=0;i<4;++i) if(command.action!=i+1 && next.look[i].combo==command.mask) throw std::runtime_error("Those buttons are already used for a glance.");
            }
        }
        model_.config=next; ++model_.revision; model_.unsaved=true;
        update_binding_conflicts();
        due_=now+(immediate ? 0:750);
        saving_feedback_=true;feedback_until_=0;
        model_.feedback=command.kind==CommandKind::apply_preset ?
            (model_.binding_conflict ? "Preset applied. Buttons already used by another control are paused. Choose new buttons. Saving settingsÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦":"Preset applied. Saving settingsÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦"):
            "Saving settingsÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦"; return true;
    } catch(const std::exception& e) { model_.feedback=e.what();saving_feedback_=false;feedback_until_=0; return false; }
}
MenuSnapshot SettingsStore::snapshot(std::uint64_t now) const { std::lock_guard lock(mutex_);auto result=model_;if(feedback_until_ && now>=feedback_until_) result.feedback.clear();return result; }
void SettingsStore::set_feedback(const std::string& text,std::uint64_t duration) { std::lock_guard lock(mutex_); model_.feedback=text;saving_feedback_=false;feedback_until_=duration ? GetTickCount64()+duration:0; }
void SettingsStore::mark_enabled_queued(bool accepted) {
    std::lock_guard lock(mutex_);enabled_.reset();
    if(!accepted) { model_.requested_enabled.reset();model_.feedback="Could not change view. Check ReSkate's menu for details."; }
}
void SettingsStore::acknowledge_master(bool enabled) {
    std::lock_guard lock(mutex_);
    if(model_.requested_enabled==enabled) { model_.requested_enabled.reset();if(model_.feedback=="Waiting for the camera update.") model_.feedback.clear(); }
}
void SettingsStore::update_binding_conflicts() {
    model_.conflicts=find_binding_conflicts(model_.config);
    model_.binding_conflict=model_.conflicts.any();
    if(model_.binding_conflict) { saving_feedback_=false;feedback_until_=0;model_.feedback="Buttons already used by another control are paused. Clear or record new buttons. Right-stick look still works."; }
}
void SettingsStore::refresh_native_controls(const NativeControls& native) {
    std::lock_guard lock(mutex_);if(model_.config.native==native) return;
    model_.config.native=native;++model_.revision;
    update_binding_conflicts();
}
void SettingsStore::refresh_shared(float fov, const first_person_vendor::first_person::Settings& spring, bool board_only, NativeCameraOptions camera) {
    std::lock_guard lock(mutex_);
    auto& config=model_.config; const auto& old=config.spring;
    if(config.fov==fov && config.on_board_only==board_only && old.enabled==spring.enabled &&
       old.offset==spring.offset && old.rotation==spring.rotation && old.up==spring.up &&
       old.down==spring.down && old.left==spring.left && old.right==spring.right && config.camera==camera) return;
    config.fov=fov;config.spring=spring;config.on_board_only=board_only;config.camera=camera;++model_.revision;
    // Mirrors are transient. ReSkate is the sole owner and persistence writer.
}
void SettingsStore::refresh_reserved(const std::array<std::uint32_t,3>& reserved) {
    for(auto value:reserved) if(!first_person_vendor::valid_controller_combo(value)) return;
    std::lock_guard lock(mutex_);
    if(model_.config.reserved==reserved) return;
    model_.config.reserved=reserved; ++model_.revision;
    update_binding_conflicts();
}
std::optional<bool> SettingsStore::take_enabled() { std::lock_guard lock(mutex_); const auto result=enabled_; enabled_.reset(); if(result) model_.requested_enabled.reset(); return result; }
void SettingsStore::publish_runtime(bool active,bool available,const std::string& status) {
    std::lock_guard lock(mutex_); model_.active=active; model_.camera_available=available; model_.status=status;
    if(model_.requested_enabled && *model_.requested_enabled==active) { model_.requested_enabled.reset();if(model_.feedback=="Waiting for the camera update.") model_.feedback.clear(); }
}
void SettingsStore::flush_due(std::uint64_t now) {
    Config next; std::uint64_t revision{};
    {
        std::lock_guard lock(mutex_);
        if(!model_.unsaved || now<due_) return;
        next=model_.config; revision=model_.revision;
    }
    try {
        write_config(path_,next,false);
        std::lock_guard lock(mutex_);
        if(revision==model_.revision) {
            model_.unsaved=false;model_.save_failed=false;model_.save_error.clear();
            if(saving_feedback_) {
                model_.feedback=model_.binding_conflict ? "Buttons already used by another control are paused. Choose new buttons.":"Current settings saved.";
                saving_feedback_=false;feedback_until_=model_.binding_conflict ? 0:GetTickCount64()+3000;
            }
        }
    } catch(const std::exception& e) {
        std::lock_guard lock(mutex_);
        log(std::string("Settings save failed: ")+e.what());
        saving_feedback_=false;feedback_until_=0;
        model_.feedback="Applied, but not saved. Check folder permissions and retry. See the mod log for details.";
        model_.save_failed=true;model_.save_error=model_.feedback;
        due_=~std::uint64_t{}; // Retry explicitly; never hammer an unwritable disk.
    }
}

namespace {
SettingsStore* live_store{};
std::atomic<CameraSink> camera_sink{};
std::atomic<ActivationSink> activation_sink{};
std::atomic<BackgroundFlush> background_flush{};
HANDLE changed{};
DWORD WINAPI saver(void*) noexcept {

    for(;;) {
        WaitForSingleObject(changed,50);
        try {
            const auto now=GetTickCount64();

            live_store->flush_due(now);
            if(const auto flush=background_flush.load()) flush();
        } catch(...) {}
    }
}
}
bool initialize_settings() {
    const auto target=data_directory()/L"first-person-settings.json";
    const auto initial=std::filesystem::exists(target) ?
        InitialSettings{read_config(target),"Saved settings loaded.",false}:
        InitialSettings{Config{},"Native defaults loaded.",true};
    live_store=new SettingsStore(target,initial.config);
    live_store->publish_runtime(false,false,"Waiting for the native camera.");
    log(initial.message);
    if(initial.save) {
        Command request; request.kind=CommandKind::retry_save; live_store->submit(request,GetTickCount64());
        live_store->flush_due(GetTickCount64());
    }
    if(!initial.save && initial.message!="Saved settings loaded.") live_store->set_feedback(initial.message);
    changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    const auto thread=changed ? CreateThread(nullptr,0,saver,nullptr,0,nullptr):nullptr;
    if(!thread) { log("ERROR: settings save worker could not start."); return false; }
    CloseHandle(thread); return true;
}
MenuSnapshot menu_snapshot() { return live_store ? live_store->snapshot():MenuSnapshot{}; }
bool submit_stock_command(const Command& command) {
    if(!live_store) return false;
    const auto accepted=live_store->submit(command,GetTickCount64());
    if(changed) SetEvent(changed); return accepted;
}
void set_camera_sink(CameraSink sink) { camera_sink=sink; }
void set_activation_sink(ActivationSink sink) { activation_sink=sink; }
void set_background_flush(BackgroundFlush flush) { background_flush=flush; }
bool submit_command(const Command& command) {
    const auto before=menu_snapshot().config;
    if(!submit_stock_command(command)) return false;
    if(command.kind==CommandKind::set_enabled) {
        if(const auto sink=activation_sink.load()) {
            const auto accepted=sink(command.enabled);
            live_store->mark_enabled_queued(accepted);return accepted;
        }
    }
    if(command.kind==CommandKind::camera_settings || command.kind==CommandKind::apply_preset) {
        if(const auto sink=camera_sink.load()) {
            std::string error;
            if(!sink(before,menu_snapshot().config,error) && live_store)
            {
                log("Stock settings sync failed: "+error);
                live_store->set_feedback("Mod settings applied. ReSkate could not update its camera settings. See the mod log.");
            }
        }
    }
    return true;
}

namespace {
std::string profile_name(const std::string& name) {
    if(name.empty()) throw std::runtime_error("Enter a profile name.");
    if(name.size()>48) throw std::runtime_error("That name is too long.");
    auto lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(lower=="recommended" || lower=="stock" || lower=="map") throw std::runtime_error("That name belongs to a built-in profile.");
    return name;
}
dingosdk::Json profile_catalog(const std::filesystem::path& directory) {
    const auto json=dingosdk::profile_config::read(directory.parent_path()/L"first-person-profiles.json");
    if(json && json->is_object()) return *json;
    dingosdk::Json result=dingosdk::Json::object();result["schema"]=1;result["presets"]=dingosdk::Json::object();return result;
}
bool preset_feedback(const std::function<void()>& operation,const char* success) {
    if(!live_store) return false;
    try { operation();live_store->set_feedback(success,4000);return true; }
    catch(const std::exception& e) { log(std::string("Preset operation failed: ")+e.what());live_store->set_feedback(std::string("Preset: ")+e.what());return false; }
}
}
std::vector<std::string> PresetLibrary::list() const {
    std::vector<std::string> result;const auto json=profile_catalog(directory_);
    if(json.contains("presets") && json.at("presets").is_object())
        for(const auto& [name,values]:json.at("presets").items())
            if(values.is_object() && !name.empty() && name.size()<=48) result.push_back(name);
    return result;
}
std::string PresetLibrary::save(const std::string& text,const Config& config,bool) const {
    const auto name=profile_name(text);auto json=profile_catalog(directory_);dingosdk::Json values=dingosdk::Json::object();
    const std::set<std::string> binds{"first_person","look_up","look_down","look_left","look_right","board_modifier","menu_key","menu_combo","noclip","forward_velocity","up_velocity","cycle_restore_pending","cycle_restore_enabled"};
    std::istringstream input(config_text(config));std::string line;
    while(std::getline(input,line)) {
        const auto equal=line.find('=');if(equal==std::string::npos)continue;
        const auto key=line.substr(0,equal);if(binds.contains(key))continue;
        values[key]=std::stod(line.substr(equal+1));
    }
    json["presets"][name]=std::move(values);
    if(!dingosdk::profile_config::write(directory_.parent_path()/L"first-person-profiles.json",json))
        throw std::runtime_error("Could not save First Person profiles.");
    return name;
}
Config PresetLibrary::load(const std::string& text) const {
    const auto json=profile_catalog(directory_);
    if(!json.contains("presets") || !json.at("presets").contains(text))throw std::runtime_error("Profile no longer exists.");
    std::ostringstream values;values<<std::setprecision(std::numeric_limits<double>::max_digits10);
    for(const auto& [key,value]:json.at("presets").at(text).items())if(value.is_number())values<<key<<'='<<value.get<double>()<<'\n';
    return read_config_text(values.str());
}
void PresetLibrary::erase(const std::string& text) const {
    auto json=profile_catalog(directory_);
    if(!json.contains("presets") || !json["presets"].erase(text))throw std::runtime_error("Profile no longer exists.");
    if(!dingosdk::profile_config::write(directory_.parent_path()/L"first-person-profiles.json",json))
        throw std::runtime_error("Could not save First Person profiles.");
}
std::vector<std::string> personal_presets() {
    try { return PresetLibrary(data_directory()/L"presets").list(); }
    catch(const std::exception& e) { if(live_store) live_store->set_feedback(std::string("Cannot list presets: ")+e.what());return {}; }
}
bool save_personal_preset(const std::string& name,bool overwrite) {
    return preset_feedback([&] { PresetLibrary(data_directory()/L"presets").save(name,menu_snapshot().config,overwrite); },(std::string(overwrite ? "Preset overwritten: ":"Preset saved: ")+name).c_str());
}
bool delete_personal_preset(const std::string& name) {
    return preset_feedback([&] { PresetLibrary(data_directory()/L"presets").erase(name); },(std::string("Preset deleted: ")+name).c_str());
}
bool load_personal_preset(const std::string& name) {
    if(!live_store) return false;
    try { Command command;command.kind=CommandKind::apply_preset;command.preset=PresetLibrary(data_directory()/L"presets").load(name);if(!submit_command(command)) return false;if(!menu_snapshot().feedback.starts_with("Mod settings applied.")) live_store->set_feedback(std::string("Preset loaded: ")+name,4000);return true; }
    catch(const std::exception& e) { log(std::string("Preset load failed: ")+e.what());live_store->set_feedback(std::string("Could not load preset: ")+e.what());return false; }
}
bool load_recommended_preset() {
    if(!live_store) return false;
    try {
        Command command;command.kind=CommandKind::apply_preset;
        command.preset=read_config(module_directory()/L"FirstPersonNative/presets/bundled/recommended.json");
        if(!submit_command(command)) return false;
        if(!menu_snapshot().feedback.starts_with("Mod settings applied.")) live_store->set_feedback("Preset loaded: Recommended",4000);return true;
    } catch(const std::exception& error) {
        log(std::string("Recommended preset could not load: ")+error.what());
        live_store->set_feedback("Could not load the recommended preset. See the mod log.");return false;
    }
}
std::optional<bool> take_enabled_command() { return live_store ? live_store->take_enabled():std::optional<bool>{}; }
void publish_shared_camera(float fov, const first_person_vendor::first_person::Settings& spring, bool board_only, NativeCameraOptions camera) { if(live_store) live_store->refresh_shared(fov,spring,board_only,camera); }
void publish_native_controls(const NativeControls& native) { if(live_store) live_store->refresh_native_controls(native); }
void publish_runtime(bool active,bool available,const std::string& status) { if(live_store) live_store->publish_runtime(active,available,status); }
void acknowledge_master(bool enabled) { if(live_store) live_store->acknowledge_master(enabled); }
}
