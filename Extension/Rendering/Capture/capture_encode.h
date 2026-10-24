#pragma once
#include "capture_image.h"
#include <optional>
#include <span>

// File formats for a screen capture (reskate.capture). Every frame of an animation is the same size.
namespace dingosdk::capture {
struct Animation {
    std::vector<Image> frames;
    std::uint32_t fps{30};
};

// A still PNG (RGBA).
std::vector<std::uint8_t> encode_png(const Image& image);
// An animated PNG that loops forever. Keeps full 8-bit alpha; flatten the frames over a matte
// (over_matte) first for an opaque one.
std::vector<std::uint8_t> encode_apng(const Animation& animation);

struct GifOptions {
    // GIF transparency is all or nothing. Pixels at least half opaque are kept, blended over this
    // colour so their soft edge does not go black; the rest are cut out. Pick something close to
    // the page the GIF will sit on.
    Rgb fringe{40, 40, 40};
    // A light, fixed (ordered) dither, which does not crawl between frames the way error diffusion does.
    bool dither{true};
};
// An animated GIF that loops forever: one 256-colour palette for every frame.
// GIF timing is in hundredths of a second, so the frame rate is kept on average, at most 50.
std::vector<std::uint8_t> encode_gif(const Animation& animation, const GifOptions& options = {});

struct WebmOptions {
    int quality{12};       // VP8 quantizer index for the colour, 0 (best) .. 127
    int alpha_quality{4};  // and for the transparency, which shows every block edge, so finer
};
// A WebM with VP8 video; frames with any transparency carry it as VP8 alpha (Chrome, Firefox and
// ffmpeg's libvpx decoder show it). WebM has no loop flag: players loop it themselves.
std::vector<std::uint8_t> encode_webm(const Animation& animation, const WebmOptions& options = {});
} // namespace dingosdk::capture
