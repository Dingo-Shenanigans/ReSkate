#include "hall_of_meat_overlay.h"
#include "hall_of_meat.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include "Engine/Vfs/game_textures.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <optional>

namespace dingosdk::overlay {
namespace {
namespace ui = addr::ui_textures;
namespace theme = dingosdk::skate_theme;
using hall_of_meat::Stat;

// The images: an icon per stat (in Stat's order), then the logo and the card's shapes.
enum Picture : std::size_t { stat_icons, logo = stat_icons + 8, row_bar, panel, scratches, underline, picture_count };
struct Source {
    const ui::Texture* texture;
    std::uint32_t side;      // fitted into a side x side square, its aspect kept
    bool brightness{};       // white with the texture's brightness as alpha (light marks on black), else its own alpha
};
constexpr std::array<Source, picture_count> sources{{
    {&ui::stopwatch, 64}, {&ui::wipeout, 64}, {&ui::wipeout_broken, 64}, {&ui::spread_eagle, 64},
    {&ui::airtime, 64}, {&ui::gap_height, 64}, {&ui::flaming_wheel, 64}, {&ui::roll, 64},
    {&ui::thrasher_wordmark, 512}, {&ui::brush_bar, 512}, {&ui::rough_tile, 256}, {&ui::scratches, 512, true},
    {&ui::streak, 512},
}};
using Images = std::array<std::optional<frostbite::Image>, picture_count>;

struct Reads {
    std::mutex mutex;
    bool started{};
    std::future<Images> pending;
    Images decoded; // kept: every atlas the overlay builds (one per swapchain) needs them
};
Reads& reads() {
    static auto* state = new Reads;
    return *state;
}

// The picture's part of the texture, white: with its own alpha, or its brightness as alpha.
frostbite::Image cut(const frostbite::Image& image, const ui::Region& region, bool brightness) {
    const auto at = [](float fraction, std::uint32_t size) { return static_cast<std::uint32_t>(fraction * static_cast<float>(size) + 0.5f); };
    if (!image.width || !image.height || image.rgba.size() != std::size_t{image.width} * image.height * 4)
        throw std::runtime_error("the texture is empty");
    const auto x0 = std::min(at(region.left, image.width), image.width - 1), y0 = std::min(at(region.top, image.height), image.height - 1);
    const auto x1 = std::clamp(at(region.right, image.width), x0 + 1, image.width);
    const auto y1 = std::clamp(at(region.bottom, image.height), y0 + 1, image.height);
    frostbite::Image result{x1 - x0, y1 - y0, std::vector<std::uint8_t>(std::size_t{x1 - x0} * (y1 - y0) * 4)};
    for (std::uint32_t y = y0; y < y1; ++y)
        std::memcpy(result.rgba.data() + std::size_t{y - y0} * result.width * 4,
                    image.rgba.data() + (std::size_t{y} * image.width + x0) * 4, std::size_t{result.width} * 4);
    for (std::size_t i = 0; i < result.rgba.size(); i += 4) {
        auto* p = result.rgba.data() + i;
        if (brightness) p[3] = std::max({p[0], p[1], p[2]});
        p[0] = p[1] = p[2] = 255;
    }
    return result;
}

Images decode() {
    Images result;
    try {
        vfs::GameTextures textures(hall_of_meat::game_folder());
        for (std::size_t index = 0; index < picture_count; ++index) {
            const auto& source = sources[index];
            try {
                result[index] = cut(textures.read(source.texture->toc, source.texture->bundle, source.texture->name, source.side),
                                    source.texture->region, source.brightness);
            } catch (const std::exception& error) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "Hall of Meat image {} is unavailable: {}.",
                    source.texture->name, error.what());
            }
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Hall of Meat's images are unavailable: {}.", error.what());
    }
    return result;
}

// The images in the atlas being built or drawn with: render thread only.
struct Loaded {
    ImFontAtlas* atlas{};
    bool filled{};
    std::array<int, picture_count> rects{}; // each one's custom rect in the atlas; -1 none
};
Loaded& loaded() {
    static Loaded state;
    return state;
}
// A picture in the atlas the overlay draws with: where, and its texture coordinates.
struct Found {
    const ImFontAtlasCustomRect* rect{};
    ImVec2 uv0, uv1;
};
bool find(Picture picture, Found& found) {
    const auto& l = loaded();
    if (!l.filled || !l.atlas || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID || l.rects[picture] < 0) return false;
    found.rect = l.atlas->GetCustomRectByIndex(l.rects[picture]);
    if (!found.rect || !found.rect->IsPacked() || !found.rect->Width || !found.rect->Height) return false;
    l.atlas->CalcCustomRectUV(found.rect, &found.uv0, &found.uv1);
    return true;
}

// Fitted into the box with its aspect kept, centred: an icon, a logo.
bool draw_image(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const auto* rect = found.rect;
    const float scale = std::min((max.x - min.x) / rect->Width, (max.y - min.y) / rect->Height);
    const ImVec2 size(rect->Width * scale, rect->Height * scale);
    const ImVec2 at(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + size.x, at.y + size.y), found.uv0, found.uv1, tint);
    return true;
}
// Its body (ui_textures.h Texture::body) stretched onto the box, the rest of it around: a brush
// stroke's bar on the box, its splatter beyond.
bool draw_shape(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const auto& body = sources[picture].texture->body;
    const float width = (max.x - min.x) / (body.right - body.left), height = (max.y - min.y) / (body.bottom - body.top);
    const ImVec2 at(min.x - body.left * width, min.y - body.top * height);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + width, at.y + height), found.uv0, found.uv1, tint);
    return true;
}
// Nine-sliced onto the box: its corners (Texture::slice of the picture) kept `edge` pixels square,
// its edges stretched along them and its middle across the rest. A box smaller than two edges
// shrinks them to half of it.
bool draw_panel(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, float edge, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const float slice = sources[picture].texture->slice;
    const float edge_x = std::clamp(edge, 0.0f, (max.x - min.x) * 0.5f), edge_y = std::clamp(edge, 0.0f, (max.y - min.y) * 0.5f);
    const float uv_x = slice * (found.uv1.x - found.uv0.x), uv_y = slice * (found.uv1.y - found.uv0.y);
    const std::array<float, 4> xs{min.x, min.x + edge_x, max.x - edge_x, max.x}, ys{min.y, min.y + edge_y, max.y - edge_y, max.y};
    const std::array<float, 4> us{found.uv0.x, found.uv0.x + uv_x, found.uv1.x - uv_x, found.uv1.x};
    const std::array<float, 4> vs{found.uv0.y, found.uv0.y + uv_y, found.uv1.y - uv_y, found.uv1.y};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            draw->AddImage(loaded().atlas->TexID, ImVec2(xs[column], ys[row]), ImVec2(xs[column + 1], ys[row + 1]),
                           ImVec2(us[column], vs[row]), ImVec2(us[column + 1], vs[row + 1]), tint);
    return true;
}

