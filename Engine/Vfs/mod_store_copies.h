#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::frostbite::ebx {
struct Document;
}

namespace dingosdk::mods {

struct Catalog;

// ReSkate hands out every cosmetic except the ones the game's store sells, and
// tells those apart by key (content_cache::Catalogs::reserved). A mod that adds
// a store item again under a key of its own gets round that: the copy is in no
// catalogue, so it is handed out like any other mod's item. What an item is
// does not change with its key, though. It still names the store item's
// appearance preset, decal textures, object or gesture, and those are what this
// check holds a mod's items against. A mod that adds such a copy is not loaded
// at all: the merge leaves it out whole (mod_merge.cpp), as it does a mod that
// cannot be merged.

// All such a mod is told, in the launcher, the game and the log (which the
// mod's author reads too): that it could not be merged, and this. Nothing of
// what was looked for or found. The number is how ReSkate's own people tell it
// from a mod that really is damaged; the log names the check by it as well,
// when something could not be read for it.
inline constexpr std::string_view store_copies_problem = "merge error 0x5343";
inline constexpr std::string_view store_copies_check = "merge check 0x5343";

// What an item is made of, whatever it is called. Lower-case; the lists sorted.
struct ItemContent {
    std::string key;  // the item's Key, as written
    // What it looks like or does: appearance presets, decal textures, a
    // player card's picture, an object's prefab and bundle, a gesture.
    std::vector<std::string> looks;
    // The meshes and morph presets it is fitted on. Mods fit looks of their own
    // on the game's shapes, a store item's included, so a shape alone says
    // nothing.
    std::vector<std::string> shapes;
    std::string data;  // a digest of every field of its item data, to compare whole
};
// Empty when the document is not an item (an asset with a Key and ItemData).
[[nodiscard]] std::optional<ItemContent> item_content(const frostbite::ebx::Document& document);

// The game's own items, which a mod's are held against.
class StoreItems final {
public:
    // `sold`: the store sells it, so ReSkate never hands it out.
    void add(const ItemContent& item, bool sold);
    // The key of the store item `item` is a copy of, or empty. It is one when
    // it has a look only a store item has, when it has no look of its own and
    // is fitted on a shape only a store item with no look has (a whole
    // costume), or when its item data is a store item's field for field.
    // Anything an item the store does not sell has as well is nobody's.
    [[nodiscard]] std::string original(const ItemContent& item) const;

private:
    struct Parts {
        std::map<std::string, std::string, std::less<>> sold;  // part -> the store item's key
        std::set<std::string, std::less<>> free;               // parts of the other items
        [[nodiscard]] const std::string* owner(std::string_view part) const;
    };
    Parts looks_, shapes_, data_;
};

struct StoreCopies {
    // Lower-case key of each copy -> the lower-case key of the store item it copies.
    std::map<std::string, std::string, std::less<>> items;
    // The mods that add them, highest priority first.
    struct Source {
        std::string mod;
        std::size_t count{};
        std::string example, original;  // one of its copies and what that copies, as written
    };
    std::vector<Source> mods;
    // How many of the game's own items the mods' were held against; none when no mod adds an item.
    std::size_t game_items{};
};

// Whether the store sells the item with this lower-case key.
using StoreItem = std::function<bool(const std::string&)>;

// Reads the items every enabled mod adds to (or changes in) the game's bundles
// and holds them against the game's own. The game's are only read when a mod
// has an item at all. Never throws: a bundle that cannot be read is left out
// (the merge leaves its mod out of the game too) and noted in `notes`.
[[nodiscard]] StoreCopies check_store_copies(const Catalog& catalog, const StoreItem& sold,
                                             std::vector<std::string>* notes = nullptr) noexcept;

} // namespace dingosdk::mods
