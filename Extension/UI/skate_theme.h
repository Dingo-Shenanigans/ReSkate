#pragma once
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// skate.'s own menu look (HUB screen), shared by the launcher and the in-game
// overlay. Colours are sampled from the game; sizes take the caller's UI scale.
namespace dingosdk::skate_theme {

inline constexpr ImU32 tile = IM_COL32(26, 26, 26, 240);
inline constexpr ImU32 tile_grey = IM_COL32(45, 45, 47, 240);
inline constexpr ImU32 tile_light = IM_COL32(61, 62, 66, 240);
inline constexpr ImU32 blue = IM_COL32(1, 131, 255, 255);
inline constexpr ImU32 blue_hover = IM_COL32(40, 152, 255, 255);
inline constexpr ImU32 blue_active = IM_COL32(0, 110, 215, 255);
// The ReSkate team's own servers: the logo's blue, a server tile tinted with it, and their name on it.
inline constexpr ImU32 official = IM_COL32(0, 145, 255, 255);
inline constexpr ImU32 official_tile = IM_COL32(8, 44, 82, 240);
inline constexpr ImU32 official_text = IM_COL32(110, 190, 255, 255);
// A server Steam friends are skating in: a tile tinted green, and its name on it.
inline constexpr ImU32 friends_tile = IM_COL32(14, 58, 32, 240);
inline constexpr ImU32 friends_text = IM_COL32(120, 226, 156, 255);
inline constexpr ImU32 black = IM_COL32(0, 0, 0, 255);
inline constexpr ImU32 white = IM_COL32(245, 245, 245, 255);
inline constexpr ImU32 grey_text = IM_COL32(150, 152, 158, 255);
inline constexpr ImU32 bar = IM_COL32(255, 177, 11, 255);
inline constexpr ImU32 bar_stripe = IM_COL32(255, 140, 0, 255);
inline constexpr ImU32 good = IM_COL32(46, 184, 92, 255);
inline constexpr ImU32 danger = IM_COL32(255, 92, 92, 255);
inline constexpr ImU32 warning = IM_COL32(255, 177, 11, 255);
inline constexpr ImU32 avatar = IM_COL32(240, 122, 30, 255);

inline float noise(unsigned n) {
    n = (n << 13) ^ n;
    return static_cast<float>((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu) / 2147483647.0f;
}

// Rotates everything drawn since `start` (a VtxBuffer size) around `centre`.
inline void rotate_since(ImDrawList* draw, int start, float degrees, ImVec2 centre) {
    const float radians = degrees * 0.01745329f, s = std::sin(radians), c = std::cos(radians);
    for (int index = start; index < draw->VtxBuffer.Size; ++index) {
        auto& p = draw->VtxBuffer[index].pos;
        const ImVec2 d(p.x - centre.x, p.y - centre.y);
        p = ImVec2(centre.x + d.x * c - d.y * s, centre.y + d.x * s + d.y * c);
    }
}

// Tiles in skate.'s menus have slightly rough, hand-cut edges.
inline void rough_rect(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, unsigned seed, float scale = 1) {
    // Reused between calls: menus draw many tiles every frame.
    thread_local std::vector<ImVec2> points;
    points.clear();
    const float step = 10 * scale, wobble = 1.4f * scale;
    unsigned n = seed * 977u;
    const auto jitter = [&] { return (noise(++n) - 0.5f) * wobble; };
    for (float x = a.x; x < b.x; x += step) points.emplace_back(x, a.y + jitter());
    for (float y = a.y; y < b.y; y += step) points.emplace_back(b.x + jitter(), y);
    for (float x = b.x; x > a.x; x -= step) points.emplace_back(x, b.y + jitter());
    for (float y = b.y; y > a.y; y -= step) points.emplace_back(a.x + jitter(), y);
    draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), colour);
}

// The dry-brush scribble skate. draws over locked tiles.
inline void scribble(ImDrawList* draw, ImVec2 a, ImVec2 b, unsigned seed, float scale = 1) {
    for (int stroke = 0; stroke < 3; ++stroke) {
        std::array<ImVec2, 15> points;
        const float y = a.y + (b.y - a.y) * (0.45f + 0.12f * static_cast<float>(stroke));
        for (int i = 0; i < static_cast<int>(points.size()); ++i) {
            const float t = static_cast<float>(i) / 14.0f;
            const float swing = std::min(16 * scale, (b.y - a.y) * 0.3f);
            points[i] = ImVec2(a.x + (b.x - a.x) * (0.08f + 0.84f * t),
                y + (i % 2 ? -1.0f : 1.0f) * swing * (0.6f + 0.4f * noise(seed * 31u + static_cast<unsigned>(i + stroke * 17))));
        }
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(0, 0, 0, 72), 0, 5 * scale);
    }
}

enum class Icon { check, busy, fail, warning };

