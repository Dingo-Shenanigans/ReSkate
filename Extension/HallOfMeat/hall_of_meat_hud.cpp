#include "hall_of_meat_hud.h"
#include "hom_art.h"
#include "hom_audio.h"
#include "hom_grade.h"
#include "hom_bones.h"
#include "hom_core.h"
#include "hom_rig.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/UI/game_view.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Trainer/trainer.h"
#include <algorithm>
#include <utility>
#include <chrono>
#include <cmath>
#include <format>
#include <imgui.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Hall Of Meat, as Skate 3 draws it when the skater crashes: the picture closes in to a dark,
// grainy spotlight, the hurt and broken bones show through the body, and bottom left a bone chip
// and a stack of metric rows sit over the Thrasher "Hall of Meat" mark and the score.
namespace dingosdk::overlay {
namespace {
using namespace dingosdk::hall_of_meat;
using Clock = std::chrono::steady_clock;

constexpr double linger_seconds = 4.0, fade_seconds = 0.8;
constexpr ImU32 cyan = IM_COL32(150, 232, 240, 255);
constexpr ImU32 white = IM_COL32(246, 246, 246, 255);
constexpr ImU32 chip_blue = IM_COL32(32, 168, 214, 235);

struct State {
    std::mutex mutex; // the game thread feeds the tracker, the presentation thread draws it
    Tracker tracker;
    double visible_until{};
    double started{};
    float shown_total{};
    int best{};
    std::uint32_t last_state{};
    double last_time{-1};
    double heartbeat{};
    float darkness{};
    double watch_logged{};
    int cancelled_seen{};
    std::uint32_t xray_serial{};
    double upright_since{-1}, getting_up_at{-1}; // the live bail's skater getting up (X-ray fading out)
    std::uint32_t sounded_serial{};
    std::size_t sounded{}; // damage events of the live bail already heard
};
State &state() {
    static State value;
    return value;
}

double clock_seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::string with_commas(int value) {
    auto text = std::to_string(value);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<std::size_t>(i), ",");
    return text;
}
ImU32 alpha(ImU32 colour, float a) {
    const auto base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff);
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(base * a, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    const float offset = std::max(1.0f, size / 14.0f);
    const auto a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), alpha(IM_COL32(0, 0, 0, 255), a * 0.85f), text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
void image(ImDrawList *draw, std::string_view name, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    HomArt art;
    if (hom_art(name, art)) draw->AddImage(art.texture, a, b, art.uv0, art.uv1, tint);
}
bool has_art(std::string_view name) {
    HomArt art;
    return hom_art(name, art);
}

// Small icons for rows the original art does not give us a texture for.
void icon_rotation(ImDrawList *draw, ImVec2 c, float r, ImU32 colour) {
    draw->PathArcTo(c, r * 0.75f, -0.3f, 4.6f, 24);
    draw->PathStroke(colour, 0, r * 0.28f);
    draw->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.5f), ImVec2(c.x + r * 0.35f, c.y - r * 0.55f),
                            ImVec2(c.x + r * 0.7f, c.y + r * 0.05f), colour);
}
void icon_air(ImDrawList *draw, ImVec2 c, float r, ImU32 colour) {
    draw->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r * 0.9f, c.y + r * 0.6f), ImVec2(c.x + r * 0.9f, c.y + r * 0.6f), colour);
    draw->AddTriangleFilled(ImVec2(c.x, c.y + r * 0.25f), ImVec2(c.x - r * 0.35f, c.y + r), ImVec2(c.x + r * 0.35f, c.y + r), alpha(colour, 0.7f));
}

struct Row {
    std::string_view icon;
    int kind;  // 0 rotation, 1 air, 2 drop, 3 duration, 4 speed
    std::string value, points;
};

