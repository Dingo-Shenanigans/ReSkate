#include "overlay_internal.h"
#include "chat_emotes.h"
#include "chat_rich.h"
#include "nametag_gradient.h"
#include "role_badge.h"
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// ReSkate's nametags. The game side hands over each other player's head position,
// name, colour and distance with the camera the client last used; they are placed here
// on the background draw list, under ReSkate's own menus and chat, taking no input.

namespace dingosdk::overlay {
namespace {
std::atomic<NametagFeed> nametag_feed{};
}
void set_nametag_feed(NametagFeed feed) noexcept { nametag_feed.store(feed); }
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
// Nearer than this a player gets their name; further, only a dot.
// How far in from the screen's edge the dots of players off screen sit (1080p pixels).
constexpr float edge_margin = 28.0f;

Nametags &nametags() {
    static Nametags value;
    return value;
}
using Vec3 = std::array<float, 3>;
float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::string distance_text(float metres) {
    char text[32]{};
    if (metres < 1000.0f) std::snprintf(text, sizeof text, "%.0f m", std::floor(metres));
    else std::snprintf(text, sizeof text, "%.1f km", metres / 1000.0f);
    return text;
}
ImU32 with_alpha(ImU32 colour, float alpha) {
    const auto a = static_cast<unsigned>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
void outlined_dot(ImDrawList *draw, ImVec2 at, float radius, ImU32 colour) {
    draw->AddCircleFilled(at, radius + std::max(1.0f, radius * 0.35f), with_alpha(IM_COL32(0, 0, 0, 255), 0.55f), 16);
    draw->AddCircleFilled(at, radius, colour, 16);
}
// A small speaker with one sound wave, its left edge at `at`, centred on `at.y`.
void speaker(ImDrawList *draw, ImVec2 at, float size, ImU32 colour) {
    const float h = size * 0.5f;
    draw->AddRectFilled(ImVec2(at.x, at.y - h * 0.35f), ImVec2(at.x + size * 0.28f, at.y + h * 0.35f), colour);
    draw->AddTriangleFilled(ImVec2(at.x + size * 0.28f, at.y - h * 0.35f), ImVec2(at.x + size * 0.62f, at.y - h),
                            ImVec2(at.x + size * 0.62f, at.y + h), colour);
    draw->AddTriangleFilled(ImVec2(at.x + size * 0.28f, at.y - h * 0.35f), ImVec2(at.x + size * 0.62f, at.y + h),
                            ImVec2(at.x + size * 0.28f, at.y + h * 0.35f), colour);
    draw->PathArcTo(ImVec2(at.x + size * 0.62f, at.y), size * 0.42f, -0.9f, 0.9f, 10);
    draw->PathStroke(colour, 0, std::max(1.0f, size * 0.11f));
}
// ---- chat bubbles over each skater's head.
// Text pops in with a slight overshoot, holds, then fades. A message longer than `bubble_max_lines`
// is cut with "..." so one line can never wall the screen off.
constexpr float bubble_base_size = 15.0f;
constexpr float bubble_wrap_factor = 7.0f;
constexpr float bubble_wrap_min = 260.0f;
constexpr std::size_t bubble_max_lines = 6;
constexpr float bubble_pad_factor = 0.6f;
constexpr float bubble_gap = 3.0f;
constexpr float bubble_margin = 6.0f;

float ease_out_back(float t) {
    constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float x = std::clamp(t, 0.0f, 1.0f) - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}
float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// A laid-out bubble, cached by its text and the font and size it was laid out with, so a line
// is measured once and drawn many frames.
struct BubbleKey {
    const ImFont *font{};
    float size{}, wrap{};
    std::string text, raw;
    bool operator<(const BubbleKey &other) const {
        return std::tie(font, size, wrap, text, raw) < std::tie(other.font, other.size, other.wrap, other.text, other.raw);
    }
};
struct BubbleLayout {
    std::string drawn;
    chat_rich::RichText rich;
    float width{}, height{};
    std::uint64_t frame{};
};
std::map<BubbleKey, BubbleLayout> &bubble_cache() {
    static std::map<BubbleKey, BubbleLayout> value;
    return value;
}
const BubbleLayout &bubble_layout(const std::string &text, const std::string &raw, ImFont *font, float size, float wrap,
                                  std::uint64_t frame) {
    auto &cache = bubble_cache();
    BubbleKey key{font, size, wrap, text, raw};
    const auto found = cache.find(key);
    if (found != cache.end()) {
        found->second.frame = frame;
        return found->second;
    }
    BubbleLayout layout;
    layout.drawn = chat_rich::restore_emotes(text, raw);
    // A message that is nothing but a few emotes shows them jumbo, like a chat app.
    std::size_t emotes{};
    const bool jumbo = chat_rich::only_emotes(layout.drawn, emotes) && emotes <= 3;
    layout.rich = chat_rich::lay_out(font, size, layout.drawn, wrap, 0.0f, bubble_max_lines, jumbo ? 2.4f : 1.3f);
    layout.width = std::max(layout.rich.width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, "Ay").x * 0.5f);
    layout.height = layout.rich.height;
    auto [it, added] = cache.emplace(std::move(key), std::move(layout));
    it->second.frame = frame;
    return it->second;
}
// Draws one bubble with its tail (if any), scaled by `pop` about its bottom-centre. Returns
// the drawn top y.
float draw_bubble(ImDrawList *draw, ImFont *body_font, float size, const BubbleLayout &layout, ImVec2 at,
                  float settled_bottom, float alpha, float pop, ImU32 accent, float rounding, bool tail, float k) {
    const float pad = size * bubble_pad_factor;
    const float width = (layout.width + pad * 2.0f) * pop;
    const float height = (layout.height + pad * 2.0f) * pop;
    const float display_width = ImGui::GetIO().DisplaySize.x;
    const float left = std::clamp(at.x - width * 0.5f, 4.0f * k, std::max(4.0f * k, display_width - width - 4.0f * k));
    const float top = settled_bottom - height;
    const ImVec2 min(left, top), max(left + width, settled_bottom);
    const ImU32 bg = with_alpha(theme::ink, 0.9f * alpha);
    const ImU32 border = with_alpha(accent, 0.95f * alpha);
    const ImU32 colour = with_alpha(theme::paper, alpha);
    // A soft drop shadow under the body.
    draw->AddRectFilled(ImVec2(min.x + 1.0f * k, min.y + 2.0f * k), ImVec2(max.x + 1.0f * k, max.y + 2.0f * k),
                        with_alpha(IM_COL32(0, 0, 0, 255), 0.35f * alpha), rounding);
    draw->AddRectFilled(min, max, bg, rounding);
    if (tail) {
        const float tail_x = std::clamp(at.x, min.x + size * 0.6f, max.x - size * 0.6f);
        draw->AddTriangleFilled(ImVec2(tail_x - size * 0.4f, max.y - 1.0f), ImVec2(tail_x + size * 0.4f, max.y - 1.0f),
                                ImVec2(tail_x, max.y + size * 0.6f), bg);
    }
    draw->AddRect(min, max, border, rounding, 0, std::max(1.0f, size * 0.1f));
    const ImVec2 origin(min.x + pad * pop, top + pad * pop);
    if (layout.rich.items.empty()) {
        draw->AddText(body_font, size * pop, origin, colour, layout.drawn.c_str());
    } else {
        chat_rich::draw_rich(draw, body_font, size, origin, layout.drawn, layout.rich, colour, alpha, pop);
    }
    return top;
}
// A player's bubble stack: newest nearest the head, older lines above, each popping in around
// its own bottom-centre. Only the newest points its tail down. Returns the y of the top of the
// stack, or `head_bottom` when nothing was drawn.
float draw_bubbles(ImDrawList *draw, ImFont *body_font, const Nametag &tag, ImVec2 at, float head_bottom, float k,
                   float max_distance, ImU32 accent, std::uint64_t frame) {
    const float nearness = 1.0f - std::clamp(tag.distance / std::max(1.0f, max_distance), 0.0f, 1.0f);
    const float size = std::clamp(bubble_base_size * k, 10.0f, 44.0f) * (0.85f + 0.15f * nearness);
    const float wrap = std::max(size * bubble_wrap_factor, bubble_wrap_min * k);
    const float pad = size * bubble_pad_factor;
    const float gap = std::max(2.0f, bubble_gap * k);
    const float margin = bubble_margin * k;
    const float rounding = size * 0.45f;

    struct Placed {
        const NametagBubble *line;
        const BubbleLayout *layout;
        float settled_bottom, height;
    };
    // Newest first; always keep the newest, older ones only while they still fit on screen.
    std::vector<Placed> placed;
    float cursor = head_bottom;
    for (auto it = tag.bubbles.rbegin(); it != tag.bubbles.rend(); ++it) {
        const auto &layout = bubble_layout(it->text, it->raw, body_font, size, wrap, frame);
        const float height = layout.height + pad * 2.0f;
        if (!placed.empty() && cursor - height < margin) break;
        placed.push_back({&*it, &layout, cursor, height});
        cursor -= height + gap;
    }
    if (placed.empty()) return head_bottom;
    // Oldest first, so the newest (which has the tail) draws on top.
    for (std::size_t i = placed.size(); i-- > 0;) {
        const auto &p = placed[i];
        const float pop = 0.55f + 0.45f * ease_out_back(p.line->appear);
        const float alpha = std::clamp(p.line->fade, 0.0f, 1.0f) * smoothstep(p.line->appear);
        if (alpha <= 0.001f) continue;
        draw_bubble(draw, body_font, size, *p.layout, at, p.settled_bottom, alpha, pop, accent, rounding, i == 0, k);
    }
    return placed.back().settled_bottom - placed.back().height;
}
} // namespace

