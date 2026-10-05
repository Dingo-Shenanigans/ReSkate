#include "Extension/Console/commands.h"
#include "style_editor.h"
#include "style_stage.h"
#include "style_layer.h"
#include <algorithm>
namespace dingosdk::console {
void register_style_commands(Commands &registry) {
    const auto on_off = [](const char *name, const char *description, void (*request)(bool), bool (*value)() noexcept) {
        auto entry = variable(name, description, Group::gameplay, argument("0|1", Type::boolean));
        entry.inspect = [value](const Model &) { return State{true, value() ? "1" : "0", {}, {}, false}; };
        entry.run = [request](const Model &, const Values &args, const Output &) { request(std::get<bool>(args[0])); };
        entry.reset = [request](const Model &, const Output &) { request(false); };
        return entry;
    };
    auto style = on_off("style", "Add your style's joint rotations to the skater's animation", style_layer::request_enabled,
                        style_layer::enabled);
    style.inspect = [](const Model &) {
        return State{true, style_layer::enabled() ? "1" : "0", {}, style_layer::status(), false};
    };
    registry.add(std::move(style));
    auto share = on_off("style share", "Show your style to other players (1) or only to yourself (0)", style_layer::request_share,
                        style_layer::sharing);
    share.reset = [](const Model &, const Output &) { style_layer::request_share(true); };
    registry.add(std::move(share));
    registry.add(on_off("style editor session", "Not tested yet: let the style editor open in a multiplayer session. Off again at the next start",
                        style_layer::request_session_test, style_layer::session_test));
    registry.add(on_off("style restyle", "Show replayed flip tricks with your current style (1) or as you skated them (0)",
                        style_layer::request_restyle, style_layer::restyling));
    auto state = argument("target");
    state.choices.assign(style::family_names.begin(), style::family_names.end());
    state.choices.insert(state.choices.end(), style::flip_trick_names.begin() + 1, style::flip_trick_names.end());
    auto joint = argument("joint");
    joint.choices.assign(style::editable_joints.begin(), style::editable_joints.end());
    auto degrees = argument("degrees", Type::number);
    degrees.minimum = -style::max_degrees;
    degrees.maximum = style::max_degrees;
    auto key = argument("keyframe", Type::unsigned_integer, true);
    key.minimum = 0;
    key.maximum = static_cast<double>(style::max_keys - 1);
    auto rotate = action("style joint", "Rotate a joint about its own X, Y and Z axes in a state or at a flip trick's keyframe. All zero removes the rotation",
                         Group::gameplay, {state, joint, degrees, degrees, degrees, key});
    rotate.run = [](const Model &, const Values &args, const Output &out) {
        auto target = style::parse_target(std::get<std::string>(args[0]));
        if (target && args.size() > 5) target->key = static_cast<std::uint8_t>(std::get<std::uint64_t>(args[5]));
        std::string error;
        if (!target)
            out("error: Unknown state or trick.");
        else if (!style_layer::request_joint(*target, std::get<std::string>(args[1]), static_cast<float>(std::get<double>(args[2])),
                                             static_cast<float>(std::get<double>(args[3])),
                                             static_cast<float>(std::get<double>(args[4])), error))
            out("error: " + error);
    };
    registry.add(std::move(rotate));
    auto shown = argument("trick");
    shown.choices.assign(style::flip_trick_names.begin() + 1, style::flip_trick_names.end());
    auto time = argument("time", Type::number);
    time.minimum = 0;
    time.maximum = style::trick_end;
    key.optional = false;
    const auto trick_of = [](const Values &args) { return style::parse_target(std::get<std::string>(args[0]))->id; };
    auto add = action("style key add", "Add a keyframe to a flip trick's timeline (0 flick, 1 catch, 2 touchdown, 3 end)",
                      Group::gameplay, {shown, time});
    add.run = [trick_of](const Model &, const Values &args, const Output &out) {
        std::string error;
        const auto made = style_layer::request_key_add(trick_of(args), static_cast<float>(std::get<double>(args[1])), error);
        out(made < 0 ? "error: " + error : "Keyframe " + std::to_string(made) + " added.");
    };
    registry.add(std::move(add));
    auto move = action("style key move", "Move a flip trick's keyframe along its timeline", Group::gameplay, {shown, key, time});
    move.run = [trick_of](const Model &, const Values &args, const Output &out) {
        std::string error;
        if (!style_layer::request_key_move(trick_of(args), static_cast<std::uint8_t>(std::get<std::uint64_t>(args[1])),
                                           static_cast<float>(std::get<double>(args[2])), error))
            out("error: " + error);
    };
    registry.add(std::move(move));
    auto remove = action("style key delete", "Delete a flip trick's keyframe. Later keyframes move down one number",
                         Group::gameplay, {shown, key});
    remove.run = [trick_of](const Model &, const Values &args, const Output &out) {
        std::string error;
        if (!style_layer::request_key_delete(trick_of(args), static_cast<std::uint8_t>(std::get<std::uint64_t>(args[1])), error))
            out("error: " + error);
    };
    registry.add(std::move(remove));
    shown.choices.push_back("off");
    auto moment = argument("time|play", Type::text, true);
    auto preview = action("style preview", "Show a flip trick's pose at a time on its timeline, or play the timeline. The game's demonstration skater also shows it",
                          Group::gameplay, {shown, moment});
    preview.run = [](const Model &, const Values &args, const Output &out) {
        const auto target = style::parse_target(std::get<std::string>(args[0]));
        const auto text = args.size() > 1 ? std::get<std::string>(args[1]) : std::string("play");
        const bool play = equal(text, "play");
        const auto at = play ? std::optional<Value>(0.0) : parse_value(Type::number, text);
        if (!at) {
            out("error: Give a time from 0 to 3, or play.");
            return;
        }
        style_layer::request_preview(target && target->trick ? target->id : std::uint8_t{}, static_cast<float>(std::get<double>(*at)), play);
        if (!target || !target->trick) out("Style preview off.");
    };
    registry.add(std::move(preview));
    auto cleared = argument("target", Type::text, true);
    cleared.choices = state.choices;
    auto clear = action("style clear", "Remove every joint rotation of the style, or those of one state or trick",
                        Group::gameplay, {cleared});
    clear.run = [](const Model &, const Values &args, const Output &) {
        style_layer::request_clear(args.empty() ? std::nullopt : style::parse_target(std::get<std::string>(args[0])));
    };
    registry.add(std::move(clear));
    auto preset_action = argument("action");
    preset_action.choices = {"load", "new", "copy", "delete", "folder"};
    auto preset_name = argument("name", Type::text, true);
    preset_name.complete = [](const Model &m, auto) { return m.style.presets; };
    auto preset = action("style preset", "Load a preset, make a new one, copy the style in use to a new name, delete one, or open their folder",
                         Group::gameplay, {preset_action, preset_name});
    preset.run = [](const Model &, const Values &args, const Output &out) {
        std::string error;
        const auto &what = std::get<std::string>(args[0]);
        if (style_layer::request_preset(what, args.size() > 1 ? std::get<std::string>(args[1]) : std::string{}, error)) out("Done.");
        else out(error);
    };
    registry.add(std::move(preset));
    clear = action("style reload", "Read the preset in use again from its file", Group::gameplay);
    clear.run = [](const Model &, const Values &, const Output &) { style_layer::request_reload(); };
    registry.add(std::move(clear));
    auto clip = argument("trick", Type::text, true);
    clip.choices.assign(style::flip_trick_names.begin() + 1, style::flip_trick_names.end());
    // No trick gives 0, which clears the stand-in.
    const auto clip_of = [](const Values &args) {
        const auto target = args.empty() ? std::nullopt : style::parse_target(std::get<std::string>(args[0]));
        return target && target->trick ? target->id : std::uint8_t{};
    };
    auto show = action("style editor show", "Play a flip trick's clip on the stand-in. With no trick, remove the shown clip",
                       Group::gameplay, {clip});
    show.run = [clip_of](const Model &, const Values &args, const Output &) { style_editor::request_show(clip_of(args)); };
    registry.add(std::move(show));
    auto which = argument("trick", Type::text);
    auto forget = action("style editor forget", "Delete the saved clip of a flip trick, or all clips. The editor learns a deleted clip again", Group::gameplay, {which});
    forget.run = [](const Model &, const Values &args, const Output &out) {
        const auto &name = std::get<std::string>(args[0]);
        const auto found = std::find(style::flip_trick_names.begin() + 1, style::flip_trick_names.end(), name);
        if (name != "all" && found == style::flip_trick_names.end()) return out("That is not a flip trick.");
        style_editor::request_forget(name == "all" ? std::uint8_t{} : static_cast<std::uint8_t>(found - style::flip_trick_names.begin()));
        out("Forgetting it.");
    };
    registry.add(std::move(forget));
    auto hide = action("style editor hide", "Remove the stand-in", Group::gameplay);
    hide.run = [](const Model &, const Values &, const Output &) { style_editor::request_hide(); };
    registry.add(std::move(hide));
    auto play = action("style editor play", "Play the shown clip in a loop from the shown moment", Group::gameplay);
    play.run = [](const Model &, const Values &, const Output &) { style_editor::request_play(true); };
    registry.add(std::move(play));
    auto pause = action("style editor pause", "Hold the shown clip at the shown moment", Group::gameplay);
    pause.run = [](const Model &, const Values &, const Output &) { style_editor::request_play(false); };
    registry.add(std::move(pause));
    auto hold = action("style editor hold", "Hold the shown clip at a time on the trick's timeline (0 to 3)", Group::gameplay, {time});
    hold.run = [](const Model &, const Values &args, const Output &) {
        style_editor::request_hold(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(hold));
    auto frames = argument("frames", Type::integer);
    frames.minimum = -600;
    frames.maximum = 600;
    auto step = action("style editor step", "Move the held clip forward or backward by frames of 1/60 s", Group::gameplay, {frames});
    step.run = [](const Model &, const Values &args, const Output &) {
        style_editor::request_step(static_cast<int>(std::get<std::int64_t>(args[0])));
    };
    registry.add(std::move(step));
    auto turn = argument("amount", Type::number);
    turn.minimum = -20;
    turn.maximum = 20;
    auto lift = argument("height", Type::number, true);
    lift.minimum = -20;
    lift.maximum = 20;
    auto orbit = action("style editor orbit", "Turn the editor camera around the stand-in (yaw, pitch), move it (distance) and raise its target (height)",
                        Group::gameplay, {turn, turn, turn, lift});
    orbit.run = [](const Model &, const Values &args, const Output &) {
        style_editor::request_orbit(static_cast<float>(std::get<double>(args[0])), static_cast<float>(std::get<double>(args[1])),
                                    static_cast<float>(std::get<double>(args[2])), args.size() > 3 ? static_cast<float>(std::get<double>(args[3])) : 0.0f);
    };
    registry.add(std::move(orbit));
    auto open = action("style editor open", "Open the style editor screen. Select the trick to edit there", Group::gameplay);
    open.run = [](const Model &m, const Values &, const Output &out) {
        // It hides other skaters and uses the game's menu: not in a session.
        if (m.multiplayer.active && !style_layer::session_test())
            return out("Leave the multiplayer session first. The style editor is not tested in a session yet (to test it: style editor session 1).");
        if (!m.style.enabled) style_layer::request_enabled(true);
        style_stage::open([] {
            style_editor::request_open();
            request_debug(overlay::DebugAction::set_style_editor, true);
        });
        out("Opening the style editor.");
    };
    registry.add(std::move(open));
    auto close = action("style editor close", "Close the style editor screen", Group::gameplay);
    close.run = [](const Model &, const Values &, const Output &) {
        request_debug(overlay::DebugAction::set_style_editor, false);
        style_editor::request_hide();
    };
    registry.add(std::move(close));
    auto takes = action("style editor takes", "List the saved clips and the state of the stand-in", Group::gameplay);
    takes.execution = Execution::local;
    takes.run = [](const Model &, const Values &, const Output &out) { out(style_editor::status()); };
    registry.add(std::move(takes));
    auto status = action("style status", "Show whether the style is applied and its counters", Group::gameplay);
    status.execution = Execution::local;
    status.run = [](const Model &, const Values &, const Output &out) { out(style_layer::status()); };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
