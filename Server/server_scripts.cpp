#include "server_scripts.h"
#include "server_host.h"
#include "server_text.h"
// Not lua.hpp: Lua is built as C++ (cmake/Dependencies.cmake), so its functions are not extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

namespace dingosdk::server {
namespace {
constexpr std::size_t memory_limit = 32u << 20; // all scripts together
constexpr int hook_every = 1000;                // instructions between budget checks
constexpr unsigned step_limit = 2000;           // ~2M instructions a call (a few ms), or a file's first run
constexpr float world_limit = 1e6f;             // metres from the origin: anything further is a mistake

// Each state keeps its Scripts in its extra space (lua.h), which hooks reach too.
Scripts &self(lua_State *lua) { return **static_cast<Scripts **>(lua_getextraspace(lua)); }

// A binding's body, with C++ exceptions (std::bad_alloc...) turned into Lua errors: one crossing
// Lua's C++ try/catch would end the pcall with status -1 and no message (ldo.c LUAI_TRY).
template <class Body> int guarded(lua_State *lua, Body &&body) {
    std::string why;
    try {
        return body();
    } catch (const std::exception &e) {
        why = e.what();
    }
    return luaL_error(lua, "%s", why.c_str());
}
std::string_view text(lua_State *lua, int at) {
    std::size_t size{};
    const char *data = luaL_checklstring(lua, at, &size);
    return {data, size};
}
int no_player(lua_State *lua) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no such player");
    return 2;
}
} // namespace

Scripts::~Scripts() {
    if (lua_) lua_close(lua_);
}

std::string Scripts::load(const std::string &folder) {
    if (lua_) lua_close(lua_); // loading again starts over
    lua_ = nullptr;
    commands_.clear();
    memory_ = 0;
    // Every allocation counts against memory_limit; a refused one is a Lua "not enough memory" error.
    lua_ = lua_newstate(
        [](void *scripts, void *old, std::size_t old_size, std::size_t size) -> void * {
            auto &memory = static_cast<Scripts *>(scripts)->memory_;
            const std::size_t before = old ? old_size : 0; // old_size is a type tag when old is null
            if (!size) {
                std::free(old);
                memory -= before;
                return nullptr;
            }
            if (size > before && memory - before + size > memory_limit) return nullptr;
            void *moved = std::realloc(old, size);
            if (moved) memory = memory - before + size;
            return moved;
        },
        this);
    if (!lua_) return "Scripts: Lua did not start.\n";
    *static_cast<Scripts **>(lua_getextraspace(lua_)) = this;
    std::string why;
    lua_pushcfunction(lua_, open);
    if (!call(0, why)) return "Scripts: " + why + "\n";

    std::vector<std::filesystem::path> files;
    std::error_code missing; // no scripts folder: no scripts
    for (std::filesystem::directory_iterator file(folder, missing), end; !missing && file != end; file.increment(missing))
        if (file->path().extension() == ".lua") files.push_back(file->path());
    std::sort(files.begin(), files.end());
    std::string errors;
    for (const auto &file : files) {
        // "t": source text only. Lua does not check bytecode, so a precompiled chunk could do anything.
        if (luaL_loadfilex(lua_, file.string().c_str(), "t") != LUA_OK) {
            const char *error = lua_tostring(lua_, -1);
            why = error ? error : "could not load";
            lua_pop(lua_, 1);
        } else if (call(0, why)) {
            continue;
        }
        errors += "Script " + file.filename().string() + ": " + why + "\n";
    }
    return errors;
}

std::optional<std::string> Scripts::run(std::string_view verb, std::uint64_t caller, std::string_view args) {
    const auto found = commands_.find(verb);
    // running_: a script's own server.kick() and the like never come back into a script.
    if (!lua_ || running_ || found == commands_.end()) return {};
    if (found->second.admin && caller && !host_.is_admin(caller)) return {};
    struct Request {
        int function;
        std::uint64_t caller;
        std::string_view args;
    } request{found->second.function, caller, args};
    // The arguments are made inside the pcall: building them can fail at the memory cap.
    constexpr lua_CFunction invoke = [](lua_State *lua) -> int {
        const auto &request = *static_cast<const Request *>(lua_touserdata(lua, 1));
        lua_rawgeti(lua, LUA_REGISTRYINDEX, request.function);
        self(lua).push_player(lua, request.caller);
        lua_pushlstring(lua, request.args.data(), request.args.size());
        lua_call(lua, 2, 1);
        return 1;
    };
    lua_pushcfunction(lua_, invoke);
    lua_pushlightuserdata(lua_, &request);
    std::string answer;
    if (call(1, answer)) return answer;
    // The details (with the script's path) are for the owner's log, not the player.
    host_.log_("[script] /" + std::string(verb) + " failed: " + clean_chat_text(answer));
    return "The /" + std::string(verb) + " command failed.";
}

