#pragma once

#include "Engine/Resource/texture.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The items a mod adds, read from the mod's own files and the installed game: what the launcher's mod
// manager lists for a player to choose from (mod_catalog.h items_left_out). Nothing the game has to
// have loaded first, so a pack that was just installed lists its items too.
namespace dingosdk::mods {

struct ModItem {
    std::string key;        // lower case, as items_left_out keeps it
    std::string asset;      // the item's asset, lower case: "items/cust_tops/own_my_shirt"
    std::string category;   // the folder of that: "cust_tops", "board_deck", "type_boardsticker"
    // Its thumbnail as the game's item grid shows it, `side` x `side`; empty when the mod has none
    // for it or it could not be read.
    frostbite::Image thumbnail;
};
struct ModItems {
    std::vector<ModItem> items;   // by category, then key; each key once
    std::string problem;          // why the mod could not be read, when it could not
};

// The items `directory` (a mod's folder, or one of its variants') adds to the game's bundles. The
// game's own items a mod changes are not among them: those stay whatever is chosen. `side`: the
// thumbnails' size in pixels, 0 for none. Never throws. Reads a few megabytes of the game's data and
// decodes a texture an item: for a worker thread.
[[nodiscard]] ModItems read_mod_items(const std::filesystem::path& data_root, const std::filesystem::path& directory,
                                      std::uint32_t side) noexcept;

} // namespace dingosdk::mods
