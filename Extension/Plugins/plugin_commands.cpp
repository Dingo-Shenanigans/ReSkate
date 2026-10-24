#include "plugin_runtime.h"
#include "Extension/Console/commands.h"

// `plugin list`, `plugin commands`, `plugin reload|off|on <id>`: the plugins' state, and a
// plugin restarted or switched off by hand without touching its mod.
namespace dingosdk::console {
namespace {
void write_lines(const Output& out, const std::string& text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find('\n', start), text.size());
        out(text.substr(start, end - start));
        start = end + 1;
    }
}
// No completion of ids: completion may run off the client thread, which alone reads the plugins.
Argument plugin_id() { return argument("id"); }
}
void register_plugin_commands(Commands& registry) {
    auto list = action("plugin list", "List the plugins: running, stopped and why, memory and time a tick", Group::console);
    list.run = [](const Model&, const Values&, const Output& out) { write_lines(out, plugins::describe()); };
    registry.add(std::move(list));

    auto commands = action("plugin commands", "List the console commands plugins added", Group::console);
    commands.run = [](const Model&, const Values&, const Output& out) { write_lines(out, plugins::list_commands()); };
    registry.add(std::move(commands));

    auto reload = action("plugin reload", "Unload a plugin and load it again from its files", Group::console, {plugin_id()});
    reload.run = [](const Model&, const Values& args, const Output& out) { out(plugins::reload(std::get<std::string>(args[0]))); };
    registry.add(std::move(reload));

    auto off = action("plugin off", "Switch a plugin off until plugin on, or the next launch", Group::console, {plugin_id()});
    off.run = [](const Model&, const Values& args, const Output& out) { out(plugins::switch_off(std::get<std::string>(args[0]))); };
    registry.add(std::move(off));

    auto on = action("plugin on", "Start a plugin that was switched off or stopped", Group::console, {plugin_id()});
    on.run = [](const Model&, const Values& args, const Output& out) { out(plugins::switch_on(std::get<std::string>(args[0]))); };
    registry.add(std::move(on));
}
} // namespace dingosdk::console
