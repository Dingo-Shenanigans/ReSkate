#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Which mod each cosmetic item came from, so its card in the customization menu can say so:
// Mods/.reskate/reskate-items.json. The merge writes it from the items the store-copy check reads
// in every mod (mod_store_copies.h); the game reads it when it builds its item catalogue.
namespace dingosdk::mods {

inline constexpr char item_sources_file[] = "reskate-items.json";

struct ItemSource {
    std::string mod;                     // its folder name
    std::string title, author, version;  // as its manifest gives them
    std::string folder;                  // where it is, from Mods/ ("x"), with its icon.png
    std::vector<std::string> keys;       // the items it adds or changes, lower case
};

// Highest priority first, as the merge orders the mods: where two list one item, the game uses
// the first one's, so that is where it is from. Empty when there is no file or it cannot be read.
[[nodiscard]] std::vector<ItemSource> read_item_sources(const std::filesystem::path& file) noexcept;
// Throws when the file cannot be written.
void write_item_sources(const std::filesystem::path& file, const std::vector<ItemSource>& sources);

// How a card names the mod: its title (Thunderstore names have underscores for spaces), and who
// made it when its manifest says: "ReSkate FPV Drone by zeex64".
[[nodiscard]] std::string item_source_name(const ItemSource& source);

} // namespace dingosdk::mods