void draw_block(ImDrawList *draw, const Result &r, float total_shown, float fade, float scale, double now, double started) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto *font = hom_font(28) ? hom_font(28) : ImGui::GetFont();
    auto *score_font = hom_font(60) ? hom_font(60) : font;
    auto *small_font = hom_font(22) ? hom_font(22) : font;
    const float left = display.x * 0.083f, right = display.x * 0.23f, bottom = display.y * 0.925f;
    const float width = right - left;
    const float intro = std::clamp(static_cast<float>((now - started) / 0.25), 0.0f, 1.0f);
    const float a = fade * intro;
    const float slide = (1.0f - intro) * -24.0f * scale;
    const float cx = left + width * 0.5f + slide;

    // Score and mark.
    const float score_size = 60.0f * scale;
    const auto score = with_commas(static_cast<int>(total_shown));
    const auto extent = score_font->CalcTextSizeA(score_size, FLT_MAX, 0.0f, score.c_str());
    const float score_y = bottom - score_size - 2.0f * scale;
    shadowed(draw, score_font, score_size, ImVec2(cx - extent.x * 0.5f, score_y), alpha(white, a), score);
    draw->AddRectFilled(ImVec2(left + slide, bottom), ImVec2(right + slide, bottom + 2.0f * scale), alpha(IM_COL32(170, 220, 235, 190), a));

    const float hom_size = 22.0f * scale;
    const auto hom = std::string("Hall of Meat");
    const auto hom_extent = small_font->CalcTextSizeA(hom_size, FLT_MAX, 0.0f, hom.c_str());
    const float hom_y = score_y - hom_size - 2.0f * scale;
    shadowed(draw, small_font, hom_size, ImVec2(cx - hom_extent.x * 0.5f, hom_y), alpha(IM_COL32(236, 236, 236, 255), a), hom);

    const float logo_h = 62.0f * scale;
    const float logo_y = hom_y - logo_h + 2.0f * scale;
    if (has_art("thrasher")) {
        HomArt logo;
        hom_art("thrasher", logo);
        const float logo_w = logo_h * logo.width / std::max(1.0f, logo.height);
        image(draw, "thrasher", ImVec2(cx - logo_w * 0.5f, logo_y), ImVec2(cx + logo_w * 0.5f, logo_y + logo_h), alpha(cyan, a));
    } else {
        const auto name = std::string("THRASHER");
        const float s = logo_h * 0.95f;
        const auto e = font->CalcTextSizeA(s, FLT_MAX, 0.0f, name.c_str());
        shadowed(draw, font, s, ImVec2(cx - e.x * 0.5f, logo_y), alpha(cyan, a), name);
    }

    // Metric rows stacked as the original does: the first to appear sits nearest the score, each
    // new one goes on top.
    std::vector<std::pair<double, Row>> shown;
    const auto add = [&](int metric, Row row) {
        if (metric_visible(r, metric)) shown.push_back({r.shown_at[metric] < 0 ? 1e30 : r.shown_at[metric], std::move(row)});
    };
    add(0, {"", 0, std::format("{:.0f}\xC2\xB0", r.rotation), r.rotation_points > 0 ? with_commas(r.rotation_points) : ""});
    add(1, {"", 1, std::format("{:05.2f}s", r.air_time), with_commas(r.air_points)});
    add(2, {"arrow", 2, std::format("{:.1f} m", r.drop), with_commas(r.drop_points)});
    add(3, {"timer", 3, std::format("{:05.2f}s", r.duration), with_commas(r.duration_points)});
    add(4, {"", 4, std::format("{:.1f} Km/h", r.peak_speed * 3.6f), with_commas(r.speed_points)});
    std::stable_sort(shown.begin(), shown.end(), [](const auto &l, const auto &rr) { return l.first > rr.first; });
    std::vector<Row> rows;
    for (auto &[at, row] : shown) rows.push_back(std::move(row));

    const float row_h = 36.0f * scale, text_size = 28.0f * scale;
    float y = logo_y - 12.0f * scale - row_h;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const ImVec2 icon_centre(left + 22.0f * scale + slide, y + row_h * 0.5f);
        const float ir = 12.0f * scale;
        const ImU32 icon_colour = alpha(cyan, a);
        if (it->kind == 0) icon_rotation(draw, icon_centre, ir, icon_colour);
        else if (it->kind == 1) icon_air(draw, icon_centre, ir, icon_colour);
        else if (it->kind == 4) {
            // Speed: three rising bars.
            for (int b = 0; b < 3; ++b) {
                const float bx = icon_centre.x - ir + b * ir * 0.75f, bh = ir * (0.6f + 0.5f * b);
                draw->AddRectFilled(ImVec2(bx, icon_centre.y + ir - bh), ImVec2(bx + ir * 0.5f, icon_centre.y + ir), icon_colour);
            }
        }
        else if (has_art(it->icon)) image(draw, it->icon, ImVec2(icon_centre.x - ir, icon_centre.y - ir), ImVec2(icon_centre.x + ir, icon_centre.y + ir), icon_colour);
        const float text_y = y + (row_h - text_size) * 0.5f;
        shadowed(draw, font, text_size, ImVec2(left + 50.0f * scale + slide, text_y), alpha(white, a), it->value);
        if (!it->points.empty()) {
            const auto e = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, it->points.c_str());
            shadowed(draw, font, text_size, ImVec2(right - e.x + slide, text_y), alpha(white, a), it->points);
        }
        y -= row_h;
    }

    // The bone chip on top: icon, count, points.
    if (r.bone_points > 0) {
        const float chip_h = row_h * 0.9f;
        const ImVec2 a0(left + slide, y + (row_h - chip_h) * 0.5f);
        draw->AddRectFilled(a0, ImVec2(left + 112.0f * scale + slide, a0.y + chip_h), alpha(chip_blue, a), 3.0f * scale);
        if (has_art("bones")) image(draw, "bones", ImVec2(a0.x + 4.0f * scale, a0.y + 3.0f * scale), ImVec2(a0.x + chip_h - 3.0f * scale, a0.y + chip_h - 3.0f * scale), alpha(IM_COL32_WHITE, a));
        // As homscoring.apt's UpdateBonus: the count reads "x2" and up, a single one shows no count.
        if (r.broken.size() >= 2)
            shadowed(draw, font, 22.0f * scale, ImVec2(a0.x + chip_h + 4.0f * scale, a0.y + 4.0f * scale), alpha(white, a), std::format("x{}", r.broken.size()));
        shadowed(draw, font, text_size, ImVec2(left + 122.0f * scale + slide, a0.y + 2.0f * scale), alpha(white, a), with_commas(r.bone_points));
    }
}

