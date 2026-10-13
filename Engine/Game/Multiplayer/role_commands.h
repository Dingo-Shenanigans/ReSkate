#pragma once
#include <algorithm>
#include <string_view>

// Which of a dedicated server's commands a role may use (Server/roles.h): asked by the server
// before it runs one, and by the game to show a player only what their role gives them.
namespace dingosdk {
// The settings menu's names for some commands, and the one each is.
inline std::string_view canonical_role_command(std::string_view verb) noexcept {
    return verb == "voice-allow" ? "voice" : verb == "object-placement" ? "placement" : verb == "object-limit" ? "objects"
         : verb == "world-layer-sync" ? "layer-sync" : verb == "noclip-allow" ? "noclip" : verb == "nobail-allow" ? "nobail"
         : verb == "boosts-allow" ? "boosts" : verb == "tuning-enforce" ? "tuning" : verb;
}
// Commands "every command" does not give: who has which role, and who has a slot kept for them.
inline bool role_listed_only(std::string_view command) noexcept { return command == "role" || command == "reserved"; }
// What every role with any command may also use.
inline bool role_common_command(std::string_view command) noexcept {
    return command.empty() || command == "help" || command == "roles";
}
// `all`: the role has every command; `commands`: the ones it lists.
template<class Commands> bool role_allows(bool all, const Commands &commands, std::string_view command) noexcept {
    command = canonical_role_command(command);
    const bool named = std::find(commands.begin(), commands.end(), command) != commands.end();
    if (named || (all && !role_listed_only(command))) return true;
    return role_common_command(command) && (all || commands.begin() != commands.end());
}
} // namespace dingosdk
