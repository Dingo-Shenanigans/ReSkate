#include "gui_internal.h"
#include "gui_renderer.h"

#include "mod_manager.h"

#include "problem.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <format>

// The main screen: the background with the wordmark over it, and down its right side everything
// that is acted on: who is playing with the way into the settings beside them, and at its foot
// MOD MANAGER and PLAY, which says whether it is ready. In the corner opposite PLAY, one tile says
// where things stand: what the newest release changed; or how far ReSkate is with what it is doing;
// or what needs the player or went wrong. Over it, a line for the team's announcement.
namespace dingosdk::launcher_gui::detail {
namespace {

void draw_background(ImDrawList* draw, ImVec2 size, float time) {
    // Vertical gradient plus a warm glow bottom-right and a cool glow top-left,
    // evaluated on a grid of bilinear quads. The quads are opaque: stacked
    // circles band, and translucent layers under the modal dim left holes on
    // an RX 9070 XT.
    struct Glow { ImVec2 centre; float radius; ImVec4 colour; };
    const Glow glows[]{
        {ImVec2(size.x * 0.95f, size.y * 1.05f), S(700), ImVec4(1.0f, 0.5f, 0.08f, 0.42f)},
        {ImVec2(size.x * 0.02f, size.y * -0.08f), S(560), ImVec4(0.23f, 0.38f, 0.9f, 0.22f)},
    };
    const auto top = ImGui::ColorConvertU32ToFloat4(color::background_top);
    const auto bottom = ImGui::ColorConvertU32ToFloat4(color::background_bottom);
    const auto field = [&](float x, float y) {
        const float v = std::clamp(y / size.y, 0.0f, 1.0f);
        ImVec4 out(top.x + (bottom.x - top.x) * v, top.y + (bottom.y - top.y) * v, top.z + (bottom.z - top.z) * v, 1);
        for (const auto& glow : glows) {
            const float t = std::min(std::hypot(x - glow.centre.x, y - glow.centre.y) / glow.radius, 1.0f);
            const float a = glow.colour.w * (1 - t) * (1 - t);
            out.x += (glow.colour.x - out.x) * a;
            out.y += (glow.colour.y - out.y) * a;
            out.z += (glow.colour.z - out.z) * a;
        }
        return ImGui::ColorConvertFloat4ToU32(out);
    };
    constexpr int columns = 32, rows = 18;
    const float cell_x = size.x / columns, cell_y = size.y / rows;
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            const float x0 = cell_x * static_cast<float>(column), y0 = cell_y * static_cast<float>(row);
            const float x1 = x0 + cell_x, y1 = y0 + cell_y;
            draw->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1),
                field(x0, y0), field(x1, y0), field(x1, y1), field(x0, y1));
        }

    // Skatepark floor: a perspective grid converging on the wordmark.
    const ImVec2 vanish(size.x * 0.5f, size.y * 0.46f);
    const float floor = size.y * 0.62f;
    for (int line = -14; line <= 14; ++line) {
        const float x = size.x * 0.5f + static_cast<float>(line) * S(120);
        draw->AddLine(ImVec2(vanish.x + (x - vanish.x) * 0.18f, floor), ImVec2(x, size.y), rgba(255, 255, 255, 0.035f), 1);
    }
    const float scroll = std::fmod(time * 0.06f, 1.0f);
    for (int row = 0; row < 9; ++row) {
        const float t = (static_cast<float>(row) + scroll) / 9.0f;
        const float y = floor + (size.y - floor) * t * t;
        draw->AddLine(ImVec2(0, y), ImVec2(size.x, y), rgba(255, 255, 255, 0.03f * t + 0.01f), 1);
    }

    // Drifting outline shapes, like chalk marks on a ramp.
    for (int index = 0; index < 28; ++index) {
        const float seed = static_cast<float>(index) * 12.9898f;
        const float fx = std::fmod(std::abs(std::sin(seed) * 43758.5453f), 1.0f);
        const float fy = std::fmod(std::abs(std::sin(seed * 1.7f) * 24634.6345f), 1.0f);
        const float drift = time * (0.004f + 0.006f * fx);
        const ImVec2 centre(size.x * std::fmod(fx + drift, 1.0f), size.y * fy + std::sin(time * 0.3f + seed) * S(6));
        const float radius = S(3.0f + 16.0f * fy * fx);
        const float alpha = 0.05f + 0.07f * fy;
        if (index % 3 == 0) {
            const float angle = time * 0.2f + seed;
            ImVec2 corners[4];
            for (int corner = 0; corner < 4; ++corner) {
                const float a = angle + static_cast<float>(corner) * 1.5707963f;
                corners[corner] = ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius);
            }
            draw->AddQuad(corners[0], corners[1], corners[2], corners[3], rgba(255, 255, 255, alpha), 1.2f);
        } else {
            draw->AddCircle(centre, radius, rgba(255, 255, 255, alpha), 0, 1.2f);
        }
    }
    // Vignette edges.
    draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.75f), size, 0, 0, rgba(0, 0, 0, 0.45f), rgba(0, 0, 0, 0.45f));
}

// A tile's icon centred at `centre`; false when it isn't loaded (use the drawn one).
bool tile_icon(ImDrawList* draw, const Background& icon, ImVec2 centre, float size, bool hovered) {
    if (!icon.id) return false;
    const float half = size * 0.5f;
    draw->AddImage(icon.id, ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half),
        ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, hovered ? 0.6f : 0.32f));
    return true;
}

void draw_photo(ImDrawList* draw, ImVec2 size, float time) {
    // Cover-fit with a slow drift so the still image feels alive.
    const float zoom = 1.04f + 0.02f * std::sin(time * 0.05f);
    const float scale = std::max(size.x / g_background.width, size.y / g_background.height) * zoom;
    const ImVec2 image(g_background.width * scale, g_background.height * scale);
    const ImVec2 origin((size.x - image.x) * 0.5f + std::sin(time * 0.037f) * S(10),
                        (size.y - image.y) * 0.5f + std::cos(time * 0.029f) * S(6));
    draw->AddImage(g_background.id, origin, ImVec2(origin.x + image.x, origin.y + image.y));
    // Keep the wordmark and bottom row readable over a busy photo.
    draw->AddRectFilled(ImVec2(0, 0), size, rgba(6, 8, 12, 0.38f));
    draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.45f), size, rgba(6, 8, 12, 0), rgba(6, 8, 12, 0),
        rgba(6, 8, 12, 0.92f), rgba(6, 8, 12, 0.92f));
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, S(90)), rgba(6, 8, 12, 0.6f), rgba(6, 8, 12, 0.6f),
        rgba(6, 8, 12, 0), rgba(6, 8, 12, 0));
}

