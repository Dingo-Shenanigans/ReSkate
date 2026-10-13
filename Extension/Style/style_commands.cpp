#include "Extension/Console/commands.h"
#include "style_commands.h"
#include "style_layer.h"
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
namespace dingosdk::console {
namespace {
Entry on_off(const char *name, const char *description, void (*request)(bool), bool (*value)() noexcept, bool reset_to) {
    auto entry = variable(name, description, Group::gameplay, argument("0|1", Type::boolean));
    entry.inspect = [value](const Model &) { return boolean_state(true, value()); };
    entry.run = [request](const Model &, const Values &args, const Output &) { request(std::get<bool>(args[0])); };
    entry.reset = [request, reset_to](const Model &, const Output &) { request(reset_to); };
    return entry;
}
Argument target_argument(bool optional = false) {
    auto target = argument("target", Type::text, optional);
    target.choices.assign(style::family_names.begin(), style::family_names.end());
    target.choices.insert(target.choices.end(), style::flip_trick_names.begin() + 1, style::flip_trick_names.end());
    return target;
}
Argument keyframe_argument(bool optional) {
    auto key = argument("keyframe", Type::unsigned_integer, optional);
    key.minimum = 0;
    key.maximum = static_cast<double>(style::max_keys - 1);
    return key;
}
std::uint8_t key_at(const Values &args, std::size_t i) { return static_cast<std::uint8_t>(std::get<std::uint64_t>(args[i])); }

void add_switches(Commands &registry) {
    auto layer = on_off("style", "Add your style's joint rotations to the skater's animation", style_layer::request_enabled,
                        style_layer::enabled, false);
    layer.inspect = [](const Model &) {
        return boolean_state(style_layer::available(), style_layer::enabled(), std::string(style_layer::unavailable_text));
    };
    registry.add(std::move(layer));
    registry.add(on_off("style share", "Show your style to other players (1) or only to yourself (0)", style_layer::request_share,
                        style_layer::sharing, true));
}

void add_joint_command(Commands &registry) {
    auto joint = argument("joint");
    joint.choices.assign(style::editable_joints.begin(), style::editable_joints.end());
    const auto degrees = [] { return number_argument("degrees", -style::max_degrees, style::max_degrees); };
    auto joint_action = action("style joint",
                               "Rotate a joint about its own X, Y and Z axes in a state or at a flip trick's keyframe. "
                               "All zero removes the rotation",
                               Group::gameplay, {target_argument(), joint, degrees(), degrees(), degrees(), keyframe_argument(true)});
    joint_action.run = [](const Model &, const Values &args, const Output &out) {
        auto target = style::parse_target(std::get<std::string>(args[0]));
        if (target && args.size() > 5) target->key = key_at(args, 5);
        std::string error;
        if (!target)
            out("error: Unknown state or trick.");
        else if (!style_layer::request_joint(*target, std::get<std::string>(args[1]), number_at(args, 2), number_at(args, 3),
                                             number_at(args, 4), error))
            out("error: " + error);
    };
    registry.add(std::move(joint_action));
}

void add_key_commands(Commands &registry) {
    const auto time = [] { return number_argument("time", 0, style::trick_end); };
    const auto where = std::format("Add a keyframe to a flip trick's timeline: 0 to 1 is the {}, 1 to 2 the {}, 2 to 3 the {}",
                                   style::part_names[0], style::part_names[1], style::part_names[2]);
    auto add = action("style key add", where, Group::gameplay, {trick_argument(), time()});
    add.run = [](const Model &, const Values &args, const Output &out) {
        std::string error;
        const auto made = style_layer::request_key_add(trick_at(args, 0), number_at(args, 1), error);
        out(made < 0 ? "error: " + error : "Keyframe " + std::to_string(made) + " added.");
    };
    registry.add(std::move(add));
    auto move = action("style key move", "Move a flip trick's keyframe along its timeline", Group::gameplay,
                       {trick_argument(), keyframe_argument(false), time()});
    move.run = [](const Model &, const Values &args, const Output &out) {
        std::string error;
        if (!style_layer::request_key_move(trick_at(args, 0), key_at(args, 1), number_at(args, 2), error)) out("error: " + error);
    };
    registry.add(std::move(move));
    auto remove = action("style key delete", "Delete a flip trick's keyframe. Keyframes with a higher number move down one",
                         Group::gameplay, {trick_argument(), keyframe_argument(false)});
    remove.run = [](const Model &, const Values &args, const Output &out) {
        std::string error;
        if (!style_layer::request_key_delete(trick_at(args, 0), key_at(args, 1), error)) out("error: " + error);
    };
    registry.add(std::move(remove));
    auto blend = action("style key blendout",
                        "Set the ms from a flip trick's keyframe back to the game's pose. off blends to the next keyframe",
                        Group::gameplay, {trick_argument(), keyframe_argument(false), argument("ms|off", Type::text)});
    blend.run = [](const Model &, const Values &args, const Output &out) {
        const auto &text = std::get<std::string>(args[2]);
        const auto ms = equal(text, "off") ? std::optional<Value>(0.0) : parse_value(Type::number, text);
        std::string error;
        if (!ms)
            out("error: Give a time in ms, or off.");
        else if (!style_layer::request_key_blend_out(trick_at(args, 0), key_at(args, 1), static_cast<float>(std::get<double>(*ms)),
                                                     error))
            out("error: " + error);
    };
    registry.add(std::move(blend));
}

void add_file_commands(Commands &registry) {
    auto clear = action("style clear", "Remove every joint rotation of the style, or those of one state or trick", Group::gameplay,
                        {target_argument(true)});
    clear.run = [](const Model &, const Values &args, const Output &) {
        style_layer::request_clear(args.empty() ? std::nullopt : style::parse_target(std::get<std::string>(args[0])));
    };
    registry.add(std::move(clear));
    auto preset_action = argument("action");
    preset_action.choices.assign(style_layer::preset_action_names.begin(), style_layer::preset_action_names.end());
    auto preset_name = argument("name", Type::text, true);
    preset_name.complete = [](const Model &m, auto) { return m.style.presets; };
    auto preset = action("style preset",
                         "Load a preset, make a new one, copy the style in use to a new name, delete one, or open their folder",
                         Group::gameplay, {preset_action, preset_name});
    preset.run = [](const Model &, const Values &args, const Output &out) {
        const auto action = style_layer::parse_preset_action(std::get<std::string>(args[0]));
        if (!action) return out("error: Use load, new, copy, delete or folder.");
        std::string error;
        if (style_layer::request_preset(*action, args.size() > 1 ? std::get<std::string>(args[1]) : std::string{}, error)) out("Done.");
        else out("error: " + error);
    };
    registry.add(std::move(preset));
    auto reload = action("style reload", "Read the preset in use again from its file", Group::gameplay);
    reload.run = [](const Model &, const Values &, const Output &) { style_layer::request_reload(); };
    registry.add(std::move(reload));
}

void add_history_commands(Commands &registry) {
    auto undo = action("style undo", "Put the style back to before the last edit", Group::gameplay);
    undo.run = [](const Model &, const Values &, const Output &out) {
        if (!style_layer::request_undo()) out("Nothing to undo.");
    };
    registry.add(std::move(undo));
    auto redo = action("style redo", "Do the last undone edit again", Group::gameplay);
    redo.run = [](const Model &, const Values &, const Output &out) {
        if (!style_layer::request_redo()) out("Nothing to redo.");
    };
    registry.add(std::move(redo));
    auto bound = argument("begin|end");
    bound.choices = {"begin", "end"};
    auto group = action("style group", "Make the edits between begin and end one undo step, such as one drag", Group::gameplay, {bound});
    group.run = [](const Model &, const Values &args, const Output &) {
        style_layer::request_group(std::get<std::string>(args[0]) == "begin");
    };
    registry.add(std::move(group));
    auto history = action("style history clear", "Forget the edits to undo and redo", Group::gameplay);
    history.run = [](const Model &, const Values &, const Output &) { style_layer::request_history_clear(); };
    registry.add(std::move(history));
    auto save = action("style save", "Write the preset in use now", Group::gameplay);
    save.run = [](const Model &, const Values &, const Output &) { style_layer::request_save(); };
    registry.add(std::move(save));
    registry.add(on_off("style autosave", "Save the preset after each edit (1), or only when you save it (0)",
                        style_layer::request_auto_save, style_layer::auto_saving, true));
}

void add_status_command(Commands &registry) {
    auto status = action("style status",
                         "Show whether the style is on, off or unavailable and why, the preset in use, and whether the editor is open",
                         Group::gameplay);
    status.run = [](const Model &, const Values &, const Output &out) {
        auto reply = style_layer::status();
        reply += style_editor_status();
        out(reply);
    };
    registry.add(std::move(status));
}
} // namespace

void register_style_commands(Commands &registry) {
    add_switches(registry);
    add_joint_command(registry);
    add_key_commands(registry);
    add_file_commands(registry);
    add_history_commands(registry);
    add_status_command(registry);
}
} // namespace dingosdk::console
