#pragma once
#include "item_browser_order.h"
#include <cstdint>
#include <optional>
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
    std::string focused_title, focused_key, focused_category;
    std::vector<std::string> grid_keys;     // every item of the grid that has the focus
    bool focused_favorite{};
    bool showing{};                         // a plugin's list is shown in the grid that has the focus (show_items)
    bool held{};                            // that list has nothing in it: the grid keeps one tile only for the game's highlight
};
View view();
// Requests from the keys, the overlay and the console; applied on the client thread.
void toggle_focused_favorite() noexcept;
void set_search(std::string text);
void set_filter(Filter filter) noexcept;
void cycle_filter() noexcept;
// A plugin's own list in the item grid that has the focus: these items (keys), in this order, taken
// from every item grid of the screen (the game keeps them all, one per tab), less what the search
// leaves out; nullopt gives the grid back. One owner at a time: empty when taken, else why not. Any thread.
std::string show_items(std::string owner, std::optional<std::vector<std::string>> keys);
// The game's check mark on these items' tiles, where the rarity icon is (it wins over a favorite's
// star); each owner's own list, replaced. Any thread.
void mark_items(std::string owner, std::vector<std::string> keys);
// Lets go of what `owner` asked for (a plugin that stops). Any thread.
void release_owner(std::string_view owner);
// An item's title as an item grid showed it this session (mods' items too), or empty. Any thread.
std::string known_title(std::string_view key);
// The table the overlay's search bar and keys use (process lifetime).
const overlay::ItemBrowserHost* overlay_host() noexcept;
// Console research aids; their answers go to the log.
void request_status() noexcept;
void request_poke(std::string arguments);

// Client thread, from the customization update, with the profile's native mutex held.
void update(std::uintptr_t base) noexcept;
void before_level_transition(unsigned next, std::uintptr_t base) noexcept;
} // namespace dingosdk::item_browser
