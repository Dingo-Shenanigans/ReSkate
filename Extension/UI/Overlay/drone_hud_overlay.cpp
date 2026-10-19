#include "overlay_internal.h"
#include <cstdint>
#include "Extension/Skater/drone_font.h"
#include "Extension/Skater/drone_osd.h"
#include "Extension/Skater/drone_beacon.h"
#include "Engine/Game/UI/game_view.h"

// The FPV drone's goggle-style display and stick overlay. What to show comes from drone_osd.h;
// this draws it over the game (pixel font, drawn rectangle by rectangle, so no font file is needed).

namespace dingosdk::overlay::detail {
namespace {
dingosdk::drone::osd::Layout& hud() {
    static dingosdk::drone::osd::Layout value;
    return value;
}

ImU32 colour_of(const dingosdk::drone::osd::Layout& layout, bool warn, int alpha = 255) {
    if (warn) return IM_COL32(255, 60, 60, alpha);
    const auto rgb = dingosdk::drone::osd::preset_colour(layout.colour);
    return IM_COL32(rgb[0], rgb[1], rgb[2], alpha);
}

// Width of a pixel-font string in screen pixels, `px` being the size of one font pixel.
float pixel_width(const std::string& text, float px) {
    if (text.empty()) return 0.0f;
    return (static_cast<float>(text.size()) * (dingosdk::drone::font::columns + 1) - 1) * px;
}

void pixel_text(ImDrawList* draw, ImVec2 at, const std::string& text, float px, float tall, ImU32 colour, ImU32 shadow) {
    const float outline = std::max(1.0f, px * 0.5f);
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 c = pass == 0 ? shadow : colour;
        const float grow = pass == 0 ? outline : 0.0f;
        float x = at.x;
        for (const char ch : text) {
            const auto rows = dingosdk::drone::font::glyph(ch);
            for (int r = 0; r < dingosdk::drone::font::rows; ++r)
                for (int col = 0; col < dingosdk::drone::font::columns; ++col)
                    if (rows[static_cast<std::size_t>(r)] & (1 << (dingosdk::drone::font::columns - 1 - col))) {
                        const float x0 = x + static_cast<float>(col) * px, y0 = at.y + static_cast<float>(r) * px * tall;
                        draw->AddRectFilled(ImVec2(x0 - grow, y0 - grow), ImVec2(x0 + px + grow, y0 + px * tall + grow), c);
                    }
            x += (dingosdk::drone::font::columns + 1) * px;
        }
    }
}

void draw_stick(ImDrawList* draw, ImVec2 centre, float half, float x, float y, ImU32 colour, ImU32 shadow) {
    const float t = std::max(1.5f, half * 0.04f);
    const ImVec2 lo(centre.x - half, centre.y - half), hi(centre.x + half, centre.y + half);
    draw->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 70));
    draw->AddRect(lo, hi, shadow, 0.0f, 0, t * 2.4f);
    draw->AddRect(lo, hi, colour, 0.0f, 0, t);
    draw->AddLine(ImVec2(centre.x - half, centre.y), ImVec2(centre.x + half, centre.y), IM_COL32(255, 255, 255, 60), t * 0.6f);
    draw->AddLine(ImVec2(centre.x, centre.y - half), ImVec2(centre.x, centre.y + half), IM_COL32(255, 255, 255, 60), t * 0.6f);
    const float limit = half * 0.86f;
    const ImVec2 dot(centre.x + std::clamp(x, -1.0f, 1.0f) * limit, centre.y - std::clamp(y, -1.0f, 1.0f) * limit);
    const float radius = half * 0.12f;
    draw->AddCircleFilled(dot, radius + t, shadow);
    draw->AddCircleFilled(dot, radius, colour);
}

std::vector<dingosdk::drone::beacon::Remote>& beacons() {
    static std::vector<dingosdk::drone::beacon::Remote> value;
    return value;
}

std::string metres_text(float metres) {
    char text[32]{};
    if (metres < 1000.0f) std::snprintf(text, sizeof text, "%.0f m", std::floor(metres));
    else std::snprintf(text, sizeof text, "%.1f km", metres / 1000.0f);
    return text;
}

