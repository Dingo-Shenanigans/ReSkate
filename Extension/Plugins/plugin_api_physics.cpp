#include "plugin_api.h"
#include "lua.h"
#include "lualib.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <new>

namespace dingosdk::plugins {
namespace {
using namespace owned_body;
constexpr char handle_type[] = "reskate.world.physics.body";
constexpr char epoch_type[] = "reskate.world.physics.epoch";
struct BodyValue { std::uint64_t owner{}; Handle handle{}; };
struct EpochValue { std::uint64_t owner{}; Handle handle{}; Sample sample{}; };
constexpr char hit_owner_type[] = "reskate.world.physics.hit_owner";
// Equality only: two hit shapes with the same read-time owner slot, inside one
// plugin instance, SDK world generation and measured epoch serial. It names no
// owner, index, kind or generation, and grants nothing.
struct HitOwnerValue { std::uint64_t owner{}, world{}, serial{}; std::uint32_t slot{}; };
int hit_owner_equal(lua_State* L) {
    const auto& a = *static_cast<const HitOwnerValue*>(luaL_checkudata(L, 1, hit_owner_type));
    const auto& b = *static_cast<const HitOwnerValue*>(luaL_checkudata(L, 2, hit_owner_type));
    lua_pushboolean(L, a.owner == b.owner && a.world == b.world && a.serial == b.serial && a.slot == b.slot);
    return 1;
}
int fail(lua_State* L, const std::string& why) { lua_pushnil(L); lua_pushlstring(L, why.data(), why.size()); return 2; }
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
void fields(lua_State* L, int at, std::initializer_list<const char*> names) {
    luaL_checktype(L, at, LUA_TTABLE);
    at = lua_absindex(L, at);
    lua_pushnil(L);
    while (lua_next(L, at)) {
        std::size_t size{};
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &size) : nullptr;
        bool found{};
        if (key) for (const char* expected : names) found = found || std::string_view(key, size) == expected;
        if (!found) luaL_error(L, "unknown native body field");
        lua_pop(L, 1);
    }
}
void array_fields(lua_State* L, int at, std::size_t count) {
    at = lua_absindex(L, at);
    lua_pushnil(L);
    while (lua_next(L, at)) {
        if (lua_type(L, -2) != LUA_TNUMBER) luaL_error(L, "ray batch lists have only sequential integer keys");
        const double key = lua_tonumber(L, -2);
        if (!std::isfinite(key) || key < 1 || key > static_cast<double>(count) || std::floor(key) != key)
            luaL_error(L, "ray batch lists have only sequential integer keys");
        lua_pop(L, 1);
    }
}
Vec vec_field(lua_State* L, int at, const char* name, bool optional = false) {
    lua_getfield(L, at, name);
    Vec v{};
    if (!optional || !lua_isnil(L, -1)) v = position_of(L, -1, name);
    lua_pop(L, 1);
    return v;
}
float number_field(lua_State* L, int at, const char* name) {
    lua_getfield(L, at, name);
    const double v = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(v) || std::abs(v) > 1000000) luaL_error(L, "native body %s must be finite and bounded", name);
    return static_cast<float>(v);
}
BodyValue body(lua_State* L, Plugin& p, int at) {
    const auto& value = *static_cast<const BodyValue*>(luaL_checkudata(L, at, handle_type));
    if (value.owner != p.instance()) luaL_error(L, "native body belongs to another plugin instance");
    return value;
}
void metatable(lua_State* L, const char* name) {
    luaL_newmetatable(L, name);
    lua_pushstring(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}
template<class T> void opaque(lua_State* L, const T& value, const char* name) {
    new (lua_newuserdata(L, sizeof(T))) T(value);
    luaL_getmetatable(L, name);
    lua_setmetatable(L, -2);
}
void text_number(lua_State* L, std::uint64_t value) { const auto text = std::to_string(value); lua_pushlstring(L, text.data(), text.size()); }
std::uint64_t sequence(lua_State* L, int at) {
    luaL_checktype(L, at, LUA_TSTRING);
    std::size_t size{};
    const char* text = luaL_checklstring(L, at, &size);
    std::uint64_t value{};
    const auto parsed = std::from_chars(text, text + size, value);
    if (!size || text[0] == '0' || parsed.ec != std::errc{} || parsed.ptr != text + size || !value) luaL_argerror(L, at, "a receipt's sequence string");
    return value;
}
void push_sample(lua_State* L, const BodyValue& b, const Sample& sample) {
    lua_createtable(L, 0, 9);
    opaque(L, EpochValue{b.owner, b.handle, sample}, epoch_type);
    lua_setfield(L, -2, "epoch");
    text_number(L, sample.epoch.serial);
    lua_setfield(L, -2, "serial");
    lua_pushnumber(L, sample.epoch.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, sample.epoch.seconds);
    lua_setfield(L, -2, "seconds");
    push_position(L, sample.position);
    lua_setfield(L, -2, "position");
    push_position(L, sample.velocity);
    lua_setfield(L, -2, "velocity");
    push_position(L, sample.angular_velocity);
    lua_setfield(L, -2, "angular_velocity");
    push_position(L, sample.inertia);
    lua_setfield(L, -2, "inertia");
    lua_pushnumber(L, sample.mass);
    lua_setfield(L, -2, "mass");
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) { lua_pushnumber(L, sample.rotation[static_cast<std::size_t>(i)]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "rotation");
}
}

