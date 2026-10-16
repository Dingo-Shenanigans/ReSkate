#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Outfit layers: more than one item at a time where the game's outfit has one (a hoodie over a tee, a
// second chain, rings on both hands). The game builds the skater from a recipe of fixed slots and only
// draws an item from a slot of the item's own category. An item worn "on top" goes into a slot the
// outfit leaves empty: its own kind's when that one is free, else one of the accessory slots, and the
// catalog item is given that slot's category besides its own (in memory, for this run of the game).
// The world skater then wears the outfit with the layers in a recipe of its own, the way Road Rash puts
// its marks on; while a game menu is up the skater is the game's again, so what is chosen there reaches
// it, and the layers go back on over the outfit as it is by then.
namespace dingosdk::outfit_layers {
// The items to wear on top, by their catalog names, in order. Any thread; used at the next client tick.
void set_layers(std::vector<std::string> items);
std::vector<std::string> layers();
struct Status {
    unsigned wanted{}, worn{}; // items asked for, and on the skater now
    std::string note;          // why some are not, when they are not
};
Status status();

bool start(std::uintptr_t base) noexcept;
// Client thread, every client tick.
void on_client_tick(std::uintptr_t client) noexcept;

// Console research aids, on the game thread, on the local skater.
// The skater's recipe template: each slot's number, name, slot id, category and what is in it.
std::string slots();
// Installed cosmetics whose name has `part` in it, with their categories.
std::string find(std::string_view part);
// The installed item whose name is `part` (or has it in its name), or empty.
std::string item_named(std::string_view part);
}