// `colour` at `opacity` (0 to 1) of its own alpha.
ImU32 faded(ImU32 colour, float opacity) {
    const auto alpha = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(opacity, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}
// Text over a soft drop shadow, the shadow as see-through as the text.
void shadowed_text(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
    const float offset = std::max(1.0f, size / 16.0f);
    const float opacity = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), faded(IM_COL32_BLACK, opacity * 0.7f), text);
    draw->AddText(font, size, at, colour, text);
}
float text_width(ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x; }

constexpr ImU32 bruised = IM_COL32(255, 196, 36, 255), broken = IM_COL32(232, 32, 32, 255),
    dark_red = IM_COL32(150, 12, 12, 255), hot = IM_COL32(255, 255, 255, 255);
ImU32 mix(ImU32 from, ImU32 to, float amount) {
    const auto channel = [&](int shift) {
        const float a = static_cast<float>((from >> shift) & 0xff), b = static_cast<float>((to >> shift) & 0xff);
        return static_cast<int>(a + (b - a) * std::clamp(amount, 0.0f, 1.0f));
    };
    return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), channel(IM_COL32_A_SHIFT));
}
// `colour` darkened to `light` (0 black, 1 itself), at `alpha`.
ImU32 lit(ImU32 colour, float light, float alpha) {
    const auto channel = [&](int shift) {
        return static_cast<ImU32>(static_cast<float>((colour >> shift) & 0xff) * std::clamp(light, 0.0f, 1.0f)) << shift;
    };
    const auto a = static_cast<ImU32>(255.0f * std::clamp(alpha, 0.0f, 1.0f));
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | a << IM_COL32_A_SHIFT;
}

