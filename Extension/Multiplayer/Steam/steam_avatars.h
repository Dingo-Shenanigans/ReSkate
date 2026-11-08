#pragma once
#include <cstdint>
#include <vector>

// A Steam friend's avatar and their Steam profile, for the pause menu's FRIENDS tab, through the
// game's own Steam session (the steam_api64.dll it loaded, as steam_social.h reads the friends).
// Never initializes Steam and never pumps or registers callbacks.
namespace dingosdk::multiplayer {
struct SteamAvatar {
    std::vector<unsigned char> rgba; // width * height * 4
    std::uint32_t width{}, height{};
};
// Their medium avatar (64 pixels square). False while it cannot be had: Steam has not downloaded it
// yet (asking starts that, so ask again in a few seconds rather than waiting for its
// AvatarImageLoaded callback), they have none, or there is no Steam session (offline). Any thread:
// these are a few quick calls the Steam client answers from its own cache.
bool steam_friend_avatar(std::uint64_t id, SteamAvatar &out);
// The player's own avatar, large (184 pixels square), for the HUB's profile button. False as above,
// including while Steam is still downloading it.
bool steam_own_avatar(SteamAvatar &out);
// Opens their Steam profile in the Steam overlay. False when it cannot: no Steam session, or the
// player has the overlay turned off.
bool steam_open_profile(std::uint64_t id);
}
