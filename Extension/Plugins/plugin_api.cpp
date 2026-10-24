#include "plugin_api.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>

// The reskate API, v1 base set plus console and storage (docs: ReSkate Plugin System Design).
// Every function checks what it is given before acting on it, and no value a plugin passes
// reaches anything outside its own VM unchecked. Errors are raised the way Lua reports them:
// luaL_error, or a C++ exception, which Luau turns into a Lua error with its message.
namespace dingosdk::plugins {
namespace {
// Events and the permission each needs (none for the base set).
struct Event {
    std::string_view name, permission;
};
constexpr std::array<Event, 31> events{{
    {"load", {}}, {"unload", {}}, {"tick", {}}, {"level_loaded", {}}, {"level_unloading", {}},
    {"trick_started", "events.tricks"}, {"trick_landed", "events.tricks"}, {"line_broken", "events.tricks"},
    {"draw", "overlay"}, {"player_joined", "session.read"}, {"player_left", "session.read"}, {"message", "net.session"},
    {"skater_state", "events.skater"}, {"bail", "events.skater"}, {"chat", "chat.read"}, {"track_changed", "music"},
    {"throwdown", "throwdowns"}, {"action", "input"}, {"screen_closed", "ui.screen"},
    {"board_landed", "events.skater"}, {"body_hit", "events.skater"}, {"bail_end", "events.skater"},
    {"physics_step", "events.physics"}, {"launch", "events.skater"}, {"trick_notes", "events.tricks"},
    {"throwdown_ui", "throwdowns"}, {"touch", "session.skaters"}, {"touch_end", "session.skaters"}, {"camera_cycle", "camera"},
    {"poll_answer", "server"}, {"poll_closed", "server"},
}};
constexpr std::size_t maximum_key = 128;
constexpr std::size_t maximum_command = 48;
constexpr std::size_t maximum_listing = 1000;

// The table being filled is on top of the stack.
void set_functions(lua_State* L, std::initializer_list<ApiFunction> functions) {
    for (const auto& [name, function] : functions) {
        lua_pushcfunction(L, function, name);
        lua_setfield(L, -2, name);
    }
}

std::string text_of(lua_State* L, int index) {
    std::size_t length = 0;
    const char* text = luaL_tolstring(L, index, &length);
    std::string result(text, length);
    lua_pop(L, 1);
    return result;
}

// What turning Lua into JSON may cost outside the VM's own heap.
struct Budget {
    std::size_t values = 1u << 20;
    std::size_t bytes = 16u << 20;
    void spend(std::size_t count, std::size_t size) {
        if (count > values || size > bytes) throw std::runtime_error("the value is too large to turn into JSON");
        values -= count;
        bytes -= size;
    }
};

Json to_json(lua_State* L, int index, int depth, Budget& budget) {
    if (depth > 32) throw std::runtime_error("tables nested deeper than 32 cannot be turned into JSON");
    index = lua_absindex(L, index);
    budget.spend(1, 0);
    switch (lua_type(L, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER: {
        const double value = lua_tonumber(L, index);
        if (!std::isfinite(value)) throw std::runtime_error("JSON has no NaN or infinity");
        if (value == std::floor(value) && std::fabs(value) <= 9007199254740992.0) return static_cast<std::int64_t>(value);
        return value;
    }
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        budget.spend(0, length);
        return std::string(text, length);
    }
    case LUA_TVECTOR: { // a position: [x, y, z]
        const float* v = lua_tovector(L, index);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) throw std::runtime_error("JSON has no NaN or infinity");
        budget.spend(3, 0);
        return Json::array({static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])});
    }
    case LUA_TTABLE: {
        luaL_checkstack(L, 4, "tables nested too deep");
        const int length = lua_objlen(L, index);
        bool list = length > 0;
        int count = 0;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            ++count;
            if (lua_type(L, -2) != LUA_TNUMBER) list = false;
            else {
                const double key = lua_tonumber(L, -2);
                if (key != std::floor(key) || key < 1 || key > length) list = false;
            }
            lua_pop(L, 1);
        }
        if (list && count == length) {
            auto result = Json::array();
            for (int i = 1; i <= length; ++i) {
                lua_rawgeti(L, index, i);
                result.push_back(to_json(L, -1, depth + 1, budget));
                lua_pop(L, 1);
            }
            return result;
        }
        auto result = Json::object();
        lua_pushnil(L);
        while (lua_next(L, index)) {
            if (lua_type(L, -2) != LUA_TSTRING)
                throw std::runtime_error("only a list (keys 1 to n) or a table with string keys can be turned into JSON");
            std::size_t size = 0;
            const char* key = lua_tolstring(L, -2, &size);
            budget.spend(0, size);
            result[std::string_view(key, size)] = to_json(L, -1, depth + 1, budget);
            lua_pop(L, 1);
        }
        return result;
    }
    default:
        throw std::runtime_error(std::string("a ") + lua_typename(L, lua_type(L, index)) + " cannot be turned into JSON");
    }
}

