#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// The glyph pages of the texts plugins draw in fonts of their own (plugin_fonts.cpp), as far as they
// are sums: the size a glyph is made at for a text of some height, where a glyph's box goes on a
// page, and which pages make way when the pages together are too many. Nothing here touches a font or
// the GPU, so the plugin tests run it as it is.
namespace dingosdk::overlay::glyph_pages {
// A page is this wide and grows downward, doubling, from the least height to the most.
inline constexpr int page_width = 1024, least_page_height = 64, most_page_height = 1024;
// A font has at most this many pages, for all its sizes together: when the last is full the one
// drawn from longest ago goes, and its glyphs are made again when they are next drawn.
inline constexpr std::size_t most_pages_a_font = 8;
// What all plugins' pages may hold on the GPU together, as RGBA (four bytes a pixel).
inline constexpr std::size_t most_bytes = 64u << 20;
inline constexpr std::size_t bytes_of(int height) { return static_cast<std::size_t>(page_width) * static_cast<std::size_t>(height) * 4; }

// The height glyphs are made at for a text `pixels` high: the next of every 2 pixels up to 32, every
// 4 up to 96 and every 8 up to 256. The glyph is then drawn scaled down to the text's own height (by
// a fifth at the very most), so a text of any size is sharp without pages for every size. A text
// taller than 256 pixels is stretched from the 256, and one under 8 shrunk from the 8.
inline constexpr int least_size = 8, most_size = 256;
constexpr int size_for(float pixels) {
    if (!(pixels > static_cast<float>(least_size))) return least_size;
    if (pixels >= static_cast<float>(most_size)) return most_size;
    int whole = static_cast<int>(pixels);
    if (static_cast<float>(whole) < pixels) ++whole;
    const int step = whole <= 32 ? 2 : whole <= 96 ? 4 : 8;
    return (whole + step - 1) / step * step;
}

// Where glyph boxes go on a page: in shelves, left to right, each as tall as its tallest box, with a
// pixel clear around every box so that a glyph drawn scaled does not pick up its neighbour.
struct Shelves {
    int height{least_page_height}; // the page's, as tall as its shelves have needed so far
    int x{1}, y{1}, shelf{};       // the open shelf: where its next box goes, and how tall it is
    struct Place {
        int x{}, y{};
        int height{}; // the page's once the box is on it
    };
    // Where a box of `wide` x `tall` pixels would go, or nothing when the page has no room for it.
    std::optional<Place> find(int wide, int tall) const {
        if (wide <= 0 || tall <= 0 || wide + 2 > page_width || tall + 2 > most_page_height) return std::nullopt;
        Place place{x, y, height};
        if (place.x + wide + 1 > page_width) { // a new shelf under the open one
            place.x = 1;
            place.y = y + shelf + 1;
        }
        if (place.y + tall + 1 > most_page_height) return std::nullopt;
        while (place.y + tall + 1 > place.height) place.height *= 2;
        return place;
    }
    // The box takes the place find gave for it.
    void take(const Place& place, int wide, int tall) {
        if (place.y != y) shelf = 0;
        x = place.x + wide + 1;
        y = place.y;
        shelf = std::max(shelf, tall);
        height = place.height;
    }
};

// A page as the budget sees it: what it holds on the GPU, and when it was last drawn from.
struct Held {
    std::size_t bytes{};
    double used{};
};
// The pages to let go so that `more` bytes fit under `most` with those that stay: the ones drawn from
// longest ago first, and never one drawn from at or after `busy` (the frame before this one). Empty
// when there is room as it is; nothing when there cannot be.
inline std::optional<std::vector<std::size_t>> make_room(std::span<const Held> pages, std::size_t more, std::size_t most, double busy) {
    std::size_t held = 0;
    for (const auto& page : pages) held += page.bytes;
    std::vector<std::size_t> idle, go;
    for (std::size_t i = 0; i < pages.size(); ++i)
        if (pages[i].used < busy) idle.push_back(i);
    std::sort(idle.begin(), idle.end(), [&](std::size_t a, std::size_t b) { return pages[a].used != pages[b].used ? pages[a].used < pages[b].used : a < b; });
    for (const auto index : idle) {
        if (held + more <= most) break;
        held -= pages[index].bytes;
        go.push_back(index);
    }
    if (held + more > most) return std::nullopt;
    return go;
}
}
