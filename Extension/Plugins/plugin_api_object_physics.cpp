#include "lualib.h"
#include "plugin_api.h"
#include "plugin_object_physics.h"
#include <charconv>

namespace dingosdk::plugins {
struct ObjectPhysicsApi {
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }
    static void ray(lua_State* L, Plugin& plugin) {
        if (++plugin.raycasts_ > plugin.limits_.raycasts_per_tick)
            luaL_error(L, "too many contact rays in one tick");
    }
};
namespace {
Plugin& allowed(lua_State* L, const char* permission, const char* api) {
    auto& plugin = plugin_of(L);
    need(L, plugin, permission, api);
    if (plugin.manifest().side != Side::client)
        luaL_error(L, "%s requires a client-only plugin", api);
    return plugin;
}
std::uint64_t id_at(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        luaL_argerror(L, 1, "an object's decimal string ID");
    const auto text = checked_text(L, 1, 20, "an object ID");
    std::uint64_t id{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !id)
        luaL_argerror(L, 1, "an object's decimal string ID");
    return id;
}
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty())
        return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
int physics(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.physics");
    const auto id = id_at(L);
    std::optional<ObjectPhysics> options;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        options.emplace();
        lua_getfield(L, 2, "dynamic");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            options->dynamic = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "surface");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            options->surface = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "mass");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TNUMBER)
                luaL_error(L, "mass must be a number");
            const auto mass = lua_tonumber(L, -1);
            if (!(mass >= .1 && mass <= 20000))
                luaL_error(L, "mass must be finite, 0.1 to 20000 kg");
            options->mass = static_cast<float>(mass);
        }
        lua_pop(L, 1);
    }
    return result(L, ObjectPhysicsApi::game(plugin).object_physics(plugin.id(), id, options));
}
int velocity(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.velocity");
    const auto id = id_at(L);
    const auto v = position_of(L, 2, "velocity");
    if (!valid_object_velocity(v))
        luaL_argerror(L, 2, "velocity must have a finite speed at most 45 m/s");
    return result(L, ObjectPhysicsApi::game(plugin).object_velocity(plugin.id(), id, v));
}
int motion(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.motion");
    const auto value = ObjectPhysicsApi::game(plugin).object_motion(plugin.id(), id_at(L));
    if (!value) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    push_position(L, value->position);
    lua_setfield(L, -2, "position");
    push_position(L, value->velocity);
    lua_setfield(L, -2, "velocity");
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushnumber(L, value->rotation[static_cast<std::size_t>(i)]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "rotation");
    lua_pushboolean(L, value->dynamic);
    lua_setfield(L, -2, "dynamic");
    lua_pushboolean(L, value->editing);
    lua_setfield(L, -2, "editing");
    lua_pushboolean(L, value->surface);
    lua_setfield(L, -2, "surface");
    lua_pushnumber(L, value->authored_mass);
    lua_setfield(L, -2, "authored_mass");
    lua_pushnumber(L, value->mass);
    lua_setfield(L, -2, "mass");
    const auto revision = std::to_string(value->edit_revision);
    lua_pushlstring(L, revision.data(), revision.size());
    lua_setfield(L, -2, "edit_revision");
    const auto generation = std::to_string(value->generation);
    lua_pushlstring(L, generation.data(), generation.size());
    lua_setfield(L, -2, "generation");
    return 1;
}
int pose(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.pose");
    const auto id = id_at(L);
    const auto position = position_of(L, 2, "position");
    luaL_checktype(L, 3, LUA_TTABLE);
    std::array<float, 4> rotation{};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, 3, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER)
            luaL_error(L, "rotation needs four numbers (x, y, z, w)");
        const auto value = lua_tonumber(L, -1);
        if (!std::isfinite(value) || std::abs(value) > 1.01)
            luaL_error(L, "rotation must be a unit quaternion");
        rotation[static_cast<std::size_t>(i)] = static_cast<float>(value);
        lua_pop(L, 1);
    }
    if (!valid_object_rotation(rotation))
        luaL_error(L, "rotation must be a unit quaternion");
    return result(L, ObjectPhysicsApi::game(plugin).object_pose(plugin.id(), id, position, rotation));
}
int marker_events(lua_State* L) {
    auto& plugin = allowed(L, "marker.read", "reskate.marker.events");
    const auto value = ObjectPhysicsApi::game(plugin).marker_events(plugin.id());
    if (!value) {
        lua_pushnil(L);
        lua_pushliteral(L, "unavailable here");
        return 2;
    }
    lua_createtable(L, static_cast<int>(value->events.size()), 0);
    int i = 0;
    for (const auto& event : value->events) {
        lua_pushlstring(L, event.data(), event.size());
        lua_rawseti(L, -2, ++i);
    }
    lua_pushboolean(L, value->overflow);
    return 2;
}
int contact(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.contact");
    const auto from = position_of(L, 1, "ray start"), to = position_of(L, 2, "ray end");
    float length2{};
    for (unsigned i = 0; i < 3; ++i)
        length2 += (to[i] - from[i]) * (to[i] - from[i]);
    if (length2 > 100)
        luaL_error(L, "a contact ray is at most 10 m");
    ObjectPhysicsApi::ray(L, plugin);
    const auto hit = ObjectPhysicsApi::game(plugin).object_contact(plugin.id(), from, to);
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 2);
    const auto id = std::to_string(hit->id);
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "id");
    push_position(L, hit->position);
    lua_setfield(L, -2, "position");
    return 1;
}
int actor(lua_State* L) {
    auto& plugin = allowed(L, "objects.physics", "reskate.objects.actor");
    need(L, plugin, "skater.read", "reskate.objects.actor");
    need(L, plugin, "input", "reskate.objects.actor");
    const auto value = ObjectPhysicsApi::game(plugin).physics_actor();
    if (!value) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    push_position(L, value->velocity);
    lua_setfield(L, -2, "velocity");
    push_position(L, value->walking_velocity);
    lua_setfield(L, -2, "walking_velocity");
    push_position(L, value->wish);
    lua_setfield(L, -2, "wish");
    lua_pushboolean(L, value->grounded);
    lua_setfield(L, -2, "grounded");
    lua_pushboolean(L, value->on_foot);
    lua_setfield(L, -2, "on_foot");
    lua_pushboolean(L, value->active);
    lua_setfield(L, -2, "active");
    lua_pushnumber(L, value->device);
    lua_setfield(L, -2, "device");
    lua_pushnumber(L, value->physics_state);
    lua_setfield(L, -2, "physics_state");
    return 1;
}
} // namespace
void install_object_physics_api(Plugin&, lua_State* L) {
    add_functions(L, "objects", {{"physics", physics}, {"velocity", velocity}, {"motion", motion}, {"pose", pose}});
    add_functions(L, "objects", {{"contact", contact}, {"actor", actor}});
    add_functions(L, "marker", {{"events", marker_events}});
}
} // namespace dingosdk::plugins
