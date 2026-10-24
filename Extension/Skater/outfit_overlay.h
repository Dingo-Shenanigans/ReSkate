#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Items worn over the local skater's outfit, for plugins (reskate.outfit): the way Road Rash wore
// its marks before it became a plugin, as one service every plugin shares. Each
// owner puts an item in a slot of the skater's recipe (by the slot's name: "tattoo_legl", "moles",
// "eyewear"); the skater wears them over the outfit through the game's own recipe copy, and is given
// back to the game while one of its menus is up (the menus change the outfit), when nothing is worn,
// and when a plugin stops. Only the face and skin layers (tattoos, moles), eyewear and hand wear (the
// game has no menu for it: a camera in the hand), and never a paid store item. Others see what the
// skater wears. Client thread throughout.
namespace dingosdk::outfit_overlay {
// The slots items may go in: a slot's name holds one of these.
bool allowed_slot(std::string_view slot) noexcept;
// `placement`, when given, is written over the item's first parameters (as floats): where a tattoo
// sits. `covers`: it goes over the player's own item in the slot (else only over an empty one).
// Empty when it is taken, otherwise why not.
std::string set(std::string owner, std::string slot, std::string item, std::vector<float> placement, bool covers);
void clear(std::string_view owner, std::string_view slot = {});
// The installed cosmetic whose name ends in `ending` (any case), or empty.
std::string find_item(std::string_view ending);
struct Slot {
    std::string name, item;
    std::vector<float> parameters;
};
// The skater's recipe slots now; empty without a skater.
std::vector<Slot> slots();
// Items worn on top of the outfit: more than one where the outfit has one (a second chain, rings on
// both hands). Each goes into a slot the outfit leaves empty, its own kind's when that one is free,
// else one of the accessory slots, then the tattoo and face slots; the catalog item is given that
// slot's category besides its own, in memory, for this run. An item never tried in a slot goes on
// alone, and comes off for the next slot if the game describes a failed load within 0.9 s; what
// loads where is kept in ReSkate.layer_slots.txt beside Skate.exe. Never a paid store item. `items`
// replaces the owner's; empty takes them off. Empty when taken, otherwise why not.
std::string set_layers(std::string owner, std::vector<std::string> items);
struct Layers {
    std::vector<std::string> items, worn; // the owner's, and those of them on the skater now
    std::string note;                     // why some are not, when they are not
};
Layers layers(std::string_view owner);
// Each client tick.
void tick() noexcept;
std::string status();
}
