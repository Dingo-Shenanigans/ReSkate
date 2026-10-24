#pragma once
#include "capture_image.h"

// Transparency from two captures of the same still picture, one over a dark background and one over
// a light one ("difference matting"). Where the board covers the background both captures agree;
// where the background shows through they differ by the full background step; part-covered pixels
// (antialiased edges, the holo's see-through bits) differ by part of it. That gives soft edges the
// game's backbuffer cannot: it has no alpha of its own.
//
// The backgrounds do not have to be pure black and white. The game tone-maps and grades them, so
// what they actually look like on screen is measured (`Backgrounds`) and used instead.
namespace dingosdk::capture {
struct Backgrounds {
    std::array<float, 3> dark{}, light{}; // the empty background's colour in each capture, 0..255
};
// The typical (median) colour of the pixels along the edge of `area` in each capture: the background, as long
// as the board is not touching the edge of the area.
Backgrounds measure_backgrounds(const PixelView& dark, const PixelView& light, Rect area) noexcept;
// The board cut out of `area`: straight RGBA, colour taken from the dark capture.
// `floor` is how much difference (0..1 of the background step) is still counted as fully opaque, to
// keep the noise of the game's dithering and film grain from leaving the board faintly see-through.
Image matte_pair(const PixelView& dark, const PixelView& light, Rect area, const Backgrounds& backgrounds,
                 float floor = 0.04f);
// Without a second background to compare against: the board keyed out of every frame of a whole
// turn (opaque captures of the same area) against the backdrop the turn reveals. See the .cpp.
// `solid_rows`: what turns is solid (a board), so any hole it closes all round is part of it. For a
// skater only small ones are: their arms and legs have backdrop between them.
void key_against_backdrop(std::vector<Image>& frames, bool solid_rows = true);
} // namespace dingosdk::capture
