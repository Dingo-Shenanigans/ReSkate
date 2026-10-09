#include "server_scripts.h"
// Not lua.hpp: Lua is built as C++ (cmake/Dependencies.cmake), so its functions are not extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <cctype>
#include <filesystem>

namespace dingosdk::server {
namespace {
    int add_command(lua_State *lua) {
        std::string verb = luaL_checkstring(lua, 1);
        luaL_checktype(lua, 2, LUA_TFUNCTION);
        for (auto &c: verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); // chat verbs arrive lowercase
        auto &commands = *static_cast<std::map<std::string, int, std::less<>> *>(lua_touserdata(lua, lua_upvalueindex(1)));
        if (const auto old = commands.find(verb); old != commands.end()) luaL_unref(lua, LUA_REGISTRYINDEX, old->second);
        lua_settop(lua, 2);
        commands[verb] = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 0;
    }
} // namespace

Scripts::~Scripts() {
    if (lua_) lua_close(lua_);
}

std::string Scripts::load(const std::string &folder) {
    if (lua_) lua_close(lua_); // loading again starts over

    commands_.clear();

    lua_ = luaL_newstate();

    lua_pushlightuserdata(lua_, &commands_);
    lua_pushcclosure(lua_, add_command, 1);
    lua_setglobal(lua_, "command");

    std::string errors;
    std::error_code missing; // No scripts folder? no scripts.

    for (const auto &file : std::filesystem::directory_iterator(folder, missing))
        if (file.path().extension() == ".lua" && luaL_dofile(lua_, file.path().string().c_str()) != LUA_OK) {
            const char *why = lua_tostring(lua_, -1);
            errors += std::string(why ? why : "unknown error") + "\n";
            lua_pop(lua_, 1);
        }
    return errors;
}

std::optional<std::string> Scripts::run(std::string_view verb, std::string_view player, std::string_view args) {
    const auto found = commands_.find(verb);
    if (found == commands_.end()) return {};
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, found->second); // push the function
    lua_pushlstring(lua_, player.data(), player.size());
    lua_pushlstring(lua_, args.data(), args.size());
    const bool ok = lua_pcall(lua_, 2, 1, 0) == LUA_OK; // pcall: a broken script answers, never crashed
    const char *text = lua_tostring(lua_, -1);
    std::string answer = ok? (text? text : "") : "Script error: " + std::string(text? text : "? no error text; contact contributors.");
    return answer;
}
} // namespace dingosdk::server