bool Scripts::call(int arguments, std::string &result) {
    steps_ = 0;
    lua_sethook(
        lua_,
        [](lua_State *lua, lua_Debug *) {
            if (++self(lua).steps_ <= step_limit) return;
            // From here every instruction errors, so a script's own pcall cannot catch this and go on.
            lua_sethook(lua, lua_gethook(lua), LUA_MASKCOUNT, 1);
            luaL_error(lua, "the script ran too long");
        },
        LUA_MASKCOUNT, hook_every);
    running_ = true;
    const int status = lua_pcall(lua_, arguments, 1, 0);
    running_ = false;
    lua_sethook(lua_, nullptr, 0, 0);
    // Read without converting: lua_tolstring on a number allocates, which can fail out here.
    result.clear();
    if (lua_type(lua_, -1) == LUA_TSTRING) {
        std::size_t size{};
        const char *data = lua_tolstring(lua_, -1, &size);
        result.assign(data, size);
    } else if (lua_isinteger(lua_, -1)) {
        result = std::to_string(lua_tointeger(lua_, -1));
    } else if (lua_type(lua_, -1) == LUA_TNUMBER) {
        char number[64];
        std::snprintf(number, sizeof number, "%.14g", static_cast<double>(lua_tonumber(lua_, -1)));
        result = number;
    } else if (status != LUA_OK) {
        result = "error object is a " + std::string(luaL_typename(lua_, -1));
    }
    lua_pop(lua_, 1);
    return status == LUA_OK;
}

std::uint64_t Scripts::target(lua_State *lua, int at) {
    std::uint64_t id{};
    if (lua_istable(lua, at)) {
        lua_getfield(lua, at, "id");
        id = static_cast<std::uint64_t>(lua_tointeger(lua, -1)); // not an integer: 0, nobody
        lua_pop(lua, 1);
    } else if (lua_isinteger(lua, at)) {
        id = static_cast<std::uint64_t>(lua_tointeger(lua, at));
    } else if (lua_type(lua, at) == LUA_TSTRING) {
        const auto *guest = host_.match_player(text(lua, at));
        return guest ? guest->member.id : 0;
    } else {
        luaL_typeerror(lua, at, "player (SteamID64, player table or name)");
    }
    const auto *guest = host_.find(id);
    return guest && guest->handshaken ? id : 0;
}

void Scripts::push_player(lua_State *lua, std::uint64_t id) {
    const auto *guest = id ? host_.find(id) : nullptr;
    const std::string name = guest ? host_.guest_name(*guest) : "Server";
    lua_createtable(lua, 0, 7);
    lua_pushinteger(lua, static_cast<lua_Integer>(id));
    lua_setfield(lua, -2, "id");
    lua_pushlstring(lua, name.data(), name.size());
    lua_setfield(lua, -2, "name");
    lua_pushboolean(lua, !id || host_.is_admin(id));
    lua_setfield(lua, -2, "admin");
    if (!guest) return;
    lua_pushinteger(lua, static_cast<lua_Integer>(guest->shared.objects().size()));
    lua_setfield(lua, -2, "objects");
    if (!guest->latest_root) return;
    const auto &at = guest->latest_root->position;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        lua_pushnumber(lua, at[axis]);
        lua_setfield(lua, -2, axis == 0 ? "x" : axis == 1 ? "y" : "z");
    }
}