void push_json_value(lua_State* L, const Json& value, int depth) {
    luaL_checkstack(L, 4, "JSON nested too deep");
    if (depth > 64) throw std::runtime_error("JSON nested too deep");
    if (value.is_null()) lua_pushnil(L);
    else if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
    else if (value.is_number()) lua_pushnumber(L, value.get<double>());
    else if (value.is_string()) lua_pushlstring(L, value.string().data(), value.string().size());
    else if (value.is_array()) {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        int index = 0;
        for (const auto& item : value) {
            push_json_value(L, item, depth + 1);
            lua_rawseti(L, -2, ++index);
        }
    } else {
        lua_createtable(L, 0, static_cast<int>(value.size()));
        for (const auto& [key, item] : value.items()) {
            lua_pushlstring(L, key.data(), key.size());
            push_json_value(L, item, depth + 1);
            lua_rawset(L, -3);
        }
    }
}

std::string key_of(lua_State* L, int index) {
    std::size_t length = 0;
    const char* key = luaL_checklstring(L, index, &length);
    if (length == 0 || length > maximum_key) luaL_error(L, "a storage key must be 1 to %d characters", static_cast<int>(maximum_key));
    return std::string(key, length);
}

// A file or folder of the plugin's own package, never outside it: the path is checked as text,
// then once more after links and junctions are resolved.
std::filesystem::path package_path(lua_State* L, Plugin& plugin, std::string_view path, bool folder) {
    std::string relative = folder && path.empty() ? std::string{} : safe_relative_path(path);
    if (relative.empty() && !(folder && path.empty()))
        luaL_error(L, "\"%s\" is not a path inside the plugin's package (use / between folders, no ..)", std::string(path).c_str());
    const auto root = plugin.directory();
    const auto full = relative.empty() ? root : root / std::filesystem::path(relative);
    std::error_code code;
    const auto real_root = std::filesystem::weakly_canonical(root, code);
    const auto real = std::filesystem::weakly_canonical(full, code);
    const auto inside = real.lexically_relative(real_root);
    if (code || (!relative.empty() && (inside.empty() || *inside.begin() == ".."))) luaL_error(L, "that path leads outside the package");
    return full;
}
}

void add_functions(lua_State* L, const char* field, std::initializer_list<ApiFunction> functions) {
    lua_getfield(L, -1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, static_cast<int>(functions.size()));
        lua_pushvalue(L, -1);
        // The parent may be a read-only table itself (reskate.throwdowns, for its flag).
        const bool locked = lua_getreadonly(L, -3) != 0;
        lua_setreadonly(L, -3, false);
        lua_setfield(L, -3, field);
        lua_setreadonly(L, -2, locked);
    }
    // A table made read-only earlier in the install is opened for this, and closed again.
    lua_setreadonly(L, -1, false);
    set_functions(L, functions);
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}
void add_functions(lua_State* L, std::initializer_list<ApiFunction> functions) { set_functions(L, functions); }
void push_functions(lua_State* L, std::initializer_list<ApiFunction> functions) {
    lua_createtable(L, 0, static_cast<int>(functions.size()));
    set_functions(L, functions);
    lua_setreadonly(L, -1, true);
}

