#pragma once
#include "item_browser_order.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Favorites, search and filters for the skater's item grids (Apparel, Accessories, Board, Tattoos,
// Gestures, Appearance). The grids stay the game's own: their tile rows are reordered and filtered in
// place, the way the music screen's Mods shelf filters playlist tiles (contrib/native-music-ui.md).
namespace dingosdk::overlay { struct ItemBrowserHost; }
namespace dingosdk::item_browser {
// What the overlay draws: read every presented frame (thread-safe, cheap).
struct View {
    bool open{};                            // an item grid has the focus
    std::string search;
    Filter filter{};
    unsigned shown{}, total{}, favorites{}; // of the grid that has the focus
    std::string focused_title;
    bool focused_favorite{};
    // The player's collections (by name) and whether the highlighted item is in each.
    std::vector<std::string> collections;
    std::vector<bool> focused_in;
    std::string filter_name;                // what the filter shows, a collection's name included
    bool focused_layer{};                   // the highlighted item is worn on top
    unsigned layers{}, layers_worn{};       // items to wear on top, and on the skater now
    std::string layers_note;
    bool custom{};                          // the player's own tabs are shown in place of the game's
    std::size_t tab{};                      // which of them is open
    std::vector<std::string> icons;         // each collection's tab icon
    bool tab_empty{};                       // the open one of them has nothing in it yet
};
View view();
// Requests from the keys, the overlay and the console; applied on the client thread.
void toggle_focused_favorite() noexcept;
void set_search(std::string text);
void set_filter(Filter filter) noexcept;
void cycle_filter() noexcept;
// The highlighted item into the collection that `typed` names (its name, or its number from 1), or out
// of it when it is in it already; a name no collection has makes a new one.
void pick_collection(std::string typed);
// The highlighted item worn on top of the outfit, or taken off again (Extension/OutfitLayers).
void toggle_focused_layer() noexcept;
// The player's own tabs (their collections, each with an icon) in place of the game's, in the item
// grids: on or off, the next or previous one, one by its number. Thread-safe.
void toggle_custom_tabs() noexcept;
void step_tab(int steps) noexcept;
void select_tab(int index) noexcept;
// Editing them (the tab editor in the overlay). make_tab answers the new tab's number (or the one of that
// name), -1 for a name that cannot be one.
int make_tab(std::string name, std::string icon);
void rename_tab(int index, std::string name);
void set_tab_icon(int index, std::string icon);
void remove_tab(int index);
void move_tab(int index, int by);
// The same from the console, thread-safe; each answers in a line for the console.
std::string list_collections();
std::string make_collection(std::string name);
std::string delete_collection(std::string name);
std::string wear_collection(std::string name); // every item of a collection on top
std::string show_collection(std::string name); // the item grids show only that collection
std::string wear_layer(std::string item, bool on);
std::string clear_layers();
// The table the overlay's search bar and keys use (process lifetime).
const overlay::ItemBrowserHost* overlay_host() noexcept;
// Console research aids; their answers go to the log.
void request_status() noexcept;
void request_poke(std::string arguments);

// Client thread, from the customization update, with the profile's native mutex held.
void update(std::uintptr_t base) noexcept;
void before_level_transition(unsigned next, std::uintptr_t base) noexcept;
} // namespace dingosdk::item_browser