// Drawing shared with the in-game menu (UI/skate_theme.h), at the launcher's scale.
using skate_theme::Icon;
using skate_theme::rotate_since;
void rough_rect(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, unsigned seed) {
    skate_theme::rough_rect(draw, a, b, colour, seed, g_scale);
}
void scribble(ImDrawList* draw, ImVec2 a, ImVec2 b, unsigned seed) { skate_theme::scribble(draw, a, b, seed, g_scale); }

// Loads the signed-in account's Steam picture for the plate, or drops it when Steam is closed or
// signed out. Only re-decoded when the cached file changes.
void refresh_steam_avatar(Ui& ui) {
    const auto path = launcher_app::steam_avatar_path();
    std::error_code error;
    const auto time = path.empty() ? fs::file_time_type{} : fs::last_write_time(path, error);
    if (path == ui.steam_avatar_path && time == ui.steam_avatar_time) return;
    if (ui.steam_avatar && g_renderer) g_renderer->release_texture(ui.steam_avatar);
    ui.steam_avatar = {};
    ui.steam_avatar_path = path;
    ui.steam_avatar_time = time;
    if (path.empty() || !g_renderer) return;
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<unsigned char> pixels;
    UINT width{}, height{};
    if (!bytes.empty() && decode_image(bytes, ImVec2(S(46), S(46)), pixels, width, height))
        ui.steam_avatar = g_renderer->upload_texture(pixels, width, height);
}

// The plate at the head of the right-hand column: one tile, cut like the others, with who is
// playing (their Steam picture, a dot for being signed in, their name over the build this is) and,
// past a line at its far end, the cog that opens the settings. Returns true when the cog was pressed.
bool account_plate(ImDrawList* draw, const Fonts& fonts, ImVec2 position, ImVec2 size, const std::string& title,
                   const std::string& version, ImTextureID avatar, bool enabled) {
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, color::tile, 11);

    // The settings: the plate's last square.
    const float cog = size.y;
    const ImVec2 cog_at(end.x - cog, position.y);
    bool hovered{};
    const bool pressed = tile_hit("##settings", cog_at, ImVec2(cog, cog), enabled, hovered);
    if (hovered) {
        draw->AddRectFilled(ImVec2(cog_at.x + S(1), position.y + S(4)), ImVec2(end.x - S(4), end.y - S(4)), color::tile_grey);
        ImGui::SetTooltip("Settings");
    }
    draw->AddLine(ImVec2(cog_at.x, position.y + S(12)), ImVec2(cog_at.x, end.y - S(12)), rgba(255, 255, 255, 0.12f));
    const ImVec2 centre(cog_at.x + cog * 0.5f, position.y + size.y * 0.5f);
    const ImU32 ink = rgba(255, 255, 255, hovered ? 1.0f : 0.72f);
    if (g_icon_settings.id) {
        const float half = S(11);
        draw->AddImage(g_icon_settings.id, ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half),
            ImVec2(0, 0), ImVec2(1, 1), ink);
    } else {
        // Without the picture: six teeth round a ring.
        for (int tooth = 0; tooth < 6; ++tooth) {
            const float angle = static_cast<float>(tooth) * 1.0471976f;
            draw->AddLine(ImVec2(centre.x + std::cos(angle) * S(6), centre.y + std::sin(angle) * S(6)),
                          ImVec2(centre.x + std::cos(angle) * S(10), centre.y + std::sin(angle) * S(10)), ink, S(5));
        }
        draw->AddCircle(centre, S(6), ink, 0, S(4));
    }

    // Who is playing: the picture set into the tile, the dot on its corner.
    const float inset = S(8), picture = size.y - inset * 2;
    const ImVec2 at(position.x + inset, position.y + inset), corner(at.x + picture, at.y + picture);
    if (avatar) {
        draw->AddImageRounded(avatar, at, corner, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
    } else {
        draw->AddRectFilled(at, corner, color::avatar, S(4));
        // A small deck where the picture would be.
        const ImVec2 c(at.x + picture * 0.5f, at.y + picture * 0.5f);
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(ImVec2(c.x - S(13), c.y - S(5)), ImVec2(c.x + S(13), c.y + S(5)), color::text, S(5));
        for (const float x : {-8.0f, 8.0f}) draw->AddCircleFilled(ImVec2(c.x + S(x), c.y), S(1.6f), color::avatar);
        rotate_since(draw, start, -40.0f, c);
    }
    draw->AddCircleFilled(ImVec2(corner.x - S(1), corner.y - S(1)), S(6.5f), color::tile);
    draw->AddCircleFilled(ImVec2(corner.x - S(1), corner.y - S(1)), S(4.5f), color::good);

    const float text_x = corner.x + S(14);
    const float block = fonts.bold->FontSize + S(4) + fonts.body->FontSize;
    const float y = position.y + (size.y - block) * 0.5f;
    const ImVec4 clip(text_x, position.y, cog_at.x - S(10), end.y);
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, y), color::text, title.c_str(), nullptr, 0, &clip);
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, y + fonts.bold->FontSize + S(4)), color::muted, version.c_str(),
                  nullptr, 0, &clip);
    return pressed;
}