// A plugin that caught its own memory or time error in a pcall gets no further: nothing it asks
// for is done, and the next safe point stops it.
Plugin& plugin_of(lua_State* L) {
    auto& plugin = *static_cast<Plugin*>(lua_callbacks(L)->userdata);
    if (plugin.halted()) luaL_error(L, "the plugin was stopped");
    return plugin;
}
void need(lua_State* L, Plugin& plugin, const char* permission, const char* what) {
    if (!plugin.has(permission))
        luaL_error(L, "%s needs the \"%s\" permission in reskate-plugin.json", what, permission);
}
std::string checked_text(lua_State* L, int index, std::size_t most, const char* what) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    if (length > most) luaL_error(L, "%s is longer than %d characters", what, static_cast<int>(most));
    return std::string(text, length);
}
Vec3 position_of(lua_State* L, int index, const char* what) {
    const float* v = luaL_checkvector(L, index);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e5f) luaL_error(L, "%s must be a finite position within 100 km", what);
    return {v[0], v[1], v[2]};
}
void push_position(lua_State* L, const Vec3& v) { lua_pushvector(L, v[0], v[1], v[2]); }

std::filesystem::path package_file(lua_State* L, Plugin& plugin, std::string_view path) { return package_path(L, plugin, path, false); }

struct ApiFunctions {
    static int log(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto message = text_of(L, 1);
        const std::string level = luaL_optstring(L, 2, "info");
        if (level != "info" && level != "warn" && level != "error") luaL_argerror(L, 2, "the level must be \"info\", \"warn\" or \"error\"");
        plugin.log(level == "error" ? LogLevel::error : level == "warn" ? LogLevel::warn : LogLevel::info, message);
        return 0;
    }
    static int print(lua_State* L) {
        std::string line;
        const int count = lua_gettop(L);
        for (int i = 1; i <= count; ++i) {
            if (i > 1) line += '\t';
            line += text_of(L, i);
        }
        plugin_of(L).log(LogLevel::info, line);
        return 0;
    }
    static int on(lua_State* L) {
        auto& plugin = plugin_of(L);
        const std::string event = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const auto known = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.name == event; });
        if (known == events.end()) luaL_error(L, "there is no event \"%s\" in this ReSkate", event.c_str());
        if (!known->permission.empty() && !plugin.has(known->permission))
            luaL_error(L, "the %s event needs the \"%s\" permission in reskate-plugin.json", event.c_str(), std::string(known->permission).c_str());
        auto& list = plugin.handlers_[event];
        if (list.size() >= plugin.limits_.handlers) luaL_error(L, "more than %d handlers for one event", static_cast<int>(plugin.limits_.handlers));
        lua_pushvalue(L, 2);
        list.push_back(lua_ref(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    static int now(lua_State* L) {
        lua_pushnumber(L, plugin_of(L).services_.now_ms());
        return 1;
    }
    static int timer(lua_State* L, bool repeat) {
        auto& plugin = plugin_of(L);
        const double ms = luaL_checknumber(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (!std::isfinite(ms) || ms < (repeat ? 1 : 0)) luaL_argerror(L, 1, repeat ? "must be 1 ms or more" : "must be 0 ms or more");
        if (plugin.timers_.size() >= plugin.limits_.timers) luaL_error(L, "more than %d timers", static_cast<int>(plugin.limits_.timers));
        lua_pushvalue(L, 2);
        const int function = lua_ref(L, -1);
        lua_pop(L, 1);
        const int id = plugin.next_timer_++;
        plugin.timers_[id] = {plugin.services_.now_ms() + ms, repeat ? ms : 0, function};
        lua_pushinteger(L, id);
        return 1;
    }
    static int after(lua_State* L) { return timer(L, false); }
    static int every(lua_State* L) { return timer(L, true); }
    static int cancel(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto found = plugin.timers_.find(luaL_checkinteger(L, 1));
        if (found != plugin.timers_.end()) {
            lua_unref(L, found->second.function);
            plugin.timers_.erase(found);
        }
        return 0;
    }
    static int level(lua_State* L) {
        const auto name = plugin_of(L).services_.level();
        if (name.empty()) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 1);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        return 1;
    }
    static int read(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto file = package_path(L, plugin, luaL_checkstring(L, 1), false);
        std::string error;
        const auto text = read_package_file(file, plugin.limits_.file_bytes, error);
        if (error == "not found") {
            lua_pushnil(L);
            return 1;
        }
        if (!error.empty()) luaL_error(L, "%s %s", luaL_checkstring(L, 1), error.c_str());
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    static int read_json(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto file = package_path(L, plugin, luaL_checkstring(L, 1), false);
        std::string error;
        const auto text = read_package_file(file, plugin.limits_.file_bytes, error);
        if (error == "not found") {
            lua_pushnil(L);
            return 1;
        }
        if (!error.empty()) luaL_error(L, "%s %s", luaL_checkstring(L, 1), error.c_str());
        push_json(L, Json::parse(text, {plugin.limits_.file_bytes, 64, 1u << 20}));
        return 1;
    }
    // variants(): the variants the plugin's own mod holds (its reskate-plugin.json "variants",
    // Engine/Vfs/mod_list.h): {list = {...}, chosen = name or nil, in_play = name or nil}, its "order"
    // first; nil when it declares none.
    static int variants(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto state = plugin.services_.game().variants(plugin.id(), plugin.directory());
        if (!state) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 3);
        lua_createtable(L, static_cast<int>(state->installed.size()), 0);
        for (std::size_t i = 0; i < state->installed.size(); ++i) {
            lua_pushlstring(L, state->installed[i].data(), state->installed[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "list");
        for (const auto& [field, value] : {std::pair{"chosen", &state->chosen}, std::pair{"in_play", &state->in_play}}) {
            if (value->empty()) continue;
            lua_pushlstring(L, value->data(), value->size());
            lua_setfield(L, -2, field);
        }
        return 1;
    }
    // choose(name): that variant of the plugin's own mod in the game: kept as the choice, the mods
    // applied again and the level loaded again with it. true, or false and why not.
    static int choose(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "variants", "reskate.package.choose");
        const std::string name = luaL_checkstring(L, 1);
        const auto why = plugin.services_.game().choose_variant(plugin.id(), plugin.directory(), name);
        lua_pushboolean(L, why.empty());
        if (why.empty()) return 1;
        lua_pushlstring(L, why.data(), why.size());
        return 2;
    }
    static int list(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto folder = package_path(L, plugin, luaL_optstring(L, 1, ""), true);
        std::vector<std::string> names;
        std::error_code code;
        for (std::filesystem::directory_iterator it(folder, code), end; !code && it != end; it.increment(code)) {
            const auto name = it->path().filename().wstring();
            if (!std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= 0x20 && c < 0x7f; })) continue;
            std::string ascii;
            for (const wchar_t c : name) ascii += static_cast<char>(c);
            if (it->is_directory(code)) ascii += '/';
            names.push_back(std::move(ascii));
            if (names.size() > maximum_listing) luaL_error(L, "the folder holds more than %d entries", static_cast<int>(maximum_listing));
        }
        std::sort(names.begin(), names.end());
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (std::size_t i = 0; i < names.size(); ++i) {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int encode(lua_State* L) {
        luaL_checkany(L, 1);
        const auto text = lua_to_json(L, 1).dump(lua_toboolean(L, 2) ? 2 : -1);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    static int decode(lua_State* L) {
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, 1, &length);
        push_json(L, Json::parse(std::string_view(text, length), {16u << 20, 64, 1u << 20}));
        return 1;
    }
    static int command(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "console", "reskate.console.command");
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, 1, &length);
        const std::string help = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        const std::string name(text, length);
        // Lowercase words of letters, digits, _ and -, one space between them, at most three.
        bool valid = !name.empty() && name.size() <= maximum_command && name.front() != ' ' && name.back() != ' ' &&
                     name.find("  ") == std::string::npos && std::count(name.begin(), name.end(), ' ') < 3;
        for (const char c : name) valid = valid && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ');
        if (!valid) luaL_error(L, "\"%s\" is not a command name: 1 to 3 lowercase words of a-z, 0-9, _ and -", name.c_str());
        if (plugin.commands_.size() >= plugin.limits_.commands) luaL_error(L, "more than %d commands", static_cast<int>(plugin.limits_.commands));
        if (plugin.commands_.contains(name)) luaL_error(L, "the command \"%s\" is registered already", name.c_str());
        if (const auto refusal = plugin.services_.claim_command(plugin.id(), name); !refusal.empty())
            luaL_error(L, "%s", refusal.c_str());
        // subcommands = {{name = "random", args = "[start]", help = "..."}, ...}: what the console's
        // lookup and `plugin commands` show after the name. The function still runs them all.
        std::vector<Plugin::Subcommand> subcommands;
        if (!lua_isnoneornil(L, 4)) {
            luaL_checktype(L, 4, LUA_TTABLE);
            const int count = static_cast<int>(lua_objlen(L, 4));
            if (count > 32) luaL_error(L, "at most 32 subcommands");
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, 4, i);
                if (!lua_istable(L, -1)) luaL_error(L, "subcommand %d is not a table {name = ..., args = ..., help = ...}", i);
                Plugin::Subcommand sub;
                const auto field = [&](const char* key, std::size_t most) {
                    lua_getfield(L, -1, key);
                    std::string value = lua_isnil(L, -1) ? std::string{} : std::string(luaL_checkstring(L, -1));
                    lua_pop(L, 1);
                    if (value.size() > most) luaL_error(L, "a subcommand's %s is at most %d characters", key, static_cast<int>(most));
                    return value;
                };
                sub.name = field("name", maximum_command);
                sub.args = field("args", 80);
                sub.help = field("help", 200);
                bool word = !sub.name.empty();
                for (const char c : sub.name) word = word && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
                if (!word) luaL_error(L, "subcommand %d's name is not one lowercase word of a-z, 0-9, _ and -", i);
                subcommands.push_back(std::move(sub));
                lua_pop(L, 1);
            }
        }
        lua_pushvalue(L, 3);
        plugin.commands_[name] = {help.substr(0, 200), lua_ref(L, -1), std::move(subcommands)};
        lua_pop(L, 1);
        return 0;
    }
    static int console_print(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "console", "reskate.console.print");
        plugin.print(text_of(L, 1));
        return 0;
    }
    static int storage_get(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "storage", "reskate.storage");
        const auto key = key_of(L, 1);
        const auto found = plugin.storage_.find(key);
        if (found == plugin.storage_.end()) {
            lua_pushnil(L);
            return 1;
        }
        push_json(L, *found);
        return 1;
    }
    // Sets a key and checks the whole store against the quota; the value it had comes back on failure.
    static void store(lua_State* L, Plugin& plugin, const std::string& key, Json value) {
        std::optional<Json> previous;
        if (const auto found = plugin.storage_.find(key); found != plugin.storage_.end()) previous = *found;
        if (value.is_null()) plugin.storage_.erase(key);
        else plugin.storage_[key] = std::move(value);
        if (const auto size = plugin.storage_.dump().size(); size > plugin.limits_.storage_bytes) {
            if (previous) plugin.storage_[key] = std::move(*previous);
            else plugin.storage_.erase(key);
            luaL_error(L, "storage is full: that would make it %d KB, and a plugin has %d KB", static_cast<int>(size >> 10),
                static_cast<int>(plugin.limits_.storage_bytes >> 10));
        }
        plugin.storage_dirty_ = true;
    }
    static int storage_set(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "storage", "reskate.storage");
        const auto key = key_of(L, 1);
        store(L, plugin, key, lua_to_json(L, 2));
        return 0;
    }
    static int storage_add(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "storage", "reskate.storage");
        const auto key = key_of(L, 1);
        const double by = luaL_optnumber(L, 2, 1);
        if (!std::isfinite(by)) luaL_argerror(L, 2, "must be a finite number");
        double value = 0;
        if (const auto found = plugin.storage_.find(key); found != plugin.storage_.end()) {
            if (!found->is_number()) luaL_error(L, "storage key \"%s\" does not hold a number", key.c_str());
            value = found->get<double>();
        }
        value += by;
        store(L, plugin, key, value == std::floor(value) && std::fabs(value) <= 9007199254740992.0
            ? Json(static_cast<std::int64_t>(value)) : Json(value));
        lua_pushnumber(L, value);
        return 1;
    }
    static int storage_keys(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "storage", "reskate.storage");
        lua_createtable(L, static_cast<int>(plugin.storage_.size()), 0);
        int index = 0;
        for (const auto& item : plugin.storage_.items()) {
            lua_pushlstring(L, item.first.data(), item.first.size());
            lua_rawseti(L, -2, ++index);
        }
        return 1;
    }
    // skater.read: the local skater as the game has it this tick, or nil without one.
    static std::optional<SkaterState> skater(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.read", what);
        return plugin.services_.skater();
    }
    static int skater_get(lua_State* L) {
        const auto s = skater(L, "reskate.skater.get");
        if (!s) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 8);
        push_position(L, s->position);
        lua_setfield(L, -2, "position");
        lua_pushnumber(L, s->heading);
        lua_setfield(L, -2, "heading");
        lua_pushnumber(L, s->speed);
        lua_setfield(L, -2, "speed");
        lua_pushnumber(L, s->vertical);
        lua_setfield(L, -2, "vertical_speed");
        lua_pushboolean(L, s->airborne);
        lua_setfield(L, -2, "airborne");
        lua_pushnumber(L, s->air_time);
        lua_setfield(L, -2, "air_time");
        if (s->right) {
            push_position(L, *s->right);
            lua_setfield(L, -2, "right");
            lua_pushboolean(L, s->switch_stance);
            lua_setfield(L, -2, "switch");
        }
        return 1;
    }
    static int skater_position(lua_State* L) {
        const auto s = skater(L, "reskate.skater.position");
        if (s) push_position(L, s->position);
        else lua_pushnil(L);
        return 1;
    }
    static int skater_heading(lua_State* L) {
        const auto s = skater(L, "reskate.skater.heading");
        if (s) lua_pushnumber(L, s->heading);
        else lua_pushnil(L);
        return 1;
    }
    static int skater_speed(lua_State* L) {
        const auto s = skater(L, "reskate.skater.speed");
        if (s) lua_pushnumber(L, s->speed);
        else lua_pushnil(L);
        return 1;
    }
    static int skater_on_ground(lua_State* L) {
        const auto s = skater(L, "reskate.skater.on_ground");
        if (s) lua_pushboolean(L, !s->airborne);
        else lua_pushnil(L);
        return 1;
    }
    // skater.teleport: true when the teleport was sent (it happens within a few seconds); false
    // and why when the host has turned teleporting off, it cannot be done now, or it is asked too often.
    static int skater_teleport(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "skater.teleport", "reskate.skater.teleport");
        const auto at = position_of(L, 1, "the teleport's position");
        float heading = 0;
        if (lua_isnoneornil(L, 2)) {
            if (const auto s = plugin.services_.skater()) heading = s->heading;
        } else {
            const double given = luaL_checknumber(L, 2);
            if (!std::isfinite(given)) luaL_argerror(L, 2, "the heading must be a finite number of radians");
            heading = static_cast<float>(std::remainder(given, 2 * 3.14159265358979323846));
        }
        const double now = plugin.services_.now_ms();
        if (now - plugin.teleport_second_ >= 1000) {
            plugin.teleport_second_ = now;
            plugin.teleports_ = 0;
        }
        const char* refusal = nullptr;
        if (++plugin.teleports_ > plugin.limits_.teleports_per_second) refusal = "asked more than twice in a second";
        else switch (plugin.services_.teleport(plugin.id(), at, heading)) {
        case Teleport::sent: break;
        case Teleport::forbidden: refusal = "the host has turned teleporting off"; break;
        case Teleport::unavailable: refusal = "teleporting is unavailable now (no skater, or a load)"; break;
        }
        lua_pushboolean(L, refusal == nullptr);
        if (!refusal) return 1;
        lua_pushstring(L, refusal);
        return 2;
    }
    // world.raycast: where the line from `from` to `to` first meets the world's collision, or nil
    // when it is clear. Collision is loaded only near the skater: far off, a ray meets nothing.
    static int raycast(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "world.raycast", "reskate.world.raycast");
        const auto from = position_of(L, 1, "the ray's start");
        const auto to = position_of(L, 2, "the ray's end");
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > plugin.limits_.raycast_metres)
            luaL_error(L, "a ray is at most %d m long", static_cast<int>(plugin.limits_.raycast_metres));
        if (++plugin.raycasts_ > plugin.limits_.raycasts_per_tick)
            luaL_error(L, "more than %d raycasts in one tick", plugin.limits_.raycasts_per_tick);
        if (const auto hit = plugin.services_.raycast(from, to)) push_position(L, *hit);
        else lua_pushnil(L);
        return 1;
    }
    // events.tricks: the score HUD now (the line's score, its multiplier, a trick going on), or nil.
    static int score(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "events.tricks", "reskate.score.current");
        const auto now = plugin.services_.score();
        if (!now) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, now->line);
        lua_setfield(L, -2, "line");
        lua_pushnumber(L, now->multiplier);
        lua_setfield(L, -2, "multiplier");
        lua_pushboolean(L, now->in_trick);
        lua_setfield(L, -2, "in_trick");
        lua_pushlstring(L, now->trick.data(), now->trick.size());
        lua_setfield(L, -2, "trick");
        lua_pushlstring(L, now->detail.data(), now->detail.size());
        lua_setfield(L, -2, "detail");
        lua_pushboolean(L, now->manual);
        lua_setfield(L, -2, "manual");
        return 1;
    }
    // Modules come from the plugin's own package, beside its entry: require("./rules") or
    // require("rules") is <entry folder>/rules.luau or rules.lua. Each runs once.
    static int require(lua_State* L) {
        auto& plugin = plugin_of(L);
        std::string name = luaL_checkstring(L, 1);
        if (name.starts_with("./")) name.erase(0, 2);
        const auto relative = safe_relative_path(name);
        if (relative.empty()) luaL_error(L, "require(\"%s\"): a module is a path inside the plugin's package, with no ..", name.c_str());
        const auto in_root = [&](const std::string& path) { return plugin.code_root_.empty() ? path : plugin.code_root_ + "/" + path; };
        std::vector<std::string> candidates;
        if (relative.ends_with(".lua") || relative.ends_with(".luau")) candidates.push_back(in_root(relative));
        else {
            candidates.push_back(in_root(relative + ".luau"));
            candidates.push_back(in_root(relative + ".lua"));
        }
        std::string key;
        std::filesystem::path file;
        for (const auto& candidate : candidates) {
            const auto path = package_path(L, plugin, candidate, false);
            std::error_code code;
            if (std::filesystem::is_regular_file(path, code)) {
                key = candidate;
                file = path;
                break;
            }
        }
        if (key.empty()) luaL_error(L, "require(\"%s\"): there is no %s", name.c_str(), candidates.front().c_str());
        lua_getref(L, plugin.modules_);
        lua_getfield(L, -1, key.c_str());
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
        if (plugin.loading_modules_.contains(key)) luaL_error(L, "require(\"%s\"): it requires itself, through another module", name.c_str());
        plugin.loading_modules_.insert(key);
        struct Done {
            Plugin& plugin;
            std::string key;
            ~Done() { plugin.loading_modules_.erase(key); }
        } done{plugin, key};
        if (!plugin.load_chunk(L, key, file)) lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, key.c_str());
        return 1;
    }
};