// Round status icons, like the ticks on the HUB's BOUNTIES tile.
inline void status_icon(ImDrawList* draw, ImVec2 centre, Icon icon, float time, float scale = 1) {
    const float r = 9 * scale;
    switch (icon) {
    case Icon::check: {
        draw->AddCircleFilled(centre, r, good);
        const ImVec2 tick[]{ImVec2(centre.x - r * 0.45f, centre.y), ImVec2(centre.x - r * 0.1f, centre.y + r * 0.38f),
                            ImVec2(centre.x + r * 0.5f, centre.y - r * 0.38f)};
        draw->AddPolyline(tick, 3, white, 0, 2.2f * scale);
        break;
    }
    case Icon::busy: {
        draw->AddCircle(centre, r - scale, white, 0, 2 * scale);
        const float angle = time * 3.0f;
        draw->AddLine(centre, ImVec2(centre.x + std::cos(angle) * r * 0.6f, centre.y + std::sin(angle) * r * 0.6f), white, 2 * scale);
        draw->AddLine(centre, ImVec2(centre.x, centre.y - r * 0.5f), white, 2 * scale);
        break;
    }
    case Icon::fail:
        draw->AddCircleFilled(centre, r, danger);
        draw->AddLine(ImVec2(centre.x - r * 0.4f, centre.y - r * 0.4f), ImVec2(centre.x + r * 0.4f, centre.y + r * 0.4f), white, 2.2f * scale);
        draw->AddLine(ImVec2(centre.x - r * 0.4f, centre.y + r * 0.4f), ImVec2(centre.x + r * 0.4f, centre.y - r * 0.4f), white, 2.2f * scale);
        break;
    case Icon::warning:
        draw->AddCircleFilled(centre, r, warning);
        draw->AddLine(ImVec2(centre.x, centre.y - r * 0.5f), ImVec2(centre.x, centre.y + r * 0.1f), black, 2.4f * scale);
        draw->AddCircleFilled(ImVec2(centre.x, centre.y + r * 0.45f), 1.4f * scale, black);
        break;
    }
}

// Yellow striped meter with a black outline, like the RIP SCORE bar.
inline void striped_bar(ImDrawList* draw, ImVec2 a, ImVec2 b, float fill, float time, float scale = 1) {
    draw->AddRectFilled(a, b, IM_COL32(10, 10, 10, 255));
    const float end = a.x + (b.x - a.x) * std::clamp(fill, 0.0f, 1.0f);
    if (end > a.x) {
        draw->AddRectFilled(a, ImVec2(end, b.y), bar);
        draw->PushClipRect(a, ImVec2(end, b.y), true);
        const float h = b.y - a.y, spacing = 9 * scale, offset = std::fmod(time * 12 * scale, spacing);
        for (float x = a.x - h - spacing + offset; x < end; x += spacing)
            draw->AddQuadFilled(ImVec2(x, b.y), ImVec2(x + 3.5f * scale, b.y), ImVec2(x + h + 3.5f * scale, a.y),
                ImVec2(x + h, a.y), bar_stripe);
        draw->PopClipRect();
    }
    draw->AddRect(ImVec2(a.x - scale, a.y - scale), ImVec2(b.x + scale, b.y + scale), black, 0, 0, 2 * scale);
}

// ImGui colours for flat, square, high-contrast widgets. Returns the count pushed.
inline int push_widget_colours() {
    const std::pair<ImGuiCol, ImU32> colours[]{
        {ImGuiCol_Text, white}, {ImGuiCol_TextDisabled, grey_text},
        {ImGuiCol_WindowBg, IM_COL32(18, 18, 19, 246)}, {ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0)},
        {ImGuiCol_PopupBg, IM_COL32(26, 26, 26, 252)}, {ImGuiCol_Border, IM_COL32(61, 62, 66, 255)},
        {ImGuiCol_FrameBg, tile_grey}, {ImGuiCol_FrameBgHovered, tile_light}, {ImGuiCol_FrameBgActive, IM_COL32(72, 73, 78, 255)},
        {ImGuiCol_Button, tile_grey}, {ImGuiCol_ButtonHovered, tile_light}, {ImGuiCol_ButtonActive, blue_active},
        {ImGuiCol_Header, tile_light}, {ImGuiCol_HeaderHovered, IM_COL32(72, 73, 78, 255)}, {ImGuiCol_HeaderActive, blue_active},
        {ImGuiCol_CheckMark, blue}, {ImGuiCol_SliderGrab, blue}, {ImGuiCol_SliderGrabActive, white},
        {ImGuiCol_TextSelectedBg, IM_COL32(1, 131, 255, 110)}, {ImGuiCol_NavCursor, blue},
        {ImGuiCol_TableHeaderBg, tile_grey}, {ImGuiCol_TableRowBg, IM_COL32(0, 0, 0, 0)},
        {ImGuiCol_TableRowBgAlt, IM_COL32(255, 255, 255, 9)}, {ImGuiCol_TableBorderLight, IM_COL32(52, 53, 56, 255)},
        {ImGuiCol_TableBorderStrong, IM_COL32(61, 62, 66, 255)}, {ImGuiCol_Separator, IM_COL32(61, 62, 66, 255)},
        {ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0)}, {ImGuiCol_ScrollbarGrab, tile_light},
        {ImGuiCol_ScrollbarGrabHovered, IM_COL32(104, 108, 112, 255)}, {ImGuiCol_ScrollbarGrabActive, blue}};
    for (const auto& [id, colour] : colours) ImGui::PushStyleColor(id, colour);
    return static_cast<int>(std::size(colours));
}

// Blue button with black text: the colours of skate.'s selected tile.
inline void push_primary_button() {
    ImGui::PushStyleColor(ImGuiCol_Button, blue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, blue_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, blue_active);
    ImGui::PushStyleColor(ImGuiCol_Text, black);
}
inline void pop_primary_button() { ImGui::PopStyleColor(4); }

} // namespace dingosdk::skate_theme
