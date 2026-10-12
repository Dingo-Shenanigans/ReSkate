#include "Extension/Console/commands.h"
#include "Extension/Style/style_commands.h"
#include "Extension/Style/style_layer.h"
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures{};
void check(bool ok, std::string_view what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
std::vector<std::string> calls;
struct Stubbed {
    bool enabled{}, share{true}, auto_save{true}, available{true}, can_undo{true};
} stubbed;
std::string target_text(const dingosdk::style::Target &target) {
    return std::format("{:d}/{}/{}", target.trick, target.id, target.key);
}
// `call` is the stub calls joined by "; ". The rows run in order.
struct Case {
    std::string_view command;
    std::string_view call;
    std::string_view reply;
};
constexpr Case cases[]{
    {"style 1", "enabled 1", ""},
    {"style", "", "style [1] - Add your style's joint rotations to the skater's animation"},
    {"style share 0", "share 0", ""},
    {"style joint grind LeftArm 10 -20 30.5", "joint 0/1/0 LeftArm 10 -20 30.5", ""},
    {"style joint Kickflip Head 5 0 0", "joint 1/2/0 Head 5 0 0", ""},
    {"style joint kickflip Head 5 0 0 2", "joint 1/2/2 Head 5 0 0", ""},
    {"style key add kickflip 0.75", "key add 2 0.75", "Keyframe 3 added."},
    {"style key move heelflip 1 2.5", "key move 3 1 2.5", ""},
    {"style key delete kickflip 2", "key delete 2 2", ""},
    {"style key delete kickflip 9", "key delete 2 9", "error: no such keyframe"},
    {"style key blendout heelflip 1 300", "key blendout 3 1 300", ""},
    {"style key blendout heelflip 1 off", "key blendout 3 1 0", ""},
    {"style key blendout heelflip 1 soon", "", "error: Give a time in ms, or off."},
    {"style clear", "clear all", ""},
    {"style clear heelflip", "clear 1/3/0", ""},
    {"style reload", "reload", ""},
    {"style preset load street", "preset load street", "Done."},
    {"style preset new street", "preset new street", "Done."},
    {"style preset copy street", "preset copy street", "Done."},
    {"style preset Delete street", "preset delete street", "Done."},
    {"style preset folder", "preset folder", "Done."},
    {"style preset load bad", "preset load bad", "error: refused"},
    {"style undo", "undo", ""},
    {"style undo", "undo", "Nothing to undo."},
    {"style redo", "redo", ""},
    {"style group begin", "group begin", ""},
    {"style group end", "group end", ""},
    {"style history clear", "history clear", ""},
    {"style save", "save", ""},
    {"style autosave 0", "autosave 0", ""},
    {"style autosave 1", "autosave 1", ""},
    {"style status", "", "status line"},
    {"style 0", "enabled 0", ""},
};
// The registry refuses these before a stub runs: a value outside its choices or its range.
constexpr std::string_view refused[]{
    "style joint kickflip Head 5 0 0 99", "style joint none Head 5 0 0", "style joint riding LeftArm 500 0 0",
    "style joint flying LeftArm 1 0 0", "style joint riding Reference 1 0 0", "style key add riding 1", "style key add kickflip 7",
    "style group maybe", "style preset rename street",
};
} // namespace

namespace dingosdk::style_layer {
void request_enabled(bool enabled) {
    stubbed.enabled = enabled;
    calls.push_back(std::format("enabled {:d}", enabled));
}
void request_share(bool share) {
    stubbed.share = share;
    calls.push_back(std::format("share {:d}", share));
}
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &) {
    calls.push_back(std::format("joint {} {} {} {} {}", target_text(target), joint, x, y, z));
    return true;
}
void request_clear(std::optional<style::Target> target) { calls.push_back("clear " + (target ? target_text(*target) : "all")); }
bool request_preset(PresetAction action, std::string_view name, std::string &error) {
    calls.push_back(std::format("preset {}{}{}", preset_action_names[static_cast<std::size_t>(action)], name.empty() ? "" : " ", name));
    if (name == "bad") error = "refused";
    return name != "bad";
}
void request_reload() { calls.push_back("reload"); }
int request_key_add(std::uint8_t trick, float time, std::string &) {
    calls.push_back(std::format("key add {} {}", trick, time));
    return 3;
}
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &) {
    calls.push_back(std::format("key move {} {} {}", trick, key, time));
    return true;
}
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &) {
    calls.push_back(std::format("key blendout {} {} {}", trick, key, ms));
    return true;
}
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error) {
    calls.push_back(std::format("key delete {} {}", trick, key));
    if (key > 5) error = "no such keyframe";
    return key <= 5;
}
bool request_undo() {
    calls.push_back("undo");
    return std::exchange(stubbed.can_undo, false);
}
bool request_redo() {
    calls.push_back("redo");
    return true;
}
void request_group(bool open) { calls.push_back(open ? "group begin" : "group end"); }
void request_history_clear() { calls.push_back("history clear"); }
void request_save() { calls.push_back("save"); }
void request_auto_save(bool on) {
    stubbed.auto_save = on;
    calls.push_back(std::format("autosave {:d}", on));
}
bool auto_saving() noexcept { return stubbed.auto_save; }
bool enabled() noexcept { return stubbed.enabled; }
bool available() noexcept { return stubbed.available; }
bool sharing() noexcept { return stubbed.share; }
std::string status() { return "status line"; }
} // namespace dingosdk::style_layer

namespace layer = dingosdk::style_layer;
static_assert(layer::grounded(100) && layer::grounded(101) && !layer::grounded(200) && layer::on_foot(504));

int main() {
    using namespace dingosdk::console;
    try {
        Commands registry;
        register_style_commands(registry);
        Model model;
        std::string printed;
        const Output out{[&](const std::string &line) { printed += line; }, {}, {}};
        const auto run = [&](std::string_view text) {
            printed.clear();
            return registry.execute(text, model, out);
        };
        const auto pass = [&](const Case &row) {
            calls.clear();
            const bool ran = run(row.command);
            std::string log;
            for (const auto &call : calls) log += (log.empty() ? "" : "; ") + call;
            return ran && log == row.call && printed == row.reply;
        };
        const std::vector<std::string> typed{"s"};
        const auto suggestions = registry.complete(typed, model);
        check(suggestions.size() == 1 && suggestions[0].text == "style", "typing s suggests style");
        const std::vector<std::string> next{"style", ""};
        check(registry.complete(next, model).size() >= 7, "style lists its sub-commands and values");
        for (const auto &row : cases) check(pass(row), row.command);
        calls.clear();
        if (const auto *share = registry.find("style share"); share && share->reset) share->reset(model, out);
        check(calls == std::vector<std::string>{"share 1"} && stubbed.share, "a reset of style share turns it on, its default");
        for (const auto command : refused) {
            calls.clear();
            check(!run(command) && calls.empty(), command);
        }
        stubbed.available = false;
        {
            const auto *style_entry = registry.find("style");
            const auto state = style_entry->inspect(model);
            check(!state.available && !state.value && state.reason == "Style is unavailable for this game build. 'style status' says why.",
                  "style reports why it is unavailable, with no value");
        }
        stubbed.available = true;
        const auto *status_entry = registry.find("style status");
        check(status_entry && status_entry->execution == Execution::game_thread, "style status runs on the game thread");
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "style commands tests passed\n";
    return failures ? 1 : 0;
}
