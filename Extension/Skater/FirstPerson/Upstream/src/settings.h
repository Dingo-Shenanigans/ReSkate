#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/mod.h"
#include "Extension/Skater/FirstPerson/Upstream/src/binding_conflicts.h"
#include <mutex>
#include <optional>
#include <vector>

namespace jishsfirstpersonmod {
enum class CommandKind { glances_enabled, remember_marker_glance, board_settings, look_only, set_enabled, binding, angle, camera_settings, retry_save, link_smoothing, look_settings, stick_smoothing, comfort_options, stick_settings, apply_preset, recommended_preset=apply_preset, menu_key, spring_on_foot, toggle_glances, cycle_restore, bail_collision, third_person_recovery, spectator_first_person };
struct Command {
    CommandKind kind{};
    unsigned action{}; // 0 toggle, 1..4 directional glances
    std::uint32_t mask{};
    float value{};
    bool enabled{};
    bool third_person_bailing{},on_board_only{};
    first_person_vendor::first_person::Settings spring;
    StickSettings stick;
    BoardLookSettings board;
    Config preset;
};
struct MenuSnapshot {
    Config config;
    std::uint64_t revision{};
    bool active{}, camera_available{}, unsaved{},binding_conflict{},save_failed{};
    BindingConflicts conflicts;
    std::optional<bool> requested_enabled;
    std::string status,feedback,save_error;
};
class SettingsStore {
public:
    SettingsStore(std::filesystem::path path,Config initial);
    bool submit(const Command& command,std::uint64_t now);
    MenuSnapshot snapshot(std::uint64_t now=GetTickCount64()) const;
    std::optional<bool> take_enabled();
    void publish_runtime(bool active,bool available,const std::string& status);
    void flush_due(std::uint64_t now);
    void refresh_shared(float fov, const first_person_vendor::first_person::Settings& spring, bool board_only, NativeCameraOptions camera={});
    void refresh_reserved(const std::array<std::uint32_t,3>& reserved);
    void refresh_native_controls(const NativeControls& native);
    void set_feedback(const std::string& text,std::uint64_t duration=0);
    void mark_enabled_queued(bool accepted);
    void acknowledge_master(bool enabled);
private:
    void update_binding_conflicts(); // Caller holds mutex_, except during construction.
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    MenuSnapshot model_;
    std::optional<bool> enabled_;
    std::uint64_t due_{},feedback_until_{};
    bool saving_feedback_{};
};
struct InitialSettings { Config config; std::string message; bool save{}; };
bool initialize_settings();
MenuSnapshot menu_snapshot();
bool submit_command(const Command& command);
bool load_recommended_preset();
class PresetLibrary {
public:
    explicit PresetLibrary(std::filesystem::path directory):directory_(std::move(directory)) {}
    std::vector<std::string> list() const;
    std::string save(const std::string& name,const Config& config,bool overwrite=false) const;
    Config load(const std::string& name) const;
    void erase(const std::string& name) const;
private:
    std::filesystem::path directory_;
};
std::vector<std::string> personal_presets();
bool save_personal_preset(const std::string& name,bool overwrite=false);
bool load_personal_preset(const std::string& name);
bool delete_personal_preset(const std::string& name);
using CameraSink=bool(*)(const Config&,const Config&,std::string&);
void set_camera_sink(CameraSink sink);
using ActivationSink=bool(*)(bool);
void set_activation_sink(ActivationSink sink);
using BackgroundFlush=void(*)();
void set_background_flush(BackgroundFlush flush);
bool submit_stock_command(const Command& command);
std::optional<bool> take_enabled_command();
void publish_runtime(bool active,bool available,const std::string& status);
void publish_native_controls(const NativeControls& native);
void publish_shared_camera(float fov, const first_person_vendor::first_person::Settings& spring, bool board_only, NativeCameraOptions camera={});
void acknowledge_master(bool enabled);
}
