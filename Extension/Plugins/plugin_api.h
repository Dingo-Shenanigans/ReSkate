#pragma once
#include "plugin_vm.h"
#include "lua.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
void install_vehicles_api(Plugin& plugin, lua_State* L);
// Native bodies a plugin owns and their ray batches (plugin_api_physics.cpp), into reskate.world.
void install_physics_api(Plugin& plugin, lua_State* L);
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
// Permission-gated, namespaced floats for authored native game UI (plugin_api_values.cpp).
void install_values_api(Plugin& plugin, lua_State* L);
void install_effects_api(Plugin& plugin, lua_State* L);
// A rigged, animated model in front of the camera (plugin_api_viewmodel.cpp).
void install_viewmodel_api(Plugin& plugin, lua_State* L);
void install_object_physics_api(Plugin& plugin, lua_State* L);
// The clips and sounds read since the last tick (reskate.animation.preload, reskate.audio.preload,
// plugin_api_world.cpp), handed into the plugin's own and called back. Plugin::tick.
void deliver_preloads(Plugin& plugin);

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

// ---- fixed words -------------------------------------------------------------------------------
// The words an API function takes or gives from a fixed set. Each list is also reskate.Enums.<name>
// (Api::install), so the check and the constant cannot drift apart.
using Words = std::span<const std::string_view>;
inline constexpr std::string_view log_levels[] = {"info", "warn", "error"};                 // Enums.LogLevel
inline constexpr std::string_view aligns[] = {"left", "centre", "center", "right"};         // Enums.Align
inline constexpr std::string_view launches[] = {"grind", "ollie", "no_comply", "boneless"};  // Enums.Launch
inline constexpr std::string_view stances[] = {"regular", "goofy"};                         // Enums.Stance
inline constexpr std::string_view pops[] = {"low", "mid", "high"};                          // Enums.Pop
inline constexpr std::string_view spaces[] = {"local", "world"};                            // Enums.Space
inline constexpr std::string_view throwdown_modes[] = {"jam", "spot_battle", "skate"};      // Enums.Throwdown
inline constexpr std::string_view throwdown_cards[] = {"skate", "spot", "spot_battle", "jam"}; // Enums.Card
inline constexpr std::string_view entity_statuses[] = {"waiting", "loading", "missing", "ready", "hidden"}; // Enums.EntityStatus
inline constexpr std::string_view send_targets[] = {"all", "server"};                       // Enums.Target
// mods.item_origins: where a key comes from. It picks these by position, so keep the order.
inline constexpr std::string_view item_sources[] = {"mod", "changed", "game", "unknown"};   // Enums.ItemSource
inline constexpr std::string_view menu_pages[] = {"multiplayer", "mod_options", "mod_manager"}; // Enums.MenuPage
// Named keys for input.key and the like (letters and digits are their own character): Enums.Key.
inline constexpr std::pair<std::string_view, int> key_names[] = {
    {"Enter", 13}, {"Backspace", 8}, {"Escape", 27}, {"End", 35}, {"Home", 36}, {"PageUp", 33}, {"PageDown", 34}, {"Space", 32},
    {"Shift", 16}, {"Ctrl", 17}, {"Alt", 18}, {"Tab", 9}, {"Left", 37}, {"Up", 38}, {"Right", 39}, {"Down", 40},
    {"[", 0xDB}, {"]", 0xDD}, {"Delete", 46}, {"LeftMouse", 1}, {"RightMouse", 2}, {"MiddleMouse", 4}};
inline bool one_of(Words words, std::string_view word) { return std::ranges::find(words, word) != words.end(); }

// ---- what most API functions start with --------------------------------------------------------
// The plugin calling, or a Lua error once it has been stopped.
Plugin& plugin_of(lua_State* L);
// A Lua error naming `what` unless the plugin's reskate-plugin.json grants `permission`.
void need(lua_State* L, Plugin& plugin, const char* permission, const char* what);
// plugin_of and need in one: the plugin calling, which has `permission`.
Plugin& permitted(lua_State* L, const char* permission, const char* what);
// The wider set's game (plugin_game.h) the plugin's host gives it.
Game& game_of(Plugin& plugin);
// A per-second budget: `second` is when the second began and `count` the calls in it. False once
// a call is past `most`.
bool within(double now, double& second, int& count, int most);

// ---- what most API functions return ------------------------------------------------------------
// true when `why` is empty; otherwise false and why.
int result(lua_State* L, std::string_view why);
// nil and why: what a call that hands back a value returns when it fails.
int failed(lua_State* L, std::string_view why);

// ---- arguments ---------------------------------------------------------------------------------
// A string argument of at most `most` bytes.
std::string checked_text(lua_State* L, int index, std::size_t most, const char* what);
// Why a text is not one the game's text takes, or null: UTF-8 (no overlong form, surrogate or NUL) of
// at most 511 UTF-16 units, as the string store's conversion holds (text_overrides.h). strings.set and
// hud.score_line check theirs with it.
const char* text_problem(std::string_view text);
// A path in the plugin's data folder (reskate.files): relative, inside it, no name Windows keeps
// for a device. `folder`: nil or "" is the folder itself, and a "/" at the end is fine.
std::string data_path(lua_State* L, int index, bool folder);
// A vector argument, finite and within 100 km of the map's centre.
Vec3 position_of(lua_State* L, int index, const char* what);
// A player: a string of the SteamID64's digits ("7656..."), a player table ({id = ...}), or a whole
// number while a Lua number holds it exactly (below 2^53; a SteamID64 is past that, and one passed
// as a number came out as another player's). Anything else, trailing characters too, or 0, is an
// argument error saying `problem`.
std::uint64_t player_id(lua_State* L, int index, const char* problem);
// A park editor object's id, as reskate.objects gives it: a decimal string, nothing after it, not 0.
std::uint64_t object_id(lua_State* L, int index);
// Item keys (the menu's grid, the mods' items): a list of at most 4000, each at most 255 bytes.
std::vector<std::string> item_keys(lua_State* L, int index);

// ---- fields of a table argument ----------------------------------------------------------------
// An option of the table at `table`, which may itself be nil: `fallback` when either is nil,
// otherwise a number from `low` to `high`. Past them the error is `message`, or "<name> is <low>
// to <high>" without one.
double number_option(lua_State* L, int table, const char* name, double fallback, double low, double high,
                     const char* message = nullptr);
// A true-or-false option the same way: `fallback` when it or the table is nil, otherwise its truth.
bool flag_option(lua_State* L, int table, const char* name, bool fallback);
// A string field of the table at `table`: empty when nil, otherwise a string of at most `most` bytes.
std::string text_field(lua_State* L, int table, const char* name, std::size_t most = static_cast<std::size_t>(-1));

// ---- results -----------------------------------------------------------------------------------
void push_position(lua_State* L, const Vec3& v);
// A rotation as plugins get one: {x, y, z, w}.
void push_quaternion(lua_State* L, const std::array<float, 4>& q);
// A list of strings, at most `most` of them.
void push_texts(lua_State* L, std::span<const std::string> texts, std::size_t most = static_cast<std::size_t>(-1));
// Fields of the table on top of the stack.
void field_number(lua_State* L, const char* name, double value);
void field_bool(lua_State* L, const char* name, bool value);
void field_vec(lua_State* L, const char* name, const Vec3& v);
void field_text(lua_State* L, const char* name, std::string_view text);

// What is up over the game as plugins get it (reskate.menu.state, menu_changed):
// {known, pause, reskate, replay_editor, page}, page nil when none of ReSkate's pages is shown.
void push_menu_state(lua_State* L, const Game::MenuState& state);
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
