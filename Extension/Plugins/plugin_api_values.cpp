#include "plugin_api.h"
#include "plugin_ui_values.h"
#include "lualib.h"
#include <cmath>

namespace dingosdk::plugins {
struct ValuesApi {
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }
};
namespace {
int ui_symbol(lua_State* L) {
    auto& plugin = plugin_of(L);
    need(L, plugin, "ui.models", "reskate.ui.symbol");
    const auto name = checked_text(L, 1, 48, "a UI value name");
    if (!ui_values::valid_name(name)) luaL_error(L, "a UI value name needs 1 to 48 letters, digits, _ or -");
    lua_pushnumber(L, ui_values::symbol(plugin.id(), name));
    return 1;
}
int ui_values_publish(lua_State* L) {
    auto& plugin = plugin_of(L);
    need(L, plugin, "ui.models", "reskate.ui.values");
    luaL_checktype(L, 1, LUA_TTABLE);
    ui_values::Values values;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TNUMBER)
            luaL_error(L, "UI values must be a table of names and numbers");
        std::size_t length{};
        const char* text = lua_tolstring(L, -2, &length);
        const std::string_view name(text, length);
        const double value = lua_tonumber(L, -1);
        if (!ui_values::valid_name(name)) luaL_error(L, "a UI value name needs 1 to 48 letters, digits, _ or -");
        if (!std::isfinite(value) || std::fabs(value) > ui_values::maximum_magnitude)
            luaL_error(L, "UI values must be finite numbers between -100000 and 100000");
        if (values.size() >= ui_values::maximum_values) luaL_error(L, "at most 32 UI values per plugin");
        values.emplace(name, static_cast<float>(value));
        lua_pop(L, 1);
    }
    const auto error = ui_values::distinct_symbols(plugin.id(), values)
        ? ValuesApi::game(plugin).publish_ui_values(plugin.id(), values)
        : std::string("UI symbol collision or reserved symbol");
    lua_pushboolean(L, error.empty());
    if (error.empty()) return 1;
    lua_pushlstring(L, error.data(), error.size());
    return 2;
}
}
void install_values_api(Plugin&, lua_State* L) {
    add_functions(L, "ui", {{"symbol", ui_symbol}, {"values", ui_values_publish}});
}
}
