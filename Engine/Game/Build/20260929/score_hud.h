#pragma once
#include <cstdint>
#include <string_view>

// The SDK's display-only score HUD model, mapped by read-only HUD probes on this build and checked
// by drawing an owned model on 2026-11-06. These are field-name hashes from the game's type data;
// feature code finds each offset through that type before reading or writing its owned model.
namespace dingosdk::game::build::v20260929::score_hud {
inline constexpr std::uint16_t score_view_size = 896;
inline constexpr std::string_view score_widget = "UI/Features/HUD/Scoring/ScoringHUD_Widget";
inline constexpr std::string_view trick_row_widget = "UI/Features/HUD/Scoring/TrickHistoryStackItem";

// The root's line points before its multiplier, its ring value, and its action/timer signals.
inline constexpr std::uint32_t line_score = 0x40bff667;
inline constexpr std::uint32_t multiplier = 0x6a41d8c6;
inline constexpr std::uint32_t action_active = 0x7aab675c;
inline constexpr std::uint32_t line_timer = 0xbd9da937;
inline constexpr std::uint32_t is_slam = 0xaf6e13b9;
inline constexpr std::uint32_t sequence_header = 0xb3e9db3c, sequence_descriptor = 0xe8264626;

// LINE is driven by the widget from LineScore and stays for its summary after the action signals
// end. We read it to yield to the game's summary; the owned model writes LineScore, never LINE.
inline constexpr std::uint32_t line_shown = 0x9a6daa54;
inline constexpr std::uint32_t action_points = 0x7d1e237c, multiplier_text = 0x9acb7cf0;
inline constexpr std::uint32_t action_fill = 0x739c95e8;
inline constexpr float action_fill_points = 400;
inline constexpr std::uint32_t line_on = 0xa0bfc5b4, ring_on = 0x2a7d50b4;
inline constexpr std::int32_t extra_info_line_score = 0;

// A copy clears both stacks and keeps the live scoring-data ref at its new model's zero default.
// Rows get newly created models; no game row or its back-reference is copied.
inline constexpr std::uint32_t scoring_data = 0xcdffb552;
inline constexpr std::uint32_t trick_list = 0x84db5623, second_stack = 0x1a93e458;
inline constexpr std::uint32_t trick_row_schema = 0xbf5e8a16;
inline constexpr std::uint16_t trick_row_size = 2048;
inline constexpr std::uint32_t row_name = 0x53bac52d;
inline constexpr std::uint32_t label_text = 0x4d8e01b9; // LabelViewModel's Text is a char* (kind 7).
}
