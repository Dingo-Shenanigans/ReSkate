// The style console commands register, complete, and pass the typed values to the style layer.
#include "Extension/Console/commands.h"
#include "Extension/Skater/style_editor.h"
#include "Extension/Skater/style_stage.h"
#include "Extension/Skater/style_layer.h"
#include <iostream>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
struct Calls {
    bool session_test{};
    bool enabled{}, share{true}, restyle{true}, cleared{}, reloaded{};
    std::optional<dingosdk::style::Target> cleared_target;
    std::string editor, debug;
    dingosdk::style::Target target{};
    std::uint8_t preview{}, key_trick{}, key{};
    float preview_time{}, key_time{}, blend_out{-1};
    bool preview_play{};
    int key_calls{}, undos{}, redos{}, saves{};
    bool auto_save{true}, history_cleared{};
    std::string group;
    std::string joint;
    float x{}, y{}, z{};
} calls;
} // namespace

// Stubs record the calls in place of the runtime.
namespace dingosdk::style_layer {
void request_enabled(bool enabled) { calls.enabled = enabled; }
void request_share(bool share) { calls.share = share; }
void request_restyle(bool restyle) { calls.restyle = restyle; }
void request_session_test(bool allowed) { calls.session_test = allowed; }
bool session_test() noexcept { return calls.session_test; }
bool restyling() noexcept { return calls.restyle; }
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &error) {
    if (joint == "Reference") {
        error = "not editable";
        return false;
    }
    calls.target = target;
    calls.joint = joint;
    calls.x = x, calls.y = y, calls.z = z;
    return true;
}
void request_clear(std::optional<style::Target> target) { calls.cleared = true, calls.cleared_target = target; }
bool request_preset(std::string_view action, std::string_view name, std::string &error) {
    calls.editor = std::string(action) + " " + std::string(name);
    if (name == "bad") error = "refused";
    return name != "bad";
}
void request_reload() { calls.reloaded = true; }
void request_preview(std::uint8_t trick, float time, bool play) { calls.preview = trick, calls.preview_time = time, calls.preview_play = play; }
int request_key_add(std::uint8_t trick, float time, std::string &) { return calls.key_trick = trick, calls.key_time = time, ++calls.key_calls, 3; }
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &) {
    return calls.key_trick = trick, calls.key = key, calls.key_time = time, true;
}
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &) {
    return calls.key_trick = trick, calls.key = key, calls.blend_out = ms, true;
}
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error) {
    calls.key_trick = trick, calls.key = key;
    if (key > 5) error = "no such keyframe";
    return key <= 5;
}
bool request_undo() { return ++calls.undos == 1; }
bool request_redo() { return ++calls.redos, true; }
void request_group(bool open) { calls.group = open ? "begin" : "end"; }
void request_history_clear() { calls.history_cleared = true; }
void request_save() { ++calls.saves; }
void request_auto_save(bool on) { calls.auto_save = on; }
bool auto_saving() noexcept { return calls.auto_save; }
bool enabled() noexcept { return calls.enabled; }
bool sharing() noexcept { return calls.share; }
std::string status() { return "status line"; }
} // namespace dingosdk::style_layer
namespace dingosdk::style_stage {
void open(std::function<void()> then) { then(); }
} // namespace dingosdk::style_stage
namespace dingosdk::style_editor {
void request_show(std::uint8_t trick) { calls.editor = "show " + std::to_string(trick); }
void request_open() { calls.editor = "open"; }
void request_forget(std::uint8_t trick) { calls.editor = "forget " + std::to_string(trick); }
void request_hide() { calls.editor = "hide"; }
void request_hold(float time) noexcept { calls.editor = "hold " + std::to_string(time); }
void request_play(bool play) noexcept { calls.editor = play ? "play" : "pause"; }
void request_step(int frames) noexcept { calls.editor = "step " + std::to_string(frames); }
void request_speed(float speed) noexcept { calls.editor = "speed " + std::to_string(speed); }
void request_orbit(float yaw, float pitch, float distance, float height) noexcept { calls.editor = std::to_string(yaw + pitch + distance + height); }
std::string status() { return "takes"; }
} // namespace dingosdk::style_editor
namespace dingosdk::console {
void request_debug(overlay::DebugAction action, bool enabled, float) {
    if (action == overlay::DebugAction::set_style_editor) calls.debug = enabled ? "opened" : "closed";
}
Argument argument(std::string name, Type type, bool optional) {
    Argument result;
    result.name = std::move(name);
    result.type = type;
    result.optional = optional;
    return result;
}
Entry action(std::string name, std::string description, Group group, std::vector<Argument> args) {
    Entry entry;
    entry.name = std::move(name);
    entry.description = std::move(description);
    entry.group = group;
    entry.arguments = std::move(args);
    return entry;
}
Entry variable(std::string name, std::string description, Group group, Argument arg) {
    auto entry = action(std::move(name), std::move(description), group, {std::move(arg)});
    entry.kind = Kind::variable;
    return entry;
}
} // namespace dingosdk::console

