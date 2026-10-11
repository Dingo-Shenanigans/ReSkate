#pragma once
#include <cstdint>
#include <string_view>

namespace dingosdk::game::build::v20260929::hud_corner {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// skate.'s HUD in the bottom left corner is HudViewModel's DpadScoringStack (UI/Features/HUD/
// HudViewModel; PersistentHUDCore's DpadScoring_Stack shows it), a stack view model
// (UI/Foundations/Components/Stack/StackViewModel) of stack item view models. In free roam it holds
// the d-pad menu (HUDDpad_Widget) at priority 1; a scoring line pushes the score HUD
// (ScoringHUD_Widget) at priority 3. HudViewModel has two roots; the d-pad lives in one of them.
//
// What shows, read from the widgets' own logic and measured in play on 2026-10-07:
// - the d-pad hides itself while it is not alone: HUDDpad_Widget's Root_Container shows only while
//   the stack's items it counts number exactly 1 (CompareInt A=B, B 1). An item at priority 2
//   counts; one at priority 4 does not (it stays in the stack, active, and changes nothing).
// - the score HUD never hides for the stack: it shows by ScoringHUDViewModel alone (below).
inline constexpr std::uint32_t hud_view_schema = 0xe4c44873;    // HudViewModel
inline constexpr std::uint32_t dpad_scoring_stack = 0xeae7b84f; // its DpadScoringStack
inline constexpr std::string_view dpad_widget = "UI/Features/HUD/Dpad/Widgets/HUDDpad_Widget";
inline constexpr std::int32_t dpad_priority = 1, score_priority = 3;
// StackViewModel
inline constexpr std::uint32_t stack_items = 0x61742cb4;        // Items
inline constexpr std::uint32_t default_transition = 0x54c9abee; // DefaultTransitionStyle, which a push gives each item
// StackItemViewModel
inline constexpr std::uint32_t item_key = 0x1e95752c;        // uint32 Key: the stack tells its items apart by it
inline constexpr std::uint32_t item_priority = 0x644a8875;   // int32 Priority
inline constexpr std::uint32_t item_transition = 0xd78d240b; // Transition
inline constexpr std::uint32_t item_content = 0x716496c8;    // Content: a presenter (its blueprint and data)
inline constexpr std::uint32_t content_blueprint = 0x8d1441c7;

// The score HUD (ui/features/hud/scoring/ScoringHUD_Widget) shows the game's UI model
// ScoringHUDViewModel, one root of the UI model registry, which its own widget logic writes on its
// own thread (expression functions on score events: no native code names the fields).
// HudWidgetActive shows the HUD (its multiplier ring and trick list); the line label shows only
// while ExtraInfoStyle is LineScore (0; 1 ScoreInfo shows the extra info instead, 2 None neither).
inline constexpr std::uint32_t score_view_schema = 0x9b74fc7c; // ScoringHUDViewModel
inline constexpr std::uint32_t hud_widget_active = 0x3aa2f739; // bool HudWidgetActive
inline constexpr std::uint32_t extra_info_style = 0x0426783f;  // int32 ScoringWidgetExtraInfoStyle
inline constexpr std::int32_t extra_info_none = 2;
}
