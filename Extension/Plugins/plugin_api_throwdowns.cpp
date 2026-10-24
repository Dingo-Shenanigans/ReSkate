#include "plugin_api.h"
#include "plugin_game.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

// The throwdown kit's side of the reskate API (throwdown_kit.h): a plugin's documents for the
// native Throwdowns UI and the native flag registration, plus the general services its modes use:
// the plugin's own data files, overlay dialogs, and the skater's push and hold. Each checks its
// permission and its arguments; the game (plugin_game.h) does the rest.
namespace dingosdk::plugins {
namespace {
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
// nil, why: what a call that hands back a value returns when it fails.
int failed(lua_State* L, const std::string& why) {
    lua_pushnil(L);
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
bool within(double now, double& second, int& count, int most) {
    if (now - second >= 1000) {
        second = now;
        count = 0;
    }
    return ++count <= most;
}
// A string field of the table at `table` (nil: empty), at most `most` long.
std::string text_field(lua_State* L, int table, const char* name, std::size_t most) {
    lua_getfield(L, table, name);
    std::string value;
    if (!lua_isnil(L, -1)) {
        if (!lua_isstring(L, -1)) luaL_error(L, "%s is a string", name);
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        if (length > most) luaL_error(L, "%s is longer than %d characters", name, static_cast<int>(most));
        value.assign(text, length);
    }
    lua_pop(L, 1);
    return value;
}
double number_field(lua_State* L, int table, const char* name, double fallback, double low, double high) {
    lua_getfield(L, table, name);
    double value = fallback;
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "%s is a number", name);
        value = lua_tonumber(L, -1);
        if (!(value >= low && value <= high)) luaL_error(L, "%s is %g to %g", name, low, high);
    }
    lua_pop(L, 1);
    return value;
}
bool bool_field(lua_State* L, int table, const char* name, bool fallback) {
    lua_getfield(L, table, name);
    const bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}
// A mode id of the plugin's own ("duel"): the SDK's is "<plugin>/<id>".
std::string own_mode(lua_State* L, int index) {
    auto id = checked_text(L, index, 32, "a mode id");
    if (id.empty() || id.find('/') != std::string::npos) luaL_argerror(L, index, "a mode id of 1 to 32 characters, without /");
    return id;
}
} // namespace
// A path in the plugin's data folder: relative, inside it, and no name Windows keeps for a device.
std::string data_path(lua_State* L, int index, bool folder) {
    if (folder && lua_isnoneornil(L, index)) return {};
    auto path = checked_text(L, index, 200, "a data path");
    while (folder && path.ends_with('/')) path.pop_back();
    if (folder && path.empty()) return {};
    if (safe_relative_path(path).empty()) luaL_argerror(L, index, "a path inside the plugin's data folder (no .., no drive, / between folders)");
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        std::string base(path.substr(start, std::min(path.find('.', start), end) - start));
        for (auto& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
        const bool numbered = base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9';
        if (numbered || std::find(devices.begin(), devices.end(), base) != devices.end()) luaL_argerror(L, index, "a name Windows keeps for a device");
        start = end + 1;
    }
    return path;
}
namespace {
} // namespace

