#include "overlay_internal.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>

// Classic scoring layouts are redrawn from reference instead of shipping EA textures.
namespace dingosdk::overlay {
namespace { std::atomic<ScoringHudFeed> scoring_hud_feed{}; }
void set_scoring_hud_feed(ScoringHudFeed feed) noexcept { scoring_hud_feed.store(feed); }
}

namespace dingosdk::overlay::detail {
namespace {
ScoringHud frame;

int multiplier_value(std::string_view text) {
    int value{};
    const auto first = text.starts_with('x') || text.starts_with('X') ? text.data() + 1 : text.data();
    const auto last = text.data() + text.size();
    if (std::from_chars(first, last, value).ec != std::errc{} || value < 1) return 1;
    return std::min(value, 99);
}

void shadow_text(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color,
                 std::string_view text, float shadow = 2.0f) {
    const std::string value(text);
    draw->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 205), value.c_str());
    draw->AddText(font, size, at, color, value.c_str());
}

void centered_text(ImDrawList* draw, ImFont* font, float size, ImVec2 center, ImU32 color,
                   std::string_view text) {
    const std::string value(text);
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, value.c_str());
    shadow_text(draw, font, size, ImVec2(center.x - extent.x * .5f, center.y - extent.y * .5f), color, value);
}

void right_text(ImDrawList* draw, ImFont* font, float size, float right, float y, ImU32 color,
                std::string_view text) {
    const std::string value(text);
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, value.c_str());
    shadow_text(draw, font, size, ImVec2(right - extent.x, y), color, value);
}

void glow_circle(ImDrawList* draw, ImVec2 center, float radius, ImU32 color) {
    const int red = (color >> IM_COL32_R_SHIFT) & 0xff;
    const int green = (color >> IM_COL32_G_SHIFT) & 0xff;
    const int blue = (color >> IM_COL32_B_SHIFT) & 0xff;
    for (int layer = 4; layer > 0; --layer)
        draw->AddCircleFilled(center, radius + layer * 5.0f,
            IM_COL32(red, green, blue, 10 + (4 - layer) * 7), 64);
}

void trick_rows(ImDrawList* draw, ImFont* font, float scale, ImVec2 origin, float width,
                ImU32 name_color, ImU32 score_color, float font_size, float row_height) {
    float y = origin.y;
    const auto first = frame.tricks.size() > 6 ? frame.tricks.size() - 6 : 0;
    for (std::size_t index = first; index < frame.tricks.size(); ++index) {
        const auto& trick = frame.tricks[index];
        shadow_text(draw, font, font_size * scale, ImVec2(origin.x, y), name_color, trick.name, 1.5f * scale);
        if (!trick.score.empty())
            right_text(draw, font, (font_size - 1) * scale, origin.x + width, y + scale, score_color, trick.score);
        y += row_height * scale;
    }
}

