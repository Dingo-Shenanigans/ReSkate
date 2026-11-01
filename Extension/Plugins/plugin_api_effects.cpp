#include "plugin_api.h"
#include "plugin_effects.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::plugins {
namespace {
EffectMatrix matrix(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    if (lua_objlen(L, index) != 16) luaL_error(L, "a transform has 16 numbers");
    EffectMatrix out{};
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, index, i + 1);
        out[i] = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
    }
    if (!valid_effect_matrix(out)) luaL_error(L, "an effect needs a finite, orthogonal transform with axes between .001 and 20 metres");
    return out;
}
}
struct EffectsApi {
    static Plugin& caller(lua_State* L) {
        auto& p = plugin_of(L);
        need(L, p, "effects", "reskate.effects");
        if (++p.effect_calls_ > 8192) luaL_error(L, "at most 8192 effect operations in a tick");
        return p;
    }
    static std::uint64_t handle(lua_State* L, Plugin& p) {
        const double id = luaL_checknumber(L, 1);
        if (!(id >= 1 && id <= 9007199254740991.0) || std::floor(id) != id)
            luaL_error(L, "an effect handle is a positive integer");
        const auto value = static_cast<std::uint64_t>(id);
        if (!p.effect_handles_.contains(value)) luaL_error(L, "that effect does not belong to this plugin");
        return value;
    }
    static int create(lua_State* L, bool decal) {
        auto& p = caller(L);
        EffectRequest request;
        request.asset = checked_text(L, 1, 240, "effect asset");
        if (!private_effect_asset(request.asset, decal) || !p.manifest_.effects.contains(request.asset))
            luaL_error(L, "effects must name a declared private mod asset; stock and store effects are refused");
        request.transform = matrix(L, 2);
        request.decal = decal;
        if (decal && !lua_isnoneornil(L, 3)) {
            luaL_checktype(L, 3, LUA_TTABLE);
            lua_getfield(L, 3, "attach"); request.attach = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            lua_getfield(L, 3, "lift");
            if (!lua_isnil(L, -1)) request.lift = static_cast<float>(luaL_checknumber(L, -1));
            lua_pop(L, 1);
            if (!(request.lift >= 0 && request.lift <= 1)) luaL_error(L, "projection lift is 0 to 1 metre");
        }
        const auto full = [&] {
            return std::count_if(p.effect_handles_.begin(), p.effect_handles_.end(),
                [decal](const auto& h) { return h.second == decal; }) >= (decal ? 2048 : 24);
        };
        // Console/event callbacks can run before the next VM tick.
        if (full()) {
            p.prune_effects();
            if (full()) { lua_pushnil(L); lua_pushliteral(L, "effect limit reached"); return 2; }
        }
        const auto id = p.services_.game().effect_create(p.id_, request);
        if (!id) { lua_pushnil(L); lua_pushliteral(L, "effect asset or renderer unavailable"); return 2; }
        p.effect_handles_.emplace(id, decal);
        lua_pushnumber(L, static_cast<double>(id));
        return 1;
    }
    static int particle(lua_State* L) { return create(L, false); }
    static int decal(lua_State* L) { return create(L, true); }
    static int move(lua_State* L) {
        auto& p = caller(L); const auto id = handle(L, p);
        const auto transform = matrix(L, 2);
        p.services_.game().effect_move(p.id_, id, transform); return 0;
    }
    static int stop(lua_State* L) {
        auto& p = caller(L); const auto id = handle(L, p);
        p.services_.game().effect_stop(p.id_, id); return 0;
    }
    static int opacity(lua_State* L) {
        auto& p = caller(L); const auto id = handle(L, p);
        const auto alpha = luaL_checknumber(L, 2);
        if (!(alpha >= 0 && alpha <= 1)) luaL_error(L, "opacity is 0 to 1");
        p.services_.game().effect_opacity(p.id_, id, static_cast<float>(alpha)); return 0;
    }
    static int release(lua_State* L) {
        auto& p = caller(L); const auto id = handle(L, p);
        p.services_.game().effect_release(p.id_, id); p.effect_handles_.erase(id); return 0;
    }
    static int state(lua_State* L) {
        auto& p = caller(L);
        const auto [ready, epoch] = p.services_.game().effects_state();
        lua_pushboolean(L, ready); lua_pushnumber(L, static_cast<double>(epoch)); return 2;
    }
    static int clear(lua_State* L) {
        auto& p = caller(L);
        p.services_.game().effects_clear(p.id_); p.effect_handles_.clear(); return 0;
    }
};
void install_effects_api(Plugin&, lua_State* L) {
    add_functions(L, "effects", {{"particle", EffectsApi::particle}, {"decal", EffectsApi::decal},
        {"move", EffectsApi::move}, {"stop", EffectsApi::stop}, {"opacity", EffectsApi::opacity},
        {"release", EffectsApi::release}, {"clear", EffectsApi::clear}, {"state", EffectsApi::state}});
}
}