// The screen at `strength` (0 to 1), as a hard hit shows it: darkened a little, the broken bones' red
// washing in from all four edges, top and bottom deeper than the sides.
void draw_break_pulse(float strength) {
    constexpr float darkening = 0.3f, edge_opacity = 0.7f, top_bottom_reach = 0.3f, sides_reach = 0.18f;
    strength = std::clamp(strength, 0.0f, 1.0f);
    const auto display = ImGui::GetIO().DisplaySize;
    if (strength <= 0 || display.x <= 0 || display.y <= 0) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 min(0, 0), max(display.x, display.y);
    draw->AddRectFilled(min, max, faded(IM_COL32_BLACK, darkening * strength));
    const auto edge = faded(broken, edge_opacity * strength), clear = faded(broken, 0);
    const float vertical = display.y * top_bottom_reach * strength, horizontal = display.x * sides_reach * strength;
    // Each band from its edge (the colour) inwards (clear); corners: top left, top right, bottom right, bottom left.
    draw->AddRectFilledMultiColor(min, ImVec2(max.x, vertical), edge, edge, clear, clear);
    draw->AddRectFilledMultiColor(ImVec2(0, max.y - vertical), max, clear, clear, edge, edge);
    draw->AddRectFilledMultiColor(min, ImVec2(horizontal, max.y), edge, clear, clear, edge);
    draw->AddRectFilledMultiColor(ImVec2(max.x - horizontal, 0), max, clear, edge, edge, clear);
}

// The hurt bodies of skate.'s skeleton mesh as an x-ray over the world, projected like the nametags:
// bruised ones yellow, broken ones a slowly pulsing red, a fresh hit flashing white-hot; the rest are
// not drawn. Every triangle of a hurt body is drawn back to front, each vertex lit by how it faces
// the camera: an outline near solid and a middle fainter, so the bones read through each other and
// against the world. A triangle reaching into an unhurt body fades out.
void draw_skeleton(const hall_of_meat::Skeleton& skeleton) {
    using Vec3 = std::array<float, 3>;
    const auto dot = [](const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto& positions = skeleton.positions;
    const auto count = positions.size();
    const auto* mesh = skeleton.mesh.get();
    if (!count || count > 0xffff || !mesh || mesh->parts.size() != count || skeleton.normals.size() != count ||
        skeleton.alpha <= 0 || !(skeleton.vertical_fov > 1 && skeleton.vertical_fov < 175))
        return;
    std::array<ImU32, skater_body::count> paint{}; // 0: not drawn
    const double time = ImGui::GetTime();
    for (std::size_t body = 0; body < skater_body::count; ++body) {
        const auto injury = skeleton.injuries[body];
        if (injury == hall_of_meat::Injury::none) continue;
        const ImU32 colour = injury == hall_of_meat::Injury::broken
            ? mix(dark_red, broken, 0.5f + 0.5f * static_cast<float>(std::sin(time * 6.0))) : bruised;
        paint[body] = mix(colour, hot, skeleton.flashes[body] * 0.8f);
    }
    constexpr float solid = 0.6f;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto& m = skeleton.camera;
    const Vec3 right{m[0], m[1], m[2]}, up{m[4], m[5], m[6]}, back{m[8], m[9], m[10]}, origin{m[12], m[13], m[14]};
    const float focal = display.y / (2.0f * std::tan(skeleton.vertical_fov * 3.14159265f / 360.0f));
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    // Render thread only: reused frame to frame.
    static std::vector<ImVec2> at;
    static std::vector<float> depth;
    static std::vector<ImU32> colour; // 0 for an unhurt body's vertex
    static std::vector<std::pair<float, std::uint32_t>> order; // a triangle's depth, its first index
    at.resize(count);
    depth.resize(count);
    colour.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = positions[i];
        const Vec3 delta{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
        depth[i] = -dot(delta, back);
        const float z = std::max(depth[i], 0.1f);
        at[i] = ImVec2(centre.x + dot(delta, right) * focal / z, centre.y - dot(delta, up) * focal / z);
        const float distance = std::sqrt(dot(delta, delta));
        const float facing = distance > 1e-4f ? std::abs(dot(skeleton.normals[i], delta)) / distance : 1.0f;
        const auto painted = paint[std::min<std::size_t>(mesh->parts[i], skater_body::count - 1)];
        const float faint = 0.25f + 0.6f * (1.0f - facing);
        colour[i] = painted ? lit(painted, 0.55f + 0.45f * facing, skeleton.alpha * (solid + (1.0f - solid) * faint)) : 0;
    }
    const auto& triangles = mesh->triangles;
    order.clear();
    for (std::uint32_t t = 0; t + 2 < triangles.size(); t += 3) {
        const auto a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        if (a >= count || b >= count || c >= count || depth[a] <= 0.1f || depth[b] <= 0.1f || depth[c] <= 0.1f ||
            (!colour[a] && !colour[b] && !colour[c]))
            continue;
        order.emplace_back(depth[a] + depth[b] + depth[c], t);
    }
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; }); // far first
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    draw->PrimReserve(static_cast<int>(order.size() * 3), static_cast<int>(count));
    const auto first = draw->_VtxCurrentIdx;
    for (std::size_t i = 0; i < count; ++i) draw->PrimWriteVtx(at[i], uv, colour[i]);
    for (const auto& [unused, t] : order)
        for (std::uint32_t corner = 0; corner < 3; ++corner) draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + triangles[t + corner]));
}