// The camera as it is now (the game moves it after the client tick), as the nametags read it.
bool live_camera(std::uintptr_t base, std::array<float, 16> &world, float &fov) {
    const auto view = latest_game_view();
    if (!view) return false;
    world = view->world;
    fov = view->vertical_fov;
    if (base && view->camera && !(view->camera & 7)) {
        std::uintptr_t vtable{};
        if (memory::peek(view->camera, vtable) &&
            (vtable == base + addr::engine::camera_vtable || vtable == base + addr::client_source_spawn::free_camera_vtable)) {
            std::array<float, 16> live{};
            float live_fov{};
            bool sound = memory::peek(view->camera + 0x50, live) && memory::peek(view->camera + 0xac, live_fov) && std::isfinite(live_fov) &&
                         live_fov > 1 && live_fov < 175;
            for (const auto value : live) sound = sound && std::isfinite(value) && std::abs(value) < 1e7f;
            if (sound) {
                world = live;
                fov = live_fov;
            }
        }
    }
    return fov > 1 && fov < 175;
}

// One X-ray bone: a pale shaft with knobbed ends, glowing red and pulsing once broken, with a
// white flash and a crack across it at the moment it went.
void draw_bone(ImDrawList *draw, std::size_t index, ImVec2 a, ImVec2 b, float r, bool broken, float since_break, float fade) {
    const ImU32 healthy = IM_COL32(190, 235, 255, 255), hurt = IM_COL32(255, 70, 40, 255), hot = IM_COL32(255, 200, 120, 255);
    const float pulse = broken ? 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.0f) : 1.0f;
    const ImU32 colour = broken ? hurt : healthy;
    const float base_alpha = (broken ? 0.95f : 0.42f) * fade;
    // Glow.
    for (int layer = 3; layer >= 1; --layer) {
        const float w = r * (1.0f + 0.9f * layer) * (broken ? 1.3f : 1.0f);
        const float a_ = base_alpha * (broken ? 0.16f : 0.07f) * pulse;
        if (index == 0) draw->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), w * 1.2f, alpha(colour, a_), 24);
        else draw->AddLine(a, b, alpha(colour, a_), w * 2.0f);
    }
    if (index == 0) {
        // Skull: a round cranium with dark sockets.
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        draw->AddCircleFilled(c, r * 1.15f, alpha(colour, base_alpha), 24);
        draw->AddCircleFilled(ImVec2(c.x - r * 0.4f, c.y + r * 0.15f), r * 0.28f, alpha(IM_COL32(10, 12, 30, 255), base_alpha * 0.8f), 12);
        draw->AddCircleFilled(ImVec2(c.x + r * 0.4f, c.y + r * 0.15f), r * 0.28f, alpha(IM_COL32(10, 12, 30, 255), base_alpha * 0.8f), 12);
    } else {
        const float shaft = index == 2 || index == 4 ? r * 1.6f : r * 1.1f;
        draw->AddLine(a, b, alpha(colour, base_alpha), shaft);
        draw->AddCircleFilled(a, r * 0.95f, alpha(colour, base_alpha), 16);
        draw->AddCircleFilled(b, r * 0.95f, alpha(colour, base_alpha), 16);
        if (index == 2) {
            // Rib cage: ribs across the column.
            const ImVec2 d(b.x - a.x, b.y - a.y);
            const float l = std::max(1.0f, std::sqrt(d.x * d.x + d.y * d.y));
            const ImVec2 n(-d.y / l, d.x / l);
            for (int i = 1; i <= 4; ++i) {
                const float t = i / 5.0f;
                const ImVec2 m(a.x + d.x * t, a.y + d.y * t);
                draw->AddLine(ImVec2(m.x - n.x * r * 2.2f, m.y - n.y * r * 2.2f), ImVec2(m.x + n.x * r * 2.2f, m.y + n.y * r * 2.2f),
                              alpha(colour, base_alpha * 0.85f), std::max(1.5f, r * 0.35f));
            }
        }
    }
    if (broken) {
        // The crack, and the flash when it snapped.
        const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 d(b.x - a.x, b.y - a.y);
        const float l = std::max(1.0f, std::sqrt(d.x * d.x + d.y * d.y));
        const ImVec2 n(-d.y / l, d.x / l), t(d.x / l, d.y / l);
        const float s = r * 1.6f;
        const ImVec2 zig[]{ImVec2(m.x - n.x * s, m.y - n.y * s), ImVec2(m.x - n.x * s * 0.3f + t.x * s * 0.35f, m.y - n.y * s * 0.3f + t.y * s * 0.35f),
                           ImVec2(m.x + n.x * s * 0.3f - t.x * s * 0.35f, m.y + n.y * s * 0.3f - t.y * s * 0.35f), ImVec2(m.x + n.x * s, m.y + n.y * s)};
        draw->AddPolyline(zig, 4, alpha(IM_COL32(20, 0, 0, 255), 0.9f * fade), 0, std::max(1.5f, r * 0.35f));
        if (since_break < 0.6f) {
            const float k = since_break / 0.6f;
            draw->AddCircleFilled(m, s * (0.8f + 1.2f * k), alpha(hot, (1.0f - k) * 0.6f * fade), 24);
            draw->AddCircle(m, s * (1.0f + 1.8f * k), alpha(IM_COL32_WHITE, (1.0f - k) * 0.7f * fade), 32, std::max(1.5f, r * 0.3f));
        }
    }
}

