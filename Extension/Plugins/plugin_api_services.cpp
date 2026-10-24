#include "plugin_api.h"
#include "plugin_game.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// General services a game layer in Lua reads and drives each tick: the skater's telemetry, stance
// and Bone Cam, the pad and keys, the free and pad-flown cameras, spectating, the session's world,
// the competition's stock physics, and skate.'s trick list and flick diagrams. None of them knows a
// throwdown mode; each checks its permission and the game (plugin_game.h) does the rest.
namespace dingosdk::plugins {
struct ServicesApi {
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }
};
namespace {
Game& game_of(Plugin& plugin) { return ServicesApi::game(plugin); }
Plugin& allowed(lua_State* L, const char* permission, const char* what) {
    auto& plugin = plugin_of(L);
    if (!plugin.has(permission)) luaL_error(L, "%s needs the \"%s\" permission in reskate-plugin.json", what, permission);
    return plugin;
}
std::uint64_t id_at(lua_State* L, int index) {
    if (lua_isnumber(L, index)) return static_cast<std::uint64_t>(lua_tonumber(L, index));
    // Steam IDs come as strings: a Lua number cannot hold one exactly.
    const std::string text = luaL_checkstring(L, index);
    std::uint64_t out{};
    for (const char c : text) {
        if (c < '0' || c > '9' || out > (~0ULL - 9) / 10) luaL_argerror(L, index, "a Steam ID");
        out = out * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return out;
}
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
void field_number(lua_State* L, const char* name, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}
void field_bool(lua_State* L, const char* name, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}
void field_vec(lua_State* L, const char* name, const Vec3& v) {
    lua_pushvector(L, v[0], v[1], v[2]);
    lua_setfield(L, -2, name);
}

// ---- skater --------------------------------------------------------------------------------------
// skater.telemetry(): the trainer's telemetry, its last jump, the physics watch and the bail's body.
int telemetry(lua_State* L) {
    auto& plugin = allowed(L, "skater.read", "reskate.skater.telemetry");
    const auto t = game_of(plugin).skater_telemetry(plugin.id());
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 20);
    field_bool(L, "skater", t->skater);
    field_bool(L, "airborne", t->airborne);
    field_vec(L, "position", t->position);
    field_number(L, "heading", t->heading);
    field_number(L, "speed", t->speed);
    field_number(L, "vertical", t->vertical);
    field_number(L, "air_time", t->air_time);
    field_number(L, "height", t->height);
    field_number(L, "top_speed", t->top_speed);
    field_number(L, "physics_state", t->physics_state);
    lua_createtable(L, 0, 16);
    const auto& j = t->last;
    field_number(L, "serial", static_cast<double>(j.serial));
    field_number(L, "takeoff_speed", j.takeoff_speed);
    field_number(L, "takeoff_angle", j.takeoff_angle);
    field_number(L, "air_time", j.air_time);
    field_number(L, "height", j.height);
    field_number(L, "distance", j.distance);
    field_number(L, "drop", j.drop);
    field_number(L, "landing_speed", j.landing_speed);
    field_number(L, "spin", j.spin);
    field_number(L, "spin_rate", j.spin_rate);
    field_number(L, "flip", j.flip);
    field_number(L, "board_turn", j.board_turn);
    field_number(L, "state", j.state);
    field_vec(L, "takeoff", j.takeoff);
    field_vec(L, "landing", j.landing);
    lua_setfield(L, -2, "last");
    lua_createtable(L, 0, 9);
    const auto& w = t->watch;
    field_bool(L, "valid", w.valid);
    field_bool(L, "ragdoll_known", w.ragdoll_known);
    field_bool(L, "ragdoll", w.ragdoll);
    field_number(L, "state", w.state);
    field_number(L, "previous", w.previous);
    field_number(L, "previous_seconds", w.previous_seconds);
    field_number(L, "changes", static_cast<double>(w.changes));
    field_number(L, "wipeouts", static_cast<double>(w.wipeouts));
    field_number(L, "runouts", static_cast<double>(w.runouts));
    lua_setfield(L, -2, "watch");
    field_bool(L, "teleport_pending", t->teleport_pending);
    field_bool(L, "slam", t->slam);
    field_bool(L, "struck", t->struck);
    if (t->body) field_vec(L, "body", *t->body);
    return 1;
}
int goofy(lua_State* L) {
    auto& plugin = allowed(L, "skater.read", "reskate.skater.goofy");
    if (const auto g = game_of(plugin).stance_goofy()) lua_pushboolean(L, *g);
    else lua_pushnil(L);
    return 1;
}
// skater.bone_cam(word, ...): no words for its setting ("game", "on" or "off"), "status" its full
// status line, "bail" a bail to show, "ringing" "1" or "0", "game" "on"/"off" (a plugin's game
// wants it), or the `bonecam` console words.
int bone_cam(lua_State* L) {
    auto& plugin = allowed(L, "camera", "reskate.skater.bone_cam");
    std::vector<std::string> words;
    for (int i = 1, n = std::min(lua_gettop(L), 8); i <= n; ++i) {
        std::size_t length = 0;
        const char* word = luaL_checklstring(L, i, &length);
        words.emplace_back(word, std::min<std::size_t>(length, 32));
    }
    const auto text = game_of(plugin).bone_cam(plugin.id(), words);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// ---- input ---------------------------------------------------------------------------------------
// input.pad_bits(menu): XInput's button bits with LT 0x10000 and RT 0x20000, or nil without a pad;
// with `menu`, read while ReSkate's menu is open too.
int pad_bits(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.pad_bits");
    if (const auto bits = game_of(plugin).pad_bits(lua_toboolean(L, 1) != 0)) lua_pushnumber(L, *bits);
    else lua_pushnil(L);
    return 1;
}
int key_code(lua_State* L, int index) {
    if (lua_isnumber(L, index)) {
        const int vk = static_cast<int>(lua_tonumber(L, index));
        if (vk < 1 || vk > 254) luaL_argerror(L, index, "a virtual key from 1 to 254");
        return vk;
    }
    const std::string name = luaL_checkstring(L, index);
    static const std::pair<const char*, int> names[] = {
        {"Enter", 13}, {"Backspace", 8}, {"Escape", 27}, {"End", 35}, {"Home", 36}, {"PageUp", 33}, {"PageDown", 34}, {"Space", 32},
        {"Shift", 16}, {"Ctrl", 17}, {"Alt", 18}, {"Tab", 9}, {"Left", 37}, {"Up", 38}, {"Right", 39}, {"Down", 40},
        {"[", 0xDB}, {"]", 0xDD}, {"Delete", 46}, {"LeftMouse", 1}, {"RightMouse", 2}, {"MiddleMouse", 4}};
    for (const auto& [n, vk] : names)
        if (name == n) return vk;
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    luaL_argerror(L, index, "a key name (A-Z, 0-9, Enter, Backspace, Escape, End, PageUp, PageDown, [, ], ...) or a virtual key");
    return 0;
}
int key(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.key");
    lua_pushboolean(L, game_of(plugin).key_down(key_code(L, 1)));
    return 1;
}
int wheel(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.wheel");
    lua_pushnumber(L, game_of(plugin).mouse_wheel());
    return 1;
}
// input.mouse(): the mouse since this plugin's last read, whatever the game does with it: {x, y
// (raw counts right and down), wheel (notches, away from you positive)}; nil while the game is not in
// front or a menu is open.
int mouse(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.mouse");
    const auto motion = game_of(plugin).mouse_motion(plugin.id());
    if (!motion) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, motion->x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, motion->y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, motion->wheel);
    lua_setfield(L, -2, "wheel");
    return 1;
}
// input.game_buttons(hidden, held, pause): the game buttons (XInput bits) kept from the game while
// asked, those held down for it, and all game input paused for half a second.
int game_buttons(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.game_buttons");
    const auto mask = [&](int index) {
        const double v = luaL_optnumber(L, index, 0);
        if (!(v >= 0 && v <= 0xffff)) luaL_argerror(L, index, "a 16-bit button mask");
        return static_cast<std::uint16_t>(v);
    };
    game_of(plugin).game_buttons(plugin.id(), mask(1), mask(2), lua_toboolean(L, 3) != 0);
    return 0;
}
// input.game_keys(keys): the keys (names, as key() takes them, or virtual keys) whose presses the game
// does not see until the next call; nil or {} gives them back. Not Escape, nor the mouse's buttons.
int game_keys(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.game_keys");
    std::vector<std::uint8_t> keys;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        const int count = lua_objlen(L, 1);
        if (count > 32) luaL_argerror(L, 1, "at most 32 keys");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 1, i);
            const int vk = key_code(L, lua_gettop(L));
            lua_pop(L, 1);
            if (vk == 27 || (vk >= 1 && vk <= 6 && vk != 3)) // Escape; the mouse buttons (3 is Cancel)
                luaL_argerror(L, 1, "Escape and the mouse's buttons always reach the game");
            keys.push_back(static_cast<std::uint8_t>(vk));
        }
    }
    game_of(plugin).game_keys(plugin.id(), std::move(keys));
    return 0;
}
int input_allowed(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.allowed");
    lua_pushboolean(L, game_of(plugin).input_allowed());
    return 1;
}
int focus(lua_State* L) {
    auto& plugin = allowed(L, "input", "reskate.input.focus");
    const auto f = game_of(plugin).input_focus();
    lua_createtable(L, 0, 3);
    field_bool(L, "focused", f.focused);
    field_bool(L, "interface_open", f.interface_open);
    field_bool(L, "gameplay", f.gameplay);
    return 1;
}