bool nametags_pending() {
    auto &value = nametags();
    value = {};
    if (const auto feed = nametag_feed.load()) {
        try { value = feed(); } catch (...) { value = {}; }
    }
    return !value.tags.empty() && value.vertical_fov > 1 && value.vertical_fov < 175;
}

void draw_nametags() {
    auto &value = nametags();
    if (value.tags.empty() || !(value.vertical_fov > 1 && value.vertical_fov < 175)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const auto &m = value.camera;
    const Vec3 right{m[0], m[1], m[2]}, up{m[4], m[5], m[6]}, back{m[8], m[9], m[10]}, origin{m[12], m[13], m[14]};
    const float k = display.y / 1080.0f;
    const float focal = display.y / (2.0f * std::tan(value.vertical_fov * 3.14159265f / 360.0f));
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    const float margin = edge_margin * k;
    auto &s = state();
    auto *font = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto *body_font = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto *draw = ImGui::GetBackgroundDrawList();
    const double animation_time = ImGui::GetTime();
    const auto frame = static_cast<std::uint64_t>(ImGui::GetFrameCount());
    // Far first, so nearer names draw over them.
    std::sort(value.tags.begin(), value.tags.end(), [](const Nametag &a, const Nametag &b) { return a.distance > b.distance; });
    const float name_range = std::max(1.0f, value.name_distance);
    for (const auto &tag : value.tags) {
        const auto animated_colour = animated_nametag_colour(tag.color, animation_time);
        const bool unnamed = tag.self || tag.nameless; // bubbles only: no name, no dot
        const Vec3 delta{tag.position[0] - origin[0], tag.position[1] - origin[1], tag.position[2] - origin[2]};
        const float depth = -dot(delta, back), side = dot(delta, right), height = dot(delta, up);
        bool on_screen = false;
        ImVec2 at;
        if (depth > 0.1f) {
            at = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
            on_screen = at.x >= margin && at.x <= display.x - margin && at.y >= margin && at.y <= display.y - margin;
        }
        if (!on_screen) {
            // The local player has no name or edge dot, only a bubble when they are visible.
            if (!unnamed && value.show_names && value.dots) {
                // The direction to them from the middle of the screen, pushed out to the edge.
                float dx = depth > 0.1f ? at.x - centre.x : side, dy = depth > 0.1f ? at.y - centre.y : -height;
                if (depth <= 0.1f && std::abs(dx) < 1e-3f && std::abs(dy) < 1e-3f) dy = 1.0f; // straight behind
                const float hx = centre.x - margin, hy = centre.y - margin;
                const float scale = std::min(std::abs(dx) > 1e-6f ? hx / std::abs(dx) : FLT_MAX,
                                             std::abs(dy) > 1e-6f ? hy / std::abs(dy) : FLT_MAX);
                outlined_dot(draw, ImVec2(centre.x + dx * scale, centre.y + dy * scale), 5.0f * k, animated_colour);
            }
            continue;
        }
        // Where a bubble's tail should point: the top of the name, or the head itself.
        float bubble_bottom = at.y;
        if (!unnamed && value.show_names) {
            if (tag.distance > name_range) {
                if (value.dots) {
                    outlined_dot(draw, at, 4.0f * k, animated_colour);
                    bubble_bottom = at.y - 6.0f * k;
                }
            } else {
                // Names shrink a little with distance and fade slightly towards the name range.
                const float nearness = 1.0f - std::clamp(tag.distance / name_range, 0.0f, 1.0f);
                const float size = std::clamp((15.0f + 7.0f * nearness) * k, 10.0f, 44.0f);
                const float alpha = 0.7f + 0.3f * nearness;
                const auto colour = with_alpha(tag.color, alpha);
                const auto shadow = with_alpha(IM_COL32(0, 0, 0, 255), 0.75f * alpha);
                const float offset = std::max(1.0f, size / 14.0f);
                const auto name_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, tag.name.c_str());
                const auto distance = distance_text(tag.distance);
                const float detail = size * 0.72f;
                const auto distance_size = font->CalcTextSizeA(detail, FLT_MAX, 0.0f, distance.c_str());
                // The name sits above the head point, the distance under it, and the role badge (as in
                // chat) before the name, the two centred together.
                const float badge = role_badge_width(font, size, tag.tag);
                const float badge_gap = badge > 0.0f ? size * 0.3f : 0.0f;
                const float left = at.x - (badge + badge_gap + name_size.x) * 0.5f;
                const ImVec2 name_at(left + badge + badge_gap, at.y - name_size.y - distance_size.y);
                draw_role_badge(draw, font, size, ImVec2(left, name_at.y), name_size.y, tag.tag, tag.color, alpha);
                draw->AddText(font, size, ImVec2(name_at.x + offset, name_at.y + offset), shadow, tag.name.c_str());
                const int name_vertices = draw->VtxBuffer.Size;
                draw->AddText(font, size, name_at, colour, tag.name.c_str());
                shade_nametag_gradient(draw, name_vertices, name_at.x, name_size.x, tag.color, animation_time);
                const ImVec2 distance_at(at.x - distance_size.x * 0.5f, at.y - distance_size.y);
                const auto dim = with_alpha(IM_COL32(230, 230, 230, 255), 0.85f * alpha);
                draw->AddText(font, detail, ImVec2(distance_at.x + offset, distance_at.y + offset), shadow, distance.c_str());
                draw->AddText(font, detail, distance_at, dim, distance.c_str());
                if (tag.talking) speaker(draw, ImVec2(left - size * 1.05f, name_at.y + name_size.y * 0.5f), size * 0.8f,
                                         with_alpha(animated_colour, alpha));
                bubble_bottom = name_at.y;
            }
        }
        if (value.show_bubbles && !tag.bubbles.empty() && tag.distance <= value.bubble_distance) {
            const float stack_top = draw_bubbles(draw, body_font, tag, at, bubble_bottom, k, value.bubble_distance,
                                                 animated_colour, frame);
            // When the name itself is not shown (nametags off, or a player past the name range),
            // label the stack so each bubble is still attributed.
            const bool named = !unnamed && value.show_names && tag.distance <= name_range;
            if (!named && !tag.name.empty() && stack_top < bubble_bottom - 1.0f) {
                const float nearness = 1.0f - std::clamp(tag.distance / name_range, 0.0f, 1.0f);
                const float label_size = std::clamp((12.0f + 3.0f * nearness) * k, 9.0f, 30.0f);
                const float alpha = 0.85f;
                const auto name_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, tag.name.c_str());
                const float badge = role_badge_width(font, label_size, tag.tag);
                const float badge_gap = badge > 0.0f ? label_size * 0.3f : 0.0f;
                const float left = at.x - (badge + badge_gap + name_size.x) * 0.5f;
                const float label_y = stack_top - name_size.y - label_size * 0.2f;
                draw_role_badge(draw, font, label_size, ImVec2(left, label_y), name_size.y, tag.tag, tag.color, alpha);
                const auto shadow = with_alpha(IM_COL32(0, 0, 0, 255), 0.8f * alpha);
                const float offset = std::max(1.0f, label_size / 14.0f);
                draw->AddText(font, label_size, ImVec2(left + badge + badge_gap + offset, label_y + offset), shadow,
                              tag.name.c_str());
                draw->AddText(font, label_size, ImVec2(left + badge + badge_gap, label_y),
                              with_alpha(animated_colour, alpha), tag.name.c_str());
            }
        }
    }
    // Drop layouts no bubble asked for this frame.
    std::erase_if(bubble_cache(), [&](const auto &entry) { return entry.second.frame != frame; });
}
} // namespace dingosdk::overlay::detail