void draw_skate1(ImDrawList* draw, float scale, ImVec2 origin, ImFont* heading, ImFont* body) {
    const float width = 342 * scale;
    const float rows = static_cast<float>(std::min<std::size_t>(frame.tricks.size(), 6));
    const float row_h = 25 * scale;
    const float list_h = std::max(34 * scale, rows * row_h + 12 * scale);
    const ImVec2 plate_min(origin.x - 11 * scale, origin.y - 8 * scale);
    const ImVec2 plate_max(origin.x + width + 12 * scale, origin.y + list_h);
    draw->AddRectFilled(plate_min, plate_max, IM_COL32(7, 8, 8, 188), 5 * scale);
    draw->AddCircleFilled(ImVec2(plate_min.x + 18 * scale, plate_min.y + 9 * scale), 19 * scale, IM_COL32(5, 5, 5, 132), 24);
    draw->AddCircleFilled(ImVec2(plate_max.x - 24 * scale, plate_min.y + 11 * scale), 24 * scale, IM_COL32(5, 5, 5, 118), 24);
    draw->AddRect(plate_min, plate_max, IM_COL32(205, 205, 190, 42), 5 * scale, 0, scale);
    trick_rows(draw, body, scale, origin, width, IM_COL32(238, 238, 226, 255), IM_COL32(214, 207, 175, 255), 20, 25);

    const float strip_y = origin.y + list_h + 7 * scale;
    draw->AddRectFilled(ImVec2(origin.x - 4 * scale, strip_y), ImVec2(origin.x + width + 5 * scale, strip_y + 43 * scale), IM_COL32(221, 222, 211, 238), 2 * scale);
    draw->AddRectFilled(ImVec2(origin.x - 4 * scale, strip_y + 34 * scale), ImVec2(origin.x + width + 5 * scale, strip_y + 43 * scale), IM_COL32(109, 111, 103, 215));
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    shadow_text(draw, heading, 31 * scale, ImVec2(origin.x + 10 * scale, strip_y + 3 * scale), IM_COL32(31, 31, 29, 255), score, scale);

    const int mult = multiplier_value(frame.multiplier);
    const float badge_w = 72 * scale;
    const ImVec2 badge_min(origin.x + width - badge_w + 3 * scale, strip_y - 8 * scale);
    const ImVec2 badge_max(origin.x + width + 5 * scale, strip_y + 47 * scale);
    draw->AddRectFilled(badge_min, badge_max, IM_COL32(244, 238, 189, 255), 2 * scale);
    draw->AddRect(badge_min, badge_max, IM_COL32(28, 25, 19, 255), 2 * scale, 0, 2 * scale);
    for (int i = 0; i < 6; ++i) {
        const float x = badge_min.x + (7 + i * 11) * scale;
        draw->AddRectFilled(ImVec2(x, badge_min.y + 3 * scale), ImVec2(x + 6 * scale, badge_min.y + 7 * scale), IM_COL32(45, 42, 31, 230));
        draw->AddRectFilled(ImVec2(x, badge_max.y - 7 * scale), ImVec2(x + 6 * scale, badge_max.y - 3 * scale), IM_COL32(45, 42, 31, 230));
    }
    centered_text(draw, heading, 27 * scale, ImVec2((badge_min.x + badge_max.x) * .5f, (badge_min.y + badge_max.y) * .5f), IM_COL32(36, 33, 25, 255), "x" + std::to_string(mult));
}

void draw_skate2(ImDrawList* draw, float scale, ImVec2 origin, ImFont* heading, ImFont* body) {
    const float width = 340 * scale;
    const float rows = static_cast<float>(std::min<std::size_t>(frame.tricks.size(), 6));
    const float list_h = std::max(32 * scale, rows * 25 * scale + 10 * scale);
    const ImVec2 ribbon_min(origin.x + 13 * scale, origin.y - 7 * scale);
    const ImVec2 ribbon_max(origin.x + width + 22 * scale, origin.y + list_h);
    draw->AddRectFilled(ribbon_min, ribbon_max, IM_COL32(246, 246, 238, 221), 9 * scale);
    draw->AddTriangleFilled(ImVec2(ribbon_min.x, ribbon_min.y), ImVec2(ribbon_min.x - 13 * scale, ribbon_min.y + 13 * scale), ImVec2(ribbon_min.x, ribbon_min.y + 28 * scale), IM_COL32(246, 246, 238, 221));
    draw->AddRect(ribbon_min, ribbon_max, IM_COL32(255, 255, 255, 110), 9 * scale, 0, scale);
    trick_rows(draw, body, scale, ImVec2(origin.x + 27 * scale, origin.y), width - 20 * scale, IM_COL32(23, 24, 22, 255), IM_COL32(77, 65, 50, 255), 20, 25);

    const float score_y = origin.y + list_h + 8 * scale;
    const ImVec2 center(origin.x + 37 * scale, score_y + 31 * scale);
    const int mult = multiplier_value(frame.multiplier);
    glow_circle(draw, center, 30 * scale, IM_COL32(255, 111, 20, 255));
    draw->AddCircleFilled(center, 31 * scale, IM_COL32(61, 26, 14, 242), 64);
    draw->AddCircle(center, 27 * scale, IM_COL32(184, 76, 29, 255), 64, 5 * scale);
    draw->AddCircle(center, 21 * scale, IM_COL32(255, 196, 121, 180), 64, 2 * scale);
    centered_text(draw, heading, 23 * scale, center, IM_COL32(255, 249, 221, 255), "x" + std::to_string(mult));
    draw->AddRectFilled(ImVec2(origin.x + 72 * scale, score_y + 7 * scale), ImVec2(origin.x + width + 22 * scale, score_y + 54 * scale), IM_COL32(248, 247, 237, 232), 4 * scale);
    draw->AddTriangleFilled(ImVec2(origin.x + width + 22 * scale, score_y + 7 * scale), ImVec2(origin.x + width + 36 * scale, score_y + 28 * scale), ImVec2(origin.x + width + 22 * scale, score_y + 54 * scale), IM_COL32(248, 247, 237, 232));
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    shadow_text(draw, heading, 34 * scale, ImVec2(origin.x + 88 * scale, score_y + 10 * scale), IM_COL32(33, 29, 25, 255), score, scale);
}