struct KitApi {
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }

    // ---- throwdowns: the kit ---------------------------------------------------------------------
    // ui(doc): the plugin's documents (setup, waiting, hud, panel, flags, match) for the native UI.
    // Every tick is fine: an unchanged document costs a comparison.
    static int ui(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "throwdowns", "reskate.throwdowns.ui");
        luaL_checktype(L, 1, LUA_TTABLE);
        Json document;
        try {
            document = lua_to_json(L, 1);
        } catch (const std::exception& e) {
            luaL_error(L, "%s", e.what());
        }
        return result(L, game(plugin).publish_throwdown_ui(plugin.id(), document));
    }
    // flag.begin(mode, game, {players, seconds, setup_page, cursor_height}): the native flag for
    // one of the plugin's modes; false while another registration is busy.
    static int flag_begin(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "throwdowns", "reskate.throwdowns.flag.begin");
        const auto mode = own_mode(L, 1);
        const double number = luaL_checknumber(L, 2);
        if (!(number >= 1 && number <= 4294967295.0) || number != std::floor(number)) luaL_argerror(L, 2, "a game number");
        throwdowns::kit::flag::Options options;
        if (!lua_isnoneornil(L, 3)) {
            luaL_checktype(L, 3, LUA_TTABLE);
            options.players = static_cast<unsigned>(number_field(L, 3, "players", options.players, 1, 10));
            options.seconds = static_cast<unsigned>(number_field(L, 3, "seconds", options.seconds, 1, 3600));
            options.setup_page = bool_field(L, 3, "setup_page", options.setup_page);
            options.cursor_height = static_cast<float>(number_field(L, 3, "cursor_height", options.cursor_height, 0, 100000));
        }
        lua_pushboolean(L, game(plugin).throwdown_flag_begin(plugin.id() + "/" + mode, static_cast<std::uint32_t>(number), options));
        return 1;
    }
    // flag.state(): {owned, waiting, placing, busy, mine, mode, game}; mode (the plugin's own id) and
    // game only when the registration is one of its modes.
    static int flag_state(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "throwdowns", "reskate.throwdowns.flag.state");
        const auto state = game(plugin).throwdown_flag_state();
        const auto prefix = plugin.id() + "/";
        const bool mine = state.mode.starts_with(prefix);
        lua_createtable(L, 0, 7);
        lua_pushboolean(L, state.owned);
        lua_setfield(L, -2, "owned");
        lua_pushboolean(L, state.waiting);
        lua_setfield(L, -2, "waiting");
        lua_pushboolean(L, state.placing);
        lua_setfield(L, -2, "placing");
        lua_pushboolean(L, state.busy);
        lua_setfield(L, -2, "busy");
        lua_pushboolean(L, mine);
        lua_setfield(L, -2, "mine");
        if (mine) {
            const auto own = state.mode.substr(prefix.size());
            lua_pushlstring(L, own.data(), own.size());
            lua_setfield(L, -2, "mode");
            lua_pushnumber(L, state.game);
            lua_setfield(L, -2, "game");
        }
        return 1;
    }
    static int flag_leave(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "throwdowns", "reskate.throwdowns.flag.leave");
        game(plugin).throwdown_flag_leave(plugin.id());
        return 0;
    }

    // ---- files: the plugin's own data folder ----------------------------------------------------------
    static Plugin& files_call(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "files", what);
        if (!within(plugin.services_.now_ms(), plugin.files_second_, plugin.file_calls_, 64)) luaL_error(L, "more than 64 file calls in a second");
        return plugin;
    }
    // list(folder?): the names in it, folders ending in "/"; or nil, why.
    static int files_list(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.list");
        const auto folder = data_path(L, 1, true);
        std::vector<std::string> names;
        if (const auto why = game(plugin).files_list(plugin.id(), folder, names); !why.empty()) return failed(L, why);
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (std::size_t i = 0; i < names.size(); ++i) {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    // read(path): the file's text, or nil, why ("not found" for none).
    static int files_read(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.read");
        const auto path = data_path(L, 1, false);
        std::string text;
        if (const auto why = game(plugin).files_read(plugin.id(), path, text); !why.empty()) return failed(L, why);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    // write(path, text): at most 1 MB a file and 1000 files; the folders it names are made.
    static int files_write(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.write");
        const auto path = data_path(L, 1, false);
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, 2, &length);
        if (length > files_most_bytes) return result(L, "a file is at most 1 MB");
        return result(L, game(plugin).files_write(plugin.id(), path, std::string_view(text, length)));
    }
    // remove(path): a file, or a folder with nothing in it.
    static int files_remove(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.remove");
        return result(L, game(plugin).files_remove(plugin.id(), data_path(L, 1, false)));
    }

    // ---- ui.dialog ----------------------------------------------------------------------------------
    // dialog(id, {title, message, field = {text, max}, list = {rows = {{text, sub, buttons = {"rename"}}},
    // empty}, buttons = {"OK", {id = "save", label = "Save"}}, file = {label, filter = "*.json"}}).
    // Answers come as throwdown_ui events: action "dialog", args {id, button, text, row, file_text}.
    // Esc (or B) answers "cancel" and closes it.
    static int dialog(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "ui.screen", "reskate.ui.dialog");
        need(L, plugin, "throwdowns", "reskate.ui.dialog (its answers are throwdown_ui events)");
        if (!within(plugin.services_.now_ms(), plugin.dialog_second_, plugin.dialog_calls_, 120)) luaL_error(L, "more than 120 dialog calls in a second");
        DialogSpec spec;
        spec.id = checked_text(L, 1, 32, "a dialog id");
        if (spec.id.empty()) luaL_argerror(L, 1, "a dialog id of 1 to 32 characters");
        luaL_checktype(L, 2, LUA_TTABLE);
        spec.title = text_field(L, 2, "title", 64);
        spec.message = text_field(L, 2, "message", 1024);
        lua_getfield(L, 2, "field");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int at = lua_gettop(L);
            spec.field.present = true;
            spec.field.max = static_cast<std::size_t>(number_field(L, at, "max", 64, 1, 256));
            spec.field.text = text_field(L, at, "text", spec.field.max);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "list");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int at = lua_gettop(L);
            spec.list = true;
            spec.empty = text_field(L, at, "empty", 256);
            lua_getfield(L, at, "rows");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                const int rows = lua_gettop(L);
                const int count = lua_objlen(L, rows);
                if (count > 64) luaL_error(L, "a dialog's list has at most 64 rows");
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, rows, i);
                    luaL_checktype(L, -1, LUA_TTABLE);
                    const int row = lua_gettop(L);
                    DialogSpec::Row r{text_field(L, row, "text", 128), text_field(L, row, "sub", 128), {}};
                    lua_getfield(L, row, "buttons");
                    if (!lua_isnil(L, -1)) {
                        luaL_checktype(L, -1, LUA_TTABLE);
                        const int buttons = lua_objlen(L, -1);
                        if (buttons > 4) luaL_error(L, "a row has at most 4 buttons");
                        for (int b = 1; b <= buttons; ++b) {
                            lua_rawgeti(L, -1, b);
                            std::size_t length = 0;
                            const char* name = lua_isstring(L, -1) ? lua_tolstring(L, -1, &length) : nullptr;
                            if (!name || !length || length > 24) luaL_error(L, "a row's buttons are names of 1 to 24 characters");
                            r.buttons.emplace_back(name, length);
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 2);
                    spec.rows.push_back(std::move(r));
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "buttons");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int at = lua_gettop(L);
            const int count = lua_objlen(L, at);
            if (count > 4) luaL_error(L, "a dialog has at most 4 buttons");
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, at, i);
                DialogSpec::Button button;
                if (lua_istable(L, -1)) {
                    button.label = text_field(L, lua_gettop(L), "label", 32);
                    button.id = text_field(L, lua_gettop(L), "id", 24);
                } else if (lua_isstring(L, -1)) {
                    std::size_t length = 0;
                    const char* label = lua_tolstring(L, -1, &length);
                    if (length > 32) luaL_error(L, "a button's label is at most 32 characters");
                    button.label.assign(label, length);
                } else luaL_error(L, "a dialog's buttons are labels or {id, label}");
                if (button.label.empty()) luaL_error(L, "a button needs a label");
                if (button.id.empty()) button.id = button.label.substr(0, 24);
                lua_pop(L, 1);
                spec.buttons.push_back(std::move(button));
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "file");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int at = lua_gettop(L);
            spec.file.present = true;
            spec.file.label = text_field(L, at, "label", 32);
            if (spec.file.label.empty()) spec.file.label = "Open a file";
            spec.file.filter = text_field(L, at, "filter", 64);
            if (spec.file.filter.empty()) spec.file.filter = "*.*";
            for (const char c : spec.file.filter)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '*' && c != '.' && c != ';' && c != '_' && c != '-')
                    luaL_error(L, "a file filter is patterns like *.json, split by ;");
        }
        lua_pop(L, 1);
        return result(L, game(plugin).dialog_open(plugin.id(), spec));
    }
    static int close_dialog(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "ui.screen", "reskate.ui.close_dialog");
        const auto id = lua_isnoneornil(L, 1) ? std::string{} : checked_text(L, 1, 32, "a dialog id");
        game(plugin).dialog_close(plugin.id(), id);
        return 0;
    }

    // ---- skater.push / hold -------------------------------------------------------------------------
    // push(velocity): the skater's velocity (m/s, world) once it is on its board again.
    static int push(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.push");
        const float* v = luaL_checkvector(L, 1);
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100) luaL_argerror(L, 1, "a velocity within 100 m/s on each axis");
        return result(L, game(plugin).skater_push(plugin.id(), Vec3{v[0], v[1], v[2]}));
    }
    // push_wait(): the skater is still getting back on its board, so a pending push waits two seconds
    // more; push_reset(): a pending push is dropped.
    static int push_wait(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.push_wait");
        return result(L, game(plugin).skater_push_wait(plugin.id()));
    }
    static int push_reset(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.push_reset");
        return result(L, game(plugin).skater_push_reset(plugin.id()));
    }
    // cruise(speed, may_slow): carried up to `speed` (m/s, 1 to 30; 0 or nil off) while rolling and not
    // braking, for the next 300 ms (call it every tick); with may_slow, eased down to it when faster.
    static int cruise(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.cruise");
        const double speed = luaL_optnumber(L, 1, 0);
        if (!(speed == 0 || (speed >= 1 && speed <= 30))) luaL_argerror(L, 1, "a cruise speed is 1 to 30 m/s, or 0 for none");
        return result(L, game(plugin).cruise(plugin.id(), static_cast<float>(speed), lua_toboolean(L, 2) != 0));
    }
    // set_velocity(velocity, add): the board's velocity (m/s, in the world) set, or added to, by the
    // next physics step with the skater on its board, on the ground or in the air.
    static int set_velocity(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.set_velocity");
        const auto v = position_of(L, 1, "a velocity");
        for (const auto a : v)
            if (!(std::abs(a) <= 100)) luaL_argerror(L, 1, "a velocity is at most 100 m/s on each axis");
        return result(L, game(plugin).set_velocity(plugin.id(), v, lua_toboolean(L, 2) != 0));
    }
    static int hold(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.control", "reskate.skater.hold");
        return result(L, game(plugin).skater_hold(plugin.id(), lua_toboolean(L, 1) != 0));
    }

    static constexpr std::size_t files_most_bytes = 1u << 20;
    static void install(Plugin& plugin, lua_State* L);
};

void install_kit_api(Plugin& plugin, lua_State* L) { KitApi::install(plugin, L); }


void KitApi::install(Plugin&, lua_State* L) {
    add_functions(L, "throwdowns", {{"ui", ui}});
    lua_getfield(L, -1, "throwdowns");
    add_functions(L, "flag", {{"begin", flag_begin}, {"state", flag_state}, {"leave", flag_leave}});
    lua_pop(L, 1);
    add_functions(L, "files", {{"list", files_list}, {"read", files_read}, {"write", files_write}, {"remove", files_remove}});
    add_functions(L, "ui", {{"dialog", dialog}, {"close_dialog", close_dialog}});
    add_functions(L, "skater", {{"push", push}, {"push_wait", push_wait}, {"push_reset", push_reset}, {"hold", hold}, {"cruise", cruise}, {"set_velocity", set_velocity}});
}
}