// An analogue FPV video look drawn over the game: scanlines, static, dark corners and rolling glitches.
// It is an overlay, so it adds texture and darkening; it cannot blur or smear the game picture itself.
float analog_knob(dingosdk::drone::Knob k) { return dingosdk::drone::knobs().value[k].load(std::memory_order_relaxed); }
std::uint32_t next_random(std::uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
float unit_random(std::uint32_t& state) { return static_cast<float>(next_random(state) & 0xffffff) / 16777216.0f; }

void draw_analog(ImDrawList* draw, const ImVec2& display) {
    using namespace dingosdk::drone;
    // Range breaks the picture up near its limit (drone_flight.h: signal_strength), with or without
    // the analogue look; past the limit only static is left.
    const float signal = std::clamp(telemetry().signal.load(std::memory_order_relaxed), 0.0f, 1.0f);
    // A hit breaks the picture up for a moment too (drone_flight.h: the glitch after a crash).
    const float breakup = std::max(1.0f - signal, std::clamp(telemetry().glitch.load(std::memory_order_relaxed), 0.0f, 1.0f));
    const bool look = analog_knob(kAnalog) >= 0.5f;
    if (!look && breakup <= 0.0f) return;
    const float noise = std::max(look ? std::clamp(analog_knob(kAnalogNoise), 0.0f, 1.0f) : 0.0f, breakup);
    const float scan = look ? std::clamp(analog_knob(kAnalogScan), 0.0f, 1.0f) : 0.0f;
    const float vignette = std::max(look ? std::clamp(analog_knob(kAnalogVignette), 0.0f, 1.0f) : 0.0f, breakup * 0.6f);
    const float glitch = std::max(look ? std::clamp(analog_knob(kAnalogGlitch), 0.0f, 1.0f) : 0.0f, breakup);

    const float time = static_cast<float>(ImGui::GetTime());
    const float unit = display.y / 1080.0f;
    static std::uint32_t frame = 1;
    std::uint32_t rng = 0x9e3779b9u ^ (++frame * 2654435761u);
    const auto alpha = [](float a) { return static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f); };

    // A cold, slightly washed-out cast, and a faint brightness flicker.
    const float flicker = 0.02f + 0.02f * std::sin(time * 37.0f) + 0.02f * unit_random(rng);
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(40, 70, 70, alpha(0.05f + 0.07f * scan)));
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, alpha(flicker * (0.4f + noise))));

    // Scanlines: a dark line every other row of a 540-line picture.
    if (scan > 0.0f) {
        const float gap = std::max(2.0f, 2.0f * unit);
        const float thickness = std::max(1.0f, gap * 0.5f);
        const ImU32 line = IM_COL32(0, 0, 0, alpha(0.10f + 0.35f * scan));
        for (float y = 0.0f; y < display.y; y += gap) draw->AddRectFilled(ImVec2(0, y), ImVec2(display.x, y + thickness), line);
    }

    // Dark corners, as soft bands in from each edge.
    if (vignette > 0.0f) {
        const int edge = alpha(0.65f * vignette), clear = 0;
        const float bx = display.x * 0.20f, by = display.y * 0.24f;
        const ImU32 dark = IM_COL32(0, 0, 0, edge), none = IM_COL32(0, 0, 0, clear);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bx, display.y), dark, none, none, dark);
        draw->AddRectFilledMultiColor(ImVec2(display.x - bx, 0), ImVec2(display.x, display.y), none, dark, dark, none);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, by), dark, dark, none, none);
        draw->AddRectFilledMultiColor(ImVec2(0, display.y - by), ImVec2(display.x, display.y), none, none, dark, dark);
    }

    // Static: random grey specks and short horizontal streaks.
    if (noise > 0.0f) {
        const int specks = static_cast<int>(2200.0f * noise);
        const float speck = std::max(1.0f, 1.5f * unit);
        for (int i = 0; i < specks; ++i) {
            const float x = unit_random(rng) * display.x, y = unit_random(rng) * display.y;
            const int v = 150 + static_cast<int>(unit_random(rng) * 105.0f);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + speck, y + speck), IM_COL32(v, v, v, alpha(0.12f + 0.35f * noise * unit_random(rng))));
        }
        const int streaks = static_cast<int>(14.0f * noise);
        for (int i = 0; i < streaks; ++i) {
            const float x = unit_random(rng) * display.x, y = unit_random(rng) * display.y;
            const float length = (20.0f + unit_random(rng) * 160.0f) * unit;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + length, y + std::max(1.0f, unit)), IM_COL32(220, 235, 235, alpha(0.10f + 0.2f * noise)));
        }
    }

    // Glitches: a pale band that rolls down the picture, and now and then a burst of torn lines.
    if (glitch > 0.0f) {
        const float cycle = 5.0f;
        const float t = std::fmod(time, cycle) / cycle;
        const float band_height = display.y * 0.09f;
        const float y = -band_height + t * (display.y + band_height * 2.0f);
        draw->AddRectFilled(ImVec2(0, y), ImVec2(display.x, y + band_height), IM_COL32(200, 230, 230, alpha(0.05f + 0.07f * glitch)));
        draw->AddRectFilled(ImVec2(0, y + band_height * 0.45f), ImVec2(display.x, y + band_height * 0.55f), IM_COL32(255, 255, 255, alpha(0.04f + 0.06f * glitch)));
        const float burst = std::fmod(time, 7.0f);
        if (burst < 0.25f * (0.4f + glitch)) {
            const int tears = 3 + static_cast<int>(8.0f * glitch);
            for (int i = 0; i < tears; ++i) {
                const float ty = unit_random(rng) * display.y;
                const float h = (2.0f + unit_random(rng) * 10.0f) * unit;
                const float shift = (unit_random(rng) - 0.5f) * display.x * 0.08f * glitch;
                draw->AddRectFilled(ImVec2(std::max(0.0f, shift), ty), ImVec2(display.x + std::min(0.0f, shift), ty + h),
                                    IM_COL32(230, 240, 240, alpha(0.10f + 0.18f * glitch)));
                draw->AddRectFilled(ImVec2(0, ty + h), ImVec2(display.x, ty + h + std::max(1.0f, unit)), IM_COL32(0, 0, 0, alpha(0.35f * glitch)));
            }
        }
    }
    // Range: as the link weakens, the picture sinks under grey snow, smoothly, until only snow is
    // left where it is lost. Drawn on top of everything above, so it reads the same with or
    // without the analogue look.
    if (breakup > 0.0f) {
        const float b = std::clamp(breakup, 0.0f, 1.0f);
        const float fade = b * b * (3.0f - 2.0f * b); // smooth in and out
        draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(24, 26, 26, alpha(0.92f * fade * fade)));
        const int flakes = static_cast<int>(9000.0f * std::pow(b, 1.4f));
        const float flake = std::max(1.0f, 2.0f * unit);
        for (int i = 0; i < flakes; ++i) {
            const float x = unit_random(rng) * display.x, y = unit_random(rng) * display.y;
            const int v = 110 + static_cast<int>(unit_random(rng) * 145.0f);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + flake, y + flake), IM_COL32(v, v, v, alpha(0.25f + 0.6f * b * unit_random(rng))));
        }
        // Torn lines of snow across the picture, more of them the weaker it gets.
        const int lines = static_cast<int>(40.0f * b * b);
        for (int i = 0; i < lines; ++i) {
            const float y = unit_random(rng) * display.y, h = (1.0f + unit_random(rng) * 3.0f) * unit;
            const int v = 150 + static_cast<int>(unit_random(rng) * 100.0f);
            draw->AddRectFilled(ImVec2(0, y), ImVec2(display.x, y + h), IM_COL32(v, v, v, alpha(0.08f + 0.3f * b * unit_random(rng))));
        }
    }
}

