#include "plugin_api.h"
#include "plugin_game.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>

// Vehicle handles grant only a held pose. Their native identity never reaches Lua.
namespace dingosdk::plugins {
namespace {
constexpr char handle_type[] = "reskate.traffic_vehicle";
struct VehicleValue { std::uint64_t owner{}, handle{}; };
int answer(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
const VehicleValue& checked(lua_State* L, Plugin& plugin) {
    const auto& value = *static_cast<const VehicleValue*>(luaL_checkudata(L, 1, handle_type));
    if (value.owner != plugin.instance()) luaL_error(L, "vehicle belongs to another plugin instance");
    return value;
}
traffic_vehicles::Quaternion quaternion(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    traffic_vehicles::Quaternion q{};
    constexpr const char* names[]{"x", "y", "z", "w"};
    for (int i = 0; i < 4; ++i) {
        lua_getfield(L, index, names[i]);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, index, i + 1); }
        const double value = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        if (!std::isfinite(value) || std::abs(value) > 1e10) luaL_error(L, "rotation must have four finite numbers");
        q[i] = static_cast<float>(value);
    }
    if (!traffic_vehicles::normalize(q)) luaL_error(L, "rotation must have a nonzero length");
    return q;
}
void push_rotation(lua_State* L, const traffic_vehicles::Quaternion& q) {
    lua_createtable(L, 4, 4);
    constexpr const char* names[]{"x", "y", "z", "w"};
    for (int i = 0; i < 4; ++i) {
        lua_pushnumber(L, q[i]); lua_rawseti(L, -2, i + 1);
        lua_pushnumber(L, q[i]); lua_setfield(L, -2, names[i]);
    }
}
}
struct VehiclesApi {
    static int take(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "world.vehicles", "reskate.world.vehicles.take");
        const auto from = position_of(L, 1, "the ray's start");
        const auto to = position_of(L, 2, "the ray's end");
        float squared{};
        for (int i = 0; i < 3; ++i) squared += (to[i] - from[i]) * (to[i] - from[i]);
        if (std::sqrt(squared) > plugin.limits_.raycast_metres)
            luaL_error(L, "a ray is at most %d m long", static_cast<int>(plugin.limits_.raycast_metres));
        if (++plugin.raycasts_ > plugin.limits_.raycasts_per_tick)
            luaL_error(L, "more than %d raycasts in one tick", plugin.limits_.raycasts_per_tick);
        // The same pick as world.pick, counted the same way; its hit names the car's shape owner.
        const auto pick = plugin.services_.game().world_pick(from, to);
        std::string why;
        std::uint64_t handle{};
        if (!pick) why = "nothing hit";
        else if (!pick->vehicle_owner) why = "not a vehicle";
        else why = plugin.services_.game().vehicle_take(plugin.instance(), pick->vehicle_owner, pick->vehicle_root, handle);
        if (!why.empty() || !handle) {
            if (why.empty()) why = "unavailable";
            lua_pushnil(L); lua_pushlstring(L, why.data(), why.size()); return 2;
        }
        new (lua_newuserdata(L, sizeof(VehicleValue))) VehicleValue{plugin.instance(), handle};
        luaL_getmetatable(L, handle_type); lua_setmetatable(L, -2);
        return 1;
    }
    static int state(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto& value = checked(L, plugin);
        const auto sample = plugin.services_.game().vehicle_read(value.owner, value.handle);
        const char* name = "lost";
        switch (sample.state) {
        case traffic_vehicles::State::finding: name = "finding"; break;
        case traffic_vehicles::State::held: name = "held"; break;
        case traffic_vehicles::State::lost: break;
        case traffic_vehicles::State::released: name = "released"; break;
        }
        lua_pushstring(L, name);
        if (sample.reason.empty()) return 1;
        lua_pushlstring(L, sample.reason.data(), sample.reason.size());
        return 2;
    }
    static int pose(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto& value = checked(L, plugin);
        const auto sample = plugin.services_.game().vehicle_read(value.owner, value.handle);
        if (sample.state != traffic_vehicles::State::held) { lua_pushnil(L); return 1; }
        push_position(L, sample.pose.position);
        push_rotation(L, sample.pose.rotation);
        return 2;
    }
    static int set_pose(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto& value = checked(L, plugin);
        traffic_vehicles::Pose pose{position_of(L, 2, "the vehicle's position"), quaternion(L, 3)};
        return answer(L, plugin.services_.game().vehicle_set_pose(value.owner, value.handle, pose));
    }
    static int release(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto& value = checked(L, plugin);
        lua_pushboolean(L, plugin.services_.game().vehicle_release(value.owner, value.handle));
        return 1;
    }
};
void install_vehicles_api(Plugin&, lua_State* L) {
    luaL_newmetatable(L, handle_type);
    lua_pushstring(L, "locked"); lua_setfield(L, -2, "__metatable");
    push_functions(L, {{"state", VehiclesApi::state}, {"pose", VehiclesApi::pose},
                       {"set_pose", VehiclesApi::set_pose}, {"release", VehiclesApi::release}});
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, true); lua_pop(L, 1);
    lua_getfield(L, -1, "world");
    add_functions(L, "vehicles", {{"take", VehiclesApi::take}});
    lua_pop(L, 1);
}
}
