#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

// Server scripts: Lua files in scripts\ next to the server that add their own /commands.
namespace dingosdk::server {
    class Scripts {
        public:
            Scripts() = default;
            Scripts(const Scripts &) = delete;
            Scripts &operator=(const Scripts &) = delete;
            ~Scripts();
            std::string load(const std::string &folder);

            // The script's reply, or nothign when no script has this command.
            std::optional<std::string> run(std::string_view verb, std::string_view player, std::string_view args);

        private:
            lua_State *lua_{};
            std::map<std::string, int, std::less<>> commands_; // verb -> luaL_ref of function
    };


} // namespace dingosdk::server