Json lua_to_json(lua_State* L, int index) {
    Budget budget;
    return to_json(L, index, 0, budget);
}

void push_json(lua_State* L, const Json& value) { push_json_value(L, value, 0); }

void Api::install(Plugin& plugin, lua_State* L) {
    const auto readonly = [&] { lua_setreadonly(L, -1, true); };
    lua_createtable(L, 0, 16);
    lua_pushinteger(L, api_version);
    lua_setfield(L, -2, "api_version");
    lua_pushlstring(L, plugin.id().data(), plugin.id().size());
    lua_setfield(L, -2, "id");
    // "client" for the copy on a player's game, "server" for the session authority's copy.
    lua_pushstring(L, plugin.services_.authority() ? "server" : "client");
    lua_setfield(L, -2, "side");
    // Named constants, read-only and built from the C++ lists so they cannot drift:
    // reskate.Enums.Side.server is "server", reskate.Enums.Event.tick is "tick". Plain strings still work.
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 3);
    for (const auto side : {Side::client, Side::server, Side::both}) {
        lua_pushstring(L, side_name(side));
        lua_setfield(L, -2, side_name(side));
    }
    readonly();
    lua_setfield(L, -2, "Side");
    lua_createtable(L, 0, static_cast<int>(events.size()));
    for (const auto& event : events) {
        lua_pushlstring(L, event.name.data(), event.name.size());
        lua_setfield(L, -2, std::string(event.name).c_str());
    }
    readonly();
    lua_setfield(L, -2, "Event");
    readonly();
    lua_setfield(L, -2, "Enums");
    using F = ApiFunctions;
    add_functions(L, {{"log", F::log}, {"on", F::on}});
    add_functions(L, "time", {{"now", F::now}, {"after", F::after}, {"every", F::every}, {"cancel", F::cancel}});
    add_functions(L, "level", {{"current", F::level}});
    add_functions(L, "package", {{"read", F::read}, {"read_json", F::read_json}, {"list", F::list}, {"variants", F::variants}, {"choose", F::choose}});
    add_functions(L, "json", {{"encode", F::encode}, {"decode", F::decode}});
    add_functions(L, "console", {{"command", F::command}, {"print", F::console_print}});
    add_functions(L, "storage", {{"get", F::storage_get}, {"set", F::storage_set}, {"add", F::storage_add}, {"keys", F::storage_keys}});
    add_functions(L, "skater", {{"get", F::skater_get}, {"position", F::skater_position}, {"heading", F::skater_heading},
                                {"speed", F::skater_speed}, {"on_ground", F::skater_on_ground}, {"teleport", F::skater_teleport}});
    add_functions(L, "score", {{"current", F::score}});
    add_functions(L, "world", {{"raycast", F::raycast}});

    install_ui_api(plugin, L);
    install_more_api(plugin, L);
    install_world_api(plugin, L);
    install_services_api(plugin, L);
    install_kit_api(plugin, L);
    install_capture_api(plugin, L);
    readonly();
    lua_setglobal(L, "reskate");
    lua_pushcfunction(L, ApiFunctions::print, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, ApiFunctions::require, "require");
    lua_setglobal(L, "require");
}

void push_player(lua_State* L, const SessionPlayer& player) {
    lua_createtable(L, 0, 4);
    const auto id = std::to_string(player.id);
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "id");
    lua_pushlstring(L, player.name.data(), player.name.size());
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, player.me);
    lua_setfield(L, -2, "me");
    lua_pushboolean(L, player.host);
    lua_setfield(L, -2, "host");
    lua_pushboolean(L, player.server);
    lua_setfield(L, -2, "server");
    lua_getfield(L, LUA_REGISTRYINDEX, "reskate.player"); // its permissions (plugin_api_world.cpp)
    lua_setmetatable(L, -2);
}
}
