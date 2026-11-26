#pragma once
#include <array>
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
// skate.'s own loading screen's board doing kickflips beside its "Loading..." (the imports of
// UI/Foundations/Loading/LoadScreen/DingoLoadScreen_Widget, found for the Animator Studio, PR 297): a
// flipbook, flip_Skateboard_Kickflip_256, of 27 frames at 30 a second, 9 across and 3 down its texture
// (2304 x 768 for a 2160 screen). Kept with the widget, or in the level roots: tried in that order.
inline constexpr std::string_view loading_board_name =
    "ui/textures/common/flipbooks/loading/flip_skateboard_kickflip_256_win32_"
    "6143a1fc-cfd7-13ed-c764-164f1af9c663_0_2160_c2cf948c-5744-ac69-b92d-360134ba7b45_input";
inline constexpr std::array<Texture, 3> loading_board{{
    {"Win32/globals.toc", "win32/ui/foundations/loading/loadscreen/dingoloadscreen_widget_bundle", loading_board_name},
    {root_toc, root_bundle, loading_board_name},
    {bam_toc, "win32/levels/game/bam_levelroot/bam_levelroot", loading_board_name},
}};
inline constexpr int loading_board_frames = 27, loading_board_columns = 9, loading_board_rows = 3;
// A film's scratches and dust, light on black (2048 x 2048, opaque): drawn by their brightness over a panel.
inline constexpr Texture scratches{root_toc, root_bundle,
    "ui/textures/common/backgrounds/img_background_slicedgrainytexture01_2048_2048_2048"};
// A short brush stroke with splatter (692 x 260): the bar is pixels 68 to 610 across, 79 to 184 down.
inline constexpr Texture brush_bar{bam_toc, bam_bundle, "ui/textures/common/banner/img_bannerbrush_small_692_692_260",
    {}, {68.0f / 692, 79.0f / 260, 611.0f / 692, 185.0f / 260}};
// A long brush streak, thick on the left and tapering right (1024 x 152): pixels 8 to 1013 across, 30 to 115 down.
inline constexpr Texture streak{root_toc, root_bundle, "ui/textures/common/brushs/img_brush_splashstreak_1024_1024_152",
    {}, {8.0f / 1024, 30.0f / 152, 1014.0f / 1024, 116.0f / 152}};

// The game's button prompts, as its own prompts draw them (Xbox, PlayStation, keyboard and mouse; in their own colours
// on clear), the "+" it sets between buttons held together, and icons of its replay editor and menus, in the order of
// plugin_ui.h's game_images from "xbox_a" on. Each is drawn from its visible part: the prompts fill only the middle of
// their squares. (The game's Xbox "menu" is the View button, two windows; its "options" the Menu button.) The keyboard's
// key is blank, for a key's name written on it: nine-sliced, a wide key keeps its ends square. Found with a scan of
// every bundle's resources on 2026-11-19.
inline constexpr std::string_view prompt_toc = scoring_toc;
inline constexpr std::string_view prompt_bundle = scoring_bundle;
inline constexpr std::string_view core_bundle = "win32/levels/game/bam_levelroot/bam_coregameassets";
inline constexpr std::array<Texture, 62> prompts{{
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_a_108_108_108"}, // xbox_a
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_b_108_108_108"}, // xbox_b
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_x_108_108_108"}, // xbox_x
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_y_108_108_108"}, // xbox_y
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_lbumper_108_108_108"}, // xbox_lb
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_rbumper_108_108_108"}, // xbox_rb
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_ltrigger_108_108_108"}, // xbox_lt
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_rtrigger_108_108_108"}, // xbox_rt
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_lstick-click_108_108_108"}, // xbox_l3
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_rstick-click_108_108_108"}, // xbox_r3
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_menu_108_108_108"}, // xbox_view
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_options_108_108_108"}, // xbox_menu
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-none_256_256_256"}, // xbox_dpad
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-up_108_108_108"}, // xbox_dpad_up
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-down_108_108_108"}, // xbox_dpad_down
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-left_108_108_108"}, // xbox_dpad_left
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-right_108_108_108"}, // xbox_dpad_right
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-vertical_108_108_108"}, // xbox_dpad_vertical
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/xbox/img_xbox_dpad-horizontal_108_108_108"}, // xbox_dpad_horizontal
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_cross_108_108_108"}, // ps_cross
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_circle_108_108_108"}, // ps_circle
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_square_108_108_108"}, // ps_square
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_triangle_108_108_108"}, // ps_triangle
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_lbumper_108_108_108"}, // ps_l1
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_rbumper_108_108_108"}, // ps_r1
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_ltrigger_108_108_108"}, // ps_l2
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_rtrigger_108_108_108"}, // ps_r2
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_lstick-click_108_108_108"}, // ps_l3
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_rstick-click_108_108_108"}, // ps_r3
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps4_touchpad_108_108_108"}, // ps4_touchpad
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps4_options_108_108_108"}, // ps4_options
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps5_touchpad_108_108_108"}, // ps5_touchpad
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps5_options_108_108_108"}, // ps5_options
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-none_256_256_256"}, // ps_dpad
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-up_108_108_108"}, // ps_dpad_up
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-down_108_108_108"}, // ps_dpad_down
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-left_108_108_108"}, // ps_dpad_left
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-right_108_108_108"}, // ps_dpad_right
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-vertical_108_108_108"}, // ps_dpad_vertical
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/ps/img_ps_dpad-horizontal_108_108_108"}, // ps_dpad_horizontal
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_bg_108_108_108", {}, {}, 0.5f}, // key
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_enter_108_108_108"}, // key_enter
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_esc_108_108_108"}, // key_esc
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_tab_108_108_108"}, // key_tab
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_space_108_108_108"}, // key_space
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_up_108_108_108"}, // key_up
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_down_108_108_108"}, // key_down
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_left_108_108_108"}, // key_left
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_keyboard_right_108_108_108"}, // key_right
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_mouse_lclick_108_108_108"}, // mouse_left
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/mnk/img_input_mouse_wheel_108_108_108"}, // mouse_wheel
    {prompt_toc, prompt_bundle, "ui/textures/icons/inputs/img_input_combinatorial_and_108_64_108"}, // prompt_and
    {bam_toc, core_bundle, "ui/textures/icons/toolbox/icon_replay_camera_128_128_128"}, // replay_camera
    {bam_toc, core_bundle, "ui/textures/features/replay/img_replay_zoom_64_64_64"}, // replay_zoom
    {bam_toc, bam_bundle, "ui/textures/common/shapes/bespoke/img_icon_condition_frame_alt_128_128_128"}, // condition_frame
    {bam_toc, core_bundle, "ui/textures/icons/replay/img_replay_effects_128_128_128"}, // replay_effects
    {bam_toc, bam_bundle, "ui/textures/icons/generic/img_icon_show_128_128_128"}, // show
    {bam_toc, bam_bundle, "ui/textures/common/shapes/bespoke/img_icon_emptyspotlight_128_128_128"}, // spotlight
    {bam_toc, core_bundle, "ui/textures/icons/img_icon_gamepad_192_96_96"}, // gamepad
    {prompt_toc, prompt_bundle, "ui/textures/icons/generic/img_generic_settings_128_128_128"}, // settings
    {bam_toc, core_bundle, "ui/textures/icons/replay/img_replay_save_128_128_128"}, // replay_save
    {bam_toc, core_bundle, "ui/textures/icons/generic/img_generic_warning_128_64_64"}, // warning
}};