// The original's HoM grade, measured from its footage: the world goes dark, cool and nearly
// colourless (ground about (30, 32, 50), edges near black), where the skater's own colours wash
// out to blue-grey. Drawn as a cool wash over the whole picture plus a spotlight that falls to
// near black away from the skater.
void draw_grade(ImDrawList *draw, ImVec2 centre, float strength) {
    if (strength <= 0.01f) return;
    const auto display = ImGui::GetIO().DisplaySize;
    // Measured against Skate 3's own footage (its settled X-ray): the lit ground around the skater is a
    // cool, near-neutral grey-blue (about 37, 36, 45), falling to black by the corners, with no haze.
    // Skate 3's colour matrix (half saturation, blue x1.2) is run on the game picture itself
    // (hom_grade.cpp); here a dark blue wash brings it down to that level and a black spotlight closes in.
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(8, 12, 24, static_cast<int>(0.55f * strength * 255.0f)));
    const float h = display.y;
    const float radii[]{0.22f * h, 0.42f * h, 0.70f * h, 1.7f * std::max(display.x, display.y)};
    const float alphas[]{0.0f, 0.35f, 0.90f, 1.0f};
    constexpr int segments = 48;
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    for (int ring = 0; ring < 3; ++ring) {
        draw->PrimReserve(segments * 6, segments * 4);
        for (int k = 0; k < segments; ++k) {
            const float a0 = k * 6.2831853f / segments, a1 = (k + 1) * 6.2831853f / segments;
            const auto at = [&](float r, float angle) { return ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r); };
            const ImU32 inner = IM_COL32(0, 1, 4, static_cast<int>(alphas[ring] * strength * 255.0f));
            const ImU32 outer = IM_COL32(0, 1, 4, static_cast<int>(alphas[ring + 1] * strength * 255.0f));
            const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
            draw->PrimWriteVtx(at(radii[ring], a0), uv, inner);
            draw->PrimWriteVtx(at(radii[ring], a1), uv, inner);
            draw->PrimWriteVtx(at(radii[ring + 1], a1), uv, outer);
            draw->PrimWriteVtx(at(radii[ring + 1], a0), uv, outer);
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
        }
    }
}