// The big action tile: blue like skate.'s selected tile, grey and scribbled when locked.
bool action_tile(ImDrawList* draw, const Fonts& fonts, ImVec2 position, ImVec2 size, const char* label,
                 const std::string& detail, bool enabled, bool secondary, const std::string& mark = {}) {
    bool hovered{};
    const bool pressed = tile_hit("##primary", position, size, enabled, hovered);
    const ImVec2 end(position.x + size.x, position.y + size.y);
    const bool blue = enabled && !secondary;
    rough_rect(draw, position, end, blue ? color::blue : enabled ? color::tile : color::tile_grey, 7);
    if (!enabled) scribble(draw, position, end, 7);
    if (hovered) draw->AddRect(ImVec2(position.x - S(3), position.y - S(3)), ImVec2(end.x + S(3), end.y + S(3)), color::text, 0, 0, S(3));
    const ImU32 ink = blue ? color::ink : enabled ? color::text : rgba(160, 162, 168);
    draw->AddText(fonts.action, fonts.action->FontSize, ImVec2(position.x + S(18), position.y + S(12)), ink, label);
    if (!detail.empty())
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(position.x + S(20), end.y - S(20) - fonts.bold->FontSize),
            blue ? rgba(0, 0, 0, 0.72f) : color::muted, detail.c_str(), nullptr, size.x - S(40));
    // On the blue tile a dark pill reads; on a grey one the usual blue does.
    if (!mark.empty())
        badge(draw, fonts, ImVec2(end.x - S(20) - badge_width(fonts, mark), position.y + S(20)), mark,
            blue ? rgba(0, 0, 0, 0.34f) : color::blue, blue ? color::text : color::ink);
    return pressed;
}

// The tile in the bottom-left corner: what the newest release changed, in its own words (its
// first line, and what its first few notes lead with), so most never need to open it. It opens the
// changelogs. Before the notes are here (or offline, where they are never fetched) it only says
// where it goes.
bool changelog_tile(ImDrawList* draw, const Fonts& fonts, ImVec2 position, ImVec2 size, bool enabled, const Changelog& log) {
    bool hovered{};
    const bool pressed = tile_hit("##changelog", position, size, enabled, hovered);
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, hovered ? color::tile_grey : color::tile, 9);
    draw->AddRectFilled(position, ImVec2(position.x + S(4), end.y), color::blue);
    if (hovered) draw->AddRect(ImVec2(position.x - S(3), position.y - S(3)), ImVec2(end.x + S(3), end.y + S(3)), color::text, 0, 0, S(3));
    const float pad = S(20), x = position.x + pad;
    const ImVec4 clip(x, position.y, end.x - pad - S(34), end.y);
    if (log.loaded && !log.notes.empty() && !log.texts.empty()) {
        using Kind = thunderstore::ReadmeLine::Kind;
        const auto& note = log.notes.front();
        // Its first line that is more than the release's name, and what its first three notes lead with.
        std::string headline, leads;
        int counted = 0;
        for (const auto& line : log.texts.front().lines) {
            if (line.kind == Kind::text && headline.empty() && !line.text.empty() && line.text.find(note.tag) == std::string::npos)
                headline = line.text;
            if (line.kind == Kind::bullet && counted < 3) {
                auto stop = line.text.find(". ");
                auto lead = line.text.substr(0, stop);
                if (!lead.empty() && lead.back() == '.') lead.pop_back();
                leads += (leads.empty() ? "" : "   /   ") + lead;
                ++counted;
            }
        }
        if (headline.empty()) headline = note.title.empty() || note.title == note.tag ? "ReSkate " + note.tag : note.title;
        const float pill = fonts.caption->FontSize + S(8);
        const float block = pill + S(8) + fonts.heading->FontSize + (leads.empty() ? 0.0f : S(6) + fonts.body->FontSize);
        float y = position.y + (size.y - block) * 0.5f;
        badge(draw, fonts, ImVec2(x, y), "WHAT'S NEW IN " + note.tag, color::blue, color::ink);
        y += pill + S(8);
        draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(x, y), color::text, headline.c_str(), nullptr, 0, &clip);
        y += fonts.heading->FontSize + S(6);
        if (!leads.empty()) draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(x, y), color::muted, leads.c_str(), nullptr, 0, &clip);
    } else {
        const float block = fonts.tile->FontSize + S(6) + fonts.body->FontSize;
        const float y = position.y + (size.y - block) * 0.5f;
        draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(x, y), color::text, "WHAT'S NEW");
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(x, y + fonts.tile->FontSize + S(6)), color::muted,
                      log.loading ? "Reading what the newest release changed..." : "See what each release changed");
    }
    // An arrow at the far end: it goes somewhere.
    const ImVec2 tip(end.x - pad, position.y + size.y * 0.5f);
    const ImU32 ink = hovered ? color::text : color::muted;
    draw->AddLine(ImVec2(tip.x - S(16), tip.y), tip, ink, S(2));
    draw->AddLine(ImVec2(tip.x - S(6), tip.y - S(6)), tip, ink, S(2));
    draw->AddLine(ImVec2(tip.x - S(6), tip.y + S(6)), tip, ink, S(2));
    return pressed;
}

// Whether ReSkate is at work on something that ends by itself: checking, updating, downloading the
// game, merging the mods, starting Skate. (The STATUS tile is for what needs an answer.)
bool working(Phase phase) {
    return phase == Phase::checking || phase == Phase::updating || phase == Phase::downloading || phase == Phase::merging ||
           phase == Phase::launching;
}

