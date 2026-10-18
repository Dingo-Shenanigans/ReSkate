#include "domination_overlay.h"
#include "domination.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <string>

namespace dingosdk::overlay {
namespace {
namespace theme = dingosdk::skate_theme;
using Address = std::uintptr_t;

constexpr ImU32 yours = IM_COL32(94, 205, 250, 255);
constexpr ImU32 open = IM_COL32(245, 245, 245, 255);

// Render thread: the view taken by domination_pending, drawn by draw_domination.
domination::OverlayView &view() {
    static auto *value = new domination::OverlayView;
    return *value;
}

ImU32 faded(ImU32 colour, float alpha) {
    const auto a = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    const float offset = std::max(1.0f, size * 0.06f);
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), faded(IM_COL32_BLACK, 0.75f * ((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f),
                  text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
float width(ImFont *font, float size, const std::string &text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x; }
void centred(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 colour, const std::string &text) {
    shadowed(draw, font, size, ImVec2(x - width(font, size, text) * 0.5f, y), colour, text);
}
std::string points(float score) {
    auto digits = std::to_string(static_cast<long long>(std::llround(score)));
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}
std::string clock(double seconds) {
    const int whole = static_cast<int>(std::ceil(seconds));
    return std::format("{}:{:02}", whole / 60, whole % 60);
}

// The camera as it is now (as the nametags read it): the game moves its camera after the client
// update the view was taken in.
bool camera(Address base, GameView &out) {
    const auto latest = latest_game_view();
    if (!latest) return false;
    out = *latest;
    if (!base || !out.camera || (out.camera & 7)) return true;
    Address vtable{};
    if (!memory::peek(out.camera, vtable) ||
        (vtable != base + addr::engine::camera_vtable && vtable != base + addr::client_source_spawn::free_camera_vtable))
        return true;
    std::array<float, 16> world{};
    float fov{};
    if (!memory::peek(out.camera + 0x50, world) || !memory::peek(out.camera + 0xac, fov)) return true;
    if (!std::isfinite(fov) || fov <= 1 || fov >= 175) return true;
    for (const auto value : world)
        if (!std::isfinite(value) || std::abs(value) > 1e7f) return true;
    out.world = world;
    out.vertical_fov = fov;
    return true;
}
// World to screen through the camera.
struct Projector {
    std::array<float, 3> right{}, up{}, back{}, origin{};
    float focal{};
    ImVec2 centre, display;
    static float dot(const std::array<float, 3> &a, const std::array<float, 3> &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    std::array<float, 3> delta(const std::array<float, 3> &p) const { return {p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]}; }
    // Depth in front of the camera (negative behind it), and where on screen when in front.
    float depth(const std::array<float, 3> &p) const { return -dot(delta(p), back); }
    ImVec2 screen(const std::array<float, 3> &p, float depth) const {
        const auto d = delta(p);
        return ImVec2(centre.x + dot(d, right) * focal / depth, centre.y - dot(d, up) * focal / depth);
    }
    // The way to a point on screen, from the centre (for one behind the camera too).
    ImVec2 heading(const std::array<float, 3> &p) const {
        const auto d = delta(p);
        return ImVec2(dot(d, right), -dot(d, up));
    }
};
std::optional<Projector> projector(Address base) {
    GameView cam;
    if (!camera(base, cam) || !(cam.vertical_fov > 1 && cam.vertical_fov < 175)) return std::nullopt;
    Projector p;
    p.display = ImGui::GetIO().DisplaySize;
    const auto &m = cam.world;
    p.right = {m[0], m[1], m[2]};
    p.up = {m[4], m[5], m[6]};
    p.back = {m[8], m[9], m[10]};
    p.origin = {m[12], m[13], m[14]};
    p.focal = p.display.y / (2.0f * std::tan(cam.vertical_fov * 3.14159265f / 360.0f));
    p.centre = ImVec2(p.display.x * 0.5f, p.display.y * 0.5f);
    return p;
}

// A player's colour: you blue, anyone else one of seven colours (the slot the view gives them,
// else by their id), open spots white. In a teams round: the team's red or blue.
constexpr ImU32 red_team = IM_COL32(240, 72, 64, 255), blue_team = IM_COL32(64, 128, 255, 255);
ImU32 team_colour(int team) { return team == 0 ? red_team : blue_team; }
ImU32 colour_of(domination::OverlayOwner kind, std::uint64_t id, int team = -1, int slot = -1) {
    using O = domination::OverlayOwner;
    if (kind == O::none) return open;
    if (team >= 0) return team_colour(team);
    if (kind == O::you) return yours;
    // Yellow, green, purple, magenta, orange, teal, tan: the ones that show best on red bowls and grey
    // concrete first; with you in blue, eight players with no two alike.
    constexpr std::array<ImU32, 7> palette{IM_COL32(255, 196, 0, 255),   IM_COL32(99, 212, 113, 255), IM_COL32(190, 146, 255, 255),
                                           IM_COL32(255, 105, 210, 255), IM_COL32(255, 150, 60, 255), IM_COL32(40, 205, 195, 255),
                                           IM_COL32(215, 185, 140, 255)};
    if (slot >= 0) return palette[static_cast<std::size_t>(slot) % palette.size()];
    auto mixed = id * 0x9E3779B97F4A7C15ull;
    mixed ^= mixed >> 29;
    return palette[mixed % palette.size()];
}
ImU32 colour_of(const domination::OverlaySpot &spot) { return colour_of(spot.owner, spot.owner_id, spot.team, spot.colour); }
ImU32 colour_of(const domination::OverlayStanding &row) { return row.team >= 0 ? team_colour(row.team) : colour_of(row.kind, row.id, -1, row.colour); }
ImU32 blend(ImU32 a, ImU32 b, float t) {
    const auto mix = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xff), y = static_cast<float>((b >> shift) & 0xff);
        return static_cast<ImU32>(x + (y - x) * std::clamp(t, 0.0f, 1.0f)) << shift;
    };
    return mix(IM_COL32_R_SHIFT) | mix(IM_COL32_G_SHIFT) | mix(IM_COL32_B_SHIFT) | (a & IM_COL32_A_MASK);
}
std::string owner_line(const domination::OverlaySpot &spot) {
    using O = domination::OverlayOwner;
    switch (spot.owner) {
    case O::none: return "Open";
    case O::you: return "YOURS  " + points(spot.top);
    case O::other: return spot.owner_name + "  " + points(spot.top);
    }
    return {};
}

// The standings rows shown when there is room for `most`: the first ones, and yours always (in
// place of the last when you are further down). Indexes into v.standings, in order.
std::vector<std::size_t> shown_rows(const domination::OverlayView &v, std::size_t most) {
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < v.standings.size() && i < most; ++i) shown.push_back(i);
    for (std::size_t i = most; i < v.standings.size() && !shown.empty(); ++i)
        if (v.standings[i].kind == domination::OverlayOwner::you) {
            shown.back() = i;
            break;
        }
    return shown;
}

// The spot list (and the players under it) at the top right, where draw_hud puts it.
std::pair<ImVec2, ImVec2> side_panel(const domination::OverlayView &v, float k, ImVec2 display) {
    const float row = 22 * k, x1 = display.x - 20 * k, x0 = x1 - 330 * k;
    float bottom = 120 * k + row * v.spots.size() + 8 * k;
    if (v.multiplayer && !v.standings.empty() && v.running) bottom += 18 * k + row * (shown_rows(v, 8).size() + 1) + 8 * k;
    return {ImVec2(x0 - 10 * k, 112 * k), ImVec2(x1 + 8 * k, bottom)};
}

// Each spot's edge on the ground near the skater, in its owner's colour: a faint glow, faded where
// the camera can't see it (what the game side found) and with distance.
void draw_outlines(ImDrawList *draw, float k, const Projector &p, const domination::OverlayView &v) {
    // The overlay has no depth: what of a line lies behind the skater is cut out where their body is
    // on screen, exactly at its edge (so the cut moves with them, and nothing beside them comes and
    // goes). The body: three capsules up from their feet, legs, torso with the arms, and the head.
    struct Capsule {
        ImVec2 a, b;
        float radius{};
    };
    struct Body {
        std::array<Capsule, 3> parts{};
        float depth = -1;
        ImVec2 lo, hi; // on screen, all of it
    } body;
    if (v.skater) {
        const auto &at = v.skater_at;
        const float d = p.depth(at);
        // Heights from the root (about 1 m above the feet) and half widths, in metres.
        constexpr std::array<std::array<float, 3>, 3> shape{{{-1.0f, -0.05f, 0.2f}, {-0.05f, 0.45f, 0.32f}, {0.5f, 0.8f, 0.14f}}};
        bool fine = d > 0.5f;
        for (std::size_t i = 0; i < shape.size() && fine; ++i) {
            const std::array<float, 3> low{at[0], at[1] + shape[i][0], at[2]}, high{at[0], at[1] + shape[i][1], at[2]};
            const float dl = p.depth(low), dh = p.depth(high);
            fine = dl > 0.3f && dh > 0.3f;
            if (fine) body.parts[i] = {p.screen(low, dl), p.screen(high, dh), shape[i][2] * p.focal / d};
        }
        if (fine) {
            body.depth = d;
            body.lo = ImVec2(1e9f, 1e9f), body.hi = ImVec2(-1e9f, -1e9f);
            for (const auto &c : body.parts)
                for (const auto &q : {c.a, c.b}) {
                    body.lo = ImVec2(std::min(body.lo.x, q.x - c.radius), std::min(body.lo.y, q.y - c.radius));
                    body.hi = ImVec2(std::max(body.hi.x, q.x + c.radius), std::max(body.hi.y, q.y + c.radius));
                }
        }
    }
    const auto in_body = [&](ImVec2 q) {
        for (const auto &c : body.parts) {
            const ImVec2 axis(c.b.x - c.a.x, c.b.y - c.a.y), to(q.x - c.a.x, q.y - c.a.y);
            const float length = axis.x * axis.x + axis.y * axis.y;
            const float t = length > 0 ? std::clamp((to.x * axis.x + to.y * axis.y) / length, 0.0f, 1.0f) : 0.0f;
            const float dx = to.x - axis.x * t, dy = to.y - axis.y * t;
            if (dx * dx + dy * dy < c.radius * c.radius) return true;
        }
        return false;
    };
    // The parts of a piece of line (as fractions along it) not hidden by the skater.
    const auto shown_parts = [&](ImVec2 a, ImVec2 b, float depth) {
        std::vector<std::pair<float, float>> parts;
        if (body.depth < 0 || depth < body.depth + 0.3f || std::max(a.x, b.x) < body.lo.x || std::min(a.x, b.x) > body.hi.x ||
            std::max(a.y, b.y) < body.lo.y || std::min(a.y, b.y) > body.hi.y) {
            parts.emplace_back(0.0f, 1.0f);
            return parts;
        }
        const auto at = [&](float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); };
        // Where it goes in and out of the body: in steps, each edge then found closely.
        const auto edge = [&](float t0, float t1) {
            const bool first = in_body(at(t0));
            for (int n = 0; n < 8; ++n) {
                const float mid = (t0 + t1) * 0.5f;
                (in_body(at(mid)) == first ? t0 : t1) = mid;
            }
            return (t0 + t1) * 0.5f;
        };
        constexpr int steps = 16;
        bool hidden = in_body(a);
        float from = 0;
        for (int n = 1; n <= steps; ++n) {
            const float t = static_cast<float>(n) / steps;
            const bool now = in_body(at(t));
            if (now == hidden) continue;
            const float cut = edge(static_cast<float>(n - 1) / steps, t);
            if (!hidden) parts.emplace_back(from, cut);
            else from = cut;
            hidden = now;
        }
        if (!hidden) parts.emplace_back(from, 1.0f);
        return parts;
    };
    for (const auto &spot : v.spots) {
        if (spot.outline.size() < 2 || spot.outline_alpha.size() != spot.outline.size()) continue;
        const auto colour = blend(colour_of(spot), theme::white, spot.flash);
        const float thick = (spot.here ? 3.0f : 2.2f) * k + spot.flash * 3.0f * k;
        // Unbroken strokes (no brighter dots where pieces overlap): the points of a run of pieces
        // that join, at one strength (in steps of a sixth).
        std::vector<ImVec2> run;
        int run_level = 0;
        const auto flush = [&] {
            if (run.size() >= 2) {
                const float a = static_cast<float>(run_level) / 6.0f, n = static_cast<float>(run.size());
                draw->AddPolyline(run.data(), static_cast<int>(n), faded(IM_COL32_BLACK, 0.12f * a), ImDrawFlags_None, thick * 3.6f);
                draw->AddPolyline(run.data(), static_cast<int>(n), faded(colour, 0.14f * a), ImDrawFlags_None, thick * 3.0f);
                draw->AddPolyline(run.data(), static_cast<int>(n), faded(colour, 0.26f * a), ImDrawFlags_None, thick * 1.7f);
                draw->AddPolyline(run.data(), static_cast<int>(n), faded(blend(colour, theme::white, 0.3f), 0.5f * a), ImDrawFlags_None, thick * 0.6f);
            }
            run.clear();
        };
        for (std::size_t i = 0; i < spot.outline.size(); ++i) {
            const std::size_t j = (i + 1) % spot.outline.size();
            const int level = static_cast<int>(std::lround(std::min(spot.outline_alpha[i], spot.outline_alpha[j]) * 6.0f));
            if (level <= 0) {
                flush();
                continue;
            }
            auto a = spot.outline[i], b = spot.outline[j];
            a[1] += 0.08f;
            b[1] += 0.08f;
            float da = p.depth(a), db = p.depth(b);
            // Cut at 2 m in front of the camera: a point beside it, barely in front, would be
            // thrown far off the screen and draw a long stray line.
            constexpr float closest = 2.0f;
            if (da < closest && db < closest) {
                flush();
                continue;
            }
            const bool cut_start = da < closest;
            const auto cut = [&](std::array<float, 3> &q, float &dq, const std::array<float, 3> &other, float dother) {
                if (dq >= closest) return;
                const float t = (closest - dq) / (dother - dq);
                for (int c = 0; c < 3; ++c) q[c] += (other[c] - q[c]) * t;
                dq = closest;
            };
            cut(a, da, b, db);
            cut(b, db, a, da);
            const ImVec2 sa = p.screen(a, da), sb = p.screen(b, db);
            const auto parts = shown_parts(sa, sb, (da + db) * 0.5f);
            for (const auto &[t0, t1] : parts) {
                const ImVec2 pa(sa.x + (sb.x - sa.x) * t0, sa.y + (sb.y - sa.y) * t0), pb(sa.x + (sb.x - sa.x) * t1, sa.y + (sb.y - sa.y) * t1);
                if (run.empty() || run_level != level || cut_start || t0 > 0) {
                    flush();
                    run = {pa};
                    run_level = level;
                }
                run.push_back(pb);
            }
            if (parts.empty() || parts.back().second < 1.0f) flush();
        }
        flush();
    }
}

