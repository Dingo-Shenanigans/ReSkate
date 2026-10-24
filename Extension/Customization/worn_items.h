#pragma once
#include <string>
#include <vector>

// What the skater wears now, read from the selected skater preset (game thread): the clothes and
// accessories by the kind of item, from head to foot, and the board's parts; an item's title as the
// game shows it. For plugins (reskate.outfit.worn, board, title); from Fit Check (pull request 326).
namespace dingosdk::worn_items {
struct Worn {
    std::string slot;  // "Headwear", "Top", ... "Shoes"; or "Deck", "Grip tape", "Grip cutout", "Trucks", "Wheels"
    std::string item;  // its key
    std::string title; // as the game shows it
};
std::vector<Worn> worn_outfit();
std::vector<Worn> worn_board();
// Its catalog title, the title an item grid showed this session (a mod's item has no catalog
// entry), or its key made readable the way the game does for a mod's item.
std::string item_title(const std::string& asset);
} // namespace dingosdk::worn_items