// Scrolling film grain over the whole picture, one noise texel per screen pixel like the original's
// noise pass (f_noise_texture scrolled every frame).
void draw_grain(ImDrawList *draw, float strength) {
    HomArt grain;
    if (strength <= 0.01f || !hom_art("grain", grain)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    // Skate 3's grain is coarse: about a pixel of its 720p picture, so the tile is scaled with the screen.
    const float tile = 128.0f * std::max(1.0f, display.y / 720.0f);
    static std::uint32_t seed = 12345u;
    seed = seed * 1664525u + 1013904223u;
    const float ox = -static_cast<float>(seed % 128u) * tile / 128.0f, oy = -static_cast<float>((seed >> 8) % 128u) * tile / 128.0f;
    const ImU32 colour = IM_COL32(255, 255, 255, static_cast<int>(strength * 255.0f));
    for (float y = oy; y < display.y; y += tile)
        for (float x = ox; x < display.x; x += tile)
            draw->AddImage(grain.texture, ImVec2(x, y), ImVec2(x + tile, y + tile), grain.uv0, grain.uv1, colour);
}

// The original's X-ray: the picture closes in to a dark, grainy spotlight on the skater, and only
// the bones that got hurt show through the body: warm white, the broken ones orange, red at the break.
void draw_xray(ImDrawList *draw, const Result &r, float bones_alpha, float fade, float darkness) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto rig = hall_of_meat::latest_rig();
    if (rig.valid) {
        // The pose was read at the client tick; carry each bone on so the X-ray keeps up with the
        // body it is drawn over. Read from the physics bodies, the pose is this frame's: carry it only by the time since
        // the read. From the animation pose (a frame behind) add that frame.
        const float extra = hall_of_meat::rig_from_bodies().load() ? 0.0f : rig.period;
        const float ahead = std::clamp(static_cast<float>(std::chrono::duration<double>(Clock::now() - rig.at).count()) + extra, 0.0f, 0.09f);
        for (auto &seg : rig.bones)
            for (std::size_t k = 0; k < 3; ++k) {
                seg.a[k] += std::clamp(seg.va[k], -30.0f, 30.0f) * ahead;
                seg.b[k] += std::clamp(seg.vb[k], -30.0f, 30.0f) * ahead;
            }
    }
    BoneProjector projector;
    float fov{};
    const bool camera = rig.valid && live_camera(rig.base, projector.camera, fov);
    if (camera) {
        projector.focal = display.y / (2.0f * std::tan(fov * 3.14159265f / 360.0f));
        projector.centre = ImVec2(display.x * 0.5f, display.y * 0.5f);
    }
    ImVec2 spot(display.x * 0.5f, display.y * 0.5f);
    float spot_depth{};
    if (camera) {
        const auto &hips = rig.bones[4];
        const std::array<float, 3> middle{(hips.a[0] + hips.b[0]) * 0.5f, (hips.a[1] + hips.b[1]) * 0.5f, (hips.a[2] + hips.b[2]) * 0.5f};
        ImVec2 at;
        if (projector.project(middle, at, spot_depth)) spot = at;
    }
    draw_grade(draw, spot, darkness * fade);
    hom_colour_strength().store(std::clamp(darkness * fade, 0.0f, 1.0f));
    {
        // A break flashes the picture with the tint of Skate 3's hom_slomo_effect (0.4, 0.2, 0.2).
        double last_break = -1;
        for (const auto &b : r.broken) last_break = std::max(last_break, b.time);
        const float since = last_break < 0 ? 1e9f : static_cast<float>(clock_seconds() - last_break);
        if (since >= 0 && since < 0.8f)
            draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(102, 51, 51, static_cast<int>(0.2f * (1.0f - since / 0.8f) * fade * 255.0f)));
    }
    draw_grain(draw, 0.5f * darkness * fade);
    if (!camera) return;
    std::array<bool, hall_of_meat::rig_bones> broken{};
    for (const auto &b : r.broken)
        if (b.bone < broken.size()) broken[b.bone] = true;
    const bool meshes = hom_bones_ready();
    // Getting up, or the bail is over: the original's X-ray is gone (only the grade fades out).
    if (bones_alpha <= 0.0f) return;
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i) {
            const float damage = r.damage[i];
            if (!broken[i] && damage <= 0.0f) continue;
            if (broken[i] != (pass == 1)) continue;
            const auto &seg = rig.bones[i];
            if (!meshes) {
                ImVec2 a, b;
                float da{}, db{};
                if (!projector.project(seg.a, a, da) || !projector.project(seg.b, b, db)) continue;
                const float px = std::clamp(seg.radius * projector.focal * 2.0f / (da + db), 1.5f, 80.0f);
                draw_bone(draw, i, a, b, px, broken[i], 1.0f, fade * bones_alpha);
                continue;
            }
            // Bruised up to dislocated shows faint to full; broken in the fracture colours.
            const float strength = broken[i] ? 1.0f : std::clamp(0.35f + 0.65f * damage, 0.35f, 1.0f);
            // Skate 3's colours, measured from the original: hurt (223, 212, 214), broken (198, 87, 64),
            // fracture (158, 43, 23), after its bone map; these tints give those through the map.
            const ImU32 tint = broken[i] ? IM_COL32(235, 103, 75, 255) : IM_COL32(255, 244, 246, 255);
            const auto &facing = seg.has_front && hall_of_meat::roll_from_joints().load() ? seg.front : rig.forward;
            draw_hom_bone(draw, projector, i, seg.a, seg.b, facing, tint, strength * fade * bones_alpha, broken[i] ? 0.85f : 0.0f);
        }
}

} // namespace

