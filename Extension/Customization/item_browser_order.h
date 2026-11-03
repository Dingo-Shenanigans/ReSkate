#pragma once
#include "outfit_kind.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which tiles of an item grid are shown, and in what order. Pure: no game state, so it is tested on its own.
namespace dingosdk::item_browser {
enum class Filter : std::uint8_t { all, favorites, mods, game };
using Keys = std::set<std::string, std::less<>>;
inline constexpr std::string_view filter_names[]{"All", "Favorites", "Mods", "skate."}; // skate.: the game's own
// The pickers' Sort. Game order is the game's own (the worn item, then by name).
enum class Sort : std::uint8_t { game, name, name_descending, pack, author };
inline constexpr std::string_view sort_names[]{"Game order", "Name A-Z", "Name Z-A", "Mod pack", "Author"};

struct Item {
    std::string key, title, category;
    bool modded{};
    std::uint64_t content{}; // the tile's native item model
    std::string mod;         // mod_words of the mod that adds or changes it; empty when none is known
    std::string author, pack, pack_title; // that mod's author, folder under Mods and name; empty when none is known
};

// What the pickers' Author, Mod pack and Sort ask for. Empty author or pack: any.
struct Narrow {
    std::string author, pack; // pack: the mod's folder
    Sort sort{};
};

// Which picker a grid is, by its items' category: clothes and accessories, the board's parts,
// tattoos, gestures, or the skater's appearance (the body's own parts: hair, makeup, skin...); none
// for what is none of these, where the pickers' filters are not offered. Each keeps its own filters.
enum class Picker : std::uint8_t { none, clothing, board, tattoos, gestures, appearance };
inline constexpr std::string_view picker_names[]{"none", "clothing", "board", "tattoos", "gestures", "appearance"};
inline constexpr std::size_t picker_count = std::size(picker_names);
// One category's kind. The game's clothing categories are "cust_" and the slot ("cust_tops",
// "cust_hats", "cust_bottoms", "cust_shoes", "cust_socks", "cust_handwear", "cust_eyewear",
// "cust_earwear", "cust_face_accessory", "cust_outfits", "cust_fullbodycostume"), which outfit_kind
// takes for the body's own parts ("custhead", "custbodies"): the prefix goes first, and what
// outfit_kind leaves out as the body's is the appearance. Unknown: nullopt.
inline std::optional<Picker> picker_kind(std::string_view category) {
    std::string folded(category);
    for (auto& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (folded.empty()) return std::nullopt;
    const auto has = [&](const char* word) { return folded.find(word) != std::string::npos; };
    for (const auto word : {"board", "deck", "truck", "wheel", "grip", "sticker", "decorator"})
        if (has(word)) return Picker::board;
    if (has("tattoo")) return Picker::tattoos;
    if (has("gesture") || has("emote")) return Picker::gestures;
    for (const auto word : {"voice", "locomotion", "bkobject"})
        if (has(word)) return Picker::none;
    for (const auto word : {"hair", "beard", "stubble", "makeup", "freckle", "acne", "vitiligo", "maturity", "mole", "skin", "scar",
                            "lipcolo", "eyecolo", "eye_colo"})
        if (has(word)) return Picker::appearance;
    if (folded.starts_with("cust_")) folded.erase(0, 5);
    // Worn on the body somewhere: eyewear, earwear, nosewear, eyebrowwear, handwear, a face
    // accessory, an outfit.
    for (const auto word : {"wear", "accessor", "costume", "outfit"})
        if (has(word)) return Picker::clothing;
    const auto kind = outfit_kind::of(folded);
    // What outfit_kind leaves out here is the body's own (custhead, custbodies, brows, nose, teeth).
    if (!kind) return Picker::appearance;
    return kind->order < 20 ? std::optional(Picker::clothing) : std::nullopt; // a name it only made readable
}
// The grid's kind: the one most of its items with a known category have (a tie goes to the first,
// from clothing on), none only when more of them are none than any one kind.
inline Picker picker_of(std::span<const Item> native) {
    std::array<std::size_t, picker_count> votes{};
    for (const auto& item : native)
        if (const auto kind = picker_kind(item.category)) ++votes[static_cast<std::size_t>(*kind)];
    std::size_t best = 1;
    for (std::size_t i = 2; i < picker_count; ++i)
        if (votes[i] > votes[best]) best = i;
    if (!votes[best] || votes[0] > votes[best]) return Picker::none;
    return static_cast<Picker>(best);
}
// The category most of the grid's items have (for the log line when a grid's picker is decided).
inline std::string main_category(std::span<const Item> native) {
    std::map<std::string_view, std::size_t> counts;
    for (const auto& item : native)
        if (!item.category.empty()) ++counts[item.category];
    const auto most = std::ranges::max_element(counts, {}, [](const auto& entry) { return entry.second; });
    return most == counts.end() ? std::string() : std::string(most->first);
}

inline std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

// What the search finds an item's mod by: its author, its name and its folder under Mods, folded once
// when the grid is read (item_origins::find), so a keystroke only looks in the string.
inline std::string mod_words(std::string_view author, std::string_view title, std::string_view folder) {
    std::string words;
    for (const auto part : {author, title, folder}) {
        if (part.empty()) continue;
        if (!words.empty()) words += '\n';
        words += folded(part);
    }
    return words;
}

// Every word typed must be in the item's name, or in its mod's author, name or folder (Item::mod). From
// three letters on a word may also be in the item's asset name, where the brand of a stock item is
// ("Own_ShoeSneaker_Vans_..."); shorter ones would match the "Own_" every asset name starts with.
inline bool matches(const Item& item, std::string_view search) {
    const auto title = folded(item.title);
    auto key = folded(item.key);
    if (key.starts_with("own_")) key.erase(0, 4);
    std::replace(key.begin(), key.end(), '_', ' ');
    const auto text = folded(search);
    for (std::size_t at = 0; at < text.size();) {
        const auto end = std::min(text.find(' ', at), text.size());
        const std::string_view word(text.data() + at, end - at);
        at = end + 1;
        if (word.empty()) continue;
        if (title.find(word) == std::string::npos && item.mod.find(word) == std::string::npos &&
            (word.size() < 3 || key.find(word) == std::string::npos))
            return false;
    }
    return true;
}

// The game lists the item being worn first and the rest by name. When the first tile is out of that
// order it is the worn one, and it keeps its place ahead of the favorites.
inline bool worn_first(std::span<const Item> native) {
    return native.size() >= 2 && folded(native[0].title) > folded(native[1].title);
}

// How many tiles at the front of a grid show no item: the "none" tile that takes an accessory off.
inline unsigned leading_blanks(std::span<const Item> native) {
    unsigned count = 0;
    while (count < native.size() && native[count].key.empty()) ++count;
    return count;
}

// `indices` (into `native`, in the game's order) in the order `sort` asks for; the game's order where
// two are alike. By pack or author, items with no known mod go last; by author, then by pack.
inline void sort_items(std::span<const Item> native, std::span<unsigned> indices, Sort sort) {
    if (sort == Sort::game || indices.size() < 2) return;
    std::vector<std::pair<std::string, unsigned>> keyed;
    keyed.reserve(indices.size());
    for (const auto i : indices) {
        const auto& item = native[i];
        std::string key;
        if (sort == Sort::name || sort == Sort::name_descending) key = folded(item.title);
        else if (sort == Sort::pack) key = item.pack.empty() ? std::string() : folded(item.pack_title);
        else if (!item.author.empty()) key = folded(item.author) + '\n' + folded(item.pack_title);
        keyed.emplace_back(std::move(key), i);
    }
    const bool last_empty = sort == Sort::pack || sort == Sort::author;
    std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
        if (last_empty && a.first.empty() != b.first.empty()) return b.first.empty();
        return sort == Sort::name_descending ? a.first > b.first : a.first < b.first;
    });
    for (std::size_t i = 0; i < keyed.size(); ++i) indices[i] = keyed[i].second;
}