// While ReSkate is at work, in WHAT'S NEW's place (which nobody reads while they wait): what it is
// doing, how far it is as a figure and as a bar across the whole tile, and under that the line of
// detail (the file, the speed). With no figure to give, the bar runs from side to side.
void progress_tile(ImDrawList* draw, const Fonts& fonts, const State& state, ImVec2 position, ImVec2 size, float time) {
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, color::tile, 9);
    draw->AddRectFilled(position, ImVec2(position.x + S(4), end.y), color::blue);
    const float pad = S(20), x = position.x + pad, right = end.x - pad;
    const bool known = state.progress >= 0;
    const float fill = std::clamp(state.progress, 0.0f, 1.0f);

    // What it is doing, and at the far end how far it is.
    const auto figure = known ? std::format("{:.0f}%", fill * 100) : std::string();
    const float figure_width = fonts.tile->CalcTextSizeA(fonts.tile->FontSize, FLT_MAX, 0, figure.c_str()).x;
    const float top = position.y + S(16);
    skate_theme::status_icon(draw, ImVec2(x + S(10), top + fonts.heading->FontSize * 0.5f), Icon::busy, time, g_scale);
    const ImVec4 clip(x + S(28), position.y, right - figure_width - S(16), end.y);
    draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(x + S(28), top), color::text, state.status.c_str(), nullptr, 0, &clip);
    if (known)
        draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(right - figure_width, top + (fonts.heading->FontSize - fonts.tile->FontSize) * 0.5f),
                      color::text, figure.c_str());

    // The bar: a dark groove, filled in blue with lighter stripes moving along it.
    const float bar_tall = S(12);
    const ImVec2 bar(x, top + fonts.heading->FontSize + S(14)), bar_end(right, bar.y + bar_tall);
    draw->AddRectFilled(bar, bar_end, rgba(0, 0, 0, 0.55f), bar_tall * 0.5f);
    const float across = bar_end.x - bar.x;
    // (Without a figure: a third of the bar, going from one end to the other and back.)
    const float from = known ? bar.x : bar.x + (across * 0.67f) * (0.5f - 0.5f * std::cos(time * 2.2f));
    const float to = known ? bar.x + across * fill : from + across * 0.33f;
    if (to - from > S(2)) {
        draw->AddRectFilled(ImVec2(from, bar.y), ImVec2(to, bar_end.y), color::blue, bar_tall * 0.5f);
        draw->PushClipRect(ImVec2(from + S(3), bar.y), ImVec2(to - S(3), bar_end.y), true);
        const float step = S(22), slide = std::fmod(time * S(40), step);
        for (float stripe = from - step * 2 + slide; stripe < to; stripe += step) {
            const ImVec2 corners[]{ImVec2(stripe, bar_end.y), ImVec2(stripe + S(10), bar.y), ImVec2(stripe + S(19), bar.y),
                                   ImVec2(stripe + S(9), bar_end.y)};
            draw->AddConvexPolyFilled(corners, 4, rgba(255, 255, 255, 0.16f));
        }
        draw->PopClipRect();
    }

    // What it is on now, under the bar.
    if (!state.detail.empty()) {
        const ImVec4 line(x, position.y, right, end.y);
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(x, bar_end.y + S(10)), color::muted, state.detail.c_str(), nullptr, 0, &line);
    }
}

// The MODS tile, the biggest after PLAY: a mod browser nobody finds is no use.
bool mods_tile(ImDrawList* draw, const Fonts& fonts, ImVec2 position, ImVec2 size, bool enabled,
               const std::string& detail, const std::string& mark, bool bad) {
    bool hovered{};
    const bool pressed = tile_hit("##mods", position, size, enabled, hovered);
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, color::tile, 4);
    if (hovered) draw->AddRect(ImVec2(position.x - S(3), position.y - S(3)), ImVec2(end.x + S(3), end.y + S(3)), color::text, 0, 0, S(3));
    // Title and detail centred as one block, so the tile's height can change.
    const float block = fonts.tile->FontSize + (detail.empty() ? 0 : fonts.body->FontSize + S(6));
    const float text_y = position.y + (size.y - block) * 0.5f;
    draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(position.x + S(18), text_y), color::text, "MOD MANAGER");
    if (!detail.empty()) {
        const float detail_y = text_y + fonts.tile->FontSize + S(6);
        float room = end.x - S(86);        // the faded icon owns the rest
        // A mod that did not load says so in the detail's colour; an update
        // count is short enough for a pill beside it.
        if (!mark.empty() && !bad) {
            room -= badge_width(fonts, mark);
            badge(draw, fonts,
                ImVec2(room, detail_y + (fonts.body->FontSize - fonts.caption->FontSize - S(8)) * 0.5f),
                mark, color::blue, color::ink);
            room -= S(10);
        }
        const ImVec4 clip(position.x + S(20), detail_y, room, detail_y + fonts.body->FontSize + S(2));
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(position.x + S(20), detail_y),
            bad ? color::danger : color::muted, detail.c_str(), nullptr, 0, &clip);
    }
    // The skate tool, faded like the wheel on SETTINGS.
    const ImVec2 centre(end.x - S(46), position.y + size.y * 0.5f);
    if (tile_icon(draw, g_icon_mods, centre, S(56), hovered)) return pressed;
    const ImU32 ink = rgba(255, 255, 255, hovered ? 0.55f : 0.3f);
    for (int layer = 0; layer < 3; ++layer) {
        const float y = centre.y - S(12) + static_cast<float>(layer) * S(12);
        const ImVec2 points[]{ImVec2(centre.x, y - S(7)), ImVec2(centre.x + S(20), y), ImVec2(centre.x, y + S(7)),
                              ImVec2(centre.x - S(20), y)};
        draw->AddPolyline(points, 4, ink, ImDrawFlags_Closed, S(3));
    }
    return pressed;
}