// The card from frame to frame: the total counts up from what it last showed, and a row fades in
// from when it came.
struct Motion {
    float total{};
    double at{}; // ImGui time of the last frame drawn
    std::array<double, 8> since{-1, -1, -1, -1, -1, -1, -1, -1}; // each stat's row: when it came onto the card
};

// The score card as skate. 3's Hall of Meat shows it, in the bottom left corner where skate.'s own HUD
// shows (hidden meanwhile: hall_of_meat_hud.h), growing upwards as rows come: a brush bar per stat
// with its icon, value and points, then a scratched panel with the THRASHER logo, the title in its
// arch, the total on a blue stroke and the badge. skate.'s colours: dark bars and panel, its blue for
// the icons, the logo and the stroke, white numbers. A shape not loaded draws a plain tile instead.
void draw_card(const hall_of_meat::Card& card, Motion& motion) {
    // From the screen's bottom left corner, in 1080p pixels; as wide as the logo it carries.
    constexpr float corner_left = 64.0f, corner_bottom = 96.0f, card_width = 250.0f;
    constexpr double row_fade_seconds = 0.3; // a new row opens and fades in this long
    const auto display = ImGui::GetIO().DisplaySize;
    if (card.opacity <= 0 || display.x <= 0 || display.y <= 0) {
        motion = {};
        return;
    }
    // The total catches up with its value in about a quarter of a second.
    const double now = ImGui::GetTime();
    const float step = static_cast<float>(std::clamp(now - motion.at, 0.0, 0.1));
    motion.at = now;
    motion.total += (static_cast<float>(card.total) - motion.total) * (1.0f - std::exp(-step * 12.0f));
    if (std::abs(static_cast<float>(card.total) - motion.total) < 1.0f) motion.total = static_cast<float>(card.total);

    auto& s = detail::state();
    auto* title_font = s.menu.title ? s.menu.title : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* draw = ImGui::GetBackgroundDrawList();
    const float k = display.y / 1080.0f, o = card.opacity;
    const auto fade = [o](ImU32 colour) { return faded(colour, o); };
    const float left = corner_left * k, right = left + card_width * k, pad = 14.0f * k;
    unsigned seed = 211u;

    // The panel's height, under the rows: the logo, the title (in the logo's arch), the total and the badge.
    const float logo_height = 72.0f * k, title_size = 22.0f * k, total_size = 56.0f * k, badge_size = 18.0f * k;
    const float edge = 16.0f * k;
    const float logo_step = logo_height * ui::thrasher_wordmark_arch + 4.0f * k;
    const float height = pad * 2.0f + logo_step + title_size + total_size + (card.badge.empty() ? 0.0f : badge_size + 4.0f * k);

    // A bar per stat: its icon, what was measured and what it scored. A new one opens and fades in,
    // and the card grows upwards by it. Every bar is drawn before any row's content, so no bar's
    // splatter covers another's numbers.
    struct Line {
        const hall_of_meat::CardRow* row{};
        float top{}, shown{};
    };
    const float row_height = 30.0f * k, row_gap = 10.0f * k, icon = 22.0f * k, text = 18.0f * k;
    std::vector<Line> lines;
    float rows_height{};
    for (const auto& row : card.rows) {
        auto& came = motion.since[static_cast<std::size_t>(row.stat)];
        if (came < 0) came = now;
        const float shown = static_cast<float>(std::clamp((now - came) / row_fade_seconds, 0.0, 1.0));
        lines.push_back({&row, rows_height, shown});
        rows_height += (row_height + row_gap) * shown * (2.0f - shown); // eases out
    }
    float y = display.y - corner_bottom * k - height - rows_height;
    for (auto& line : lines) line.top += y;
    y += rows_height;
    for (const auto& line : lines) {
        const ImVec2 min(left, line.top), max(right, line.top + row_height);
        const auto colour = faded(theme::tile, o * line.shown * 0.92f);
        if (!draw_shape(draw, row_bar, min, max, colour)) theme::rough_rect(draw, min, max, colour, ++seed, k);
    }
    for (const auto& line : lines) {
        const auto& row = *line.row;
        const float middle = line.top + row_height * 0.5f;
        const auto row_fade = [&](ImU32 colour) { return faded(colour, o * line.shown); };
        float x = left + pad;
        if (draw_image(draw, static_cast<Picture>(stat_icons + static_cast<std::size_t>(row.stat)), ImVec2(x, middle - icon * 0.5f),
                       ImVec2(x + icon, middle + icon * 0.5f), row_fade(theme::blue)))
            x += icon + 10.0f * k;
        shadowed_text(draw, bold, text, ImVec2(x, middle - text * 0.5f), row_fade(theme::white), row.value.c_str());
        const auto points = hall_of_meat::grouped(row.points);
        shadowed_text(draw, bold, text, ImVec2(right - pad - text_width(bold, text, points), middle - text * 0.5f),
                      row_fade(theme::white), points.c_str());
    }

    const ImVec2 min(left, y), max(right, y + height);
    const auto tile = fade(faded(theme::tile, 0.92f));
    if (draw_panel(draw, panel, min, max, edge, tile))
        draw_shape(draw, scratches, ImVec2(min.x + edge, min.y + edge), ImVec2(max.x - edge, max.y - edge), fade(faded(theme::white, 0.5f)));
    else
        theme::rough_rect(draw, min, max, tile, ++seed, k);
    const float centre = (left + right) * 0.5f;
    const auto centred = [&](ImFont* font, float size, ImU32 colour, const std::string& line) {
        shadowed_text(draw, font, size, ImVec2(centre - text_width(font, size, line) * 0.5f, y), colour, line.c_str());
        y += size;
    };
    y += pad;
    draw_image(draw, logo, ImVec2(left + pad, y), ImVec2(right - pad, y + logo_height), fade(theme::blue));
    y += logo_step;
    centred(heading, title_size, fade(theme::white), "Hall of Meat");
    const auto total = hall_of_meat::grouped(static_cast<long long>(motion.total + 0.5f));
    const float stroke = text_width(title_font, total_size, total) * 0.5f + 20.0f * k;
    draw_shape(draw, underline, ImVec2(centre - stroke, y + total_size * 0.55f), ImVec2(centre + stroke, y + total_size * 0.95f),
               fade(theme::blue));
    centred(title_font, total_size, fade(theme::white), total);
    if (!card.badge.empty()) {
        y += 4.0f * k;
        centred(bold, badge_size, fade(card.highlight ? theme::good : theme::grey_text), card.badge);
    }
}

