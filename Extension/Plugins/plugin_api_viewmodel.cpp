#include "plugin_api.h"
#include "plugin_game.h"
#include "lualib.h"
#include <cmath>
#include <filesystem>

// reskate.viewmodel: a rigged, animated model drawn in front of the camera (Extension/UI/Overlay/viewmodel.h),
// as shooters draw the player's arms and gun; made with contrib/viewmodel_export.py. Positions here
// are in the view: x right, y up, z forward, metres from the eye.
namespace dingosdk::plugins {
namespace {
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

// A number field of the table at `index`, kept when absent, within [low, high].
void number_field(lua_State* L, int index, const char* name, float low, float high, float& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        const double value = luaL_checknumber(L, -1);
        if (!(value >= low && value <= high)) luaL_error(L, "the viewmodel's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = static_cast<float>(value);
    }
    lua_pop(L, 1);
}
// A vector field, each part within [low, high].
void vector_field(lua_State* L, int index, const char* name, float low, float high, Vec3& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isvector(L, -1)) luaL_error(L, "the viewmodel's %s is a vector", name);
        const float* v = lua_tovector(L, -1);
        for (int i = 0; i < 3; ++i)
            if (!(v[i] >= low && v[i] <= high)) luaL_error(L, "each part of the viewmodel's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = {v[0], v[1], v[2]};
    }
    lua_pop(L, 1);
}
} // namespace

struct ViewmodelApi {
    static Plugin& caller(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "viewmodel", what);
        return plugin;
    }
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }
    // load(path): reads the package's .rsvm file (and the pictures it names, beside it) in the
    // background; info().status says when it is ready.
    static int load(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.load");
        const auto path = checked_text(L, 1, 200, "a viewmodel's path");
        auto extension = std::filesystem::path(path).extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".rsvm") luaL_error(L, "a viewmodel is a .rsvm file (contrib/viewmodel_export.py makes one)");
        const auto file = package_file(L, plugin, path);
        std::error_code code;
        if (!std::filesystem::is_regular_file(file, code)) luaL_error(L, "\"%s\" is not a file in the plugin's package", path.c_str());
        return result(L, game(plugin).viewmodel_load(plugin.id(), file));
    }
    static int unload(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.unload");
        game(plugin).viewmodel_unload(plugin.id());
        return 0;
    }
    // info(): {status = "loading" | "ready" | "failed", why, clips = {{name, duration}}, bones = {names}},
    // or nil when this plugin has none loaded.
    static int info(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.info");
        const auto found = game(plugin).viewmodel_info(plugin.id());
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, found->status.data(), found->status.size());
        lua_setfield(L, -2, "status");
        if (!found->why.empty()) {
            lua_pushlstring(L, found->why.data(), found->why.size());
            lua_setfield(L, -2, "why");
        }
        lua_createtable(L, static_cast<int>(found->clips.size()), 0);
        for (std::size_t i = 0; i < found->clips.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushlstring(L, found->clips[i].first.data(), found->clips[i].first.size());
            lua_setfield(L, -2, "name");
            lua_pushnumber(L, found->clips[i].second);
            lua_setfield(L, -2, "duration");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "clips");
        lua_createtable(L, static_cast<int>(found->bones.size()), 0);
        for (std::size_t i = 0; i < found->bones.size(); ++i) {
            lua_pushlstring(L, found->bones[i].data(), found->bones[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "bones");
        return 1;
    }
    // show(look): drawn for the next 300 ms (so every tick), with {offset, yaw, pitch, roll, fov,
    // light, light_colour, sky, ground, ambient, brightness}, each optional.
    static int show(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.show");
        Game::ViewmodelLook look;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TTABLE);
            vector_field(L, 1, "offset", -5, 5, look.offset);
            number_field(L, 1, "yaw", -360, 360, look.yaw);
            number_field(L, 1, "pitch", -360, 360, look.pitch);
            number_field(L, 1, "roll", -360, 360, look.roll);
            number_field(L, 1, "fov", 20, 120, look.fov);
            vector_field(L, 1, "light", -1e3f, 1e3f, look.light);
            vector_field(L, 1, "light_colour", 0, 4, look.light_colour);
            vector_field(L, 1, "sky", 0, 4, look.sky);
            vector_field(L, 1, "ground", 0, 4, look.ground);
            number_field(L, 1, "ambient", 0, 4, look.ambient);
            number_field(L, 1, "brightness", 0, 4, look.brightness);
        }
        return result(L, game(plugin).viewmodel_show(plugin.id(), look));
    }
    static int hide(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.hide");
        return result(L, game(plugin).viewmodel_show(plugin.id(), std::nullopt));
    }
    // play(clip, {loop = false, speed = 1, fade = 0.1}): from its start, faded in over what played.
    static int play(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.play");
        const auto clip = checked_text(L, 1, 120, "a clip's name");
        bool loop{};
        float speed{1}, fade{0.1f};
        if (!lua_isnoneornil(L, 2)) {
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_getfield(L, 2, "loop");
            loop = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            number_field(L, 2, "speed", 0, 10, speed);
            number_field(L, 2, "fade", 0, 2, fade);
        }
        return result(L, game(plugin).viewmodel_play(plugin.id(), clip, loop, speed, fade));
    }
    static int stop(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.stop");
        game(plugin).viewmodel_stop(plugin.id());
        return 0;
    }
    // state(): {clip, time, duration, done} of what plays, or nil.
    static int state(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.state");
        const auto now = game(plugin).viewmodel_state(plugin.id());
        if (!now) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, now->clip.data(), now->clip.size());
        lua_setfield(L, -2, "clip");
        lua_pushnumber(L, now->time);
        lua_setfield(L, -2, "time");
        lua_pushnumber(L, now->duration);
        lua_setfield(L, -2, "duration");
        lua_pushboolean(L, now->done);
        lua_setfield(L, -2, "done");
        return 1;
    }
    // bone(name): {position, x, y, z} of a bone now, in the view with show's placement, or nil.
    static int bone(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.bone");
        const auto name = checked_text(L, 1, 120, "a bone's name");
        const auto found = game(plugin).viewmodel_bone(plugin.id(), name);
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        push_position(L, found->position);
        lua_setfield(L, -2, "position");
        push_position(L, found->x);
        lua_setfield(L, -2, "x");
        push_position(L, found->y);
        lua_setfield(L, -2, "y");
        push_position(L, found->z);
        lua_setfield(L, -2, "z");
        return 1;
    }
};

void install_viewmodel_api(Plugin&, lua_State* L) {
    add_functions(L, "viewmodel", {{"load", ViewmodelApi::load}, {"unload", ViewmodelApi::unload}, {"info", ViewmodelApi::info},
                                   {"show", ViewmodelApi::show}, {"hide", ViewmodelApi::hide}, {"play", ViewmodelApi::play},
                                   {"stop", ViewmodelApi::stop}, {"state", ViewmodelApi::state}, {"bone", ViewmodelApi::bone}});
}
} // namespace dingosdk::plugins