int main() {
    using namespace dingosdk::console;
    try {
        Commands registry;
        register_style_commands(registry);
        const Model model;
        std::string printed;
        const Output out{[&](const std::string &line) { printed += line; }, {}, {}};
        const auto run = [&](std::string_view text) {
            printed.clear();
            return registry.execute(text, model, out);
        };
        // The console asks for completions when the player types "s".
        const std::vector<std::string> typed{"s"};
        const auto suggestions = registry.complete(typed, model);
        check(suggestions.size() == 1 && suggestions[0].text == "style", "typing s suggests style");
        const std::vector<std::string> next{"style", ""};
        check(registry.complete(next, model).size() >= 7, "style lists its sub-commands and values");
        check(run("style 1") && calls.enabled, "style 1 switches the layer on");
        check(run("style") && printed.find("status line") != std::string::npos, "style alone shows the status");
        check(run("style share 0") && !calls.share, "style share 0 hides the style from others");
        check(run("style restyle 0") && !calls.restyle, "style restyle 0 leaves replays as they were skated");
        check(run("style joint grind LeftArm 10 -20 30.5") && calls.target == dingosdk::style::Target{false, 1} &&
                  calls.joint == "LeftArm" && calls.x == 10 && calls.y == -20 && calls.z == 30.5f,
              "style joint passes the state, joint and angles");
        check(run("style joint Kickflip Head 5 0 0") && calls.target == dingosdk::style::Target{true, 2},
              "style joint accepts a flip trick");
        check(run("style joint kickflip Head 5 0 0 2") && calls.target == dingosdk::style::Target{true, 2, 2},
              "style joint takes a keyframe");
        check(!run("style joint kickflip Head 5 0 0 99"), "a keyframe number past the limit is refused");
        check(!run("style joint none Head 5 0 0"), "the no-trick entry is not a target");
        check(!run("style joint riding LeftArm 500 0 0"), "an angle past the limit is refused");
        check(!run("style joint flying LeftArm 1 0 0"), "an unknown state is refused");
        check(!run("style joint riding Reference 1 0 0"), "a joint outside the editable list is refused");
        check(run("style preview heelflip") && calls.preview == 3 && calls.preview_play, "style preview plays a flip trick by default");
        check(run("style preview kickflip 1.5") && calls.preview == 2 && calls.preview_time == 1.5f && !calls.preview_play,
              "style preview holds a time on the timeline");
        check(run("style preview heelflip play") && calls.preview == 3 && calls.preview_play, "style preview plays through");
        check(run("style preview kickflip later") && printed.find("error") == 0, "style preview refuses a time that is not a number");
        check(run("style key add kickflip 0.75") && calls.key_trick == 2 && calls.key_time == 0.75f && printed == "Keyframe 3 added.",
              "style key add places a keyframe");
        check(run("style key move heelflip 1 2.5") && calls.key_trick == 3 && calls.key == 1 && calls.key_time == 2.5f, "style key move moves one");
        check(run("style key delete kickflip 2") && calls.key == 2 && printed.empty(), "style key delete removes one");
        check(run("style key blendout heelflip 1 300") && calls.key_trick == 3 && calls.key == 1 && calls.blend_out == 300.0f,
              "style key blendout sets a keyframe's blend out in ms");
        check(run("style key blendout heelflip 1 off") && calls.blend_out == 0.0f, "and off sends it to the next keyframe again");
        check(run("style key blendout heelflip 1 soon") && printed.find("error") == 0, "a blend out that is not a time is refused");
        check(run("style key delete kickflip 9") && printed.find("error") == 0, "a keyframe the trick does not have is reported");
        check(!run("style key add riding 1") && !run("style key add kickflip 7"), "keyframes belong to flip tricks and to the timeline");
        check(run("style preview off") && calls.preview == 0, "style preview off ends it");
        check(!run("style preview riding"), "style preview refuses a state");
        check(run("style clear") && calls.cleared && !calls.cleared_target, "style clear removes the rotations");
        check(run("style clear heelflip") && calls.cleared_target == dingosdk::style::Target{true, 3}, "style clear takes one trick");
        check(run("style reload") && calls.reloaded, "style reload reads the saved style");
        check(run("style editor show") && calls.editor == "show 0" && run("style editor show heelflip") && calls.editor == "show 3",
              "style editor show plays a trick's clip, or clears the stand-in");
        check(run("style editor hold 1.5") && calls.editor.starts_with("hold 1.5") && run("style editor play") && calls.editor == "play" &&
                  run("style editor pause") && calls.editor == "pause",
              "style editor hold, play and pause move through it");
        check(run("style editor hide") && calls.editor == "hide" && run("style editor takes") && printed == "takes",
              "style editor hide removes the stand-in, and takes lists the clips");
        check(run("style editor step -3") && calls.editor == "step -3", "style editor step moves by frames");
        check(run("style editor speed 0.25") && calls.editor.starts_with("speed 0.25"), "style editor speed sets the playback speed");
        check(!run("style editor speed 2") && !run("style editor speed 0"), "the speed is a fraction of the recorded speed");
        check(run("style undo") && calls.undos == 1 && printed.empty() && run("style undo") && printed == "Nothing to undo.",
              "style undo undoes the last edit, and says when there is none");
        check(run("style redo") && calls.redos == 1, "style redo does it again");
        check(run("style group begin") && calls.group == "begin" && run("style group end") && calls.group == "end" && !run("style group maybe"),
              "style group makes the edits between begin and end one undo step");
        check(run("style history clear") && calls.history_cleared, "style history clear forgets the steps");
        check(run("style save") && calls.saves == 1, "style save writes the preset now");
        check(run("style autosave 0") && !calls.auto_save && run("style autosave 1") && calls.auto_save, "style autosave switches the saving after each edit");
        calls.history_cleared = false;
        check(run("style 0") && !calls.enabled && run("style editor open") && calls.editor == "open" && calls.debug == "opened" && calls.enabled &&
                  calls.history_cleared,
              "style editor open switches the layer on, opens the editor screen and starts the undo steps again");
        check(run("style preset new street") && calls.editor == "new street" && run("style preset folder") && calls.editor == "folder " &&
                  !run("style preset rename street"),
              "style preset names an action and a preset");
        calls.history_cleared = false;
        check(run("style editor close") && calls.editor == "hide" && calls.debug == "closed" && calls.history_cleared,
              "style editor close removes the stand-in, closes the screen and forgets the undo steps");
        check(run("style status") && printed == "status line", "style status prints the status");
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "style commands tests passed\n";
    return failures ? 1 : 0;
}