// The mods with items in a grid, for the Mod pack choices: one each, by name. And their authors, for
// the Author choices: one each, by name, none for a mod that names none.
struct Pack {
    std::string folder, title, author;
};
inline std::vector<Pack> packs_in(std::span<const Item> native) {
    std::vector<Pack> packs;
    for (const auto& item : native)
        if (!item.pack.empty() && std::none_of(packs.begin(), packs.end(), [&](const Pack& p) { return p.folder == item.pack; }))
            packs.push_back({item.pack, item.pack_title.empty() ? item.pack : item.pack_title, item.author});
    std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return folded(a.title) < folded(b.title); });
    return packs;
}
inline std::vector<std::string> authors_of(std::span<const Pack> packs) {
    std::vector<std::string> authors;
    for (const auto& pack : packs)
        if (!pack.author.empty() && std::find(authors.begin(), authors.end(), pack.author) == authors.end()) authors.push_back(pack.author);
    std::stable_sort(authors.begin(), authors.end(), [](const std::string& a, const std::string& b) { return folded(a) < folded(b); });
    return authors;
}

// Indices into `native` (the game's own order): the tiles that show no item (they stay where they
// are, in front, whatever is searched for), the worn item, the favorites, then everything else,
// each kept in the game's order or put in the one `narrow` asks for, less what the search, the
// filter and `narrow`'s author and pack leave out. Never empty for a grid that has tiles: a grid
// with nothing in it cannot take the focus back.
inline std::vector<unsigned> arrange(std::span<const Item> native, const std::set<std::string, std::less<>>& favorites,
                                     std::string_view search, Filter filter, const Narrow& narrow = {}) {
    const auto shown = [&](const Item& item) {
        if (filter == Filter::favorites && !favorites.contains(item.key)) return false;
        if (!narrow.author.empty() && item.author != narrow.author) return false;
        if (!narrow.pack.empty() && item.pack != narrow.pack) return false;
        if (filter == Filter::mods && !item.modded) return false;
        if (filter == Filter::game && item.modded) return false;
        return matches(item, search);
    };
    std::vector<unsigned> result;
    result.reserve(native.size());
    const unsigned blanks = leading_blanks(native);
    for (unsigned i = 0; i < blanks; ++i) result.push_back(i);
    const unsigned first = blanks + (worn_first(native.subspan(blanks)) ? 1 : 0);
    if (first > blanks && shown(native[blanks])) result.push_back(blanks);
    for (unsigned pass = 0; pass < 2; ++pass) {
        const auto from = result.size();
        for (unsigned i = first; i < native.size(); ++i)
            if (favorites.contains(native[i].key) == (pass == 0) && shown(native[i])) result.push_back(i);
        sort_items(native, std::span(result).subspan(from), narrow.sort);
    }
    if (result.empty() && !native.empty()) result.push_back(0);
    return result;
}
} // namespace dingosdk::item_browser