struct PhysicsApi {
    static Plugin& plugin(lua_State* L) {
        auto& p = plugin_of(L);
        need(L, p, "world.physics", "reskate.world.physics");
        if (p.services_.authority()) luaL_error(L, "native bodies are client phase only");
        return p;
    }
    static int prepare(lua_State* L) {
        auto& p = plugin(L);
        if (lua_gettop(L) != 0) luaL_error(L, "prepare takes no arguments");
        if (p.body_prepares_ >= 1) return result(L, "asked to prepare native bodies more than once in one tick");
        ++p.body_prepares_;
        return result(L, p.services_.game().body_prepare());
    }
    static int create(lua_State* L) {
        auto& p = plugin(L);
        fields(L, 1, {"capsules", "mass", "inertia", "position", "rotation"});
        Definition d;
        d.mass = number_field(L, 1, "mass");
        d.inertia = vec_field(L, 1, "inertia");
        d.position = vec_field(L, 1, "position");
        lua_getfield(L, 1, "rotation");
        luaL_checktype(L, -1, LUA_TTABLE);
        if (lua_objlen(L, -1) != 4) luaL_error(L, "a body rotation has four XYZW values");
        for (int i = 0; i < 4; ++i) { lua_rawgeti(L, -1, i + 1); d.rotation[static_cast<std::size_t>(i)] = static_cast<float>(luaL_checknumber(L, -1)); lua_pop(L, 1); }
        lua_pop(L, 1);
        lua_getfield(L, 1, "capsules");
        luaL_checktype(L, -1, LUA_TTABLE);
        d.count = lua_objlen(L, -1);
        if (!d.count || d.count > maximum_capsules) luaL_error(L, "a body has 1 to 8 capsules");
        for (std::size_t i = 0; i < d.count; ++i) {
            lua_rawgeti(L, -1, static_cast<int>(i + 1));
            fields(L, -1, {"a", "b", "radius"});
            const auto at = lua_absindex(L, -1);
            d.capsules[i] = {vec_field(L, at, "a"), vec_field(L, at, "b"), number_field(L, at, "radius")};
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!valid(d)) luaL_error(L, "invalid native body: mass 1..1000, inertia .01..10000, unit XYZW rotation, capsule endpoints +/-10m and radius .01..2m");
        Handle h;
        const auto why = p.services_.game().body_create(p.instance(), d, h);
        if (!why.empty()) return fail(L, why);
        if (!h.generation || h.slot >= retained_capacity) return fail(L, "native body did not return an owned generation");
        opaque(L, BodyValue{p.instance(), h}, handle_type);
        return 1;
    }
    static int read(lua_State* L) {
        auto& p = plugin(L);
        const auto b = body(L, p, 1);
        Sample sample;
        const auto why = p.services_.game().body_read(p.instance(), b.handle, sample);
        if (!why.empty()) return fail(L, why);
        push_sample(L, b, sample);
        return 1;
    }
    static int submit(lua_State* L) {
        auto& p = plugin(L);
        const auto b = body(L, p, 1);
        const auto& epoch = *static_cast<const EpochValue*>(luaL_checkudata(L, 2, epoch_type));
        if (epoch.owner != p.instance() || epoch.handle != b.handle) luaL_error(L, "body epoch belongs to another body or plugin instance");
        fields(L, 3, {"force", "torque", "impulse", "impulse_id"});
        Command command;
        command.epoch = epoch.sample.epoch;
        command.force = vec_field(L, 3, "force", true);
        command.torque = vec_field(L, 3, "torque", true);
        command.impulse = vec_field(L, 3, "impulse", true);
        lua_getfield(L, 3, "impulse_id");
        if (!lua_isnil(L, -1)) {
            const double id = luaL_checknumber(L, -1);
            if (!std::isfinite(id) || id < 1 || id > 4294967295.0 || std::floor(id) != id) luaL_error(L, "impulse_id is a positive uint32 event number");
            command.impulse_id = static_cast<std::uint32_t>(id);
        }
        lua_pop(L, 1);
        if (!valid(command)) luaL_error(L, "native body command exceeds its force, torque or one-shot impulse bound");
        std::uint64_t assigned{};
        const auto why = p.services_.game().body_submit(p.instance(), b.handle, command, assigned);
        if (!why.empty()) return result(L, why);
        if (!assigned) return result(L, "native body did not return a command sequence");
        lua_pushboolean(L, true);
        lua_pushnil(L);
        text_number(L, assigned);
        return 3;
    }
    static int probe_batch(lua_State* L) {
        auto& p = plugin(L);
        need(L, p, "world.raycast", "reskate.world.physics.probe_batch");
        if (lua_gettop(L) < 3 || lua_gettop(L) > 4) luaL_error(L, "probe_batch takes body, measured epoch, rays and optional query options");
        const auto b = body(L, p, 1);
        const auto& epoch = *static_cast<const EpochValue*>(luaL_checkudata(L, 2, epoch_type));
        if (epoch.owner != p.instance() || epoch.handle != b.handle) luaL_error(L, "body epoch belongs to another body or plugin instance");
        body_query::Request request;
        request.observation = epoch.sample;
        luaL_checktype(L, 3, LUA_TTABLE);
        request.count = lua_objlen(L, 3);
        if (!request.count || request.count > body_query::maximum_rays) luaL_error(L, "a ray batch has 1 to 32 rays");
        array_fields(L, 3, request.count);
        for (std::size_t i = 0; i < request.count; ++i) {
            lua_rawgeti(L, 3, static_cast<int>(i + 1));
            fields(L, -1, {"from", "to"});
            const auto at = lua_absindex(L, -1);
            request.rays[i] = {vec_field(L, at, "from"), vec_field(L, at, "to")};
            lua_pop(L, 1);
        }
        bool preset_requested{}, named_requested{};
        if (!lua_isnoneornil(L, 4)) {
            fields(L, 4, {"exclude_bodies", "exclude_local_actor", "require_contact", "query_preset", "include_shape_categories",
                "require_named_actor_categories_absent", "include_hit_owner"});
            lua_getfield(L, 4, "query_preset");
            if (!lua_isnil(L, -1)) {
                // Only an actual string naming a fixed preset; numbers are not coerced
                // and no raw native filter can be expressed.
                std::size_t size{};
                const char* text = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &size) : nullptr;
                bool known{};
                for (const auto preset : {body_query::QueryPreset::camera, body_query::QueryPreset::terrain, body_query::QueryPreset::all})
                    if (text && std::string_view(text, size) == body_query::name(preset)) { request.preset = preset; known = true; }
                if (!known) luaL_error(L, "query_preset is \"camera\", \"terrain\" or \"all\"");
                preset_requested = true;
                // "all" tests every shape with a category, actors' shapes included, so
                // it is an observation: checked before any ray counter or backend work.
                if (request.preset == body_query::QueryPreset::all)
                    need(L, p, "world.physics.observe", "reskate.world.physics.probe_batch query_preset \"all\"");
            }
            lua_pop(L, 1);
            lua_getfield(L, 4, "require_contact");
            if (!lua_isnil(L, -1)) { luaL_checktype(L, -1, LUA_TBOOLEAN); request.require_contact = lua_toboolean(L, -1) != 0; }
            lua_pop(L, 1);
            lua_getfield(L, 4, "exclude_local_actor");
            if (!lua_isnil(L, -1)) { luaL_checktype(L, -1, LUA_TBOOLEAN); request.exclude_local_actor = lua_toboolean(L, -1) != 0; }
            lua_pop(L, 1);
            lua_getfield(L, 4, "exclude_bodies");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                request.exclusions = lua_objlen(L, -1);
                if (request.exclusions > body_query::maximum_exclusions) luaL_error(L, "a ray batch excludes at most 8 additional owned bodies");
                array_fields(L, -1, request.exclusions);
                for (std::size_t i = 0; i < request.exclusions; ++i) {
                    lua_rawgeti(L, -1, static_cast<int>(i + 1));
                    request.excluded[i] = body(L, p, -1).handle;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 4, "include_shape_categories");
            // Passive hit metadata: only an actual boolean, and true also needs the
            // observation permission before any ray counter or backend work.
            if (!lua_isnil(L, -1)) { luaL_checktype(L, -1, LUA_TBOOLEAN); request.include_shape_categories = lua_toboolean(L, -1) != 0; }
            lua_pop(L, 1);
            lua_getfield(L, 4, "require_named_actor_categories_absent");
            // A refusal-only check: it adds one batch fact and shows no category, so
            // world.physics and world.raycast are enough.
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TBOOLEAN);
                request.require_named_actor_categories_absent = lua_toboolean(L, -1) != 0;
                named_requested = true;
            }
            lua_pop(L, 1);
            lua_getfield(L, 4, "include_hit_owner");
            if (!lua_isnil(L, -1)) { luaL_checktype(L, -1, LUA_TBOOLEAN); request.include_hit_owner = lua_toboolean(L, -1) != 0; }
            lua_pop(L, 1);
            if (request.include_shape_categories) need(L, p, "world.physics.observe", "reskate.world.physics.probe_batch include_shape_categories");
            if (request.include_hit_owner) need(L, p, "world.physics.observe", "reskate.world.physics.probe_batch include_hit_owner");
        }
        const float distance_limit = p.limits_.raycast_metres < 1000 ? p.limits_.raycast_metres : 1000;
        if (!body_query::valid(request, distance_limit)) luaL_error(L, "invalid ray batch: finite nonzero rays within the configured distance");
        if (static_cast<std::int64_t>(p.raycasts_) + static_cast<std::int64_t>(request.count) > p.limits_.raycasts_per_tick)
            luaL_error(L, "more than %d raycasts in one tick", p.limits_.raycasts_per_tick);
        p.raycasts_ += static_cast<int>(request.count);
        const auto allowed = p.native_casts_.remaining();
        body_query::Result out;
        if (!allowed) { out.failure = body_query::Failure::budget; out.budget_exhausted = true; }
        else out = p.services_.game().body_probe_batch(p.instance(), b.handle, request, allowed);
        const bool accounted = out.accounting_valid && p.native_casts_.consume(out.native_casts, allowed);
        if (!accounted) {
            p.native_casts_.poison();
            out.available = false;
            if (out.accounting_valid || out.failure != body_query::Failure::query_backend_exception)
                out.failure = body_query::Failure::backend;
        }
        if (out.available) {
            bool valid = out.count == request.count && out.failure == body_query::Failure::none &&
                out.native_casts >= request.count && out.native_casts <= request.count * body_query::casts_per_ray &&
                // A fact is the backend's, and only for what was asked. The category
                // claim is not identity exclusion, so no result may hold both.
                (!out.local_actor_excluded || request.exclude_local_actor) &&
                (!out.named_actor_categories_absent || request.require_named_actor_categories_absent) &&
                !(out.local_actor_excluded && out.named_actor_categories_absent);
            for (std::size_t i = 0; i < request.count && valid; ++i) valid = body_query::consistent(request.rays[i], out.rays[i]) &&
                (!request.require_contact || out.rays[i].status != body_query::Status::hit ||
                    (out.rays[i].normal_available && out.rays[i].surface_velocity_available));
            if (!valid) { out.available = false; out.failure = body_query::Failure::backend; }
        }
        lua_createtable(L, 0, 5);
        lua_pushstring(L, out.available ? "available" : "unavailable");
        lua_setfield(L, -2, "status");
        if (!out.available) { lua_pushstring(L, body_query::name(out.failure == body_query::Failure::none ? body_query::Failure::unavailable : out.failure)); lua_setfield(L, -2, "reason"); }
        if (accounted) { lua_pushinteger(L, static_cast<int>(out.native_casts)); lua_setfield(L, -2, "native_casts"); }
        lua_pushboolean(L, out.budget_exhausted);
        lua_setfield(L, -2, "budget_exhausted");
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "epoch");
        // What the backend established, never the request's flag.
        lua_pushboolean(L, out.available && out.local_actor_excluded);
        lua_setfield(L, -2, "local_actor_excluded");
        lua_pushboolean(L, out.available);
        lua_setfield(L, -2, "owned_bodies_excluded");
        // Only a call that named a preset gets this field, so callers that allow-list
        // the earlier result fields see an unchanged table. It repeats the request:
        // native_casts and status say what ran, and nothing here says what was hit.
        if (preset_requested) { lua_pushstring(L, body_query::name(request.preset)); lua_setfield(L, -2, "requested_query_preset"); }
        // Likewise only for a call that named the option, and from the backend's result.
        if (named_requested) {
            lua_pushboolean(L, out.available && out.named_actor_categories_absent);
            lua_setfield(L, -2, "named_actor_categories_absent");
        }
        lua_createtable(L, static_cast<int>(request.count), 0);
        for (std::size_t i = 0; i < request.count; ++i) {
            const auto& hit = out.rays[i];
            const bool present = out.available && hit.status == body_query::Status::hit;
            lua_createtable(L, 0, 5);
            lua_pushstring(L, !out.available ? "unavailable" : present ? "hit" : "clear");
            lua_setfield(L, -2, "status");
            if (present) {
                push_position(L, hit.position);
                lua_setfield(L, -2, "position");
                lua_pushnumber(L, hit.fraction);
                lua_setfield(L, -2, "fraction");
                lua_pushstring(L, hit.surface_velocity_available ? "measured" : "unknown");
                lua_setfield(L, -2, "motion");
                lua_pushboolean(L, hit.normal_available);
                lua_setfield(L, -2, "normal_available");
                lua_pushboolean(L, hit.surface_velocity_available);
                lua_setfield(L, -2, "surface_velocity_available");
                if (hit.normal_available) { push_position(L, hit.normal); lua_setfield(L, -2, "normal"); }
                if (hit.surface_velocity_available) { push_position(L, hit.surface_velocity); lua_setfield(L, -2, "surface_velocity"); }
                // Only a request that opted in sees it; absent means unavailable, never zero.
                if (request.include_shape_categories && hit.in_categories_available) { lua_pushnumber(L, hit.in_categories); lua_setfield(L, -2, "in_categories"); }
                // Absent when the slot could not be read. An assigned slot becomes an
                // equality token; the slot itself never reaches Lua.
                if (request.include_hit_owner && hit.owner_available) {
                    lua_pushboolean(L, hit.owner_assigned);
                    lua_setfield(L, -2, "owner_assigned");
                    if (hit.owner_assigned) {
                        opaque(L, HitOwnerValue{p.instance(), request.observation.epoch.world, request.observation.epoch.serial, hit.owner}, hit_owner_type);
                        lua_setfield(L, -2, "owner");
                    }
                }
            }
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "rays");
        return 1;
    }
    static int receipt(lua_State* L) {
        auto& p = plugin(L);
        const auto b = body(L, p, 1);
        Receipt receipt;
        const auto selected = lua_isnoneornil(L, 2) ? std::uint64_t{} : sequence(L, 2);
        const auto why = p.services_.game().body_receipt(p.instance(), b.handle, selected, receipt);
        if (!why.empty()) return fail(L, why);
        lua_createtable(L, 0, 9);
        lua_pushstring(L, name(receipt.outcome));
        lua_setfield(L, -2, "outcome");
        text_number(L, receipt.sequence);
        lua_setfield(L, -2, "sequence");
        text_number(L, receipt.requested.serial);
        lua_setfield(L, -2, "requested_serial");
        text_number(L, receipt.produced.serial);
        lua_setfield(L, -2, "produced_serial");
        lua_pushboolean(L, receipt.force_attempted);
        lua_setfield(L, -2, "force_attempted");
        lua_pushboolean(L, receipt.torque_attempted);
        lua_setfield(L, -2, "torque_attempted");
        lua_pushboolean(L, receipt.retained);
        lua_setfield(L, -2, "retained");
        lua_pushboolean(L, receipt.quarantined);
        lua_setfield(L, -2, "quarantined");
        if (receipt.successor_available) { push_sample(L, b, receipt.successor); lua_setfield(L, -2, "successor"); }
        return 1;
    }
    static int acknowledge(lua_State* L) {
        auto& p = plugin(L);
        const auto b = body(L, p, 1);
        return result(L, p.services_.game().body_acknowledge(p.instance(), b.handle, sequence(L, 2)));
    }
    static int release(lua_State* L) {
        auto& p = plugin(L);
        const auto b = body(L, p, 1);
        return result(L, p.services_.game().body_release(p.instance(), b.handle));
    }
};
void install_physics_api(Plugin&, lua_State* L) {
    metatable(L, handle_type);
    metatable(L, epoch_type);
    luaL_newmetatable(L, hit_owner_type);
    lua_pushstring(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pushcfunction(L, hit_owner_equal, "hit owner equality");
    lua_setfield(L, -2, "__eq");
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
    // reskate.world is read-only by now; add_functions opens it for the new field and closes it.
    lua_getfield(L, -1, "world");
    add_functions(L, "physics", {{"prepare", PhysicsApi::prepare}, {"create", PhysicsApi::create}, {"read", PhysicsApi::read}, {"submit", PhysicsApi::submit},
        {"receipt", PhysicsApi::receipt}, {"acknowledge", PhysicsApi::acknowledge}, {"release", PhysicsApi::release}, {"probe_batch", PhysicsApi::probe_batch}});
    lua_pop(L, 1);
}
} // namespace dingosdk::plugins
