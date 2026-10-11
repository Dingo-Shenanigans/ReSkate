#pragma once
#include "item_browser_order.h"
#include <cstdint>
#include <string>
#include <string_view>

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
};
View view();
// Requests from the keys, the overlay and the console; applied on the client thread.
void toggle_focused_favorite() noexcept;
void set_search(std::string text);
void set_filter(Filter filter) noexcept;
void cycle_filter() noexcept;
// The table the overlay's search bar and keys use (process lifetime).
const overlay::ItemBrowserHost* overlay_host() noexcept;
// Console research aids; their answers go to the log.
void request_status() noexcept;
void request_poke(std::string arguments);

// Client thread, from the customization update, with the profile's native mutex held.
void update(std::uintptr_t base) noexcept;
void before_level_transition(unsigned next, std::uintptr_t base) noexcept;
} // namespace dingosdk::item_browser