// In WHAT'S NEW's place while something needs the player (the game is not installed or is out of
// date, a ReSkate update is ready, mods were left out) or went wrong: what it is and what to do
// about it, with the tile's edge in the warning colour, or red for a failure. PLAY, beside it,
// is the answer (INSTALL, UPDATE, RETRY), so this only has to say why. A failure also gets the way
// to the logs and to copy what happened, for whoever is asked to help. (It was a STATUS tile of
// its own, up the left side of the picture, saying what PLAY and this corner already say.)
void state_tile(ImDrawList* draw, const Fonts& fonts, const State& state, ImVec2 position, ImVec2 size, float time,
                const fs::path& logs, bool modal) {
    const bool failed = state.phase == Phase::failed;
    const auto problem = failed ? launcher_problem::explain(state.status) : launcher_problem::Problem{};
    const std::string headline = failed ? problem.headline : state.status;
    const std::string detail = failed ? problem.advice : state.detail;
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, color::tile, 9);
    draw->AddRectFilled(position, ImVec2(position.x + S(4), end.y), failed ? color::danger : color::warning);

    // The builds this is about: on hover, and in what Copy details copies.
    std::string builds;
    if (state.config) {
        builds = "skate. build " + (state.config->game.build_id.empty() ? state.config->game.manifest_id : state.config->game.build_id);
        if (update::binary_updates_enabled()) builds += "\nReSkate " + state.config->runtime.version;
    }

    const float pad = S(20), button_width = S(124), button_tall = S(30);
    const float buttons_x = end.x - pad - button_width;
    const float text_x = position.x + pad + S(28), text_end = failed ? buttons_x - S(18) : end.x - pad;
    const float line = fonts.body->FontSize + S(2);
    const float detail_tall = detail.empty() ? 0.0f
        : std::min(fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, text_end - text_x, detail.c_str()).y, line * 2);
    const float block = fonts.heading->FontSize + (detail.empty() ? 0.0f : S(6) + detail_tall);
    const float y = position.y + (size.y - block) * 0.5f;
    skate_theme::status_icon(draw, ImVec2(position.x + pad + S(10), y + fonts.heading->FontSize * 0.5f), failed ? Icon::fail : Icon::warning,
                             time, g_scale);
    const ImVec4 clip(text_x, position.y, text_end, end.y);
    draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(text_x, y), failed ? color::danger : color::text, headline.c_str(), nullptr, 0,
                  &clip);
    if (!detail.empty()) {
        // Two lines of it at most: the rest is in the logs.
        const float detail_y = y + fonts.heading->FontSize + S(6);
        const ImVec4 lines(text_x, detail_y, text_end, detail_y + detail_tall);
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, detail_y), color::muted, detail.c_str(), nullptr, text_end - text_x, &lines);
    }
    bool over_buttons = false;
    if (failed) {
        const float top = position.y + (size.y - button_tall * 2 - S(8)) * 0.5f;
        ImGui::BeginDisabled(modal);
        ImGui::SetCursorScreenPos(ImVec2(buttons_x, top));
        if (ImGui::Button("Open logs", ImVec2(button_width, button_tall))) open_path(logs);
        over_buttons |= ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(ImVec2(buttons_x, top + button_tall + S(8)));
        if (ImGui::Button("Copy details", ImVec2(button_width, button_tall))) {
            // The raw message, not the friendly one: this is for the Discord.
            ImGui::SetClipboardText(("ReSkate launcher\n" + state.status + "\n" + builds + "\n").c_str());
        }
        over_buttons |= ImGui::IsItemHovered();
        ImGui::EndDisabled();
    }
    if (!builds.empty() && !modal && !over_buttons && ImGui::IsMouseHoveringRect(position, end)) ImGui::SetTooltip("%s", builds.c_str());
}

// Reads the announcement in the background once it is due (as the launcher starts, then every ten
// minutes), and takes in a read once it is done. A read that fails keeps what is shown.
void refresh_announcement(Announcement& news, double time) {
    {
        std::lock_guard lock(news.mutex);
        if (news.arrived) {
            news.arrived = false;
            news.shown = std::move(news.incoming);
            news.text = news.shown ? thunderstore::readme_lines(news.shown->markdown, 400) : thunderstore::Readme{};
        }
    }
    if (time < news.next_read || news.loading.exchange(true)) return;
    news.next_read = time + 600;
    if (news.worker.joinable()) news.worker.join();
    news.worker = std::thread([&news] {
        std::optional<Announcement::Message> found;
        bool read{};
        try {
            std::string body;
            launcher_update::http_stream(L"https://api.reskate.dev/api/v1/announcement", 64 * 1024, 15000,
                                         [&](const char* data, DWORD size) { body.append(data, size); });
            const auto root = Json::parse(body);
            const auto& entry = root.at("announcement");
            if (entry.is_object()) {
                Announcement::Message message;
                message.version = entry.at("version").get<std::uint64_t>();
                message.important = entry.value("level", std::string()) == "important";
                message.title = entry.at("title").string();
                message.markdown = entry.value("markdown", std::string());
                message.link = entry.value("link", std::string());
                if (!message.link.starts_with("https://")) message.link.clear();
                message.link_label = entry.value("link_label", std::string());
                if (message.link_label.empty()) message.link_label = "Learn more";
                if (!message.title.empty()) found = std::move(message);
            }
            read = true;
        } catch (const std::exception& failure) {
            logging::write(logging::Level::warning, logging::Channel::launcher,
                           std::string("Could not read the announcement: ") + failure.what());
        }
        {
            std::lock_guard lock(news.mutex);
            if (read) {
                news.incoming = std::move(found);
                news.arrived = true;
            }
        }
        news.loading = false;
    });
}

// The announcement there is to show: the one the backend has now. (It could be hidden, when it
// was a card as tall as the picture; a line is not in anyone's way, and a notice someone hid
// was a notice they then never saw change.)
const Announcement::Message* announcement_to_show(const Ui& ui) {
    const auto& news = ui.announcement;
    return news.shown ? &*news.shown : nullptr;
}

// The announcement on the home screen: one line over the corner tile, NEWS (or IMPORTANT, in the
// warning colour) and its title, which opens it in full (announcement_window). (It was a card
// with all of its text, as tall as the picture.)
void announcement_banner(const Fonts& fonts, Ui& ui, ImVec2 position, ImVec2 size, bool enabled) {
    const auto* message = announcement_to_show(ui);
    if (!message) return;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(position.x + size.x, position.y + size.y);
    bool hovered{};
    if (tile_hit("##announcement", position, size, enabled, hovered)) ui.news = true;
    rough_rect(draw, position, end, hovered ? color::tile_grey : color::tile, 13);
    const ImU32 accent = message->important ? color::warning : color::blue;
    draw->AddRectFilled(position, ImVec2(position.x + S(4), end.y), accent);
    if (hovered) draw->AddRect(ImVec2(position.x - S(3), position.y - S(3)), ImVec2(end.x + S(3), end.y + S(3)), color::text, 0, 0, S(3));
    const float pad = S(20), middle = position.y + size.y * 0.5f;
    const std::string pill = message->important ? "IMPORTANT" : "NEWS";
    const float pill_tall = fonts.caption->FontSize + S(8);
    badge(draw, fonts, ImVec2(position.x + pad, middle - pill_tall * 0.5f), pill, accent, color::ink);
    const float title_x = position.x + pad + badge_width(fonts, pill) + S(12);
    const ImVec4 clip(title_x, position.y, end.x - pad - S(34), end.y);
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(title_x, middle - fonts.bold->FontSize * 0.5f), color::text, message->title.c_str(),
                  nullptr, 0, &clip);
    // An arrow at the far end, as on WHAT'S NEW under it: it opens.
    const ImVec2 tip(end.x - pad, middle);
    const ImU32 ink = hovered ? color::text : color::muted;
    draw->AddLine(ImVec2(tip.x - S(16), tip.y), tip, ink, S(2));
    draw->AddLine(ImVec2(tip.x - S(6), tip.y - S(6)), tip, ink, S(2));
    draw->AddLine(ImVec2(tip.x - S(6), tip.y + S(6)), tip, ink, S(2));
}