// ---- camera --------------------------------------------------------------------------------------
int free_camera(lua_State* L) {
    auto& plugin = allowed(L, "camera", "reskate.camera.free");
    return result(L, game_of(plugin).free_camera(plugin.id(), lua_toboolean(L, 1) != 0));
}
// camera.pad_flight(on): the free camera flown from the pad while placing things from the air (the
// bumpers or Q/E its height; the triggers stay the plugin's). Held by one plugin; released with it.
int pad_flight(lua_State* L) {
    auto& plugin = allowed(L, "camera", "reskate.camera.pad_flight");
    return result(L, game_of(plugin).pad_flight(plugin.id(), lua_toboolean(L, 1) != 0));
}

// ---- session -------------------------------------------------------------------------------------
int spectate(lua_State* L) {
    auto& plugin = allowed(L, "session.read", "reskate.session.spectate");
    game_of(plugin).spectate(lua_isnoneornil(L, 1) ? 0 : id_at(L, 1));
    return 0;
}
// session.world(): {world = id (a string; it changes with the map or session), barred = the local
// player's mods change scoring, map = the map's name}.
int world(lua_State* L) {
    auto& plugin = allowed(L, "session.read", "reskate.session.world");
    auto& g = game_of(plugin);
    lua_createtable(L, 0, 3);
    const auto id = std::to_string(g.world_id());
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "world");
    field_bool(L, "barred", g.world_barred());
    const auto map = g.world_map();
    lua_pushlstring(L, map.data(), map.size());
    lua_setfield(L, -2, "map");
    return 1;
}

