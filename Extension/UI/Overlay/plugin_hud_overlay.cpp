#include "overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Bars that native plugins ask for through the plugin API (hud_bar), in the bottom-left corner,
// drawn on the background list under ReSkate's menus. They take no input.

namespace dingosdk::overlay {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto bar_life = std::chrono::milliseconds(1000);
struct Bar {
    std::string label;
    float fraction{};
    std::uint32_t rgb{};
    Clock::time_point seen{};
};
std::mutex &bars_mutex() {
    static auto *value = new std::mutex;
    return *value;
}
std::map<std::string, Bar, std::less<>> &bars() {
    static auto *value = new std::map<std::string, Bar, std::less<>>;
    return *value;
}
} // namespace

void set_plugin_hud_bar(std::string_view id, std::string_view label, float fraction, std::uint32_t rgb) noexcept {
    try {
        if (id.empty() || id.size() > 64 || !std::isfinite(fraction)) return;
        std::lock_guard lock(bars_mutex());
        auto &bar = bars()[std::string(id)];
        bar.label.assign(label.substr(0, 48));
        bar.fraction = std::clamp(fraction, 0.0f, 1.0f);
        bar.rgb = rgb & 0xffffffu;
        bar.seen = Clock::now();
    } catch (...) {}
}
void clear_plugin_hud_bar(std::string_view id) noexcept {
    try {
        std::lock_guard lock(bars_mutex());
        const auto found = bars().find(id);
        if (found != bars().end()) bars().erase(found);
    } catch (...) {}
}
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
namespace theme = dingosdk::skate_theme;
// The bars to draw this frame, taken in plugin_hud_pending.
std::vector<std::pair<std::string, Bar>> &frame_bars() {
    static std::vector<std::pair<std::string, Bar>> value;
    return value;
}
ImU32 with_alpha(ImU32 colour, float alpha) {
    const auto a = static_cast<unsigned>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
ImU32 bar_colour(const Bar &bar) {
    if (bar.rgb) return IM_COL32((bar.rgb >> 16) & 0xff, (bar.rgb >> 8) & 0xff, bar.rgb & 0xff, 255);
    return bar.fraction > 0.5f ? theme::good : bar.fraction > 0.25f ? theme::warning : theme::danger;
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    const float offset = std::max(1.0f, size / 16.0f);
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), IM_COL32(0, 0, 0, 170), text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
} // namespace

bool plugin_hud_pending() {
    auto &out = frame_bars();
    out.clear();
    try {
        const auto now = Clock::now();
        std::lock_guard lock(bars_mutex());
        for (auto it = bars().begin(); it != bars().end();) {
            if (now - it->second.seen > bar_life) it = bars().erase(it);
            else { out.emplace_back(it->first, it->second); ++it; }
        }
    } catch (...) { out.clear(); }
    return !out.empty();
}

void draw_plugin_hud() {
    const auto &list = frame_bars();
    if (list.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.8f, 2.0f);
    auto *draw = ImGui::GetBackgroundDrawList();
    auto &s = state();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    const float left = 32.0f * scale, width = 250.0f * scale, height = 42.0f * scale, gap = 8.0f * scale;
    float bottom = display.y - 150.0f * scale;
    unsigned seed = 41;
    for (const auto &[id, bar] : list) {
        const float top = bottom - height;
        theme::rough_rect(draw, ImVec2(left, top), ImVec2(left + width, bottom), with_alpha(theme::tile, 0.85f),
                          seed++, scale);
        const float pad = 10.0f * scale, text_size = 15.0f * scale;
        std::string label = bar.label;
        for (auto &c : label)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        shadowed(draw, heading, text_size, ImVec2(left + pad, top + 5.0f * scale), theme::grey_text, label);
        const std::string percent = std::to_string(static_cast<int>(bar.fraction * 100.0f + 0.5f)) + "%";
        const auto extent = heading->CalcTextSizeA(text_size, FLT_MAX, 0.0f, percent.c_str());
        shadowed(draw, heading, text_size, ImVec2(left + width - pad - extent.x, top + 5.0f * scale), theme::white,
                 percent);
        const float x0 = left + pad, x1 = left + width - pad, y0 = top + 27.0f * scale, y1 = top + 35.0f * scale;
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), theme::tile_light, 2.0f * scale);
        if (bar.fraction > 0.0f)
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * bar.fraction, y1), bar_colour(bar), 2.0f * scale);
        bottom = top - gap;
    }
}
} // namespace dingosdk::overlay::detail
