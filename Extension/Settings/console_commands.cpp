#include "Extension/Console/commands.h"
#include "named_settings.h"
#include "gameplay_settings_override.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <format>
namespace dingosdk::console {
namespace {
const NamedSettingModel *named_model(const Model &model, std::size_t index, std::string_view name) {
    // The runtime only appends names, so the registry's index normally still
    // matches. Avoid searching thousands of rows for every autocomplete suggestion.
    if (index < model.engine_settings.size() && equal(model.engine_settings[index].name, name))
        return &model.engine_settings[index];
    for (const auto &setting : model.engine_settings)
        if (equal(setting.name, name)) return &setting;
    return nullptr;
}
}
void register_settings_commands(Commands &registry) {
    using GroupModel = overlay::OfflineFeatureGroupModel;
    struct Setting {
        const char *name;
        const char *alias;
        const char *description;
        overlay::OfflineFeatureGroup group;
        GroupModel overlay::OfflineFeatureModel::*member;
    };
    const Setting settings[]{
        {"activities", "feature.activities", "Enable offline activity features",
         overlay::OfflineFeatureGroup::activities, &overlay::OfflineFeatureModel::activities},
        {"fasttravel", "feature.fast_travel", "Enable fast travel features", overlay::OfflineFeatureGroup::fast_travel,
         &overlay::OfflineFeatureModel::fast_travel},
        {"progression enabled", "feature.progression", "Enable offline progression systems",
         overlay::OfflineFeatureGroup::progression, &overlay::OfflineFeatureModel::progression},
        {"developermenus", "feature.developer_menus", "Enable the available developer menus",
         overlay::OfflineFeatureGroup::developer_menus, &overlay::OfflineFeatureModel::developer_menus},
        {"boardwear", "feature.board_wear", "Enable the shipped board wear graph",
         overlay::OfflineFeatureGroup::board_wear, &overlay::OfflineFeatureModel::board_wear},
        {"playercollision", "feature.player_collision", "Allow the game's own party collision option",
         overlay::OfflineFeatureGroup::player_collision, &overlay::OfflineFeatureModel::player_collision}};
    for (const auto &setting : settings) {
        auto entry = variable(setting.name, setting.description, Group::gameplay, argument("0|1", Type::boolean));
        entry.aliases = {setting.alias};
        entry.inspect = [setting](const Model &m) {
            const auto &value = m.offline.*setting.member;
            auto result = boolean_state(value.available, value.effective, "These engine settings are unavailable.");
            result.overridden = value.override_active;
            return result;
        };
        entry.run = [setting](const Model &, const Values &args, const Output &) {
            request_feature(setting.group, std::get<bool>(args[0]));
        };
        entry.reset = [setting](const Model &, const Output &) { request_feature(setting.group, false); };
        registry.add(std::move(entry));
    }
    // Definitions and runtime reads come from the same native field contract.
    for (const auto &definition : gameplay_engine_variable_definitions()) {
        auto entry = variable(definition.name, definition.description, Group::engine, argument("0|1", Type::boolean));
        entry.aliases = {definition.qualified_name};
        entry.inspect = [name = definition.name](const Model &m) {
            const auto it = std::find_if(m.offline.variables.begin(), m.offline.variables.end(),
                                         [&](const auto &v) { return equal(name, v.name); });
            if (it == m.offline.variables.end())
                return State{false, {}, "The engine settings object has not been resolved.", {}, false};
            auto result =
                boolean_state(it->available, it->value, "The engine field is unavailable for the current game state.");
            result.overridden = it->override_active;
            result.detail = it->override_active ? "Session override active" : "Engine value";
            return result;
        };
        entry.run = [name = definition.name](const Model &, const Values &args, const Output &) {
            request_engine_variable(name, false, std::get<bool>(args[0]));
        };
        entry.reset = [name = definition.name](const Model &, const Output &) {
            request_engine_variable(name, true, false);
        };
        registry.add(std::move(entry));
    }
    // Engine settings are listed from the game's own settings registry
    // (named_settings.cpp); names seen in earlier sessions are included.
    const auto names = named_setting_names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto &name = names[index];
        if (registry.find(name) || !parse_console_command(name))
            continue;
        auto value = argument("value");
        value.complete = [index, name](const Model &m, auto) {
            if (const auto *v = named_model(m, index, name))
                return v->choices;
            return std::vector<std::string>{};
        };
        auto entry = variable(name, {}, Group::engine, std::move(value));
        entry.inspect = [index, name](const Model &m) {
            if (const auto *v = named_model(m, index, name))
                return State{v->available, v->available ? std::optional<std::string>(v->value) : std::nullopt,
                             v->reason, v->type + (v->override_active ? "; session override" : "; engine value"),
                             v->override_active};
            return State{false, {}, "Waiting for native settings on the game update thread.", {}, false};
        };
        entry.run = [name](const Model &, const Values &args, const Output &out) {
            out(request_named_setting(name, std::get<std::string>(args[0]), false));
        };
        entry.reset = [name](const Model &, const Output &out) { out(request_named_setting(name, {}, true)); };
        registry.add(std::move(entry));
    }
    auto scale = argument("scale", Type::number);
    scale.minimum = min_menu_scale;
    scale.maximum = max_menu_scale;
    auto menu = variable("ui scale", "Scale the ReSkate menu and its text", Group::console, std::move(scale));
    menu.execution = Execution::local;
    menu.inspect = [](const Model &m) {
        return State{true, std::format("{:.2f}", m.menu_scale), {}, "Saved with your profile.", false};
    };
    menu.run = [](const Model &, const Values &args, const Output &out) {
        const auto value = static_cast<float>(std::get<double>(args[0]));
        out(profile_runtime::set_local_menu_scale(value)
                ? std::format("Menu scale set to {:.2f}.", value)
                : std::format("error: Choose a scale from {:.2f} to {:.2f}.", min_menu_scale, max_menu_scale));
    };
    menu.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_scale(default_menu_scale);
        out("Menu scale reset.");
    };
    registry.add(std::move(menu));

    auto full_scale = argument("scale", Type::number);
    full_scale.minimum = min_menu_scale;
    full_scale.maximum = max_menu_scale;
    auto menu_full = variable("ui scalefull", "Scale the full screen ReSkate menu and its text", Group::console, std::move(full_scale));
    menu_full.execution = Execution::local;
    menu_full.inspect = [](const Model &m) {
        return State{true, std::format("{:.2f}", m.menu_scale_fullscreen), {}, "Saved with your profile.", false};
    };
    menu_full.run = [](const Model &, const Values &args, const Output &out) {
        const auto value = static_cast<float>(std::get<double>(args[0]));
        out(profile_runtime::set_local_menu_scale(value, true)
                ? std::format("Full screen menu scale set to {:.2f}.", value)
                : std::format("error: Choose a scale from {:.2f} to {:.2f}.", min_menu_scale, max_menu_scale));
    };
    menu_full.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_scale(default_menu_scale, true);
        out("Full screen menu scale reset.");
    };
    registry.add(std::move(menu_full));

    auto fullscreen = variable("ui fullscreen", "Show the ReSkate menu full screen, made for a controller (0: a window)",
                               Group::console, argument("0|1", Type::boolean));
    fullscreen.execution = Execution::local;
    fullscreen.inspect = [](const Model &m) { return boolean_state(true, m.menu_fullscreen, {}); };
    fullscreen.run = [](const Model &, const Values &args, const Output &out) {
        const bool on = std::get<bool>(args[0]);
        profile_runtime::set_local_menu_fullscreen(on);
        out(on ? "The ReSkate menu fills the screen." : "The ReSkate menu is a window.");
    };
    fullscreen.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_fullscreen(false);
        out("The ReSkate menu is a window.");
    };
    registry.add(std::move(fullscreen));
}
} // namespace dingosdk::console