void draw_markers(ImDrawList *draw, ImFont *bold, ImFont *body, float k, const Projector &p, const domination::OverlayView &v) {
    const auto &display = p.display;
    struct Placed {
        const domination::OverlaySpot *spot;
        ImVec2 at;
        float depth;
    };
    std::vector<Placed> placed;
    std::vector<const domination::OverlaySpot *> arrows;
    for (const auto &spot : v.spots) {
        const float depth = p.depth(spot.marker);
        const ImVec2 at = depth >= 0.5f ? p.screen(spot.marker, depth) : ImVec2(-1e6f, -1e6f);
        const bool seen = depth >= 0.5f && at.x >= -50 && at.x <= display.x + 50 && at.y >= -50 && at.y <= display.y + 50;
        if (seen) placed.push_back({&spot, at, depth});
        if (spot.arrow && (!seen || at.x < 0 || at.x > display.x || at.y < 0 || at.y > display.y)) arrows.push_back(&spot);
    }
    std::sort(placed.begin(), placed.end(), [](const Placed &a, const Placed &b) { return a.depth > b.depth; }); // far first
    // Labels: near spots only (the one the skater is on always), and a farther label that would
    // cover a nearer one's is left out (decided nearest first).
    constexpr float label_range = 40;
    std::vector<bool> labelled(placed.size());
    {
        std::vector<std::pair<ImVec2, ImVec2>> taken{side_panel(v, k, display)}; // not under the spot list
        for (std::size_t n = placed.size(); n-- > 0;) {
            const auto &pl = placed[n];
            if (pl.depth > label_range && !pl.spot->here) continue;
            const float size = std::clamp(560.0f * k / pl.depth, 13.0f * k, 22.0f * k), w = width(bold, size, pl.spot->name) * 0.5f + 4 * k;
            const ImVec2 a(pl.at.x - w, pl.at.y), b(pl.at.x + w, pl.at.y + size * 2.4f + 24 * k);
            const bool covers = std::ranges::any_of(taken, [&](const auto &box) { return a.x < box.second.x && b.x > box.first.x && a.y < box.second.y && b.y > box.first.y; });
            if (covers && !pl.spot->here) continue;
            labelled[n] = true;
            taken.emplace_back(a, b);
        }
    }
    const double t = ImGui::GetTime();
    for (std::size_t n = 0; n < placed.size(); ++n) {
        const auto &pl = placed[n];
        const auto &spot = *pl.spot;
        const auto colour = blend(colour_of(spot), theme::white, spot.flash * 0.7f);
        const float r = std::clamp(420.0f * k / pl.depth, 5.0f * k, 18.0f * k) * (1 + 0.35f * spot.flash);
        const float closeness = std::clamp(1.4f - pl.depth / 80.0f, 0.35f, 1.0f); // far markers fade
        if (spot.here) {
            const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(t * 5.0));
            draw->AddCircle(pl.at, r * (1.5f + 0.3f * pulse), faded(colour, 0.9f), 32, 3.0f * k);
        }
        // A new owner: a ring goes out from the marker.
        if (spot.flash > 0) draw->AddCircle(pl.at, r * (1.2f + 2.2f * (1 - spot.flash)), faded(colour_of(spot), spot.flash), 40, 4.0f * k);
        draw->AddCircleFilled(pl.at, r, faded(IM_COL32_BLACK, 0.45f * closeness), 32);
        if (spot.owner != domination::OverlayOwner::none) draw->AddCircleFilled(pl.at, r * 0.72f, faded(colour, closeness), 32);
        draw->AddCircle(pl.at, r, faded(colour, closeness), 32, 2.0f * k);
        if (!labelled[n]) continue;
        const float size = std::clamp(560.0f * k / pl.depth, 13.0f * k, 22.0f * k);
        centred(draw, bold, size, pl.at.x, pl.at.y + r + 3 * k, faded(theme::white, closeness), spot.name);
        centred(draw, body, size * 0.85f, pl.at.x, pl.at.y + r + 3 * k + size, faded(colour, closeness), owner_line(spot));
        // Your own best, when the spot is someone else's.
        if (spot.mine > 0 && spot.owner != domination::OverlayOwner::you && spot.here)
            centred(draw, body, size * 0.8f, pl.at.x, pl.at.y + r + 3 * k + size * 1.9f, faded(yours, closeness), "your best " + points(spot.mine));
    }
    // Arrows at the screen's edge to the nearest spots that are not yours, off screen or behind.
    const float inset = 70 * k;
    const ImVec2 half(display.x * 0.5f - inset, display.y * 0.5f - inset);
    const auto panel = side_panel(v, k, display);
    for (const auto *spot : arrows) {
        auto d = p.heading(spot->marker);
        if (p.depth(spot->marker) < 0.5f && std::abs(d.x) < 1e-3f && std::abs(d.y) < 1e-3f) d = ImVec2(0, 1);
        const float length = std::sqrt(d.x * d.x + d.y * d.y);
        if (length < 1e-4f) continue;
        d = ImVec2(d.x / length, d.y / length);
        const float scale = std::min(std::abs(d.x) > 1e-4f ? half.x / std::abs(d.x) : 1e9f, std::abs(d.y) > 1e-4f ? half.y / std::abs(d.y) : 1e9f);
        ImVec2 tip(p.centre.x + d.x * scale, p.centre.y + d.y * scale);
        if (tip.x > panel.first.x - 30 * k && tip.y < panel.second.y + 24 * k) tip.x = panel.first.x - 30 * k; // beside the spot list
        const ImVec2 side(-d.y, d.x);
        const float s = 16 * k;
        const auto colour = colour_of(*spot);
        const ImVec2 a(tip.x + d.x * s, tip.y + d.y * s), b(tip.x - d.x * s * 0.6f + side.x * s * 0.8f, tip.y - d.y * s * 0.6f + side.y * s * 0.8f),
            c(tip.x - d.x * s * 0.6f - side.x * s * 0.8f, tip.y - d.y * s * 0.6f - side.y * s * 0.8f);
        draw->AddTriangleFilled(a, b, c, faded(IM_COL32_BLACK, 0.5f));
        draw->AddTriangleFilled(ImVec2(a.x - d.x * 2 * k, a.y - d.y * 2 * k), b, c, faded(colour, 0.95f));
        const auto label = std::format("{}  {:.0f} m", spot->name, spot->distance);
        const float w = width(body, 16 * k, label);
        const ImVec2 at(std::clamp(tip.x - d.x * 34 * k - w * 0.5f, 6 * k, display.x - w - 6 * k), std::clamp(tip.y - d.y * 34 * k - 8 * k, 6 * k, display.y - 24 * k));
        shadowed(draw, body, 16 * k, at, faded(colour, 0.95f), label);
    }
}

