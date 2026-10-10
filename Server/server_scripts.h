#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // Loading again (scripts reload) starts over: commands, handlers, timers and globals.
    std::string load(const std::filesystem::path &folder);
    const std::filesystem::path &folder() const { return folder_; }
    // The script's reply, or nothing when no script has this command (or it is for admins and
    // `caller` is not one). `caller` is the player's SteamID64, 0 for the console.
    std::optional<std::string> run(std::string_view verb, std::uint64_t caller, std::string_view args);
    // The commands `caller` may run (0: the console), as "goto, rules"; "" for none.
    std::string help(std::uint64_t caller) const;
    // What server.on() can wait for. The Host reports them as they happen, deep in its own
    // loops; they are queued and their handlers run from tick(), where a handler may kick,
    // teleport or message anyone without pulling a player out from under the Host.
    enum class Event : unsigned char { join, leave, chat };
    void happened(Event, std::uint64_t id, std::string name, std::string text = {});
    void tick(); // the queued events' handlers, then the timers that are due

  private:
    struct Command {
        int function{}; // luaL_ref in the registry
        bool admin{};
    };
    Host &host_;
    lua_State *lua_{};
    std::filesystem::path folder_;
    std::map<std::string, Command, std::less<>> commands_;
    struct Happened {
        Event event{};
        std::uint64_t id{};
        std::string name, text; // name: as they were known, for a player who has since left
    };
    struct Timer {
        std::uint64_t due{}, every{}; // steady-clock microseconds; every 0: once
        int function{};
    };
    std::array<std::vector<int>, 3> handlers_; // luaL_refs, by Event
    std::vector<Happened> happened_;
    std::map<std::int64_t, Timer> timers_;
    std::int64_t timer_ids_{};
    std::size_t memory_{}; // bytes Lua holds now
    unsigned steps_{};     // instruction-budget hooks this call
    bool running_{};       // a script is running: the server never calls back into one
    // Calls the function under `arguments` on the stack with the instruction budget; its first
    // result (or the error) as text. Nothing here runs unprotected: an error at the memory cap
    // outside a pcall would end the server.
    bool call(int arguments, std::string &result);
    // A script function run with the budget: `function` given the player `id` (when `arguments`
    // is 1 or more; their table, or {id, name} if they have left) and `text` (2).
    struct Request {
        int function{};
        int arguments{};
        std::uint64_t id{};
        std::string_view name, text;
    };
    bool call(const Request &, std::string &result);
    static int open(lua_State *); // the libraries and the `server` table
    // The connected player a script names (a SteamID64, a player table or the start of a name), or 0.
    std::uint64_t target(lua_State *, int at);
    void push_player(lua_State *, std::uint64_t id, std::string_view gone = {}); // 0: the console
    void cancel(std::int64_t timer);
};
} // namespace dingosdk::server
