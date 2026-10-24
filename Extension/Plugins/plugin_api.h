#pragma once
#include "plugin_vm.h"
#include "lua.h"
#include <initializer_list>

// Inside the plugin host only: the reskate API a plugin's VM gets.
namespace dingosdk::plugins {
struct Api {
    // The `reskate` table, `print` and `require`, before the VM is sandboxed.
    static void install(Plugin& plugin, lua_State* L);
};
// The overlay, menu, sound, mods, session and message parts of the API (plugin_api_ui.cpp), into
// the reskate table on top of the stack.
void install_ui_api(Plugin& plugin, lua_State* L);
// Phase 3's parts of the API (plugin_api_more.cpp), into the reskate table on top of the stack.
void install_more_api(Plugin& plugin, lua_State* L);
// The wider set's game parts (plugin_api_world.cpp), into the same table.
void install_world_api(Plugin& plugin, lua_State* L);
// General services for a game layer: the skater's telemetry and Bone Cam, the pad and keys, the
// free and pad-flown cameras, the session's world, competition physics and the trick feed
// (plugin_api_services.cpp).
void install_services_api(Plugin& plugin, lua_State* L);
// The throwdown kit's documents and native flag, the plugin's data files, overlay dialogs and the
// skater's push (with its wait and reset) and hold (plugin_api_throwdowns.cpp).
void install_kit_api(Plugin& plugin, lua_State* L);
// Screen captures, the game menu's preview turned and read, what the skater wears, and the data
// folder shown (plugin_api_capture.cpp).
void install_capture_api(Plugin& plugin, lua_State* L);

// ---- adding to the reskate table ---------------------------------------------------------------
// A new part of the API is a file of `static int name(lua_State*)` functions and an install_*_api
// that lists them, called from Api::install:
//
//     add_functions(L, "skater", {{"get", skater_get}, {"teleport", skater_teleport}});  // reskate.skater.*
//     add_functions(L, {{"log", log}, {"on", on}});                                      // reskate.*
//
// Each function checks its own permission (need) and arguments, as the rest of the API does.
struct ApiFunction {
    const char* name;
    lua_CFunction function;
};
// Adds the functions to reskate.<field>, the reskate table being on top of the stack (or to a table
// inside it, such as reskate.throwdowns, when that is on top). The table is made when there is none
// yet, and another file may add to one already there; it is left read-only.
void add_functions(lua_State* L, const char* field, std::initializer_list<ApiFunction> functions);
// Adds the functions to the reskate table itself (on top of the stack).
void add_functions(lua_State* L, std::initializer_list<ApiFunction> functions);
// Pushes a new read-only table of the functions, kept outside reskate (the draw and ui handles).
void push_functions(lua_State* L, std::initializer_list<ApiFunction> functions);

// ---- what most API functions start with --------------------------------------------------------
// The plugin calling, or a Lua error once it has been stopped.
Plugin& plugin_of(lua_State* L);
// A Lua error naming `what` unless the plugin's reskate-plugin.json grants `permission`.
void need(lua_State* L, Plugin& plugin, const char* permission, const char* what);
// A string argument of at most `most` bytes.
std::string checked_text(lua_State* L, int index, std::size_t most, const char* what);
// A path in the plugin's data folder (reskate.files): relative, inside it, no name Windows keeps
// for a device. `folder`: nil or "" is the folder itself, and a "/" at the end is fine.
std::string data_path(lua_State* L, int index, bool folder);
// A vector argument, finite and within 100 km of the map's centre.
Vec3 position_of(lua_State* L, int index, const char* what);
void push_position(lua_State* L, const Vec3& v);

// A player as plugins get one: {id = "<SteamID64>", name, me, host}.
void push_player(lua_State* L, const SessionPlayer& player);
// A file's bytes when it is a regular file no larger than `most`; otherwise empty, and `error`
// says why ("not found" when there is none).
std::string read_package_file(const std::filesystem::path& file, std::size_t most, std::string& error);
// A file of the plugin's own package by its path there; a Lua error for one outside it.
std::filesystem::path package_file(lua_State* L, Plugin& plugin, std::string_view path);
// JSON and Lua values, both ways. Throws std::runtime_error on a value JSON cannot hold.
Json lua_to_json(lua_State* L, int index);
void push_json(lua_State* L, const Json& value);
}
