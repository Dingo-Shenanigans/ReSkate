#pragma once
#include <cstdint>
#include <string_view>

// The "Steam picture" player card icon: a card wearing it shows its player's Steam profile picture.
namespace dingosdk::steam_avatar_cards {
// The icon: one of the game's developer card icons (free, so other players' games show it too),
// listed in the menus as "Steam picture" and owned by everyone while the feature is on.
inline constexpr std::string_view icon_key = "own_pcard_icon_dev_00001";
// The title starts with a space, which shows as nothing and sorts before any letter: the icon comes
// first where the menus list items by title.
inline constexpr std::string_view icon_title = " Steam picture";
inline constexpr std::string_view icon_description = "Your Steam profile picture.";
// The launcher's switch (Settings, "Steam pictures on player cards"; --no-steam-card-pictures). Off,
// the icon is the plain developer icon it is made from and no card is changed.
bool enabled() noexcept;
// On the client thread: every player card in the game's menus wearing the icon, whose player has a
// Steam account, gets that account's picture in the icon's place (the card's native web image).
void tick(std::uintptr_t base) noexcept;
}
