#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
    // A whole costume, worn instead of clothes (its asset is one of the game's full-body
    // costumes). Its shape is the costume, whatever looks it also names.
    bool costume{};
};
// Empty when the document is not an item (an asset with a Key and ItemData).
[[nodiscard]] std::optional<ItemContent> item_content(const frostbite::ebx::Document& document);

// The game's own items, which a mod's are held against.
class StoreItems final {
public:
    // `sold`: the store sells it, so ReSkate never hands it out.
    void add(const ItemContent& item, bool sold);
    // The key of the store item `item` is a copy of, or empty. It is one when
    // it has a look only a store item has, when it is fitted on a shape only
    // a store item with no look has (a whole costume, with or without a look
    // of the mod's own over it), or when its item data is a store item's
    // field for field.
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

// A costume is its mesh more than anything it names, and a mesh can be shipped again under any
// name, whole or in part, in an item or beside one. So the meshes a mod ships are also held
// against the geometry of the costumes the store sells: where their vertices are. A vertex is
// told by its place to half a millimetre.
[[nodiscard]] std::uint64_t mesh_point(const std::array<float, 3>& position) noexcept;
// One level of detail of a mesh, as the places of its vertices: sorted, each once.
[[nodiscard]] std::vector<std::uint64_t> mesh_points(const std::vector<std::array<float, 3>>& positions);

class StoreMeshes final {
public:
    // One level of detail of one of the game's meshes. `key`: the costume's item; any other
    // mesh of the game's (clothes, a free costume) is added as not sold, under any key.
    void add(std::string_view key, bool sold, const std::vector<std::uint64_t>& points);
    [[nodiscard]] bool empty() const noexcept { return owners_.empty(); }
    // The key of the store costume a mesh with these points takes from, or empty. It does when
    // enough of them (shared_least) are points only that one costume has, and they are a tenth
    // of that costume's finest level or three tenths of the mesh. A point two of the game's
    // meshes have is nobody's: the body under two costumes, the socks one wears that are also
    // an item anyone has.
    [[nodiscard]] std::string original(const std::vector<std::uint64_t>& points) const;
    static constexpr std::size_t shared_least = 300;
    // For a report: the store costume most of these points are only in, and how many are, whether
    // or not that is enough to call the mesh taken from it (empty key: none are any costume's).
    struct Closest {
        std::string key;
        std::size_t shared{}, costume_points{};
    };
    [[nodiscard]] Closest closest(const std::vector<std::uint64_t>& points) const;

private:
    static constexpr std::uint32_t nobody = 0xffffffffU;
    struct Owner {
        std::string key;
        bool sold{};
        std::size_t largest{};  // its finest level of detail, in points
    };
    std::vector<Owner> owners_;
    std::map<std::string, std::uint32_t, std::less<>> by_key_;
    std::map<std::uint64_t, std::uint32_t> points_;  // -> its one owner, or nobody
};

struct StoreCopies {
    // Lower-case key of each copy -> the lower-case key of the store item it copies.
    std::map<std::string, std::string, std::less<>> items;
    // The mods that add them, highest priority first.
    struct Source {
        std::string mod;
        std::size_t count{};
        std::string example, original;  // one of its copies and what that copies, as written
        // How that one was found: its mesh has the costume's geometry, or its item is fitted on
        // what the costume is (its shape, or a store item's looks or data).
        bool by_mesh{};
    };
    std::vector<Source> mods;
    // Every mesh level a mod ships and how much of a store costume's geometry is in it, for a
    // report of the check (the Paid Cosmetics Detector, the test tool): a copy or not.
    struct MeshSeen {
        std::string mod, mesh;
        std::size_t points{};
        StoreMeshes::Closest closest;
    };
    std::vector<MeshSeen> meshes_seen;
    // How many of the game's own items the mods' were held against; none when no mod adds an item.
    std::size_t game_items{};
    // And how many of the mods' meshes against how many of the game's costumes.
    std::size_t mod_meshes{}, game_costumes{};
};

// Whether the store sells the item with this lower-case key.
using StoreItem = std::function<bool(const std::string&)>;

// Reads the items every enabled mod adds to (or changes in) the game's bundles
// and holds them against the game's own. The game's are only read when a mod
// has an item at all. Never throws: a bundle that cannot be read is left out
// (the merge leaves its mod out of the game too) and noted in `notes`.
// The game's own files are read on up to `threads` threads beside the caller's
// (`background`: at a priority under the game's, for a merge while it runs); the verdicts do
// not depend on the count.
[[nodiscard]] StoreCopies check_store_copies(const Catalog& catalog, const StoreItem& sold,
                                             std::vector<std::string>* notes = nullptr, std::size_t threads = 0,
                                             bool background = false) noexcept;

} // namespace dingosdk::mods