// ---- physics -------------------------------------------------------------------------------------
// physics.competition(on): the game's stock physics while the local player competes (the player's
// own tuning comes back after); stock_ready(): the stock tuning has been read; prepare(): read it
// now, switching nothing.
int competition(lua_State* L) {
    auto& plugin = allowed(L, "skater.control", "reskate.physics.competition");
    game_of(plugin).competition(plugin.id(), lua_toboolean(L, 1) != 0);
    return 0;
}
int stock_ready(lua_State* L) {
    auto& plugin = allowed(L, "skater.control", "reskate.physics.stock_ready");
    lua_pushboolean(L, game_of(plugin).stock_physics_ready());
    return 1;
}
int prepare(lua_State* L) {
    auto& plugin = allowed(L, "skater.control", "reskate.physics.prepare");
    game_of(plugin).prepare_physics();
    return 0;
}

// physics.tuning(id): one tuning value, {id, label, group, kind, value, stock, tuner}, or nil and why.
void push_tuning(lua_State* L, const Game::Tuning& t) {
    lua_createtable(L, 0, 7);
    const auto text = [&](const char* field, const std::string& value) {
        lua_pushlstring(L, value.data(), value.size());
        lua_setfield(L, -2, field);
    };
    text("id", t.id);
    text("label", t.label);
    text("group", t.group);
    text("kind", t.kind);
    lua_pushnumber(L, t.value);
    lua_setfield(L, -2, "value");
    lua_pushnumber(L, t.stock);
    lua_setfield(L, -2, "stock");
    if (!t.tuner.empty()) text("tuner", t.tuner);
}
std::string tuning_id(lua_State* L, int index) {
    std::size_t length{};
    const char* id = luaL_checklstring(L, index, &length);
    if (length == 0 || length > 200) luaL_argerror(L, index, "a tuning value's id is 1 to 200 characters");
    return {id, length};
}
int tuning(lua_State* L) {
    auto& plugin = allowed(L, "skater.read", "reskate.physics.tuning");
    std::string why;
    const auto t = game_of(plugin).tuning(tuning_id(L, 1), why);
    if (!t) {
        lua_pushnil(L);
        lua_pushlstring(L, why.data(), why.size());
        return 2;
    }
    push_tuning(L, *t);
    return 1;
}
// physics.find(text): the values whose id has every word of `text`, at most 64.
int find_tuning(lua_State* L) {
    auto& plugin = allowed(L, "skater.read", "reskate.physics.find");
    std::size_t length{};
    const char* text = luaL_checklstring(L, 1, &length);
    if (length > 200) luaL_argerror(L, 1, "at most 200 characters");
    const auto found = game_of(plugin).find_tuning({text, length}, 64);
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        push_tuning(L, found[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}
// physics.tune(id, value): the value over the player's own while the plugin keeps it; nil lets go.
int tune(lua_State* L) {
    auto& plugin = allowed(L, "skater.control", "reskate.physics.tune");
    const auto id = tuning_id(L, 1);
    std::optional<double> value;
    if (lua_type(L, 2) == LUA_TBOOLEAN) value = lua_toboolean(L, 2) ? 1.0 : 0.0;
    else if (!lua_isnoneornil(L, 2)) {
        value = luaL_checknumber(L, 2);
        if (!std::isfinite(*value) || std::abs(*value) > 1e6) luaL_argerror(L, 2, "a finite number within a million");
    }
    return result(L, game_of(plugin).tune(plugin.id(), id, value));
}

// ---- tricks --------------------------------------------------------------------------------------
// tricks.feed(on): skate.'s trick list, entry by entry, as trick_notes events while on.
int trick_feed(lua_State* L) {
    auto& plugin = allowed(L, "events.tricks", "reskate.tricks.feed");
    game_of(plugin).trick_feed(plugin.id(), lua_toboolean(L, 1) != 0);
    return 0;
}
// tricks.gesture(name): the flick diagram of a flip trick as skate. names it ({{x, y}, ...}: x to the
// left, y pulled back, the stick's gate the unit circle, a regular stance); empty until the game's
// gestures are read (the first call starts that) or for a trick without one.
int gesture(lua_State* L) {
    auto& plugin = allowed(L, "events.tricks", "reskate.tricks.gesture");
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    const auto path = game_of(plugin).trick_gesture(std::string_view(name, std::min<std::size_t>(length, 96)));
    lua_createtable(L, static_cast<int>(path.size()), 0);
    for (std::size_t i = 0; i < path.size(); ++i) {
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, path[i][0]);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, path[i][1]);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// tricks.label(from, to): the trick ticker shows `to` where the game's label is exactly `from`
// ("Board Stall"); to nil, that label is the game's again. 1 to 63 characters each, 16 labels a plugin.
int label(lua_State* L) {
    auto& plugin = allowed(L, "hud", "reskate.tricks.label");
    std::size_t from_length = 0, to_length = 0;
    const char* from = luaL_checklstring(L, 1, &from_length);
    const char* to = lua_isnoneornil(L, 2) ? "" : luaL_checklstring(L, 2, &to_length);
    if (!from_length || from_length > 63 || to_length > 63) luaL_error(L, "a label is 1 to 63 characters");
    return result(L, game_of(plugin).trick_label(plugin.id(), std::string_view(from, from_length), std::string_view(to, to_length)));
}
// tricks.record_as(from, to, within_ms): the next trick the game records whose id is one of `from`
// (a list of 1 to 16 ids, or one id), within `within_ms` (1 to 2000), is recorded as the id `to`.
// The game's trick ids, 0 to 336. One pending record a plugin: a new one replaces it.
int record_as(lua_State* L) {
    auto& plugin = allowed(L, "skater.control", "reskate.tricks.record_as");
    const auto id_at = [&](int index) {
        const double value = luaL_checknumber(L, index);
        if (!(value >= 0 && value <= 336) || value != std::floor(value)) luaL_error(L, "a trick id is a whole number, 0 to 336");
        return static_cast<std::uint32_t>(value);
    };
    std::vector<std::uint32_t> from;
    if (lua_istable(L, 1)) {
        const int count = lua_objlen(L, 1);
        if (count < 1 || count > 16) luaL_error(L, "a record names 1 to 16 trick ids");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 1, i);
            from.push_back(id_at(lua_gettop(L)));
            lua_pop(L, 1);
        }
    } else from.push_back(id_at(1));
    const auto to = id_at(2);
    const double within = luaL_checknumber(L, 3);
    if (!(within >= 1 && within <= 2000)) luaL_error(L, "a record waits 1 to 2000 ms");
    return result(L, game_of(plugin).trick_record_as(plugin.id(), std::move(from), to, static_cast<int>(within)));
}