// The announcement in full, over the home screen: a panel as tall as what it says, its edge in
// the announcement's colour, with what kind it is, its title, its text set as the changelogs'
// notes are, and at its foot the button to its link.
void announcement_window(const Fonts& fonts, ImVec2 size, Ui& ui) {
    const auto* message = announcement_to_show(ui);
    if (!message) {
        ui.news = false;
        return;
    }
    // How tall it came to the last time it was drawn (its text is wrapped, so only drawing it
    // says): the panel is that tall, up to what the window has room for, where its text scrolls.
    static float wanted = 0;
    static std::uint64_t wanted_for = 0;
    if (wanted_for != message->version) {
        wanted_for = message->version;
        wanted = 0;
    }
    const float most = size.y - S(80);
    const auto panel = begin_panel("##announcement_panel", size,
        ImVec2(std::min(S(640), size.x - S(120)), std::clamp(wanted > 0 ? wanted : S(420), S(220), most)));
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 accent = message->important ? color::warning : color::blue;
    const ImVec2 corner = ImGui::GetWindowPos();
    draw->AddRectFilled(corner, ImVec2(corner.x + S(5), corner.y + panel.y), accent);

    // What kind it is, over its title.
    const std::string pill = message->important ? "IMPORTANT" : "NEWS";
    badge(draw, fonts, ImGui::GetCursorScreenPos(), pill, accent, color::ink);
    ImGui::Dummy(ImVec2(0, fonts.caption->FontSize + S(8) + S(4)));
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(fonts.tile);
    ImGui::TextUnformatted(message->title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(4)));
    {
        const ImVec2 line = ImGui::GetCursorScreenPos();
        draw->AddLine(line, ImVec2(line.x + ImGui::GetContentRegionAvail().x, line.y), color::outline);
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    const float footer = ImGui::GetFrameHeight() + S(24) + S(14);
    const float body_top = ImGui::GetCursorPosY();
    ImGui::BeginChild("##announcement_body", ImVec2(0, std::max(S(40), panel.y - body_top - footer)), ImGuiChildFlags_NavFlattened);
    if (ui.announcement.text.lines.empty()) ImGui::TextDisabled("There is no more to it than its title.");
    else draw_notes(fonts, ui.announcement.text, {}, false);
    const float body = ImGui::GetCursorPosY();
    ImGui::EndChild();
    wanted = body_top + body + S(6) + footer;

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    bool close = false;
    if (!message->link.empty()) {
        push_primary_button();
        if (ImGui::Button((message->link_label + "##announcement_link").c_str())) open_url(message->link);
        pop_primary_button();
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    // (Blue when there is no link to be that: the panel's one thing to press.)
    if (message->link.empty()) push_primary_button();
    close |= ImGui::Button("CLOSE", ImVec2(S(110), 0));
    if (message->link.empty()) pop_primary_button();
    default_focus();
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ui.news = false;
    ImGui::End();
}

} // namespace

void page_title(ImDrawList* draw, const Fonts& fonts, ImVec2 position, const char* text, float wanted) {
    const int start = draw->VtxBuffer.Size;
    const float size = wanted > 0 ? wanted : fonts.title->FontSize;
    const auto extent = fonts.title->CalcTextSizeA(size, FLT_MAX, 0, text);
    draw->AddText(fonts.title, size, ImVec2(position.x + S(3), position.y + S(4)), rgba(0, 0, 0, 0.5f), text);
    draw->AddText(fonts.title, size, position, color::text, text);
    skate_theme::rotate_since(draw, start, -4.0f, ImVec2(position.x + extent.x * 0.5f, position.y + extent.y * 0.5f));
}

void window_buttons(ImDrawList* draw, HWND window, ImVec2 size) {
    const ImVec2 button(S(46), S(34));
    bool hovered{};
    ImVec2 position(size.x - button.x * 2, 0);
    if (tile_hit("##minimise", position, button, true, hovered)) ShowWindow(window, SW_MINIMIZE);
    if (hovered) draw->AddRectFilled(position, ImVec2(position.x + button.x, button.y), rgba(255, 255, 255, 0.08f));
    const ImVec2 middle(position.x + button.x * 0.5f, button.y * 0.5f);
    draw->AddLine(ImVec2(middle.x - S(5), middle.y), ImVec2(middle.x + S(5), middle.y), color::text, S(1));
    position.x += button.x;
    if (tile_hit("##close", position, button, true, hovered)) PostMessageW(window, WM_CLOSE, 0, 0);
    if (hovered) draw->AddRectFilled(position, ImVec2(position.x + button.x, button.y), rgba(232, 17, 35));
    const ImVec2 cross(position.x + button.x * 0.5f, button.y * 0.5f);
    draw->AddLine(ImVec2(cross.x - S(5), cross.y - S(5)), ImVec2(cross.x + S(5), cross.y + S(5)), color::text, S(1));
    draw->AddLine(ImVec2(cross.x - S(5), cross.y + S(5)), ImVec2(cross.x + S(5), cross.y - S(5)), color::text, S(1));
}