std::string held_time(double seconds) {
    const int whole = static_cast<int>(seconds);
    return std::format("{}:{:02}", whole / 60, whole % 60);
}

// ---- animation (times are the view's ages, in seconds) ----------------------------------
float unit(float t) { return std::clamp(t, 0.0f, 1.0f); }
float ease_out(float t) {
    t = 1 - unit(t);
    return 1 - t * t * t;
}
// Overshoots a little before it settles: a pop.
float ease_pop(float t) {
    t = unit(t) - 1;
    constexpr float c = 2.2f;
    return 1 + (c + 1) * t * t * t + c * t * t;
}
// 0 to 1 over `in` seconds from `age`, and back to 0 over the last `out` seconds of `left`.
float fade(float age, float left, float in, float out) { return unit(age / in) * unit(left / out); }
// A slanted bar, like skate.'s brushed banners: from x0 to x1 at y, `height` tall, leaning right.
void slant_bar(ImDrawList *draw, float x0, float x1, float y, float height, float lean, ImU32 colour) {
    if (x1 <= x0) return;
    draw->AddQuadFilled(ImVec2(x0 + lean, y), ImVec2(x1 + lean, y), ImVec2(x1 - lean, y + height), ImVec2(x0 - lean, y + height), colour);
}
// Sparks thrown out from `at`: short lines that fly out and fade over `life` seconds.
void sparks(ImDrawList *draw, ImVec2 at, float k, float age, float life, ImU32 colour, unsigned seed, int count = 14) {
    if (age <= 0 || age >= life) return;
    const float t = age / life, out = ease_out(t);
    for (int i = 0; i < count; ++i) {
        const float angle = (static_cast<float>(i) + theme::noise(seed + i) * 0.6f) * 6.2831853f / static_cast<float>(count);
        const float reach = (150 + 140 * theme::noise(seed * 7 + i)) * k, length = (34 - 26 * t) * k;
        const ImVec2 d(std::cos(angle), std::sin(angle) * 0.55f);
        const ImVec2 p0(at.x + d.x * reach * out, at.y + d.y * reach * out), p1(p0.x - d.x * length, p0.y - d.y * length);
        draw->AddLine(p1, p0, faded(i % 3 == 0 ? theme::white : colour, 1 - t), (4 - 2 * t) * k);
    }
}
// Results: who won, the standings, the round's awards, and who owns each spot, coming in one after
// another: the screen darkens, the panel slides up, the winner's band wipes in and the winner pops
// (with sparks when it's you), the standings slide in row by row with their points
// counting up, the awards pop in as cards, then the spots.
void draw_results(ImDrawList *draw, ImFont *title, ImFont *bold, ImFont *body, float k, const domination::OverlayView &v) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float t = static_cast<float>(v.results_age);
    draw->AddRectFilled(ImVec2(0, 0), display, faded(IM_COL32_BLACK, 0.45f * ease_out(t / 0.4f)));
    const float line = 30 * k, w = 900 * k;
    const auto rows = v.multiplayer ? shown_rows(v, 8) : std::vector<std::size_t>{};
    std::vector<std::pair<std::string, std::string>> awards;
    if (!v.most_steals.empty()) awards.emplace_back("MOST STEALS", v.most_steals);
    if (!v.best_trick.empty()) awards.emplace_back("BEST TRICK", v.best_trick);
    if (!v.held_longest.empty()) awards.emplace_back("HELD LONGEST", v.held_longest);
    const std::size_t spot_rows = (v.spots.size() + 1) / 2;
    const float h = 150 * k + (rows.empty() ? 0 : 34 * k + line * static_cast<float>(rows.size()) + 4 * k) + (awards.empty() ? 0 : 84 * k) + 44 * k +
                    24 * k * static_cast<float>(spot_rows) + 20 * k;
    const float rise = (1 - ease_out(t / 0.45f)) * 70 * k, pa = ease_out(t / 0.35f);
    const ImVec2 a(display.x * 0.5f - w * 0.5f, std::max(20 * k, display.y * 0.5f - h * 0.5f) + rise), b(a.x + w, a.y + h);
    const ImU32 own = v.teams && v.my_team >= 0 ? team_colour(v.my_team) : yours;
    theme::rough_rect(draw, ImVec2(a.x + 8 * k, a.y + 10 * k), ImVec2(b.x + 8 * k, b.y + 10 * k), faded(IM_COL32_BLACK, 0.5f * pa), 7, k);
    theme::rough_rect(draw, a, b, faded(theme::tile, 0.95f * pa), 3, k);
    // The header: a band in the winner's colour (gold when it's you), and the winner's line.
    const bool winner_band = v.you_won || (!v.tie && !rows.empty());
    const ImU32 band_colour = v.you_won ? theme::bar : winner_band ? colour_of(v.standings[rows.front()]) : theme::tile_light;
    const float band = ease_out((t - 0.15f) / 0.3f), mid = a.y + 55 * k;
    const int first = draw->VtxBuffer.Size;
    slant_bar(draw, a.x - 14 * k, a.x - 14 * k + (w + 28 * k) * band, a.y + 16 * k, 78 * k, 16 * k, faded(band_colour, 0.95f * pa));
    if (const float s = (t - 0.5f) / 0.6f; s > 0 && s < 1) {
        const float sx = a.x + (w + 28 * k) * ease_out(s);
        slant_bar(draw, sx - 60 * k, sx, a.y + 16 * k, 78 * k, 16 * k, faded(theme::white, 0.4f * (1 - s)));
    }
    theme::rotate_since(draw, first, -1.2f, ImVec2(display.x * 0.5f, mid));
    const std::string head = v.winner.empty() ? std::string("ROUND OVER") : v.winner;
    const float hs = 64 * k * (0.5f + 0.5f * ease_pop((t - 0.3f) / 0.35f)), ha = unit((t - 0.3f) / 0.12f), hw = width(title, hs, head);
    draw->AddText(title, hs, ImVec2(display.x * 0.5f - hw * 0.5f + 3 * k, mid - hs * 0.55f + 4 * k), faded(IM_COL32_BLACK, 0.35f * ha), head.c_str());
    draw->AddText(title, hs, ImVec2(display.x * 0.5f - hw * 0.5f, mid - hs * 0.55f), faded(winner_band ? theme::black : theme::white, ha), head.c_str());
    if (v.you_won) sparks(draw, ImVec2(display.x * 0.5f, mid), k * 1.6f, t - 0.32f, 0.8f, theme::bar, 77, 20);
    // What you have, the spots and points counting up.
    int team_spots = 0;
    for (const auto &row : v.standings)
        if (v.teams && row.team == v.my_team) team_spots = row.spots;
    const float up = ease_out((t - 0.45f) / 0.7f);
    const int counted = static_cast<int>(std::round(static_cast<float>(v.teams && v.my_team >= 0 ? team_spots : v.taken) * up));
    const auto mine = v.teams && v.my_team >= 0
                          ? std::format("TEAM {} OWNS {} OF {} SPOTS    YOU TOOK {}", v.my_team == 0 ? "RED" : "BLUE", counted, v.spots.size(), v.taken)
                          : std::format("YOU OWN {} OF {} SPOTS    {}", counted, v.spots.size(), points(v.total * up));
    centred(draw, bold, 24 * k, display.x * 0.5f, a.y + 108 * k, faded(own, unit((t - 0.45f) / 0.2f)), mine);
    float y = a.y + 150 * k;
    // The standings: a row each, sliding in from the right one after another; the winner's row
    // glows, yours is outlined.
    if (!rows.empty()) {
        const float c_name = a.x + 76 * k, c_spots = a.x + w * 0.56f, c_held = a.x + w * 0.68f, c_steals = a.x + w * 0.80f, c_points = b.x - 30 * k;
        const float ta = unit((t - 0.55f) / 0.2f);
        const auto heading = [&](float x, const char *text, bool right) {
            draw->AddText(bold, 16 * k, ImVec2(right ? x - width(bold, 16 * k, text) : x, y), faded(theme::grey_text, ta), text);
        };
        heading(c_name, "PLAYER", false);
        heading(c_spots, "SPOTS", true);
        heading(c_held, "HELD", true);
        heading(c_steals, "STEALS", true);
        heading(c_points, "POINTS", true);
        y += 34 * k;
        for (std::size_t n = 0; n < rows.size(); ++n, y += line) {
            const auto &st = v.standings[rows[n]];
            const float rt = t - 0.65f - 0.1f * static_cast<float>(n), ra = unit(rt / 0.2f);
            if (ra <= 0) continue;
            const float dx = (1 - ease_out(rt / 0.35f)) * 80 * k;
            const auto colour = colour_of(st);
            const bool top = rows[n] == 0 && !v.tie, me = st.kind == domination::OverlayOwner::you || (v.teams && st.team == v.my_team);
            const ImVec2 r0(a.x + 18 * k + dx, y - 3 * k), r1(b.x - 18 * k + dx, y + line - 3 * k);
            if (top) draw->AddRectFilled(r0, r1, faded(colour, (0.18f + 0.1f * std::sin(t * 4.0f)) * ra), 4 * k);
            if (me) draw->AddRect(r0, r1, faded(own, 0.7f * ra), 4 * k, 0, 2 * k);
            ImFont *font = me ? bold : body;
            const float fs = 21 * k;
            const auto cell = [&](float x, const std::string &text, ImU32 c, bool right) {
                draw->AddText(font, fs, ImVec2((right ? x - width(font, fs, text) : x) + dx, y), faded(c, ra), text.c_str());
            };
            cell(a.x + 30 * k, std::to_string(rows[n] + 1) + ".", theme::grey_text, false);
            draw->AddCircleFilled(ImVec2(c_name - 16 * k + dx, y + fs * 0.55f), 7 * k, faded(colour, ra), 16);
            cell(c_name, st.name.substr(0, 22), theme::white, false);
            cell(c_spots, std::to_string(st.spots), colour, true);
            cell(c_held, held_time(st.hold), theme::white, true);
            cell(c_steals, std::to_string(st.steals), theme::white, true);
            cell(c_points, points(st.total * ease_out((rt - 0.1f) / 0.6f)), colour, true);
        }
        y += 4 * k;
    }
    // The awards: a card each, popping in after the standings.
    const float after = 0.75f + 0.1f * static_cast<float>(rows.size());
    if (!awards.empty()) {
        y += 20 * k;
        const float gap = 14 * k, cw = (w - 48 * k - gap * static_cast<float>(awards.size() - 1)) / static_cast<float>(awards.size());
        for (std::size_t n = 0; n < awards.size(); ++n) {
            const float at = t - after - 0.15f * static_cast<float>(n), s = ease_pop(at / 0.35f), ca = unit(at / 0.15f);
            if (ca <= 0) continue;
            const float cxm = a.x + 24 * k + (cw + gap) * static_cast<float>(n) + cw * 0.5f, cym = y + 30 * k;
            const ImVec2 p0(cxm - cw * 0.5f * s, cym - 30 * k * s), p1(cxm + cw * 0.5f * s, cym + 30 * k * s);
            theme::rough_rect(draw, p0, p1, faded(theme::tile_light, 0.9f * ca), 11 + static_cast<unsigned>(n), k);
            draw->AddRectFilled(p0, ImVec2(p0.x + 5 * k, p1.y), faded(theme::bar, ca));
            centred(draw, bold, 15 * k * s, cxm, cym - 24 * k * s, faded(theme::bar, ca), awards[n].first);
            centred(draw, body, 19 * k * s, cxm, cym - 2 * k * s, faded(theme::white, ca), awards[n].second);
        }
        y += 64 * k;
    }
    // Who owns what: each spot's dot and owner, one after another.
    y += 16 * k;
    draw->AddText(bold, 17 * k, ImVec2(a.x + 24 * k, y), faded(theme::grey_text, unit((t - after - 0.4f) / 0.3f)), "WHO OWNS WHAT");
    y += 28 * k;
    const float column = (w - 48 * k) * 0.5f;
    for (std::size_t i = 0; i < v.spots.size(); ++i) {
        const float st = t - after - 0.45f - 0.025f * static_cast<float>(i), ia = unit(st / 0.2f);
        if (ia <= 0) continue;
        const auto &spot = v.spots[i];
        const float x = a.x + 24 * k + column * static_cast<float>(i / spot_rows), ry = y + 24 * k * static_cast<float>(i % spot_rows);
        const auto colour = colour_of(spot);
        draw->AddCircleFilled(ImVec2(x + 6 * k, ry + 9 * k), 6 * k * (0.6f + 0.4f * ease_pop(st / 0.3f)), faded(colour, ia), 16);
        draw->AddText(body, 17 * k, ImVec2(x + 18 * k, ry), faded(theme::white, ia), spot.name.substr(0, 22).c_str());
        const auto who = spot.owner == domination::OverlayOwner::none ? std::string("open") : std::format("{} {}", spot.owner_name.substr(0, 12), points(spot.top));
        draw->AddText(body, 17 * k, ImVec2(x + column - 12 * k - width(body, 17 * k, who), ry), faded(colour, ia), who.c_str());
    }
}

