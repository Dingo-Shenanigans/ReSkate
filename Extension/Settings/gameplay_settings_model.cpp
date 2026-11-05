#include "gameplay_settings_override.h"
#include "gameplay_settings_internal.h"
#include "Engine/Core/Json/json.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>

namespace dingosdk {
using namespace gameplay_settings_detail;
namespace {
bool group_has_lease(const SettingsState& state, overlay::OfflineFeatureGroup group) {
    for (std::size_t i = 0; i < field_specs.size(); ++i)
        if (field_specs[i].group == group && state.leases[i].owned) return true;
    return false;
}

overlay::OfflineFeatureGroupModel refresh_group_from_variables(
    const SettingsState& state, overlay::OfflineFeatureGroup group) {
    overlay::OfflineFeatureGroupModel model;
    bool found{};
    model.available = true;
    model.effective = true;
    for (std::size_t i = 0; i < field_specs.size(); ++i) {
        if (field_specs[i].group != group) continue;
        found = true;
        model.available = model.available && state.model.variables[i].available;
        model.effective = model.effective && state.model.variables[i].available &&
            state.model.variables[i].value;
    }
    model.available = model.available && found;
    model.effective = model.effective && found;
    // A failed write can leave a byte under a restoration lease before the
    // request reaches its normal success state. Keep that group visibly active
    // so the same checkbox can restore it; Restore All is only a convenience.
    model.override_active = state.requested[group_index(group)] || group_has_lease(state, group);
    return model;
}

bool any_lease(const SettingsState& state) {
    return std::any_of(state.leases.begin(), state.leases.end(),
        [](const Lease& lease) { return lease.owned; });
}


const char* json_bool(bool value) { return value ? "true" : "false"; }

void write_group_json(std::ostringstream& json, const overlay::OfflineFeatureGroupModel& group) {
    json << "{\"available\":" << json_bool(group.available)
         << ",\"effective\":" << json_bool(group.effective)
         << ",\"override_active\":" << json_bool(group.override_active) << '}';
}
} // namespace

namespace gameplay_settings_detail {
void reset_variable_model(SettingsState& state) {
    auto& variables = state.model.variables;
    if (variables.size() != field_specs.size()) variables.resize(field_specs.size());
    for (std::size_t i = 0; i < field_specs.size(); ++i) {
        auto& variable = variables[i];
        variable.name = field_specs[i].name;
        variable.available = false;
        variable.value = false;
        variable.override_active = state.leases[i].owned;
    }
}

void refresh_model(SettingsState& state) {
    ++state.refreshes;
    auto& model = state.model;
    model.available = state.active;
    reset_variable_model(state);
    if (!state.active) return;

    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)> cache{};
    bool abandoned{};
    for (std::size_t i = 0; i < field_specs.size(); ++i) {
        try {
            const auto snapshot = snapshot_field(state, i, cache);
            abandoned = release_replaced_lease(state, snapshot) || abandoned;
            auto& variable = model.variables[i];
            variable.available = true;
            variable.value = snapshot.value == 1;
        } catch (...) {
            model.variables[i].available = false;
            model.variables[i].value = false;
        }
    }
    for (std::size_t i = 0; i < field_specs.size(); ++i)
        model.variables[i].override_active = state.leases[i].owned;
    if (abandoned)
        model.status = "Released replaced gameplay settings identity without a stale write.";

    model.activities = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::activities);
    model.fast_travel = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::fast_travel);
    model.progression = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::progression);
    model.developer_menus = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::developer_menus);
    model.board_wear = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::board_wear);
    model.player_collision = refresh_group_from_variables(state, overlay::OfflineFeatureGroup::player_collision);
    model.settings_owned = any_lease(state);
    const bool all_ready = model.activities.available && model.fast_travel.available &&
        model.progression.available &&
        model.developer_menus.available && model.board_wear.available &&
        model.player_collision.available;
    if (all_ready && !state.registry_ready) {
        state.registry_ready = true;
        if (model.status == "Waiting for the game's settings registry.")
            model.status = "Process-local gameplay settings are ready.";
    }
    std::ostringstream detail;
    detail << "Process-local settings: activities=" << model.activities.effective
           << ", fast_travel_gate=" << model.fast_travel.effective
           << ", progression=" << model.progression.effective
           << ", developer_menus=" << model.developer_menus.effective
           << ", board_wear=" << model.board_wear.effective
           << ", player_collision=" << model.player_collision.effective;
    state.detail = detail.str();
}

GameplaySettingsOverrideObservation observation_locked(const SettingsState& state) {
    std::ostringstream json;
    json << "{\"event\":\"gameplay_settings_override\",\"active\":" << json_bool(state.active)
         << ",\"engine_thread\":" << state.engine_thread
         << ",\"settings_owned\":" << json_bool(state.model.settings_owned)
         << ",\"requests\":" << state.requests
         << ",\"rejected\":" << state.rejected
         << ",\"writes\":" << state.writes << ",\"restores\":" << state.restores
         << ",\"abandoned\":" << state.abandoned
         << ",\"activities\":";
    write_group_json(json, state.model.activities);
    json << ",\"fast_travel\":"; write_group_json(json, state.model.fast_travel);
    json << ",\"progression\":"; write_group_json(json, state.model.progression);
    json << ",\"developer_menus\":"; write_group_json(json, state.model.developer_menus);
    json << ",\"variables\":[";
    for (std::size_t i = 0; i < state.model.variables.size(); ++i) {
        const auto& variable = state.model.variables[i];
        if (i) json << ',';
        json << "{\"name\":" << json::quoted(variable.name)
             << ",\"available\":" << json_bool(variable.available)
             << ",\"value\":" << json_bool(variable.value)
             << ",\"override_active\":" << json_bool(variable.override_active) << '}';
    }
    json << ']';
    json << ",\"status\":" << json::quoted(state.model.status) << '}';
    return {state.model, json.str(), state.detail};
}
} // namespace gameplay_settings_detail
} // namespace dingosdk
