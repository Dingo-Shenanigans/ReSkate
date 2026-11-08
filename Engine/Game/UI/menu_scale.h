#pragma once

namespace dingosdk {
// ReSkate menu scale. The menu chrome is laid out in pixels, so one factor
// scales both the fonts and that layout. Kept to a range that stays legible on
// a small window and still fits the page content on a large one.
inline constexpr float min_menu_scale = 0.75f, max_menu_scale = 2.0f, default_menu_scale = 1.0f;
// How the ReSkate menu is laid out (`ui layout`): a sheet docked to the screen's right edge, the game
// in view beside it, or a window in the middle of the screen with the pages down its left side.
inline constexpr int menu_layout_sheet = 0, menu_layout_hub = 1;
// How the full screen park editor is laid out (`editor layout`): the world filling the screen with a
// hotbar of objects along the bottom, or a rail of tools down the left with panels beside it.
inline constexpr int park_editor_layout_hotbar = 0, park_editor_layout_rail = 1;
// The sheet's width and the window's size, in pixels at 1080 lines and a menu scale of 1, so a size
// dragged once reads the same at any resolution and scale. The defaults, and the range a saved size
// is kept to (the sheet is also never drawn wider than half the screen, nor the window larger than it).
inline constexpr float default_menu_sheet_width = 680.0f, min_menu_sheet_width = 420.0f, max_menu_sheet_width = 2000.0f;
inline constexpr float default_menu_hub_width = 1120.0f, default_menu_hub_height = 720.0f;
inline constexpr float min_menu_hub_width = 760.0f, min_menu_hub_height = 520.0f, max_menu_hub_side = 4000.0f;
} // namespace dingosdk
