#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
struct SteamSocialPlayer {
    std::uint64_t id{};
    std::string name;
    bool online{}, playing{};
    // The public server or lobby a friend is skating in (its browser row's id), when they
    // are in one: what their game tells Steam friends (steam_friend_join.h). 0: none, a
    // private session, or a build that does not say.
    std::uint64_t session{};
    bool operator==(const SteamSocialPlayer &) const = default;
};
struct SteamSocialSnapshot {
    SteamSocialPlayer local;
    std::vector<SteamSocialPlayer> friends;
    std::uint64_t revision{};
};
// Read the game's existing Steam session. Never initializes Steam, changes
// presence, or sends friend/invite requests. Poll only on the client thread.
// The local player has no `session` here: the session model knows theirs.
std::shared_ptr<const SteamSocialSnapshot> steam_social_snapshot();
}