// Game thread, once per client tick after the trainer has published its telemetry.
void hall_of_meat_tick() {
    auto &s = state();
    const auto telemetry = trainer::telemetry();
    std::lock_guard lock(s.mutex);
    // Off while its asset mod is not installed or is turned off in the MODS tab.
    if (!telemetry.skater || hom_directory().empty()) {
        if (s.tracker.phase() == Phase::bailing) s.tracker.reset();
        s.visible_until = 0;
        hall_of_meat::hide_game_ui().store(false);
        s.last_time = -1;
        return;
    }
    const double now = clock_seconds();
    if (now == s.last_time) return;
    s.last_time = now;
    Sample in;
    in.time = now;
    in.position = telemetry.position;
    in.speed = telemetry.speed;
    in.vertical = telemetry.vertical;
    in.airborne = telemetry.airborne;
    in.physics_state = telemetry.physics_state;
    in.heading = telemetry.heading;
    if (const auto rig = hall_of_meat::latest_rig(); rig.valid) {
        in.bones_valid = true;
        in.upright = hall_of_meat::rig_upright().load();
        in.lying = hall_of_meat::rig_lying().load();
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i)
            for (std::size_t k = 0; k < 3; ++k) in.bone_centres[i][k] = (rig.bones[i].a[k] + rig.bones[i].b[k]) * 0.5f;
    }
    // Calibration log: every physics state change, and a heartbeat, so a missed bail can be traced.
    if (in.physics_state != s.last_state && s.tracker.phase() != Phase::bailing)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: state {} -> {} ({:.1f} km/h, airborne {}).",
                     s.last_state, in.physics_state, in.speed * 3.6f, in.airborne);
    if (now - s.heartbeat > 10.0) {
        s.heartbeat = now;
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: watching, state {}, skeleton {}.", in.physics_state,
                     in.bones_valid ? "read" : "not read");
    }
    if (s.tracker.phase() == Phase::bailing && in.physics_state != s.last_state)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: physics state {} -> {} at {:.1f} km/h.",
                     s.last_state, in.physics_state, in.speed * 3.6f);
    s.last_state = in.physics_state;
    // Off the board and not bailing: four times a second, what the crash test sees (for tuning).
    if (s.tracker.phase() != Phase::bailing && in.physics_state == 504 && !in.upright && now - s.watch_logged >= 0.25) {
        s.watch_logged = now;
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: off board, {}.", s.tracker.watch_line());
    }
    const bool was_bailing = s.tracker.phase() == Phase::bailing;
    const bool done = s.tracker.update(in);
    hall_of_meat::hide_game_ui().store(s.tracker.showing() || now < s.visible_until);
    if (s.tracker.cancelled() != s.cancelled_seen) {
        s.cancelled_seen = s.tracker.cancelled();
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: that was a glide landing, not a crash: taken back.");
    }
    if (!was_bailing && s.tracker.phase() == Phase::bailing) {
        s.started = now;
        s.shown_total = 0;
        // Calibration: the X-ray skull and pelvis against the skater's own position.
        if (const auto rig = hall_of_meat::latest_rig(); rig.valid)
            logging::log(logging::Level::info, logging::Channel::assets,
                         "Hall Of Meat: bail started by {}: skater ({:.2f}, {:.2f}, {:.2f}), skull ({:.2f}, {:.2f}, {:.2f}), hips ({:.2f}, {:.2f}, {:.2f}).",
                         s.tracker.trigger(), in.position[0], in.position[1], in.position[2], rig.bones[0].a[0], rig.bones[0].a[1], rig.bones[0].a[2],
                         rig.bones[4].a[0], rig.bones[4].a[1], rig.bones[4].a[2]);
        else
            logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: skeleton not readable at bail start.");
    }
    {
        // Each new damage level is heard, as Skate 3 plays its HoM bone sounds.
        const Result &r = s.tracker.phase() == Phase::bailing ? s.tracker.live() : s.tracker.result();
        // A bail not yet known to be a crash (a glide's landing) is not heard until it is.
        const bool hold = s.tracker.phase() == Phase::bailing && !s.tracker.showing();
        if (r.serial != s.sounded_serial) {
            s.sounded_serial = r.serial;
            s.sounded = 0;
        }
        for (; !hold && s.sounded < r.events.size(); ++s.sounded) {
            const auto &e = r.events[s.sounded];
            const int n = std::max(1, level_count(static_cast<std::size_t>(e.part)));
            hall_of_meat::play_bone_sound(static_cast<float>(e.level) / static_cast<float>(n), e.top);
        }
    }
    if (done) {
        const auto &r = s.tracker.result();
        s.visible_until = now + linger_seconds + fade_seconds;
        s.best = std::max(s.best, r.total);
        logging::log(logging::Level::info, logging::Channel::assets,
                     "Hall Of Meat: bail {} scored {} (bones {} {}, air {:.2f}s {}, drop {:.1f}m {}, time {:.2f}s {}, speed {:.1f}m/s {}, rot {:.0f} {}) - {}.",
                     r.serial, r.total, r.broken.size(), r.bone_points, r.air_time, r.air_points, r.drop, r.drop_points, r.duration,
                     r.duration_points, r.peak_speed, r.speed_points, r.rotation, r.rotation_points, r.title);
    }
}

