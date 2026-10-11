#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which tiles of an item grid are shown, and in what order. Pure: no game state, so it is tested on its own.
namespace dingosdk::item_browser {
enum class Filter : std::uint8_t { all, favorites, mods, game };
inline constexpr std::string_view filter_names[]{"All", "Favorites", "Mods", "Game items"};

struct Item {
    std::string key, title, category;
    bool modded{};
    std::uint64_t content{}; // the tile's native item model
};

inline std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

// Every word typed must be in the item's name. From three letters on a word may also be in the item's
// asset name, where the brand of a stock item is ("Own_ShoeSneaker_Vans_..."); shorter ones would match
// the "Own_" every asset name starts with.
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
        if (title.find(word) == std::string::npos && (word.size() < 3 || key.find(word) == std::string::npos)) return false;
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

// Indices into `native` (the game's own order): the tiles that show no item (they stay where they
// are, in front, whatever is searched for), the worn item, the favorites, then everything else,
// each kept in the game's order, less what the search and the filter leave out. Never empty for a
// grid that has tiles: a grid with nothing in it cannot take the focus back.
inline std::vector<unsigned> arrange(std::span<const Item> native, const std::set<std::string, std::less<>>& favorites,
                                     std::string_view search, Filter filter) {
    const auto shown = [&](const Item& item) {
        if (filter == Filter::favorites && !favorites.contains(item.key)) return false;
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
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = first; i < native.size(); ++i)
            if (favorites.contains(native[i].key) == (pass == 0) && shown(native[i])) result.push_back(i);
    if (result.empty() && !native.empty()) result.push_back(0);
    return result;
}
} // namespace dingosdk::item_browser