int Scripts::open(lua_State *lua) {
    // The libraries a script needs, and none that reach files, processes, other code or Lua's insides.
    static constexpr luaL_Reg libraries[] = {{LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string},
                                             {LUA_TABLIBNAME, luaopen_table},   {LUA_MATHLIBNAME, luaopen_math},
                                             {LUA_UTF8LIBNAME, luaopen_utf8},   {LUA_OSLIBNAME, luaopen_os}};
    for (const auto &library : libraries) {
        luaL_requiref(lua, library.name, library.func, 1);
        lua_pop(lua, 1);
    }
    // No code but the scripts' own files: load and loadfile take bytecode, string.dump makes it.
    for (const char *name : {"dofile", "loadfile", "load", "collectgarbage"}) {
        lua_pushnil(lua);
        lua_setglobal(lua, name);
    }
    lua_getglobal(lua, LUA_STRLIBNAME);
    lua_pushnil(lua);
    lua_setfield(lua, -2, "dump");
    lua_pop(lua, 1);
    // No finalizers: Lua runs __gc with hooks off (lgc.c GCTM), out of the instruction budget, and
    // lua_close runs them too. An object is only finalized if its metatable has __gc when set.
    lua_getglobal(lua, "setmetatable");
    lua_pushcclosure(
        lua,
        [](lua_State *lua) -> int {
            if (lua_istable(lua, 2)) {
                lua_pushliteral(lua, "__gc");
                if (lua_rawget(lua, 2) != LUA_TNIL) return luaL_argerror(lua, 2, "scripts cannot use __gc");
            }
            lua_settop(lua, 2);
            lua_pushvalue(lua, lua_upvalueindex(1));
            lua_insert(lua, 1);
            lua_call(lua, 2, 1);
            return 1;
        },
        1);
    lua_setglobal(lua, "setmetatable");
    // os: the clock and calendar only (not execute, exit, getenv, remove, rename, tmpname, setlocale).
    lua_newtable(lua);
    lua_getglobal(lua, LUA_OSLIBNAME);
    for (const char *name : {"time", "clock", "date"}) {
        lua_getfield(lua, -1, name);
        lua_setfield(lua, -3, name);
    }
    lua_pop(lua, 1);
    lua_setglobal(lua, LUA_OSLIBNAME);

    // A server command with `argument` from a script, as the console runs it; logged, since the
    // console logs only what is typed into it. The argument goes through the chat cleaner: no
    // control characters or line breaks reach the command line or the log.
    static constexpr auto console = [](lua_State *lua, std::string line) {
        auto &host = self(lua).host_;
        const auto answer = host.command(line);
        host.log_("[script] " + clean_chat_text(line) + ": " + clean_chat_text(answer));
        lua_pushlstring(lua, answer.data(), answer.size());
        return 1;
    };
    static constexpr luaL_Reg api[] = {
        // server.command(name, function(player, args) ... end [, {admin = true}])
        {"command",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 auto name = lower(text(lua, 1));
                 luaL_checktype(lua, 2, LUA_TFUNCTION);
                 bool admin{};
                 if (!lua_isnoneornil(lua, 3)) {
                     luaL_checktype(lua, 3, LUA_TTABLE);
                     lua_getfield(lua, 3, "admin");
                     admin = lua_toboolean(lua, -1);
                 }
                 // What a player can type after the slash.
                 const auto allowed = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; };
                 if (name.empty() || name.size() > 32 || !std::all_of(name.begin(), name.end(), allowed))
                     return luaL_argerror(lua, 1, "a command name is 1 to 32 letters, digits, - or _");
                 auto &commands = self(lua).commands_;
                 lua_pushvalue(lua, 2);
                 const int function = luaL_ref(lua, LUA_REGISTRYINDEX);
                 if (const auto old = commands.find(name); old != commands.end()) luaL_unref(lua, LUA_REGISTRYINDEX, old->second.function);
                 commands[name] = {function, admin};
                 return 0;
             });
         }},
        // server.players(): every connected player's table.
        {"players",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 auto &scripts = self(lua);
                 std::vector<std::uint64_t> ids;
                 for (const auto &[id, guest] : scripts.host_.guests_)
                     if (guest->handshaken) ids.push_back(id);
                 lua_createtable(lua, static_cast<int>(ids.size()), 0);
                 for (std::size_t i = 0; i < ids.size(); ++i) {
                     scripts.push_player(lua, ids[i]);
                     lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
                 }
                 return 1;
             });
         }},
        // server.player(who): their table, or nil.
        {"player",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 auto &scripts = self(lua);
                 const auto id = scripts.target(lua, 1);
                 if (!id) return no_player(lua);
                 scripts.push_player(lua, id);
                 return 1;
             });
         }},
        // server.say(text): a chat line from the server to everyone.
        {"say",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 self(lua).host_.send_chat(text(lua, 1));
                 return 0;
             });
         }},
        // server.tell(who, text): chat lines to one player; false when nobody matches.
        {"tell",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 auto &scripts = self(lua);
                 const auto id = scripts.target(lua, 1);
                 const auto line = text(lua, 2);
                 auto *guest = scripts.host_.find(id);
                 if (guest) scripts.host_.reply(*guest, line);
                 lua_pushboolean(lua, guest != nullptr);
                 return 1;
             });
         }},
        // server.announce(text): a chat line and the announcement card.
        {"announce",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 self(lua).host_.announce(text(lua, 1));
                 return 0;
             });
         }},
        // server.kick(who), server.ban(who or an offline SteamID64): the server's answer.
        {"kick",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 const auto id = self(lua).target(lua, 1);
                 return id ? console(lua, "kick " + std::to_string(id)) : no_player(lua);
             });
         }},
        {"ban",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 const auto id = lua_isinteger(lua, 1) ? static_cast<std::uint64_t>(lua_tointeger(lua, 1)) : self(lua).target(lua, 1);
                 return id ? console(lua, "ban " + std::to_string(id)) : no_player(lua);
             });
         }},
        // server.teleport(who, x, y, z): true when sent.
        {"teleport",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 auto &scripts = self(lua);
                 const auto id = scripts.target(lua, 1);
                 std::array<float, 3> to{};
                 for (int axis = 0; axis < 3; ++axis) {
                     const auto value = luaL_checknumber(lua, 2 + axis);
                     if (!std::isfinite(value) || std::abs(value) > world_limit) return luaL_argerror(lua, 2 + axis, "not a place in the world");
                     to[static_cast<std::size_t>(axis)] = static_cast<float>(value);
                 }
                 auto &host = scripts.host_;
                 auto *guest = host.find(id);
                 bool sent{};
                 if (guest && guest->world_ready) {
                     auto p = host.packet(PacketKind::teleport, host.now_);
                     p.teleport = to;
                     sent = host.send_packet(*guest, p, true, false);
                 }
                 lua_pushboolean(lua, sent);
                 return 1;
             });
         }},
        // server.map(): the map's name; server.map(name): changes it, the server's answer.
        {"map",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 if (lua_isnoneornil(lua, 1)) {
                     const auto name = self(lua).host_.map_name();
                     lua_pushlstring(lua, name.data(), name.size());
                     return 1;
                 }
                 return console(lua, "map " + clean_chat_text(text(lua, 1)));
             });
         }},
        // server.tod(time): morning, noon, night... or default; the server's answer.
        {"tod", [](lua_State *lua) -> int { return guarded(lua, [&] { return console(lua, "tod " + clean_chat_text(text(lua, 1))); }); }},
        // server.clear_objects(): deletes every placed object; the server's answer.
        {"clear_objects", [](lua_State *lua) -> int { return guarded(lua, [&] { return console(lua, "clear-objects"); }); }},
        // server.log(...), and print(...): a line in the server's log.
        {"log",
         [](lua_State *lua) -> int {
             return guarded(lua, [&] {
                 std::string line;
                 for (int i = 1, count = lua_gettop(lua); i <= count; ++i) {
                     std::size_t size{};
                     const char *part = luaL_tolstring(lua, i, &size);
                     if (i > 1) line += ' ';
                     line.append(part, size);
                     lua_pop(lua, 1);
                 }
                 self(lua).host_.log_("[script] " + clean_chat_text(line));
                 return 0;
             });
         }},
        {nullptr, nullptr}};
    luaL_newlib(lua, api);
    lua_getfield(lua, -1, "log");
    lua_setglobal(lua, "print");
    lua_setglobal(lua, "server");
    return 0;
}
} // namespace dingosdk::server