// Render thread only: this frame's, and the card's motion.
struct Shown {
    hall_of_meat::Frame frame;
    Motion motion;
};
Shown& shown() {
    static Shown value;
    return value;
}
} // namespace

void prepare_hall_of_meat_images() noexcept {
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        if (std::exchange(state.started, true)) return;
        state.pending = std::async(std::launch::async, &decode);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "Hall of Meat's images are unavailable: their reading could not start.");
    }
}

void reserve_hall_of_meat_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept {
    auto& l = loaded();
    l = {};
    l.rects.fill(-1);
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        // A read still running stays pending for the next atlas.
        if (state.pending.valid() && state.pending.wait_for(wait) == std::future_status::ready) state.decoded = state.pending.get();
        for (std::size_t index = 0; index < picture_count; ++index) {
            const auto& image = state.decoded[index];
            if (!image) continue;
            if (!l.atlas) {
                atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
                atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
                l.atlas = &atlas;
            }
            l.rects[index] = atlas.AddCustomRectRegular(static_cast<int>(image->width), static_cast<int>(image->height));
        }
    } catch (...) {
        l = {};
    }
}

void fill_hall_of_meat_images(ImFontAtlas& atlas) noexcept {
    auto& l = loaded();
    if (l.atlas != &atlas) return;
    try {
        unsigned char* pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) {
            l = {};
            return;
        }
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        for (std::size_t index = 0; index < picture_count; ++index) {
            if (l.rects[index] < 0) continue;
            const auto& image = *state.decoded[index];
            const auto* rect = atlas.GetCustomRectByIndex(l.rects[index]);
            if (!rect || !rect->IsPacked() || rect->X + image.width > static_cast<unsigned>(width) ||
                rect->Y + image.height > static_cast<unsigned>(height)) {
                l = {};
                return;
            }
            for (std::uint32_t y = 0; y < image.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4, static_cast<std::size_t>(image.width) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool hall_of_meat_pending() {
    auto& s = shown();
    try {
        s.frame = hall_of_meat::frame();
    } catch (...) {
        s.frame = {};
    }
    return !s.frame.skeleton.positions.empty() || s.frame.card.opacity > 0 || s.frame.break_pulse > 0;
}

void draw_hall_of_meat() {
    auto& s = shown();
    draw_break_pulse(s.frame.break_pulse); // under the skeleton and the card
    draw_skeleton(s.frame.skeleton);
    draw_card(s.frame.card, s.motion);
}
}
