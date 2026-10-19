#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Input/controller_bindings.h"
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"
#include "Extension/Skater/FirstPerson/Upstream/src/stick_look.h"
#include "Extension/Skater/FirstPerson/Upstream/src/board_look.h"
#include "Extension/Skater/FirstPerson/Upstream/src/native_controls.h"

namespace jishsfirstpersonmod {
struct GameplayInput;
struct NativeCameraOptions {
    bool stabilize=true,follow_flips=true,camera_cycle=false;
    float smoothing=60,head_pitch=86,head_roll=0,bob=25;
    bool operator==(const NativeCameraOptions&) const=default;
};
struct Config {
    NativeCameraOptions camera;
    bool glances_enabled=true;
    bool toggle_glances=false;
    bool remember_marker_glance=false;
    NativeControls native; // Observed settings/bindings, never saved in the mod INI.
    unsigned menu_key=VK_INSERT;
    std::uint32_t menu_combo{};
    std::uint32_t toggle{};
    first_person_vendor::FirstPersonLookControls look = first_person_vendor::default_look_controls;
    first_person_vendor::first_person::Settings spring;
    float fov{};
    bool link_smoothing=true, look_smoothing_initialized=false;
    first_person_vendor::first_person::Settings look_spring;
    first_person_vendor::first_person::Settings stick_spring;
    bool stick_smoothing_initialized=false;
    bool look_only=false;
    bool third_person_bailing=false, on_board_only=false;
    bool third_person_recovery=false;
    bool third_person_camera_switching() const { return third_person_bailing || (!look_only && third_person_recovery); }
    bool bail_camera_collision=true;
    bool spectator_first_person=false;
    bool cycle_restore_pending=false,cycle_restore_enabled=false; // Native preference retained during comfort override.
    bool disable_spring_on_foot=false;
    StickSettings stick;
    BoardLookSettings board;
    std::array<std::uint32_t,3> reserved{};
};
inline first_person_vendor::first_person::Settings glance_settings(const Config& config) {
    auto settings=config.link_smoothing ? config.spring:config.look_spring;
    settings.offset={}; settings.rotation={}; return settings;
}
inline void initialize_stick_smoothing(Config& config) {
    if(config.stick_smoothing_initialized) return;
    config.stick_spring=glance_settings(config);
    if(config.link_smoothing && config.disable_spring_on_foot && !config.look_only) config.stick_spring.enabled=false;
    config.stick_smoothing_initialized=true;
}
void log(const std::string& text) noexcept;
std::filesystem::path module_directory();
Config read_config(const std::filesystem::path& path);
Config read_config_text(const std::string& text);
std::string config_text(const Config& config,bool include_shared=true);
void validate_config(const Config& config);
void write_config(const std::filesystem::path& path,const Config& config,bool include_shared=true);
std::filesystem::path data_directory();
}
