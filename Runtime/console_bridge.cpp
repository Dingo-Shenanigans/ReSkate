#include "runtime_internal.h"
#include "Extension/Console/commands.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Progression/mission_progression_override.h"
#include "Extension/Settings/gameplay_settings_override.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/skater_slot_override.h"
#include <array>

namespace dingosdk::runtime::detail {
namespace {
dingosdk::overlay::Model console_model_snapshot() {
    auto& r = runtime();
    // Runs on the game thread before a command: the settings it inspects are current.
    publish_named_settings(r);
    dingosdk::overlay::Model model;
    std::shared_ptr<const NamedSettings> settings;
    {
        std::lock_guard lock(r.mutex);
        model = r.model;
        model.debug = r.debug_model;
        model.offline = r.offline_model;
        settings = r.named_settings;
    }
    if (settings) model.engine_settings = *settings;
    return model;
}

void console_callback_result(bool queued, const std::array<char, 512>& result,
                             std::string_view fallback) {
    std::string line = queued ? "ok: " : "error: ";
    line += result[0] ? result.data() : fallback;
    console_line(line);
}

bool queue_console_debug(dingosdk::overlay::DebugAction action, bool enabled = false,
                         float value = 0.0f) {
    std::array<char, 512> result{};
    const dingosdk::overlay::DebugRequest request{action, enabled, value};
    const auto queued = queue_debug(nullptr, request, result.data(), result.size());
    result.back() = '\0';
    console_callback_result(queued, result, "Debug request rejected.");
    return queued;
}

bool queue_console_feature(dingosdk::overlay::OfflineFeatureGroup group, bool enabled) {
    std::array<char, 512> result{};
    const dingosdk::overlay::OfflineFeatureRequest request{group, enabled};
    const auto queued = queue_offline_feature(nullptr, request, result.data(), result.size());
    result.back() = '\0';
    console_callback_result(queued, result, "Gameplay setting request rejected.");
    return queued;
}

void apply_console_engine_variable(std::string_view name,
                                   dingosdk::EngineVariableAction action, bool value) {
    auto& r = runtime();
    {
        std::lock_guard lock(r.mutex);
        if (!r.requests.executing_console(GetCurrentThreadId()) || r.requests.loading()) {
            console_line("error: Engine variables require a scheduled command in an idle local session.");
            return;
        }
    }
    const dingosdk::EngineVariableRequest request{std::string(name), action, value};
    auto observation = dingosdk::update_gameplay_engine_variable(request);
    merge_skater_slot_observation(
        observation.model, dingosdk::skater_slot_override_observation());
    merge_main_mission_observation(
        observation.model, dingosdk::main_mission_override_observation());
    if (observation.json != r.last_gameplay_settings_override_observation) {
        record(observation.json);
        r.last_gameplay_settings_override_observation = observation.json;
    }
    const auto status = observation.model.status;
    {
        std::lock_guard lock(r.mutex);
        r.offline_model = std::move(observation.model);
        ++r.offline_revision;
    }
    console_line(status.empty() ? "Engine variable request completed." : status);
}

std::string apply_console_named_setting(std::string_view name, std::string_view value, bool restore, bool all) {
    auto& r = runtime();
    {
        std::lock_guard lock(r.mutex);
        if (r.engine_thread.load() != GetCurrentThreadId() || !r.named_context_ready || r.observer_failed ||
            r.requests.loading() || !r.requests.executing_console(GetCurrentThreadId()))
            return "error: Native settings require an idle local session on the game update thread.";
    }
    auto result = all ? dingosdk::restore_named_settings() : dingosdk::player_change_named_setting(name, value, restore);
    publish_named_settings(r);
    return result;
}
}
void execute_console_command(const std::string& command) {
    try {
        // A plugin's command never shadows one of ReSkate's: the host refuses those names.
        if (dingosdk::plugins::run_console(command)) return;
        const dingosdk::console::Output output{[](const std::string& line) { console_line(line); }, {}, {}};
        dingosdk::console::game_commands().execute(command, console_model_snapshot(), output);
    } catch (const std::exception& error) {
        console_line(std::string("error: ") + error.what());
    } catch (...) {
        console_line("error: console command raised an internal exception.");
    }
}
}

namespace dingosdk::console {
void request_debug(overlay::DebugAction action, bool enabled, float value) {
    dingosdk::runtime::detail::queue_console_debug(action, enabled, value);
}
void request_feature(overlay::OfflineFeatureGroup group, bool enabled) {
    dingosdk::runtime::detail::queue_console_feature(group, enabled);
}
void request_engine_variable(std::string_view name, bool restore, bool value) {
    dingosdk::runtime::detail::apply_console_engine_variable(name, restore ? EngineVariableAction::restore : EngineVariableAction::set, value);
}
std::string request_named_setting(std::string_view name, std::string_view value, bool restore) {
    return dingosdk::runtime::detail::apply_console_named_setting(name, value, restore, false);
}
std::string request_reset_named_settings() { return dingosdk::runtime::detail::apply_console_named_setting({}, {}, true, true); }
void request_level(const std::string& level, const std::string& start,
    const std::string& sublevel, const std::string& substart) {
    std::array<char, 512> result{};
    const auto queued = dingosdk::runtime::detail::queue_load(nullptr, level.c_str(), start.c_str(), sublevel.c_str(), substart.c_str(),
        result.data(), result.size());
    result.back() = '\0';
    dingosdk::runtime::detail::console_callback_result(queued, result, "Load request rejected.");
}
}