// A little quadcopter: an X with a propeller circle at each end, drawn around `at`.
void drone_icon(ImDrawList* draw, ImVec2 at, float size, ImU32 colour, ImU32 shadow) {
    const float arm = size * 0.5f, prop = size * 0.2f, t = std::max(1.5f, size * 0.09f);
    const ImVec2 ends[4] = {{-arm, -arm}, {arm, -arm}, {arm, arm}, {-arm, arm}};
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 c = pass == 0 ? shadow : colour;
        const float w = pass == 0 ? t * 2.4f : t;
        draw->AddLine(ImVec2(at.x + ends[0].x, at.y + ends[0].y), ImVec2(at.x + ends[2].x, at.y + ends[2].y), c, w);
        draw->AddLine(ImVec2(at.x + ends[1].x, at.y + ends[1].y), ImVec2(at.x + ends[3].x, at.y + ends[3].y), c, w);
        for (const auto& e : ends) draw->AddCircle(ImVec2(at.x + e.x, at.y + e.y), prop, c, 14, w);
    }
    draw->AddCircleFilled(at, size * 0.1f, colour);
}

// Markers for other players' drones, placed over the world with the camera the client last used.
void draw_beacons(const std::vector<dingosdk::drone::beacon::Remote>& list) {
    if (list.empty()) return;
    const auto view = dingosdk::latest_game_view();
    if (!view || !(view->vertical_fov > 1 && view->vertical_fov < 175)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const auto& m = view->world;
    const float k = display.y / 1080.0f;
    const float focal = display.y / (2.0f * std::tan(view->vertical_fov * 3.14159265f / 360.0f));
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    const float margin = 40.0f * k;
    auto& s = state();
    auto* font = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* draw = ImGui::GetBackgroundDrawList();
    const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    const ImU32 colour = IM_COL32(90, 232, 255, static_cast<int>(255 * pulse));
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    for (const auto& other : list) {
        // The marker sits above the drone's model (Multiplayer/Remote/native_prop.h), not on it:
        // anchored this far above the drone, then lifted by half the icon and a gap on screen.
        constexpr float above_model = 0.25f;
        const float dx = other.position[0] - m[12], dy = other.position[1] + above_model - m[13], dz = other.position[2] - m[14];
        const float depth = -(dx * m[8] + dy * m[9] + dz * m[10]);
        const float side = dx * m[0] + dy * m[1] + dz * m[2];
        const float height = dx * m[4] + dy * m[5] + dz * m[6];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const std::string label = (other.name.empty() ? std::string("Player") : other.name) + "'s drone";
        const std::string metres = metres_text(distance);
        const float size = std::clamp(14.0f * k, 10.0f, 30.0f);
        ImVec2 at;
        bool on_screen = false;
        if (depth > 0.1f) {
            at = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
            on_screen = at.x >= margin && at.x <= display.x - margin && at.y >= margin && at.y <= display.y - margin;
        }
        if (on_screen) {
            // Stacked upwards from the model: the icon, the distance, then the name.
            const float icon = std::clamp(18.0f * k, 12.0f, 32.0f);
            const ImVec2 icon_at(at.x, at.y - icon * 0.5f - 6.0f * k);
            drone_icon(draw, icon_at, icon, colour, shadow);
            const float offset = std::max(1.0f, size / 14.0f);
            const float fine_size = size * 0.78f;
            const auto metres_size = font->CalcTextSizeA(fine_size, FLT_MAX, 0.0f, metres.c_str());
            const ImVec2 metres_at(at.x - metres_size.x * 0.5f, icon_at.y - icon * 0.5f - 4.0f * k - metres_size.y);
            draw->AddText(font, fine_size, ImVec2(metres_at.x + offset, metres_at.y + offset), shadow, metres.c_str());
            draw->AddText(font, fine_size, metres_at, IM_COL32(235, 235, 235, 230), metres.c_str());
            const auto text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
            const ImVec2 text_at(at.x - text_size.x * 0.5f, metres_at.y - text_size.y);
            draw->AddText(font, size, ImVec2(text_at.x + offset, text_at.y + offset), shadow, label.c_str());
            draw->AddText(font, size, text_at, colour, label.c_str());
        } else {
            // Off screen: an arrow at the screen's edge pointing to it, with the distance.
            float ex = depth > 0.1f ? at.x - centre.x : side, ey = depth > 0.1f ? at.y - centre.y : -height;
            if (depth <= 0.1f && std::abs(ex) < 1e-3f && std::abs(ey) < 1e-3f) ey = 1.0f; // straight behind
            const float hx = centre.x - margin, hy = centre.y - margin;
            const float scale = std::min(std::abs(ex) > 1e-6f ? hx / std::abs(ex) : FLT_MAX,
                                         std::abs(ey) > 1e-6f ? hy / std::abs(ey) : FLT_MAX);
            const ImVec2 edge(centre.x + ex * scale, centre.y + ey * scale);
            const float length = std::sqrt(ex * ex + ey * ey);
            const ImVec2 dir(length > 1e-6f ? ex / length : 0.0f, length > 1e-6f ? ey / length : 1.0f);
            const ImVec2 perp(-dir.y, dir.x);
            const float a = 13.0f * k;
            const ImVec2 tip(edge.x + dir.x * a, edge.y + dir.y * a);
            const ImVec2 b1(edge.x - dir.x * a * 0.5f + perp.x * a * 0.8f, edge.y - dir.y * a * 0.5f + perp.y * a * 0.8f);
            const ImVec2 b2(edge.x - dir.x * a * 0.5f - perp.x * a * 0.8f, edge.y - dir.y * a * 0.5f - perp.y * a * 0.8f);
            draw->AddTriangleFilled(ImVec2(tip.x + 2.0f * k, tip.y + 2.0f * k), ImVec2(b1.x + 2.0f * k, b1.y + 2.0f * k),
                                    ImVec2(b2.x + 2.0f * k, b2.y + 2.0f * k), shadow);
            draw->AddTriangleFilled(tip, b1, b2, colour);
            const float fine_size = size * 0.78f;
            const auto metres_size = font->CalcTextSizeA(fine_size, FLT_MAX, 0.0f, metres.c_str());
            ImVec2 text_at(edge.x - dir.x * a * 2.2f - metres_size.x * 0.5f, edge.y - dir.y * a * 2.2f - metres_size.y * 0.5f);
            text_at.x = std::clamp(text_at.x, 4.0f * k, display.x - metres_size.x - 4.0f * k);
            text_at.y = std::clamp(text_at.y, 4.0f * k, display.y - metres_size.y - 4.0f * k);
            const float offset = std::max(1.0f, fine_size / 14.0f);
            draw->AddText(font, fine_size, ImVec2(text_at.x + offset, text_at.y + offset), shadow, metres.c_str());
            draw->AddText(font, fine_size, text_at, colour, metres.c_str());
        }
    }
}
} // namespace

