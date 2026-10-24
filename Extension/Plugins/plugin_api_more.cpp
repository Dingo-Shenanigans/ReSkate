#include "plugin_api.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>

// Phase 3's parts of the reskate API (docs: ReSkate Plugin System Design, "the wider set"). As in
// plugin_api.cpp, every value is checked before it leaves the VM.
namespace dingosdk::plugins {
namespace {
// A name another plugin calls or listens for: letters, digits and _ . -, 1 to 48 of them.
std::string identifier(lua_State* L, int index, const char* what) {
    auto name = checked_text(L, index, 48, what);
    const bool valid = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
    if (!valid) luaL_error(L, "%s must be 1 to 48 letters, digits, _ . or -", what);
    return name;
}
}

struct MoreApi {
    // ---- libraries: functions one plugin offers others --------------------------------------
    // expose({name = function, ...}): what other plugins get from require_plugin. Replaces the last.
    static int expose(lua_State* L) {
        auto& plugin = plugin_of(L);
        luaL_checktype(L, 1, LUA_TTABLE);
        std::map<std::string, int, std::less<>> next;
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TFUNCTION) {
                for (const auto& item : next) lua_unref(L, item.second);
                luaL_error(L, "expose takes a table of names and functions");
            }
            const std::string name = lua_tostring(L, -2);
            if (next.size() >= plugin.limits_.exposed || name.size() > 48) {
                for (const auto& item : next) lua_unref(L, item.second);
                luaL_error(L, "at most %d functions, each name 48 characters", static_cast<int>(plugin.limits_.exposed));
            }
            lua_pushvalue(L, -1);
            next[name] = lua_ref(L, -1);
            lua_pop(L, 2);
        }
        for (const auto& item : plugin.exposed_) lua_unref(L, item.second);
        plugin.exposed_ = std::move(next);
        return 0;
    }
    // A proxy's call: its upvalues are the plugin and the function's name. Arguments and the result
    // are copied as JSON: tables, strings, numbers and booleans cross; functions never do.
    static int proxy(lua_State* L) {
        auto& plugin = plugin_of(L);
        const std::string target = lua_tostring(L, lua_upvalueindex(1));
        const std::string function = lua_tostring(L, lua_upvalueindex(2));
        auto arguments = Json::array();
        const int count = lua_gettop(L);
        for (int i = 1; i <= count; ++i) arguments.push_back(lua_to_json(L, i));
        Json result;
        const auto error = plugin.services_.call_plugin(plugin.id(), target, function, arguments, result);
        if (!error.empty()) luaL_error(L, "%s.%s: %s", target.c_str(), function.c_str(), error.c_str());
        push_json(L, result);
        return 1;
    }
    // require_plugin(name): a table of the functions that plugin exposes, or nil and why.
    static int require_plugin(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto target = identifier(L, 1, "a plugin's name");
        std::vector<std::string> names;
        if (const auto error = plugin.services_.exposed(target, names); !error.empty()) {
            lua_pushnil(L);
            lua_pushlstring(L, error.data(), error.size());
            return 2;
        }
        lua_createtable(L, 0, static_cast<int>(names.size()));
        for (const auto& name : names) {
            lua_pushlstring(L, target.data(), target.size());
            lua_pushlstring(L, name.data(), name.size());
            lua_pushcclosurek(L, proxy, "proxy", 2, nullptr);
            lua_setfield(L, -2, name.c_str());
        }
        lua_setreadonly(L, -1, true);
        return 1;
    }

    // ---- events between plugins ----------------------------------------------------------------
    // emit(event, data?): to every other plugin with an on_plugin handler for it, at the next tick.
    static int emit(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto event = identifier(L, 1, "an event's name");
        Json data = lua_isnoneornil(L, 2) ? Json{} : lua_to_json(L, 2);
        if (data.dump().size() > plugin.limits_.event_bytes)
            luaL_error(L, "an event's data is at most %d KB as JSON", static_cast<int>(plugin.limits_.event_bytes >> 10));
        if (plugin.tick_number_ != plugin.emit_tick_) {
            plugin.emit_tick_ = plugin.tick_number_;
            plugin.emitted_ = 0;
        }
        if (++plugin.emitted_ > plugin.limits_.events_per_tick)
            luaL_error(L, "more than %d events in one tick", static_cast<int>(plugin.limits_.events_per_tick));
        lua_pushboolean(L, plugin.services_.emit(plugin.id(), event, std::move(data)));
        return 1;
    }
    // on_plugin(source, event, fn(source, data)): source is a plugin's name, or "*" for any.
    static int on_plugin(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto source = lua_isstring(L, 1) && std::string_view(lua_tostring(L, 1)) == "*" ? std::string("*")
                                                                                                 : identifier(L, 1, "a plugin's name");
        const auto event = identifier(L, 2, "an event's name");
        luaL_checktype(L, 3, LUA_TFUNCTION);
        if (plugin.listeners_.size() >= plugin.limits_.listeners)
            luaL_error(L, "more than %d on_plugin handlers", static_cast<int>(plugin.limits_.listeners));
        lua_pushvalue(L, 3);
        plugin.listeners_.push_back({source, event, lua_ref(L, -1)});
        lua_pop(L, 1);
        return 0;
    }

    // ---- net.http ------------------------------------------------------------------------------
    // request({url = "https://host/path", method = "GET" | "POST", body = "...", content_type = "..."},
    // function(status, body, error)): to a host its manifest's "domains" lists, HTTPS only.
    static int http_request(lua_State* L) {
        auto& plugin = plugin_of(L);
        if (!plugin.has("net.http")) luaL_error(L, "reskate.http.request needs the \"net.http\" permission in reskate-plugin.json");
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_getfield(L, 1, "url");
        const auto url = checked_text(L, -1, plugin.limits_.http_url_bytes, "a web request's url");
        lua_pop(L, 1);
        lua_getfield(L, 1, "method");
        const std::string method = lua_isnil(L, -1) ? "GET" : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        if (method != "GET" && method != "POST") luaL_error(L, "a web request's method is \"GET\" or \"POST\"");
        lua_getfield(L, 1, "body");
        const std::string body = lua_isnil(L, -1) ? std::string{} : checked_text(L, -1, plugin.limits_.http_body_bytes, "a web request's body");
        lua_pop(L, 1);
        lua_getfield(L, 1, "content_type");
        const std::string type = lua_isnil(L, -1) ? std::string{} : checked_text(L, -1, 100, "a content type");
        lua_pop(L, 1);
        for (const char c : type)
            if (c < 0x20 || c > 0x7e) luaL_error(L, "a content type is printable text");
        // https://<host>[:443]<path>: nothing else, the host one the manifest lists.
        constexpr std::string_view scheme = "https://";
        if (!url.starts_with(scheme)) luaL_error(L, "a web request is HTTPS: its url starts with https://");
        const auto rest = std::string_view(url).substr(scheme.size());
        const auto slash = rest.find('/');
        std::string host(rest.substr(0, slash));
        std::string path = slash == std::string_view::npos ? std::string("/") : std::string(rest.substr(slash));
        if (host.ends_with(":443")) host.resize(host.size() - 4);
        for (auto& c : host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        for (const char c : url)
            if (c <= 0x20 || c > 0x7e) luaL_error(L, "a url is printable text with no spaces");
        if (host.find_first_of("@:[]") != std::string::npos) luaL_error(L, "a url's host is a name from \"domains\", with no user, password or other port");
        const auto& domains = plugin.manifest().domains;
        if (std::find(domains.begin(), domains.end(), host) == domains.end())
            luaL_error(L, "%s is not in reskate-plugin.json's \"domains\"", host.c_str());
        const double now = plugin.services_.now_ms();
        std::erase_if(plugin.http_sent_, [&](double at) { return now - at >= 60000; });
        if (static_cast<int>(plugin.http_sent_.size()) >= plugin.limits_.http_per_minute)
            luaL_error(L, "more than %d web requests in a minute", plugin.limits_.http_per_minute);
        if (static_cast<int>(plugin.http_pending_.size()) >= plugin.limits_.http_in_flight)
            luaL_error(L, "more than %d web requests waiting for an answer", plugin.limits_.http_in_flight);
        const auto id = plugin.next_http_++;
        if (!plugin.services_.http({id, plugin.id(), method, host, path, body, type})) {
            lua_pushnil(L);
            lua_pushstring(L, "web requests cannot be made now");
            return 2;
        }
        plugin.http_sent_.push_back(now);
        lua_pushvalue(L, 2);
        plugin.http_pending_[id] = lua_ref(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, static_cast<int>(id));
        return 1;
    }

    static void install(Plugin& plugin, lua_State* L);
};

void install_more_api(Plugin& plugin, lua_State* L) { MoreApi::install(plugin, L); }

void MoreApi::install(Plugin& plugin, lua_State* L) {
    (void)plugin;
    // The reskate table is on top of the stack.
    using F = MoreApi;
    add_functions(L, {{"expose", F::expose}, {"require_plugin", F::require_plugin}, {"emit", F::emit}, {"on_plugin", F::on_plugin}});
    add_functions(L, "http", {{"request", F::http_request}});
}
}
