#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Which mod each cosmetic item is from, as the last merge found (Engine/Vfs/mod_item_sources.h):
// the item catalogue's description line and the item card the overlay draws beside the game's.
namespace dingosdk::item_origins {
struct Origin {
    std::string title, author, version;  // title with spaces for underscores
    std::string name;                    // "<title> by <author>"
    std::filesystem::path icon;          // the mod's icon.png (it may not exist)
    std::size_t items{};                 // how many items the mod adds or changes
};
// The mod the item with this key is from, or null. Reads the merge's file again when it changed,
// looking at most every two seconds; cheap otherwise. Any thread.
std::shared_ptr<const Origin> find(std::string_view key);
// Whether the game itself ships the item (a mod only changes it), by the content cache.
bool game_item(std::string_view key);
// A mod's item's description: three empty lines (no-break spaces, in UTF-8; plain spaces could
// be taken for no description), room at the bottom of the game's item card for the overlay to
// show the mod (UI/Overlay/item_browser_overlay.cpp, draw_mod_card).
inline constexpr std::string_view reserved_description = "\xc2\xa0\n\xc2\xa0\n\xc2\xa0";
} // namespace dingosdk::item_origins