bool drone_hud_pending() {
    hud() = dingosdk::drone::osd::build();
    beacons().clear();
    if (dingosdk::drone::knobs().value[dingosdk::drone::kShowBeacons].load(std::memory_order_relaxed) >= 0.5f)
        beacons() = dingosdk::drone::beacon::snapshot();
    return hud().active || !beacons().empty();
}

void draw_drone_hud() {
    const auto& layout = hud();
    {   // The analogue look goes under everything else, so markers and the OSD stay crisp.
        const auto size = ImGui::GetIO().DisplaySize;
        if (layout.active && size.x > 0 && size.y > 0) draw_analog(ImGui::GetBackgroundDrawList(), size);
    }
    draw_beacons(beacons());
    if (!layout.active) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    const float unit = display.y / 1080.0f;
    const float px = std::max(1.0f, layout.scale * 2.0f * unit);   // one font pixel
    constexpr float tall = 1.45f;                                   // goggle glyphs are taller than wide
    const float text_height = dingosdk::drone::font::rows * px * tall;
    const ImU32 shadow = IM_COL32(0, 0, 0, 220);
    const ImU32 colour_normal = colour_of(layout, false);
    auto* smooth_font = ImGui::GetFont();

    for (const auto& item : layout.items) {
        const ImU32 colour = colour_of(layout, item.warn);
        const float height = text_height;
        float width;
        if (layout.font == 0) width = pixel_width(item.text, px);
        else width = smooth_font->CalcTextSizeA(height, FLT_MAX, 0.0f, item.text.c_str()).x;
        ImVec2 at(item.x * display.x, item.y * display.y);
        if (item.align == dingosdk::drone::osd::Align::centre) at.x -= width * 0.5f;
        else if (item.align == dingosdk::drone::osd::Align::right) at.x -= width;
        if (item.bottom) at.y -= height;
        if (layout.font == 0) {
            pixel_text(draw, at, item.text, px, tall, colour, shadow);
        } else {
            const float offset = std::max(1.0f, height / 14.0f);
            draw->AddText(smooth_font, height, ImVec2(at.x + offset, at.y + offset), shadow, item.text.c_str());
            draw->AddText(smooth_font, height, at, colour, item.text.c_str());
        }
    }

    if (layout.crosshair) {
        const float arm = 14.0f * unit * layout.scale, gap = 4.0f * unit * layout.scale, t = std::max(2.0f, 2.0f * unit * layout.scale);
        const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
        for (int pass = 0; pass < 2; ++pass) {
            const ImU32 col = pass == 0 ? shadow : colour_normal;
            const float w = pass == 0 ? t * 2.2f : t;
            draw->AddLine(ImVec2(c.x - gap - arm, c.y), ImVec2(c.x - gap, c.y), col, w);
            draw->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + gap + arm, c.y), col, w);
            draw->AddLine(ImVec2(c.x, c.y - gap - arm), ImVec2(c.x, c.y - gap), col, w);
            draw->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + gap + arm), col, w);
        }
    }

    if (layout.sticks.show) {
        // Mode 2: left stick = yaw (side to side) and throttle (up and down); right stick = roll and pitch.
        const float half = 0.5f * 0.17f * display.y * std::clamp(layout.sticks.size, 0.4f, 3.0f);
        const float margin = layout.margin * display.x;
        const float bottom = display.y * (1.0f - layout.margin) - text_height * 1.6f;
        const float cy = bottom - half;
        const ImVec2 left(margin + half, cy), right(display.x - margin - half, cy);
        draw_stick(draw, left, half, layout.sticks.yaw, layout.sticks.throttle, colour_normal, shadow);
        draw_stick(draw, right, half, layout.sticks.roll, layout.sticks.pitch, colour_normal, shadow);
    }
}
} // namespace dingosdk::overlay::detail
