#pragma once
#include "Engine/Game/Input/input_lease.h"
#include "lua.h"
#include "lualib.h"
#include <cmath>
#include <string_view>

// Parse a complete normalized Pad before entering the native publication gate.
// Missing controls are neutral; malformed or misspelled controls never publish.
namespace dingosdk::plugins::pad_fields {
template<std::size_t N>
inline void fields(lua_State* L, int at, const char* const (&names)[N]) {
    at = lua_absindex(L, at);
    luaL_checktype(L, at, LUA_TTABLE);
    if (lua_getmetatable(L, at)) luaL_error(L, "input write Pad tables must be plain");
    lua_pushnil(L);
    while (lua_next(L, at)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "input write Pad keys must be strings");
        std::size_t length{};
        const auto* text = lua_tolstring(L, -2, &length);
        const std::string_view key(text, length);
        bool found = false;
        for (const auto* name : names) if (key == name) { found = true; break; }
        if (!found) luaL_error(L, "unknown input write Pad field");
        lua_pop(L, 1);
    }
}
inline double scalar(lua_State* L, int at, const char* name, double low) {
    lua_rawgetfield(L, at, name);
    double value = 0;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "input write %s must be a number", name);
        value = lua_tonumber(L, -1);
        if (!std::isfinite(value) || value < low || value > 1)
            luaL_error(L, "input write %s is outside its finite range", name);
    }
    lua_pop(L, 1);
    return value;
}
inline input_lease::Pad normalized(lua_State* L, int at) {
    at = lua_absindex(L, at);
    static constexpr const char* names[]{"left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger", "buttons"};
    fields(L, at, names);
    const auto axis = [&](const char* name) {
        const double value = scalar(L, at, name, -1);
        return static_cast<std::int16_t>(std::round(value * (value < 0 ? 32768.0 : 32767.0)));
    };
    const auto trigger = [&](const char* name) {
        return static_cast<std::uint8_t>(std::round(scalar(L, at, name, 0) * 255.0));
    };
    input_lease::Pad pad{};
    pad.left_x = axis("left_x"); pad.left_y = axis("left_y");
    pad.right_x = axis("right_x"); pad.right_y = axis("right_y");
    pad.left_trigger = trigger("left_trigger"); pad.right_trigger = trigger("right_trigger");
    lua_rawgetfield(L, at, "buttons");
    if (!lua_isnil(L, -1)) {
        static constexpr const char* buttons[]{"up", "down", "left", "right", "start", "back", "left_stick", "right_stick", "lb", "rb", "a", "b", "x", "y"};
        static constexpr std::uint16_t bits[]{1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 4096, 8192, 16384, 32768};
        const int button_at = lua_absindex(L, -1);
        fields(L, button_at, buttons);
        for (std::size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
            lua_rawgetfield(L, button_at, buttons[i]);
            if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN)
                luaL_error(L, "input write buttons.%s must be boolean", buttons[i]);
            if (lua_toboolean(L, -1)) pad.buttons = static_cast<std::uint16_t>(pad.buttons | bits[i]);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return pad;
}
}
