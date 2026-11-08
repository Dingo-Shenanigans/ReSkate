#pragma once
#include "Extension/UI/skate_theme.h"
#include <imgui.h>
#include <algorithm>
#include <iterator>

namespace dingosdk::overlay::theme {
// The game's own palette (UI/skate_theme.h), shared with the launcher.
inline constexpr ImU32 ink = IM_COL32(26, 26, 26, 255);
inline constexpr ImU32 paper = skate_theme::white;
inline constexpr ImU32 blue = skate_theme::blue;
inline constexpr ImU32 muted = skate_theme::grey_text;

// Deterministic screen-print details shared by the menu and console.
inline void halftone(ImDrawList* draw, ImVec2 at, int columns, int rows, ImU32 color) {
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            draw->AddCircleFilled(ImVec2(at.x + x * 7.0f + (y % 2) * 3.0f, at.y + y * 7.0f),
                0.7f + 0.16f * static_cast<float>((x + y) % 4), color, 6);
}
// The focus as skate.'s menus draw it: the tile in the game's blue, its text black, and a field of
// halftone dots growing towards its right end (the Settings tiles, the Skaters tiles, a list's row).
inline constexpr ImU32 focus_fill = skate_theme::blue;
inline void focus_dots(ImDrawList* draw, ImVec2 a, ImVec2 b, float scale) {
    const float height = b.y - a.y, width = std::min(b.x - a.x, std::max(height * 1.6f, 60 * scale)), step = 6.5f * scale;
    const float left = b.x - width;
    const ImU32 dot = IM_COL32(0, 38, 96, 190);
    int row = 0;
    for (float y = a.y + step * .7f; y < b.y - step * .4f; y += step, ++row)
        for (float x = left + (row % 2) * step * .5f; x < b.x - step * .4f; x += step) {
            const float t = (x - left) / width;
            if (t < .08f) continue;
            draw->AddCircleFilled(ImVec2(x, y), scale * (.35f + 1.9f * t * t), dot, 6);
        }
}
// A tile with skate.'s hand-cut edges; in the focus blue, with its dots too.
inline void tile_rect(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, unsigned seed, float scale = 1) {
    skate_theme::rough_rect(draw, a, b, colour, seed, scale);
    if (colour == focus_fill) focus_dots(draw, a, b, scale);
}
// A white keycap followed by what it does, like the game's "Esc Back"
// prompt. Returns the width drawn.
inline float keycap(ImDrawList* draw, ImFont* font, float scale, ImVec2 at, const char* key, const char* label) {
    const auto cap = font->CalcTextSizeA(12 * scale, FLT_MAX, 0, key);
    draw->AddRectFilled(at, ImVec2(at.x + cap.x + 12 * scale, at.y + 20 * scale), paper, 3 * scale);
    draw->AddText(font, 12 * scale, ImVec2(at.x + 6 * scale, at.y + (20 * scale - cap.y) * .5f), skate_theme::black, key);
    const auto text = font->CalcTextSizeA(14 * scale, FLT_MAX, 0, label);
    draw->AddText(font, 14 * scale, ImVec2(at.x + cap.x + 20 * scale, at.y + 2 * scale), paper, label);
    return cap.x + 20 * scale + text.x;
}
// The same on the grey plate skate. puts each of its page prompts on ("Esc Back" at the foot of
// Settings and Skaters). Returns the width drawn, plate and all.
inline float prompt(ImDrawList* draw, ImFont* font, float scale, ImVec2 at, const char* key, const char* label) {
    const float pad = 7 * scale, tall = 20 * scale + pad * 2;
    const auto cap = font->CalcTextSizeA(12 * scale, FLT_MAX, 0, key);
    const auto text = font->CalcTextSizeA(14 * scale, FLT_MAX, 0, label);
    const float width = cap.x + 20 * scale + text.x + pad * 2;
    const ImVec2 a(at.x, at.y - pad), b(at.x + width, at.y - pad + tall);
    draw->AddRectFilled(a, b, IM_COL32(118, 118, 120, 120), 2 * scale);
    draw->AddRect(a, b, IM_COL32(150, 150, 152, 110), 2 * scale, 0, 1.2f * scale);
    keycap(draw, font, scale, ImVec2(at.x + pad, at.y), key, label);
    return width;
}
}
