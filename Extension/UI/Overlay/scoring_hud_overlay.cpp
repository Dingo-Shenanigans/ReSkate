#include "overlay_internal.h"
#include <atomic>
#include <charconv>

// Authentic-era scoring layouts drawn from detached game-model text. The Skate 3 badge is
// vector-drawn, like ProbablyFineSoft's asset mod, so no copyrighted game image data ships.
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

void outlined_text(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    constexpr ImU32 shadow = IM_COL32(0, 0, 0, 230);
    draw->AddText(font, size, ImVec2(at.x - 2, at.y), shadow, text.c_str());
    draw->AddText(font, size, ImVec2(at.x + 2, at.y), shadow, text.c_str());
    draw->AddText(font, size, ImVec2(at.x, at.y - 2), shadow, text.c_str());
    draw->AddText(font, size, ImVec2(at.x, at.y + 2), shadow, text.c_str());
    draw->AddText(font, size, at, color, text.c_str());
}

void draw_skate1(ImDrawList* draw, float scale, ImVec2 origin, ImFont* font) {
    const float width = 360 * scale;
    float y = origin.y;
    for (const auto& trick : frame.tricks) {
        const std::string line = trick.score.empty() ? trick.name : trick.name + "  " + trick.score;
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + 36 * scale), IM_COL32(5, 5, 5, 205));
        outlined_text(draw, font, 25 * scale, ImVec2(origin.x + 12 * scale, y + 4 * scale), IM_COL32_WHITE, line);
        y += 39 * scale;
    }
    const float score_y = y + 8 * scale;
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    outlined_text(draw, font, 38 * scale, ImVec2(origin.x + 10 * scale, score_y), IM_COL32_WHITE, score);
    const int mult = multiplier_value(frame.multiplier);
    const ImVec2 badge(origin.x + width - 78 * scale, score_y + 2 * scale);
    draw->AddRectFilled(badge, ImVec2(badge.x + 70 * scale, badge.y + 38 * scale), IM_COL32(18, 18, 18, 235));
    for (int i = 0; i < 6; ++i) {
        draw->AddRectFilled(ImVec2(badge.x + i * 12 * scale, badge.y - 4 * scale),
            ImVec2(badge.x + (i * 12 + 7) * scale, badge.y), IM_COL32(235, 235, 220, 255));
        draw->AddRectFilled(ImVec2(badge.x + i * 12 * scale, badge.y + 38 * scale),
            ImVec2(badge.x + (i * 12 + 7) * scale, badge.y + 42 * scale), IM_COL32(235, 235, 220, 255));
    }
    outlined_text(draw, font, 25 * scale, ImVec2(badge.x + 14 * scale, badge.y + 4 * scale), IM_COL32(255, 205, 55, 255), "x" + std::to_string(mult));
}

void draw_skate2(ImDrawList* draw, float scale, ImVec2 origin, ImFont* font) {
    float y = origin.y;
    for (const auto& trick : frame.tricks) {
        const std::string line = trick.score.empty() ? trick.name : trick.name + "  " + trick.score;
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + 370 * scale, y + 34 * scale), IM_COL32(245, 245, 240, 235), 3 * scale);
        draw->AddText(font, 24 * scale, ImVec2(origin.x + 12 * scale, y + 4 * scale), IM_COL32(20, 20, 20, 255), line.c_str());
        y += 37 * scale;
    }
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    outlined_text(draw, font, 42 * scale, ImVec2(origin.x + 72 * scale, y + 7 * scale), IM_COL32_WHITE, score);
    const ImVec2 center(origin.x + 32 * scale, y + 29 * scale);
    draw->AddCircleFilled(center, 27 * scale, IM_COL32(45, 20, 12, 220), 48);
    draw->AddCircle(center, 24 * scale, IM_COL32(242, 112, 45, 255), 48, 5 * scale);
    outlined_text(draw, font, 21 * scale, ImVec2(center.x - 15 * scale, center.y - 13 * scale), IM_COL32_WHITE,
        "x" + std::to_string(multiplier_value(frame.multiplier)));
}

void draw_skate3(ImDrawList* draw, float scale, ImVec2 origin, ImFont* font) {
    float y = origin.y;
    for (const auto& trick : frame.tricks) {
        const std::string line = trick.score.empty() ? trick.name : trick.name + "   " + trick.score;
        outlined_text(draw, font, 25 * scale, ImVec2(origin.x + 76 * scale, y), IM_COL32_WHITE, line);
        y += 31 * scale;
    }
    const ImVec2 center(origin.x + 32 * scale, y + 34 * scale);
    draw->AddCircleFilled(center, 30 * scale, IM_COL32(15, 37, 58, 205), 64);
    draw->AddCircle(center, 27 * scale, IM_COL32(20, 125, 220, 255), 64, 5 * scale);
    draw->AddCircle(center, 21 * scale, IM_COL32(8, 20, 34, 255), 64, 2 * scale);
    outlined_text(draw, font, 22 * scale, ImVec2(center.x - 15 * scale, center.y - 14 * scale), IM_COL32_WHITE,
        "x" + std::to_string(multiplier_value(frame.multiplier)));
    const auto score = frame.line_score.empty() ? "0" : frame.line_score;
    outlined_text(draw, font, 43 * scale, ImVec2(origin.x + 75 * scale, y + 8 * scale), IM_COL32_WHITE, score);
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
    const float scale = std::clamp(display.y / 1080.0f, 0.72f, 2.0f);
    const float rows = static_cast<float>(std::max<std::size_t>(frame.tricks.size(), 1));
    const ImVec2 origin(36 * scale, display.y - (90 + rows * 39) * scale);
    auto* draw = ImGui::GetBackgroundDrawList();
    auto* font = state().menu.heading ? state().menu.heading : ImGui::GetFont();
    switch (frame.style) {
    case ScoringHudStyle::skate1: draw_skate1(draw, scale, origin, font); break;
    case ScoringHudStyle::skate2: draw_skate2(draw, scale, origin, font); break;
    case ScoringHudStyle::skate3: draw_skate3(draw, scale, origin, font); break;
    default: break;
    }
}
}
