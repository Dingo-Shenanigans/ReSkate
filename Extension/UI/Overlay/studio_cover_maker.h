#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The Animator Studio's pack covers (the MARKET's cover designer): a cover made from the shop's own pieces - the skater
// or a texture behind it (or the player's picture), the pack's name on a torn banner in one of the shop's fonts, a tag
// stuck in a corner - or the player's picture as it is. Drawn on the CPU off the render thread, from the art and fonts
// built into ReSkate.dll, the same way the mockup (contrib/animator-studio/market_mockup) draws it.
namespace dingosdk::overlay {
struct CoverDesign {
    std::string base{"skater"};  // skater, print, grip, concrete, paper, blue, own
    std::string own;             // the player's picture (UTF-8 path), for "own" and as_is
    bool as_is{};                // the player's picture as it is, nothing put on it
    std::string title;
    std::string font{"anton"};   // anton, bebas, dirt, sedgwick, knewave
    std::string banner{"blue"};  // blue, white, grip, tape, none
    std::string place{"bottom"}; // top, middle, bottom
    std::string tag;             // the tag's words, "" none
    std::uint32_t seed{3};       // which way the paper tore
};
// The cover, side x side, RGBA.
std::vector<std::uint8_t> make_cover(const CoverDesign& design, int side);
// As a PNG ("" when it could not be made).
std::string cover_png(const std::vector<std::uint8_t>& rgba, int side);
// Lets go of the pictures read while covers were being made.
void release_cover_art();
// The design as JSON (kept with its pack) and back.
std::string cover_design_json(const CoverDesign& design);
bool read_cover_design(const std::string& json, CoverDesign& out);
} // namespace dingosdk::overlay