void draw_hud(ImDrawList *draw, ImFont *title, ImFont *bold, ImFont *body, float k, const domination::OverlayView &v) {
    const auto display = ImGui::GetIO().DisplaySize;
    // Timer and count, top centre.
    const float top = 18 * k;
    const bool hurry = v.running && v.left <= 30;
    const auto time = v.running ? clock(v.left) : std::string("0:00");
    std::string line;
    // (Teams: the team's spots, from its standings row.)
    const auto team_spots = [&] {
        for (const auto &row : v.standings)
            if (row.team == v.my_team) return row.spots;
        return 0;
    };
    if (v.teams) line = v.my_team >= 0 ? std::format("DOMINATION   team {}   {} spots, you own {}", v.my_team == 0 ? "RED" : "BLUE", team_spots(), v.taken)
                                       : "DOMINATION   picking teams...";
    else line = std::format("DOMINATION   you own {} / {}   {}", v.taken, v.spots.size(), points(v.total));
    const float panel = std::max({width(title, 54 * k, time), width(bold, 22 * k, line), 340 * k}) + 40 * k; // (as wide as its text)
    draw->AddRectFilled(ImVec2(display.x * 0.5f - panel * 0.5f, top - 6 * k), ImVec2(display.x * 0.5f + panel * 0.5f, top + 54 * k + 34 * k),
                        faded(theme::tile, 0.75f), 8 * k);
    centred(draw, title, 54 * k, display.x * 0.5f, top, hurry ? theme::danger : theme::white, time);
    centred(draw, bold, 22 * k, display.x * 0.5f, top + 54 * k + 4 * k, v.teams && v.my_team >= 0 ? team_colour(v.my_team) : yours, line);
    // (Below skate.'s own trick line, which runs across the top centre under the timer.)
    const float under = std::max(top + 54 * k + 46 * k, 190 * k);
    // Spot list, top right: each spot in its owner's colour.
    const float row = 22 * k, x1 = display.x - 20 * k, x0 = x1 - 330 * k;
    float y = 120 * k;
    draw->AddRectFilled(ImVec2(x0 - 10 * k, y - 8 * k), ImVec2(x1 + 8 * k, y + row * v.spots.size() + 8 * k), faded(theme::tile, 0.6f), 6 * k);
    for (const auto &spot : v.spots) {
        const auto colour = colour_of(spot);
        if (spot.flash > 0) draw->AddRectFilled(ImVec2(x0 - 6 * k, y), ImVec2(x1 + 4 * k, y + row), faded(colour, 0.45f * spot.flash), 4 * k);
        draw->AddCircleFilled(ImVec2(x0 + 6 * k, y + row * 0.5f), 6 * k, colour, 16);
        if (spot.here) draw->AddCircle(ImVec2(x0 + 6 * k, y + row * 0.5f), 10 * k, theme::white, 16, 2 * k);
        shadowed(draw, spot.here ? bold : body, 17 * k, ImVec2(x0 + 20 * k, y + 2 * k), theme::white, spot.name);
        auto score = spot.owner == domination::OverlayOwner::none ? std::string("-") : points(spot.top);
        if (spot.owner == domination::OverlayOwner::other) score = spot.owner_name.substr(0, 10) + " " + score;
        shadowed(draw, body, 17 * k, ImVec2(x1 - width(body, 17 * k, score), y + 2 * k), colour, score);
        y += row;
    }
    // Standings, under the spot list (multiplayer): most spots first.
    if (v.multiplayer && !v.standings.empty() && v.running) {
        y += 18 * k;
        const auto shown = shown_rows(v, 8);
        draw->AddRectFilled(ImVec2(x0 - 10 * k, y - 8 * k), ImVec2(x1 + 8 * k, y + row * (shown.size() + 1) + 8 * k), faded(theme::tile, 0.6f), 6 * k);
        shadowed(draw, bold, 17 * k, ImVec2(x0, y + 2 * k), theme::grey_text, v.teams ? "TEAMS          spots   held" : "PLAYERS        spots   held");
        y += row;
        for (const auto i : shown) {
            const auto &st = v.standings[i];
            const auto colour = colour_of(st);
            draw->AddCircleFilled(ImVec2(x0 + 6 * k, y + row * 0.5f), 6 * k, colour, 16);
            shadowed(draw, st.kind == domination::OverlayOwner::you ? bold : body, 17 * k, ImVec2(x0 + 20 * k, y + 2 * k), theme::white,
                     st.name.substr(0, 16));
            const auto right = std::format("{}   {}", st.spots, held_time(st.hold));
            shadowed(draw, body, 17 * k, ImVec2(x1 - width(body, 17 * k, right), y + 2 * k), colour, right);
            y += row;
        }
    }
    // The spot the trick going on counts for, bottom centre.
    if (v.running && !v.trick_spot.empty()) {
        centred(draw, bold, 26 * k, display.x * 0.5f, display.y - 170 * k, yours, "on " + v.trick_spot);
    } else if (v.running && v.countdown == 0) {
        // Standing on a spot: whose it is, and what it takes.
        for (const auto &spot : v.spots) {
            if (!spot.here) continue;
            using O = domination::OverlayOwner;
            std::string upper = spot.name;
            std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            const auto beat = [&](std::string holder) {
                return std::format("{}   {} {}: beat it{}", upper, holder, points(spot.top), spot.mine > 0 ? std::format("   (your best {})", points(spot.mine)) : "");
            };
            const auto text = spot.owner == O::none ? upper + "   open: any trick takes it"
                            : spot.owner == O::you ? std::format("{}   yours  {}", upper, points(spot.top))
                            : spot.ours ? std::format("{}   your team's ({} {})", upper, spot.owner_name, points(spot.top))
                            : beat(spot.owner_name);
            centred(draw, bold, 26 * k, display.x * 0.5f, display.y - 170 * k, spot.ours ? yours : colour_of(spot), text);
        }
    }
    // The feed under the timer: who did what, a small slanted banner each in the player's colour
    // (yours for your own, the taker's when a spot of yours goes), wiping in and fading out.
    // Not over the results.
    float ty = under;
    for (const auto &t : v.toasts) {
        if (v.results) break;
        const bool own = t.owner == domination::OverlayOwner::none && t.kind == 1;
        const ImU32 colour = t.owner != domination::OverlayOwner::none ? colour_of(t.owner, t.id, t.team, t.colour)
                           : t.kind == 1                                ? (v.teams && v.my_team >= 0 ? team_colour(v.my_team) : yours)
                           : t.kind == 2                                ? theme::warning
                           : t.kind == 3                                ? theme::danger
                                                                        : theme::tile_light;
        const bool loud = own || t.kind == 3; // a spot you won or lost
        const float alpha = fade(t.age, t.left, 0.12f, 0.5f), in = ease_out(t.age / 0.2f);
        const float size = (loud ? 22.0f : 18.0f) * k * (loud ? 0.9f + 0.1f * ease_pop(t.age / 0.3f) : 1.0f), height = size + 12 * k;
        const float tw = width(bold, size, t.text), wide = tw + 44 * k, bx = display.x * 0.5f - wide * 0.5f;
        // Dark text on light bands (yellow, light blue, white), light text on dark ones.
        const auto cr = (colour >> IM_COL32_R_SHIFT) & 0xff, cg = (colour >> IM_COL32_G_SHIFT) & 0xff, cb = (colour >> IM_COL32_B_SHIFT) & 0xff;
        const bool light = 0.299f * cr + 0.587f * cg + 0.114f * cb > 150;
        slant_bar(draw, bx + 4 * k, bx + 4 * k + wide * in, ty + 4 * k, height, 8 * k, faded(IM_COL32_BLACK, 0.4f * alpha));
        slant_bar(draw, bx, bx + wide * in, ty, height, 8 * k, faded(colour, (loud ? 0.92f : 0.8f) * alpha));
        if (const float sweep = (t.age - 0.1f) / 0.4f; loud && sweep > 0 && sweep < 1) {
            const float sx = bx + wide * ease_out(sweep);
            slant_bar(draw, sx - 24 * k, sx, ty, height, 8 * k, faded(theme::white, 0.4f * (1 - sweep) * alpha));
        }
        draw->AddText(bold, size, ImVec2(display.x * 0.5f - tw * 0.5f, ty + 6 * k), faded(light ? theme::black : theme::white, alpha * unit(t.age / 0.15f)), t.text.c_str());
        ty += (height + 6 * k) * (0.4f + 0.6f * in);
    }
    // The countdown: 3, 2, 1, then GO, big in the middle.
    if (v.running && (v.countdown > 0 || v.go)) {
        const auto text = v.countdown > 0 ? std::to_string(v.countdown) : std::string("GO!");
        centred(draw, title, 160 * k, display.x * 0.5f, display.y * 0.36f, v.countdown > 0 ? theme::white : yours, text);
    }
    if (v.results) draw_results(draw, title, bold, body, k, v);
}
} // namespace

bool domination_pending() {
    try {
        view() = domination::overlay_view();
    } catch (...) {
        view() = {};
    }
    return view().visible;
}

void draw_domination() {
    const auto &v = view();
    if (!v.visible) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float k = display.y / 1080.0f;
    auto &s = detail::state();
    auto *title = s.menu.title ? s.menu.title : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto *draw = ImGui::GetBackgroundDrawList();
    if (v.running)
        if (const auto p = projector(v.base)) {
            domination::note_camera(p->origin);
            draw_outlines(draw, k, *p, v);
            draw_markers(draw, bold, body, k, *p, v);
        }
    draw_hud(draw, title, bold, body, k, v);
}
}
