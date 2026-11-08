#include "Extension/Console/commands.h"
#include "named_settings.h"
#include "gameplay_settings_override.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Rendering/replay_export.h"
#include <algorithm>
#include <charconv>
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

    auto layout_choice = argument("sheet|hub");
    layout_choice.choices = {"sheet", "hub"};
    auto layout = variable("ui layout", "Lay the ReSkate menu out as a sheet on the right edge or as a window (hub)", Group::console,
                           std::move(layout_choice));
    layout.execution = Execution::local;
    layout.inspect = [](const Model &m) {
        return State{true, std::string(m.menu_layout == menu_layout_hub ? "hub" : "sheet"), {}, "Saved with your profile.", false};
    };
    layout.run = [](const Model &, const Values &args, const Output &out) {
        const bool hub = lower(std::get<std::string>(args[0])) == "hub";
        profile_runtime::set_local_menu_layout(hub ? menu_layout_hub : menu_layout_sheet);
        out(hub ? "The ReSkate menu is a window." : "The ReSkate menu is a sheet on the right edge.");
    };
    layout.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_layout(menu_layout_sheet);
        out("The ReSkate menu is a sheet on the right edge.");
    };
    registry.add(std::move(layout));

    auto editor_choice = argument("hotbar|rail");
    editor_choice.choices = {"hotbar", "rail"};
    auto editor_layout = variable("editor layout",
                                  "Lay the park editor out with a hotbar of objects along the bottom or a rail of tools down the left",
                                  Group::objects, std::move(editor_choice));
    editor_layout.execution = Execution::local;
    editor_layout.inspect = [](const Model &m) {
        return State{true, std::string(m.park_editor_layout == park_editor_layout_rail ? "rail" : "hotbar"), {},
                     "Saved with your profile.", false};
    };
    editor_layout.run = [](const Model &, const Values &args, const Output &out) {
        const bool rail = lower(std::get<std::string>(args[0])) == "rail";
        profile_runtime::set_local_park_editor_layout(rail ? park_editor_layout_rail : park_editor_layout_hotbar);
        out(rail ? "The park editor has a rail of tools down the left." : "The park editor has a hotbar along the bottom.");
    };
    editor_layout.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_park_editor_layout(park_editor_layout_hotbar);
        out("The park editor has a hotbar along the bottom.");
    };
    registry.add(std::move(editor_layout));

    // The menu saves these itself when its edge or corner is dragged; they are commands so that goes the
    // way the scale does, and so a size that went wrong can be put back.
    auto width = argument("width", Type::number);
    width.minimum = min_menu_sheet_width;
    width.maximum = max_menu_sheet_width;
    auto sheet = variable("ui sheetwidth", "Width of the ReSkate menu's side sheet, in pixels at 1080 lines and scale 1", Group::console,
                          std::move(width));
    sheet.execution = Execution::local;
    sheet.inspect = [](const Model &m) {
        return State{true, m.menu_sheet_width > 0 ? std::format("{:.0f}", m.menu_sheet_width) : std::string("default"), {},
                     "Saved with your profile.", m.menu_sheet_width > 0};
    };
    sheet.run = [](const Model &, const Values &args, const Output &out) {
        const auto value = static_cast<float>(std::get<double>(args[0]));
        out(profile_runtime::set_local_menu_sheet_width(value)
                ? std::format("Side sheet width set to {:.0f}.", value)
                : std::format("error: Choose a width from {:.0f} to {:.0f}.", min_menu_sheet_width, max_menu_sheet_width));
    };
    sheet.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_sheet_width(0);
        out("Side sheet width reset.");
    };
    registry.add(std::move(sheet));

    auto hub = variable("ui hubsize", "Size of the ReSkate menu's window, WIDTHxHEIGHT in pixels at 1080 lines and scale 1", Group::console,
                        argument("WIDTHxHEIGHT"));
    hub.execution = Execution::local;
    hub.inspect = [](const Model &m) {
        const bool saved = m.menu_hub_width > 0 && m.menu_hub_height > 0;
        return State{true, saved ? std::format("{:.0f}x{:.0f}", m.menu_hub_width, m.menu_hub_height) : std::string("default"), {},
                     "Saved with your profile.", saved};
    };
    hub.run = [](const Model &, const Values &args, const Output &out) {
        // "1120x720": two whole or decimal numbers either side of an x.
        float w = 0, h = 0;
        const auto &text = std::get<std::string>(args[0]);
        const auto x = text.find_first_of("xX");
        const auto number = [](std::string_view part, float &into) {
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), into);
            return error == std::errc{} && end == part.data() + part.size();
        };
        const bool parsed = x != std::string::npos && number(std::string_view(text).substr(0, x), w) &&
                            number(std::string_view(text).substr(x + 1), h);
        out(parsed && profile_runtime::set_local_menu_hub_size(w, h)
                ? std::format("Menu window size set to {:.0f}x{:.0f}.", w, h)
                : std::format("error: Give a size such as 1120x720, from {:.0f}x{:.0f} to {:.0f}x{:.0f}.", min_menu_hub_width,
                              min_menu_hub_height, max_menu_hub_side, max_menu_hub_side));
    };
    hub.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_hub_size(0, 0);
        out("Menu window size reset.");
    };
    registry.add(std::move(hub));

    // Read by the exporter as each MP4 export begins (replay_export.cpp), so a change applies to the next.
    auto quality_choice = argument("game|low|medium|high|veryhigh");
    for (const auto name : replay_export::quality_names) quality_choice.choices.emplace_back(name);
    auto quality = variable("replay quality", "Quality of the replay editor's MP4 export: game keeps the game's own bitrate; low, "
                            "medium, high and veryhigh set it from the video's size and frame rate (about 6, 12, 25 and 45 Mbit/s "
                            "at 1080p60)",
                            Group::graphics, std::move(quality_choice));
    quality.execution = Execution::local;
    quality.inspect = [](const Model &m) {
        const auto index = static_cast<std::size_t>(std::clamp(m.replay_export_quality, 0, replay_export::quality_count - 1));
        return State{true, std::string(replay_export::quality_names[index]), {}, "Saved with your profile.",
                     m.replay_export_quality != replay_export::quality_game};
    };
    quality.run = [](const Model &, const Values &args, const Output &out) {
        const auto chosen = lower(std::get<std::string>(args[0]));
        const auto found = std::find(replay_export::quality_names.begin(), replay_export::quality_names.end(), chosen);
        if (found == replay_export::quality_names.end()) {
            out("error: Choose game, low, medium, high or veryhigh.");
            return;
        }
        const int index = static_cast<int>(found - replay_export::quality_names.begin());
        profile_runtime::set_local_replay_export_quality(index);
        out(index == replay_export::quality_game ? std::string("Replay exports keep the game's own bitrate.")
                                                 : std::format("Replay exports are encoded at {} quality.", chosen));
    };
    quality.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_replay_export_quality(replay_export::quality_game);
        out("Replay exports keep the game's own bitrate.");
    };
    registry.add(std::move(quality));
}
} // namespace dingosdk::console
