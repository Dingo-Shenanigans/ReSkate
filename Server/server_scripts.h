#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

// Server scripts: Lua files in scripts\ next to the server that add their own /commands and use
// the server through the `server` table (Server/README.txt, Scripts). The owner writes the scripts;
// players only type arguments, so what stops a player turning those into code or a crash is here:
// text-only chunks (no bytecode) and no load/dofile/require; no io, os.execute, debug or package;
// a memory cap and an instruction budget per call; every value from Lua checked before it reaches
// the Host, and players named by SteamID64, never held as pointers across calls.
namespace dingosdk::server {
class Host;

class Scripts {
  public:
    explicit Scripts(Host &host) : host_(host) {}
    Scripts(const Scripts &) = delete;
    Scripts &operator=(const Scripts &) = delete;
    ~Scripts();
    // Every .lua file in the folder, in name order; the scripts' errors, one a line ("" = none).
    std::string load(const std::string &folder);
    // The script's reply, or nothing when no script has this command (or it is for admins and
    // `caller` is not one). `caller` is the player's SteamID64, 0 for the console.
    std::optional<std::string> run(std::string_view verb, std::uint64_t caller, std::string_view args);

  private:
    struct Command {
        int function{}; // luaL_ref in the registry
        bool admin{};
    };
    Host &host_;
    lua_State *lua_{};
    std::map<std::string, Command, std::less<>> commands_;
    std::size_t memory_{}; // bytes Lua holds now
    unsigned steps_{};     // instruction-budget hooks this call
    bool running_{};       // a script is running: the server never calls back into one
    // Calls the function under `arguments` on the stack with the instruction budget; its first
    // result (or the error) as text. Nothing here runs unprotected: an error at the memory cap
    // outside a pcall would end the server.
    bool call(int arguments, std::string &result);
    static int open(lua_State *); // the libraries and the `server` table
    // The connected player a script names (a SteamID64, a player table or the start of a name), or 0.
    std::uint64_t target(lua_State *, int at);
    void push_player(lua_State *, std::uint64_t id); // 0: the console
};
} // namespace dingosdk::server