void draw_skate3(ImDrawList* draw, float scale, ImVec2 origin, ImFont* heading, ImFont* body) {
    const float width = 338 * scale;
    const float rows = static_cast<float>(std::min<std::size_t>(frame.tricks.size(), 6));
    const float list_h = std::max(34 * scale, rows * 25 * scale + 12 * scale);
    const ImVec2 plate_min(origin.x + 57 * scale, origin.y - 7 * scale);
    const ImVec2 plate_max(origin.x + width + 24 * scale, origin.y + list_h);
    draw->AddRectFilled(plate_min, plate_max, IM_COL32(6, 13, 20, 150), 5 * scale);
    draw->AddRectFilled(ImVec2(plate_min.x, plate_max.y - 3 * scale), plate_max, IM_COL32(56, 173, 245, 185), 3 * scale);
    draw->AddRect(plate_min, plate_max, IM_COL32(178, 220, 246, 50), 5 * scale, 0, scale);
    trick_rows(draw, body, scale, ImVec2(origin.x + 70 * scale, origin.y), width - 62 * scale, IM_COL32(247, 249, 250, 255), IM_COL32(190, 218, 236, 255), 20, 25);

    const float score_y = origin.y + list_h + 8 * scale;
    const ImVec2 center(origin.x + 30 * scale, score_y + 29 * scale);
    const int mult = multiplier_value(frame.multiplier);
    glow_circle(draw, center, 27 * scale, IM_COL32(25, 139, 232, 255));
    draw->AddCircleFilled(center, 30 * scale, IM_COL32(9, 20, 32, 220), 64);
    draw->AddCircle(center, 27 * scale, IM_COL32(94, 184, 244, 255), 64, 4 * scale);
    draw->AddCircle(center, 21 * scale, IM_COL32(11, 42, 70, 255), 64, 2 * scale);
    draw->PathArcTo(center, 24 * scale, -1.45f, 3.75f, 42);
    draw->PathStroke(IM_COL32(163, 227, 255, 245), 0, 3 * scale);
    centered_text(draw, heading, 22 * scale, center, IM_COL32_WHITE, "x" + std::to_string(mult));
    const ImVec2 score_min(origin.x + 65 * scale, score_y + 3 * scale);
    const ImVec2 score_max(origin.x + width + 24 * scale, score_y + 56 * scale);
    draw->AddRectFilled(score_min, score_max, IM_COL32(5, 13, 21, 178), 5 * scale);
    draw->AddRectFilled(ImVec2(score_min.x, score_max.y - 4 * scale), score_max, IM_COL32(33, 137, 218, 230), 4 * scale);
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    shadow_text(draw, heading, 37 * scale, ImVec2(score_min.x + 13 * scale, score_min.y + 4 * scale), IM_COL32_WHITE, score);
}
}

bool scoring_hud_pending() {
    frame = {};
    if (const auto feed = scoring_hud_feed.load()) {
        try { frame = feed(); } catch (...) { frame = {}; }
    }
    return frame.active && frame.style != ScoringHudStyle::game;
}

void draw_scoring_hud() {
    if (!frame.active || frame.style == ScoringHudStyle::game) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.72f, 1.55f);
    const float rows = static_cast<float>(std::clamp<std::size_t>(frame.tricks.size(), 1, 6));
    const ImVec2 origin(42 * scale, display.y - (104 + rows * 25) * scale);
    auto* draw = ImGui::GetBackgroundDrawList();
    auto* heading = state().menu.heading ? state().menu.heading : ImGui::GetFont();
    auto* body = state().menu.body ? state().menu.body : heading;
    switch (frame.style) {
    case ScoringHudStyle::skate1: draw_skate1(draw, scale, origin, heading, body); break;
    case ScoringHudStyle::skate2: draw_skate2(draw, scale, origin, heading, body); break;
    case ScoringHudStyle::skate3: draw_skate3(draw, scale, origin, heading, body); break;
    default: break;
    }
}
}