void frame(Launcher& launcher, const Fonts& fonts, HWND window, Ui& ui, ModsPanel& mods_panel) {
    const auto& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    const float time = static_cast<float>(ImGui::GetTime());
    const auto state = launcher.snapshot();
    const bool qr_open = !state.qr.empty();
    // The Thunderstore listing loads in the background from the start, so the
    // MODS tile can say when installed mods have updates.
    collect_listing(mods_panel);
    refresh_listing(mods_panel, time);
    prepare_mods(mods_panel, launcher.session());
    // Whether the privacy policy or terms changed since the player agreed (looked up once).
    check_legal_versions(ui, launcher.settings());
    // The team's announcement for the home screen (offline: none is read).
    if (!launcher.settings().offline) refresh_announcement(ui.announcement, time);
    // And what the releases changed, for WHAT'S NEW and the changelogs.
    ui.notes.prereleases = launcher.settings().prereleases;
    if (!launcher.settings().offline) pump_changelog(ui.notes);
    // A mod dropped on the window opens the Mods panel and installs it.
    {
        std::lock_guard lock(g_dropped_mutex);
        if (!g_dropped.empty() && !ui.settings && !ui.sign_in && !qr_open && !state.prompt) {
            ui.mods = true;
            mods_panel.tab = 0;
            if (!mods_panel.scanned) scan(mods_panel, launcher.session());
            start_install(mods_panel, g_dropped.front(), false);
        }
        g_dropped.clear();
    }
    // The Steam name for the plate, and the one-time offline notice, both come
    // from the first Steam check; steam_offline_seen keeps it to one decision.
    if (time - ui.steam_checked > 5) {
        ui.steam_name = launcher_app::steam_persona_name();
        refresh_steam_avatar(ui);
        ui.steam_checked = time;
        if (!ui.steam_offline_seen) {
            ui.steam_offline_seen = true;
            ui.steam_offline = !launcher.settings().offline && !launcher_app::steam_signed_in();
        }
    }
    const bool modal = ui.settings || ui.changelog || ui.news || ui.legal.open >= 0 || ui.legal.agreement || ui.mods || ui.sign_in || qr_open || state.prompt.has_value() ||
        ui.steam_offline || state.phase == Phase::mods_broken || ui.mods_update_prompt;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    auto* draw = ImGui::GetWindowDrawList();
    if (g_background.id) draw_photo(draw, size, time);
    else draw_background(draw, size, time);
    std::string version = "Development build";
    if (update::binary_updates_enabled() && state.config && !state.config->launcher.version.empty())
        version = "Launcher " + state.config->launcher.version;
    const bool greet = !launcher.settings().offline && !ui.steam_name.empty();
    // Down the right side, over the picture, from the foot up: PLAY and MOD MANAGER; and at its
    // head, who is playing, with the settings' wheel.
    const float column = S(360), gap = S(14), margin = S(64);
    const float column_x = size.x - margin - column;
    const ImVec2 primary_size(column, S(150));
    const ImVec2 primary(column_x, size.y - S(56) - primary_size.y);
    const ImVec2 mods_size(column, S(116));
    const ImVec2 mods_position(primary.x, primary.y - gap - mods_size.y);
    const ImVec2 plate(column_x, S(52)), plate_size(column, S(58));
    page_title(draw, fonts, ImVec2(S(56), S(46)), "RESKATE");

    window_buttons(draw, window, size);

    ImGui::BeginDisabled(modal);
    const char* label = "PLAY";
    std::string detail;
    bool enabled = !launcher.busy();
    bool secondary = false;
    switch (state.phase) {
    case Phase::checking: label = "CHECKING"; enabled = false; break;
    case Phase::update_available: label = "UPDATE"; detail = "New ReSkate files are ready"; break;
    case Phase::updating: label = "UPDATING"; enabled = false; break;
    case Phase::game_missing: label = "INSTALL"; detail = "Download skate. from Steam"; break;
    case Phase::game_outdated: label = "DOWNLOAD"; detail = "Get the supported build from Steam"; break;
    case Phase::downloading:
        label = "CANCEL"; enabled = true; secondary = true;
        detail = state.progress >= 0 ? std::format("Downloading  {:.0f}%", state.progress * 100) : "Downloading";
        break;
    case Phase::merging: label = "MODS"; enabled = false; detail = "Merging your mods before Skate starts"; break;
    case Phase::mods_broken: label = "PLAY"; enabled = false; detail = "Waiting on an answer about your mods"; break;
    case Phase::ready:
        label = "PLAY";
        // Release builds install the configured runtime before PLAY appears.
        // That it is ready, and what pressing it starts: the map Skate opens in and how many mods
        // load. (The STATUS tile, which said it was ready, shows only while there is more to say.)
        {
            std::string map = "San Vansterdam";
            if (const auto& chosen = launcher.settings().start_map; !chosen.empty()) {
                const auto* known = ui.start_maps ? launcher_start_maps::find_start_map(*ui.start_maps, chosen) : nullptr;
                map = known ? known->name : std::string();
            }
            const auto mods_text = ui.mods_enabled == 1 ? std::string("1 mod") : std::format("{} mods", ui.mods_enabled);
            detail = !state.config ? std::string("Ready   /   Offline")
                   : map.empty() ? (ui.mods_enabled ? "Ready   /   " + mods_text : std::string("Ready"))
                   : ui.mods_enabled ? "Ready   /   " + map + " with " + mods_text : "Ready   /   " + map;
        }
        // Not while mods are still coming in: Skate would start on a Mods folder half written.
        if (mods_panel.installing) {
            enabled = false;
            detail = "Waiting for your mods to finish installing";
        } else if (mods_panel.removal.busy) {
            enabled = false;
            detail = "Waiting for your mods to finish uninstalling";
        }
        break;
    case Phase::launching: label = "LAUNCHING"; enabled = false; break;
    case Phase::failed: label = "RETRY"; break;
    }
    const auto act = [&] {
        switch (state.phase) {
        case Phase::update_available: launcher.apply_updates(); break;
        case Phase::game_missing:
        case Phase::game_outdated: open_sign_in(launcher, ui, false); break;
        case Phase::downloading: launcher.cancel(); break;
        case Phase::ready:
            // Mods with updates waiting: ask first, once a session. Playing on
            // the old version is a choice, not a mistake, so it stays offered.
            if (ui.mods_pending && !ui.mods_updates_ignored) ui.mods_update_prompt = true;
            else launcher.play();
            break;
        case Phase::failed: launcher.check(); break;
        default: break;
        }
    };
    const auto play_mark = state.phase == Phase::ready && ui.mods_pending
        ? (ui.mods_pending == 1 ? std::string("1 MOD UPDATE") : std::format("{} MOD UPDATES", ui.mods_pending))
        : std::string();
    if (action_tile(draw, fonts, primary, primary_size, label, detail, enabled && !modal, secondary, play_mark)) act();
    // A controller starts on PLAY.
    if (!modal) default_focus();
    if (account_plate(draw, fonts, plate, plate_size, greet ? "Welcome, " + ui.steam_name : std::string("ReSkate"), version,
            launcher.settings().offline ? ImTextureID{} : ui.steam_avatar, !modal)) ui.settings = true;
    if (time - ui.mods_checked > 5 && !ui.mods) {
        std::size_t enabled_mods = 0, installed = 0;
        const auto list = mods::scan_mods(launcher_mods::mods_root(launcher.session().paths.directory).parent_path());
        for (const auto& entry : list.entries) {
            ++installed;
            if (entry.enabled) ++enabled_mods;
        }
        // A mod the game threw out of the merge is named here, so nobody has
        // to wonder which one stopped working. (One left out for a conflict is
        // no fault of its own: the mod manager shows it with the mod it lost to.)
        std::vector<std::string> dropped;
        for (const auto& entry : list.entries) {
            const auto excluded = list.excluded.find(entry.mod.name);
            const bool conflict = excluded != list.excluded.end() && !excluded->second.empty() &&
                                  excluded->second.front().starts_with("conflicts with ");
            if (entry.enabled && ((excluded != list.excluded.end() && !conflict) || !entry.mod.outdated.empty()))
                dropped.push_back(entry.mod.title);
        }
        const auto pending = updates(mods_panel.store, installed_versions(list, true)).size();
        ui.mods_pending = pending;
        ui.mods_enabled = enabled_mods;
        ui.mods_mark_bad = !dropped.empty();
        ui.mods_mark = !dropped.empty() ? std::string("NOT LOADED")
                     : pending == 1 ? std::string("1 UPDATE")
                     : pending ? std::format("{} UPDATES", pending)
                     : std::string();
        ui.mods_detail = dropped.size() == 1 ? dropped.front() + " did not load"
                       : !dropped.empty() ? std::format("{} mods did not load", dropped.size())
                       : installed ? std::format("{} of {} enabled", enabled_mods, installed)
                       : std::string("Browse and install from Thunderstore");
        ui.mods_checked = time;
    }
    if (mods_tile(draw, fonts, mods_position, mods_size, !modal, ui.mods_detail, ui.mods_mark, ui.mods_mark_bad)) {
        ui.mods = true;
        mods_panel.scanned = false;
    }

    // Level with PLAY's bottom edge, in the opposite corner, and as wide as the room up to the column allows.
    const float left = S(60), left_width = std::min(S(880), column_x - left - S(48));
    const ImVec2 changelog_size(left_width, S(104));
    const ImVec2 changelog_position(left, size.y - S(56) - changelog_size.y);
    // While ReSkate is at work, how far it is takes WHAT'S NEW's place; and so does what needs
    // the player or went wrong, while anything does.
    if (working(state.phase)) progress_tile(draw, fonts, state, changelog_position, changelog_size, time);
    else if (state.phase != Phase::ready)
        state_tile(draw, fonts, state, changelog_position, changelog_size, time, launcher.session().paths.logs, modal);
    else if (changelog_tile(draw, fonts, changelog_position, changelog_size, !modal, ui.notes)) ui.changelog = true;
    // Over it, the announcement, as a line.
    const ImVec2 banner_size(left_width, S(46));
    announcement_banner(fonts, ui, ImVec2(left, changelog_position.y - S(12) - banner_size.y), banner_size, !modal);
    // An important one opens itself the first time it is here, so it is seen once; a line after that.
    if (const auto* message = announcement_to_show(ui);
        message && message->important && message->version > launcher.settings().announcement_opened && !modal) {
        launcher.settings().announcement_opened = message->version;
        launcher.save();
        ui.news = true;
    }

    if (enabled && !secondary && !modal &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) act();
    ImGui::EndDisabled();

    if (modal) draw->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 9, 0.72f));
    g_drag_allowed = !modal && !ImGui::IsAnyItemHovered();
    ImGui::End();

    // An update choice flipped while a check was already running is checked as soon as it ends.
    launcher.check_again_if_update_choices_changed();
    if (state.prompt) prompt_window(launcher, fonts, size, *state.prompt, ui);
    else if (qr_open) qr_window(launcher, fonts, size, state.qr);
    else if (ui.sign_in) sign_in_window(launcher, fonts, size, ui);
    // Opened from Settings or the agreement, and back to it when closed.
    else if (ui.legal.open >= 0) legal_window(fonts, size, ui);
    else if (ui.legal.agreement) legal_agreement_window(launcher, fonts, size, ui, window);
    else if (ui.settings) settings_window(launcher, fonts, size, ui, window);
    else if (ui.news) announcement_window(fonts, size, ui);
    else if (ui.changelog)
        changelog_window(fonts, size, ui, update::binary_updates_enabled() && state.config ? state.config->runtime.version : std::string());
    else if (ui.mods) mods_window(launcher, fonts, size, ui, mods_panel, window);
    else if (state.phase == Phase::mods_broken)
        mods_broken_window(launcher, fonts, size, ui, mods_panel, state.mod_problems);
    else if (ui.mods_update_prompt) mods_outdated_window(launcher, fonts, size, ui, mods_panel);
    else if (ui.steam_offline) steam_offline_window(fonts, size, ui);
}

} // namespace dingosdk::launcher_gui::detail