// The faces skate.'s UI writes in, TrueType files as they are (resources of type 0x063994c5) with its English font
// configuration: Gilroy SemiBold (its labels), Gilroy ExtraBold (its values and headings) and SkaterMarker Bold (the
// marker face of its screens' titles), in the order of plugin_ui.h's game_faces. Found on 2026-11-19.
inline constexpr std::string_view face_toc = "Win32/ui.toc";
inline constexpr std::string_view face_bundle = "win32/configurations/delmarrimefontconfiguration_languageformat_english";
inline constexpr std::array<std::string_view, 3> faces{
    "ui/styleguide/fonts/gilroy/gilroy-semibold", "ui/styleguide/fonts/gilroy/gilroy-extrabold", "ui/styleguide/fonts/skatermarker/skatemarker_bold"};

// The picture on the loading screen of each of the game's own maps, by the map's name as a
// server's vote for the next map gives it (world_level_name). Each is in its loading screen's
// own widget bundle, in the globals superbundle, at two sizes: this is the smaller, 1280 by
// 720 (the name without a size is not a texture). Found by following each LoadScreen_*_Widget's
// "Map_Texture" and read from the game's files on 2026-10-11. The tutorial island has no
// loading screen of its own.
struct MapPicture {
    std::string_view map, bundle, name;
};
inline constexpr std::string_view map_picture_toc = "Win32/globals.toc";
inline constexpr MapPicture map_pictures[]{
    {"San Vansterdam", "win32/ui/foundations/loading/loadscreen/loadscreen_bam_widget_bundle",
     "ui/textures/screens/loadscreen/img_loadscreen_sanvan_1440_1280_720"},
    {"Isle of Grom", "win32/ui/foundations/loading/loadscreen/loadscreen_isleofgrom_widget_bundle",
     "ui/textures/screens/loadscreen/img_loadscreen_gromday_1440_1280_720"},
    {"Super Ultra Mega Resort", "win32/ui/foundations/loading/loadscreen/loadscreen_megapark_widget_bundle",
     "ui/textures/screens/loadscreen/img_loadscreen_summercamp_default_1440_1280_720"},
    {"Stadium 1", "win32/ui/foundations/loading/loadscreen/loadscreen_stadium_widget_bundle",
     "ui/textures/screens/loadscreen/img_loadscreen_stadium_default_1440_1280_720"},
    {"Stadium 2", "win32/ui/foundations/loading/loadscreen/loadscreen_stadium_nanaimo_widget_bundle",
     "ui/textures/screens/loadscreen/img_loadscreen_stadium_nanaimo_1440_1280_720"},
};
}
