#pragma once
#include "plugin_approval.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Plugins in the game (Extension/Plugins): Luau scripts in mods, each in its own sandboxed VM
// (plugin_vm.h), run on the client tick. A thread looks for plugins every 2 s, so a mod switched
// on or off, or a plugin's files changed, takes effect without a restart.
//
// Every function but start is for the client thread, the one that runs console commands.
namespace dingosdk::plugins {
// Once, at startup, with the game's image base (the score HUD is read from it). Plugins load at the
// first client tick after discovery finds them.
bool start(std::uintptr_t image_base) noexcept;
void on_client_tick() noexcept;
// The level being played, or empty without one: plugins hear level_loaded and level_unloading.
void set_level(std::string_view level) noexcept;
// Runs a plugin's console command when the text starts with one. False when none matched, and
// the text goes to ReSkate's own commands.
bool run_console(const std::string& text) noexcept;
// The session's throwdown packets that are plugin messages (plugin_wire.h), and those plugins send.
// Client thread, from the session's tick.
void receive_message(std::uint64_t sender, const std::vector<std::uint8_t>& bytes) noexcept;
std::vector<std::vector<std::uint8_t>> take_outgoing() noexcept;
// The `plugin` console commands.
std::string describe();
std::string list_commands();
// The console commands plugins added, for the console's lookup: each name, its help and the plugin
// that owns it (a server copy's as "<id>@server"). Client thread; read again at most twice a second.
struct CommandHint {
    std::string name, args, help, plugin; // name may be several words: a declared subcommand
};
std::vector<CommandHint> command_hints();
std::string reload(std::string_view id);
std::string switch_off(std::string_view id);
std::string switch_on(std::string_view id);
// Plugin developer mode (Engine/Vfs/initfs.h, shared with the launcher): a plugin that has an answer
// keeps it when its files change. False from set_developer_mode when it could not be saved. Any thread.
bool developer_mode();
bool set_developer_mode(bool on) noexcept;
// The plugins that may load, for the menu: each one is asking, on or off for the files it has now.
// Those waiting for an answer come first, then on, then off, each by title.
// Any thread.
struct Installed {
    std::string id, title, author; // read by a player; the author is empty when unknown
    std::vector<PermissionGroup> can; // what it can do, in plain words by category
    Approval approval;
};
std::vector<Installed> installed();
// The player turning a plugin on or off (it is applied on the next client tick). Any thread.
void set_enabled(std::string_view id, bool on);
}
