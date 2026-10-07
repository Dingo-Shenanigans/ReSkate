#pragma once
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace dingosdk::overlay::detail {
inline constexpr double nametag_gradient_period = 4.0;

// The roles whose name and badge shimmer, each between `from` and its role colour.
struct NametagGradient { ImU32 from, to; };
inline constexpr std::array nametag_gradients{
    NametagGradient{multiplayer::nametag_developer_start, multiplayer::nametag_developer},
    NametagGradient{multiplayer::nametag_creator_start, multiplayer::nametag_creator},
    NametagGradient{multiplayer::nametag_homie_start, multiplayer::nametag_homie},
    NametagGradient{multiplayer::nametag_centrix_start, multiplayer::nametag_centrix},
};
inline const NametagGradient* nametag_gradient(ImU32 colour) noexcept {
    for (const auto& gradient : nametag_gradients)
        if ((colour & ~IM_COL32_A_MASK) == (gradient.to & ~IM_COL32_A_MASK)) return &gradient;
    return nullptr;
}

inline ImU32 animated_nametag_colour(ImU32 colour, double seconds, float across = 0.0f) noexcept {
    const auto* gradient = nametag_gradient(colour);
    if (!gradient) return colour;
    // Render-time phase: cached chat lines animate too, without changing their role colour.
    const double phase = std::clamp(across, 0.0f, 1.0f) * 0.5 -
                         std::fmod(seconds, nametag_gradient_period) / nametag_gradient_period;
    const double blend = (1.0 - std::cos(2.0 * std::numbers::pi * phase)) * 0.5;
    ImU32 result = colour & IM_COL32_A_MASK;
    for (const int shift : {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT}) {
        const double from = (gradient->from >> shift) & 0xff;
        const double to = (gradient->to >> shift) & 0xff;
        result |= static_cast<ImU32>(std::lround(from + (to - from) * blend)) << shift;
    }
    return result;
}

inline void shade_nametag_gradient(ImDrawList* draw, int first_vertex, float left, float width,
                                   ImU32 role_colour, double seconds) noexcept {
    if (!nametag_gradient(role_colour) || width <= 0.0f) return;
    for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i) {
        auto& vertex = draw->VtxBuffer[i];
        const auto colour = animated_nametag_colour(role_colour, seconds, (vertex.pos.x - left) / width);
        // Preserve distance/chat fading and anti-aliased edges; never recolour earlier shadows.
        vertex.col = (vertex.col & IM_COL32_A_MASK) | (colour & ~IM_COL32_A_MASK);
    }
}
} // namespace dingosdk::overlay::detail
