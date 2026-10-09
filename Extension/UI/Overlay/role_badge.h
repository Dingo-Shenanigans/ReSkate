#pragma once
#include "nametag_gradient.h"
#include <imgui.h>
#include <algorithm>
#include <string>

// The role badge ("Dev", "Staff", "Content Creator", "Centrix", "Homie", "Admin", "Host", "Friend") before a player's name, in chat
// and on nametags: a pill in the role's colour (animated for the roles in nametag_gradient.h) with dark text.
namespace dingosdk::overlay::detail {
inline float role_badge_width(ImFont *font, float size, const std::string &tag) {
    return tag.empty() ? 0.0f : font->CalcTextSizeA(size * 0.78f, FLT_MAX, 0.0f, tag.c_str()).x + size * 0.7f;
}
// `at` is the top left of a name line `line_height` tall, which the badge is centred on.
inline void draw_role_badge(ImDrawList *draw, ImFont *font, float size, ImVec2 at, float line_height,
                            const std::string &tag, ImU32 colour, float alpha) {
    if (tag.empty()) return;
    const float text_size = size * 0.78f;
    const auto extent = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, tag.c_str());
    const float height = std::min(line_height, extent.y + size * 0.2f);
    const ImVec2 min(at.x, at.y + (line_height - height) * 0.5f), max(at.x + extent.x + size * 0.7f, min.y + height);
    const auto a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * alpha;
    const int badge_vertices = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a * 235.0f) << IM_COL32_A_SHIFT),
                        size * 0.22f);
    shade_nametag_gradient(draw, badge_vertices, min.x, max.x - min.x, colour, ImGui::GetTime());
    draw->AddText(font, text_size, ImVec2(min.x + size * 0.35f, min.y + (height - extent.y) * 0.5f),
                  IM_COL32(18, 18, 22, static_cast<int>(255.0f * alpha)), tag.c_str());
}
} // namespace dingosdk::overlay::detail
