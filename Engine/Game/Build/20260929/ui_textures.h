#pragma once
#include <string_view>

namespace dingosdk::game::build::v20260929::ui_textures {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// Icons, logos and UI shapes skate. ships, read from the installed game's data for the overlay
// (Engine/Vfs/game_textures.h): where each texture is, by superbundle TOC, bundle and name.
// Found with a scan of every texture the game ships (78,566 in 6,056 bundles) on 2026-10-06.

// A part of a picture, as fractions of its width and height.
struct Region {
    float left{}, top{}, right{1}, bottom{1};
};
struct Texture {
    std::string_view toc, bundle, name;
    Region region{}; // the part of the texture that is the picture
    // A shape's: the part of the picture a box is laid on (a brush stroke's bar, without its
    // splatter), and how much of each edge a panel keeps unstretched at any size (nine-slice), as a
    // fraction of the picture's width and height.
    Region body{};
    float slice{};
};

// skate.'s scoring icons: white on clear, 64 pixels (the wipeout's broken one 68 x 64).
inline constexpr std::string_view scoring_toc = "Win32/globals.toc";
inline constexpr std::string_view scoring_bundle = "win32/configurations/gameconfigurations/delmargameconfiguration";
inline constexpr Texture airtime{scoring_toc, scoring_bundle, "ui/textures/icons/scoring/img_icon_scoring_airtime_64_64_64"};
inline constexpr Texture wipeout{scoring_toc, scoring_bundle, "ui/textures/icons/scoring/img_icon_scoring_wipeout_64_64_64"};
inline constexpr Texture wipeout_broken{scoring_toc, scoring_bundle,
    "ui/textures/icons/scoring/img_icon_scoring_wipeout_broken_64_68_64"};
inline constexpr Texture spread_eagle{scoring_toc, scoring_bundle, "ui/textures/icons/scoring/img_icon_scoring_spreadeagle_64_64_64"};
inline constexpr Texture gap_height{scoring_toc, scoring_bundle, "ui/textures/icons/scoring/img_icon_scoring_gapheight_64_64_64"};
inline constexpr Texture roll{scoring_toc, scoring_bundle, "ui/textures/icons/scoring/img_icon_scoring_roll_64_64_64"};

// The game's level roots, whose bundles hold most of its UI textures.
inline constexpr std::string_view root_toc = "Win32/levels/game/dingolevel_root/dingolevel_root.toc";
inline constexpr std::string_view root_bundle = "win32/levels/game/dingolevel_root/dingolevel_root";
inline constexpr std::string_view bam_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
inline constexpr std::string_view bam_bundle = "win32/levels/game/bam_levelroot/bam_levelroot";

// The stopwatch of the game's timers, white on clear.
inline constexpr Texture stopwatch{root_toc, root_bundle, "ui/textures/icons/generic/img_generic_stopwatch_64_64_64"};
// The speed line challenge's flaming wheel: white and black on clear.
inline constexpr Texture flaming_wheel{bam_toc, bam_bundle,
    "ui/textures/icons/activities/challenges/icon/img_icon_speedlinechallenge_256_128_128"};

// The THRASHER wordmark: the THRASHER / SKATEBOARD MAGAZINE logo of the Thrasher tee (512 x 256, grey on
// clear), its wordmark alone (pixels 13 to 498 across, 39 to 192 down; the subtitle is below). Drawn in
// true proportion, unlike the logos squeezed for a garment. Streamed: its bundle keeps only the small mips.
inline constexpr Texture thrasher_wordmark{"Win32/items.toc",
    "win32/characters/maincharacters/generic/cas/clothing/licensed/thrasher/apparel/top/shirt/tshirtrelaxed/2023/colorways/"
    "thrasher_shirt_tshirtrelaxed_00004_ap_cas_main_bundlereftable",
    "characters/materials/logo/licensed/thrasher/logo_thrasher_2x1_011_co", {13.0f / 512, 39.0f / 256, 499.0f / 512, 193.0f / 256}};
// The wordmark arches: between a fifth in from either side its letters end above this, as a fraction
// of its height (at 55% in the middle, 69% a fifth in), leaving room beneath the arch.
inline constexpr float thrasher_wordmark_arch = 0.70f;

// skate.'s UI shapes, the stuff its menus are built from: white on clear, tinted as they are drawn.
// A tile with rough, hand-cut edges (256 x 256): about 16 pixels of each edge are rough.
inline constexpr Texture rough_tile{root_toc, root_bundle, "ui/textures/common/tiles/img_tilebackground_roughfull_256_256_256",
    {}, {}, 32.0f / 256};
// A film's scratches and dust, light on black (2048 x 2048, opaque): drawn by their brightness over a panel.
inline constexpr Texture scratches{root_toc, root_bundle,
    "ui/textures/common/backgrounds/img_background_slicedgrainytexture01_2048_2048_2048"};
// A short brush stroke with splatter (692 x 260): the bar is pixels 68 to 610 across, 79 to 184 down.
inline constexpr Texture brush_bar{bam_toc, bam_bundle, "ui/textures/common/banner/img_bannerbrush_small_692_692_260",
    {}, {68.0f / 692, 79.0f / 260, 611.0f / 692, 185.0f / 260}};
// A long brush streak, thick on the left and tapering right (1024 x 152): pixels 8 to 1013 across, 30 to 115 down.
inline constexpr Texture streak{root_toc, root_bundle, "ui/textures/common/brushs/img_brush_splashstreak_1024_1024_152",
    {}, {8.0f / 1024, 30.0f / 152, 1014.0f / 1024, 116.0f / 152}};
}