// mods.content(folder): every file under <mod>/<folder>/ of every enabled mod, `folder` one the
// manifest's "content" names: {{mod, title, path, size}, ...}, mod by mod in load order; nil, why.
std::string content_folder(lua_State* L, Plugin& plugin, int index, const char* what) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    std::string folder(text, std::min<std::size_t>(length, 64));
    for (auto& c : folder) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    const auto& declared = plugin.manifest().content;
    if (std::find(declared.begin(), declared.end(), folder) == declared.end())
        luaL_error(L, "%s(\"%s\"): name \"%s\" in the manifest's \"content\" first", what, folder.c_str(), folder.c_str());
    return folder;
}
int content(lua_State* L) {
    auto& plugin = allowed(L, "mods.read", "reskate.mods.content");
    const auto folder = content_folder(L, plugin, 1, "reskate.mods.content");
    std::vector<Game::ContentFile> files;
    if (const auto why = game_of(plugin).content_list(folder, files); !why.empty()) {
        lua_pushnil(L);
        lua_pushlstring(L, why.data(), why.size());
        return 2;
    }
    lua_createtable(L, static_cast<int>(files.size()), 0);
    for (std::size_t i = 0; i < files.size(); ++i) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, files[i].mod.data(), files[i].mod.size());
        lua_setfield(L, -2, "mod");
        lua_pushlstring(L, files[i].title.data(), files[i].title.size());
        lua_setfield(L, -2, "title");
        lua_pushlstring(L, files[i].path.data(), files[i].path.size());
        lua_setfield(L, -2, "path");
        lua_pushnumber(L, static_cast<double>(files[i].size));
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}
// mods.read_content(mod, folder, path): that file's text, at most 1 MB; nil, why ("not found" for none).
int read_content(lua_State* L) {
    auto& plugin = allowed(L, "mods.read", "reskate.mods.read_content");
    std::size_t mod_length = 0;
    const char* mod = luaL_checklstring(L, 1, &mod_length);
    if (!mod_length || mod_length > 64) luaL_argerror(L, 1, "a mod's folder name, 1 to 64 characters");
    const auto folder = content_folder(L, plugin, 2, "reskate.mods.read_content");
    const auto path = safe_relative_path(luaL_checkstring(L, 3));
    if (path.empty()) luaL_argerror(L, 3, "a path inside the mod's folder (use / between folders, no ..)");
    std::string text;
    if (const auto why = game_of(plugin).content_read(std::string_view(mod, mod_length), folder, path, text); !why.empty()) {
        lua_pushnil(L);
        lua_pushlstring(L, why.data(), why.size());
        return 2;
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

} // namespace

void install_services_api(Plugin&, lua_State* L) {
    add_functions(L, "skater", {{"telemetry", telemetry}, {"goofy", goofy}, {"bone_cam", bone_cam}});
    add_functions(L, "input", {{"pad_bits", pad_bits}, {"key", key}, {"wheel", wheel}, {"mouse", mouse}, {"game_buttons", game_buttons}, {"game_keys", game_keys}, {"allowed", input_allowed}, {"focus", focus}});
    add_functions(L, "session", {{"spectate", spectate}, {"world", world}});
    add_functions(L, "camera", {{"free", free_camera}, {"pad_flight", pad_flight}});
    add_functions(L, "physics", {{"competition", competition}, {"stock_ready", stock_ready}, {"prepare", prepare}, {"tuning", tuning},
                                 {"find", find_tuning}, {"tune", tune}});
    add_functions(L, "tricks", {{"feed", trick_feed}, {"gesture", gesture}, {"label", label}, {"record_as", record_as}});
    add_functions(L, "mods", {{"content", content}, {"read_content", read_content}});
}
} // namespace dingosdk::plugins
