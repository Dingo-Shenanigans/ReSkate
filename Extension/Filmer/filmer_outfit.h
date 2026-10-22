#pragma once
// The camcorder worn as hand wear (a cosmetic made with ReSkate Studio, filmer_hold.h's Grip): put
// in the skater's hand-wear slot while the arm holds the camera out, and the skater's own item put
// back when it stops, as the drone puts goggles on its pilot (Extension/Skater/drone_outfit.cpp).
// The game has no menu for hand wear, so this is how it is worn. Client thread.
#include <string>
#include <string_view>

namespace dingosdk::filmer::outfit {
// The game's fingerless glove, the donor the camcorder is made from: a Studio mod that changes it
// puts the camcorder in its place.
inline constexpr std::string_view default_item = "Own_HandwearGlove_Gen_GloveFingerless_00001";
// Every client tick: `wanted` while the camcorder should be in the hand; `item` the cosmetic (its full
// name or the end of it, any case; empty: default_item).
// `slot`: a word of the outfit slot it goes in ("hand", "eyewear"; empty: "hand"); the camcorder is
// weighted to the hand whichever slot carries it, so a slot whose items the game draws properly will do.
void tick(bool wanted, std::string_view item, std::string_view slot = {}) noexcept;
// One line for the status: what is worn, or why not.
std::string status();
} // namespace dingosdk::filmer::outfit
