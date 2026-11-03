#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// What of other players' outfits this PC shows: items from mods can be hidden on everyone, on one
// player, by item or by the mod they come from. A hidden item is shown the way an item this PC does
// not have is (native_cosmetics.cpp): the slot's default item, or nothing. The game's own items are
// only ever hidden by name. Nobody else is affected. Kept in Mods/.reskate-outfit-filter.json.
namespace dingosdk::multiplayer::outfit_filter {
struct Rules {
    bool mod_items = true;                       // off: everyone else wears the game's own items only
    std::set<std::string, std::less<>> items;    // item keys hidden on everyone, lower case
    std::set<std::string, std::less<>> mods;     // mod folders ("Owner-Name") hidden on everyone, lower case
    std::map<std::uint64_t, std::string> players; // shown in the game's own items only, with the name they had
    bool operator==(const Rules &) const = default;
};
// What this PC knows of an item: whether the game ships it, and the folder of the mod it comes
// from or a mod changes (lower case; empty for none).
struct ItemFacts {
    bool game{};
    std::string mod;
};
// Whether `player`'s item `key` is shown as the default instead. An item nobody's rule names shows.
bool hides(const Rules &rules, std::uint64_t player, std::string_view key, const ItemFacts &facts);

// An item key as a name to read, for an item this PC has no title for: "Own_Nike_SB_x_Dunk_-_Worn"
// is "Nike SB x Dunk - Worn". The "Own_" the game puts first (twice, in some mods) goes.
std::string readable_key(std::string_view key);

std::string to_json(const Rules &rules);
Rules from_json(std::string_view text) noexcept;
inline constexpr char rules_file[] = ".reskate-outfit-filter.json";

// The rules in effect: read from the Mods folder the first time they are asked for. Any thread.
std::shared_ptr<const Rules> rules();
// Changes every time the rules do, so an outfit already shown is put on again.
std::uint64_t revision() noexcept;
// Replaces the rules and saves them. False when they could not be saved (they apply all the same).
bool set_rules(Rules rules);
// The facts of an item on this PC (Extension/Customization/item_origins.h). Any thread.
ItemFacts facts(std::string_view key);

// An item a player wears that a mod adds or changes, as their outfit was last put on here.
struct WornItem {
    std::string key, title;
    std::string mod, mod_title; // its mod's folder (lower case) and title; empty when not installed here
    bool installed{}, hidden{};
};
// The session's game thread, when it puts a player's outfit on; the menu reads them.
void note_worn(std::uint64_t player, std::vector<WornItem> items);
std::vector<WornItem> worn(std::uint64_t player);
// The mod folders anyone was seen wearing items of since the last call, lower case.
std::set<std::string, std::less<>> take_seen_mods();
} // namespace dingosdk::multiplayer::outfit_filter
