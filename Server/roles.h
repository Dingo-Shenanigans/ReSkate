#pragma once
#include "Engine/Game/Multiplayer/role_commands.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::server {
// Roles (data/roles.json, beside the bans). A role is a name, the tag its players wear in the
// game, the server commands they may use, the permissions they have beyond commands, and a
// rank. A player has the role they were given, or else the default role, the one everybody
// has who was given none. A role has what the default role has as well as its own, unless it
// says it does not inherit (a muted role). Nobody uses a command on a player whose rank is
// theirs or higher, or gives out a role that is: the console alone is above every role.
struct Role {
    std::string name;             // 1 to 24 of a-z, 0-9, - and _
    std::string tag;              // shown before the player's name; empty: no tag
    std::string color = "#FFFFFF"; // the tag's colour, #RRGGBB
    int rank{};                   // 0 to 1000
    bool all_commands{};          // "*": every command but the ones only a listing gives
    std::vector<std::string> commands;
    std::uint32_t permissions{};  // multiplayer::role_* bits
    bool inherit = true;          // also has what the default role has
    // What it comes to with the default role's (Roles::settle): what is checked and sent.
    bool granted_all{};
    std::vector<std::string> granted_commands;
    std::uint32_t granted{};
};
struct Roles {
    std::vector<Role> list;
    std::string default_role;                     // the role of everyone given none; empty: they have no role
    std::map<std::uint64_t, std::string> players; // SteamID64 -> role name

    const Role *find(std::string_view name) const {
        const auto at = std::find_if(list.begin(), list.end(), [&](const Role &role) { return role.name == name; });
        return at == list.end() ? nullptr : &*at;
    }
    // The role a player was given, if any; and the one they have (that, or the default).
    const Role *given(std::uint64_t id) const {
        const auto at = players.find(id);
        return at == players.end() ? nullptr : find(at->second);
    }
    const Role *of(std::uint64_t id) const {
        const auto *role = given(id);
        return role || default_role.empty() ? role : find(default_role);
    }
    // After any change to the roles: works out what each comes to.
    void settle() {
        const Role none{};
        const auto *shared = find(default_role);
        for (auto &role : list) {
            const auto &base = role.inherit && shared && shared != &role ? *shared : none;
            role.granted_all = role.all_commands || base.all_commands;
            role.granted = role.permissions | base.permissions;
            role.granted_commands = role.commands;
            for (const auto &command : base.commands)
                if (std::find(role.granted_commands.begin(), role.granted_commands.end(), command) == role.granted_commands.end())
                    role.granted_commands.push_back(command);
        }
    }
};
inline constexpr std::size_t max_roles = 32, max_role_players = 4096;
// What a permission is called in the file, and what it gives.
inline constexpr std::array<std::pair<std::string_view, std::uint32_t>, 11> role_permissions{{
    {"chat", multiplayer::role_chat},                         // talks in chat (without it: muted)
    {"votes", multiplayer::role_votes},                       // starts the votes that are on, and votes
    {"parties", multiplayer::role_parties},                   // forms and joins parties, and their chat
    {"whisper", multiplayer::role_whisper},                   // sends private messages (/w)
    {"reserved_slot", multiplayer::role_reserved_slot},       // joins a full server, in a slot of their own
    {"unlimited_objects", multiplayer::role_objects},         // no object limit; builds and resizes when players may not
    {"movement", multiplayer::role_movement},                 // noclip, No Bail and the boosts when players may not
    {"no_afk_kick", multiplayer::role_afk},                   // never removed for being away
    {"vote_kick_immune", multiplayer::role_vote_immune},      // cannot be kicked by a vote
    {"anticheat_alerts", multiplayer::role_alerts},           // told in chat what the anti-cheat finds
    {"polls", multiplayer::role_polls},                       // starts and ends polls when players may not
}};
// Whether a role may use a command (Engine/Game/Multiplayer/role_commands.h: "*" gives every
// command but role and reserved, which a role has only when it lists them).
inline std::string_view canonical_command(std::string_view verb) noexcept { return canonical_role_command(verb); }
inline bool allows(const Role *role, std::string_view command) noexcept {
    return role && role_allows(role->granted_all, role->granted_commands, command);
}
inline bool staff(const Role *role) noexcept { return role && (role->granted_all || !role->granted_commands.empty()); }
// What every player does (chat, votes, parties, private messages): a player with no role at
// all, on a server with no default role, does them as they always have.
inline bool may(const Role *role, std::uint32_t permission) noexcept { return !role || (role->granted & permission); }
// The roles a new server starts with: everybody is a player, and nobody is in the others.
Roles default_roles();
// Why these roles cannot be used, or empty.
std::string roles_error(const Roles &);
} // namespace dingosdk::server
