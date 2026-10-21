#pragma once
#include "turnaround_image.h"
#include <string>
#include <string_view>

// The optional "parts used" card drawn beside the board in a turnaround: a heading, then each
// part's slot and item name, in the menu's fonts.
namespace dingosdk::turnaround {
struct Part {
    std::string slot; // "Deck", "Grip tape", "Trucks", "Wheels", ...
    std::string name; // the item's title as the game shows it (UTF-8)
};
// TrueType font files (the menu's: ReSkate builds them in as FONT_HEADING / FONT_BODY / FONT_BRUSH).
struct PanelFonts {
    std::span<const std::uint8_t> heading; // the item names
    std::span<const std::uint8_t> body;    // the slot labels
    std::span<const std::uint8_t> brush;   // the title, in the game's brush lettering
};
// A card `height` pixels tall (and about three quarters as wide), on a dark, mostly opaque rounded
// panel, so it reads over a transparent background as well as a matte.
Image render_parts_panel(std::string_view title, std::span<const Part> parts, std::uint32_t height,
                         const PanelFonts& fonts);
} // namespace dingosdk::turnaround