bool hall_of_meat_hud_pending() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.tracker.showing() || clock_seconds() < s.visible_until;
}

std::string hall_of_meat_command(std::string_view verb, const std::vector<std::string> &words) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (verb == "roll") {
        if (!words.empty()) hall_of_meat::roll_from_joints().store(words[0] != "0");
        return std::string("Hall Of Meat: bone roll from ") + (hall_of_meat::roll_from_joints().load() ? "the joints." : "the body forward.");
    }
    if (verb == "phys") {
        hall_of_meat::phys_probe_requested().store(true);
        return "Hall Of Meat: reading the physics bodies, see ReSkate.log.";
    }
    if (verb == "rig") {
        hall_of_meat::rig_probe_requested().store(true);
        return "Hall Of Meat: reading the skeleton, see ReSkate.log.";
    }
    if (verb == "status") {
        return std::format("Hall Of Meat: {}, best bail {}.", s.tracker.phase() == Phase::bailing ? "bailing" : "watching", s.best);
    }
    return "Hall Of Meat: status, rig, phys, roll 0|1.";
}

void draw_hall_of_meat_hud() {
    auto &s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *draw = ImGui::GetBackgroundDrawList();
    std::lock_guard lock(s.mutex);
    hom_colour_strength().store(0.0f);
    const double now = clock_seconds();
    const bool live = s.tracker.showing();
    if (!live && now >= s.visible_until) return;
    const Result &r = live ? s.tracker.live() : s.tracker.result();
    float fade = 1.0f;
    if (!live) fade = static_cast<float>(std::clamp((s.visible_until - now) / fade_seconds, 0.0, 1.0));
    // The score counts up toward its target.
    const float target = static_cast<float>(r.total);
    s.shown_total += (target - s.shown_total) * std::min(1.0f, ImGui::GetIO().DeltaTime * 8.0f);
    if (std::abs(target - s.shown_total) < 1.0f) s.shown_total = target;
    {
        // Darkness: deep once the body is settling (as the original reveals its X-ray), lighter mid-tumble.
        const float dark_target = live ? ((s.tracker.live().duration > 0.7f) ? 1.0f : 0.5f) : 0.0f;
        s.darkness += (dark_target - s.darkness) * std::min(1.0f, ImGui::GetIO().DeltaTime * 4.0f);
        // Getting up ends the X-ray for good: once the skater has been upright for a moment the bones
        // fade out and stay gone, however the body bends on the way up (it flickered on and off before).
        if (live) {
            if (r.serial != s.xray_serial) {
                s.xray_serial = r.serial;
                s.upright_since = s.getting_up_at = -1;
            }
            if (hall_of_meat::rig_upright().load()) {
                if (s.upright_since < 0) s.upright_since = now;
                if (s.getting_up_at < 0 && now - s.upright_since >= 0.12) s.getting_up_at = now;
            } else {
                s.upright_since = -1;
            }
        }
        const float bones_alpha = !live ? 0.0f : s.getting_up_at < 0 ? 1.0f
                                                 : std::clamp(1.0f - static_cast<float>(now - s.getting_up_at) / 0.25f, 0.0f, 1.0f);
        if (live || s.darkness > 0.02f) draw_xray(draw, r, bones_alpha, live ? 1.0f : fade, s.darkness);
    }
    draw_block(draw, r, s.shown_total, fade, scale, now, s.started);
}
} // namespace dingosdk::overlay
