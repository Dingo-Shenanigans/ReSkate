#include "gui_internal.h"

#include "mod_manager.h"
#include "problem.h"

#include <cmath>
#include <format>

// The main screen: background, name plate, tiles and the STATUS tile.
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
void progress_bar(ImDrawList* draw, ImVec2 a, ImVec2 b, float fill, float time) {
    skate_theme::striped_bar(draw, a, b, fill, time, g_scale);
}

// Name plate in the top-left corner, like the HUB's player card.
void draw_plate(ImDrawList* draw, const Fonts& fonts, ImVec2 position, const std::string& title, const std::string& version) {
    const float h = S(46);
    const float text = std::max(fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, title.c_str()).x,
                                fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, version.c_str()).x);
    const float width = std::max(S(300), h + S(28) + text);
    draw->AddRectFilled(ImVec2(position.x + h, position.y + S(3)), ImVec2(position.x + width, position.y + h - S(3)),
        rgba(44, 47, 49, 0.94f));
    draw->AddRectFilled(position, ImVec2(position.x + h, position.y + h), color::avatar);
    // A small deck on the avatar tile.
    const ImVec2 c(position.x + h * 0.5f, position.y + h * 0.5f);
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(ImVec2(c.x - S(15), c.y - S(5.5f)), ImVec2(c.x + S(15), c.y + S(5.5f)), color::text, S(5.5f));
    for (const float x : {-9.0f, 9.0f}) draw->AddCircleFilled(ImVec2(c.x + S(x), c.y), S(1.8f), color::avatar);
    rotate_since(draw, start, -40.0f, c);
    draw->AddCircleFilled(ImVec2(position.x + S(3), position.y + S(3)), S(5), color::good);
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(position.x + h + S(12), position.y + S(5)), color::text, title.c_str());
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(position.x + h + S(12), position.y + S(24)), color::muted, version.c_str());
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

bool settings_tile(ImDrawList* draw, const Fonts& fonts, ImVec2 position, ImVec2 size, bool enabled) {
    bool hovered{};
    const bool pressed = tile_hit("##settings", position, size, enabled, hovered);
    const ImVec2 end(position.x + size.x, position.y + size.y);
    rough_rect(draw, position, end, color::tile, 3);
    if (hovered) draw->AddRect(ImVec2(position.x - S(3), position.y - S(3)), ImVec2(end.x + S(3), end.y + S(3)), color::text, 0, 0, S(3));
    draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(position.x + S(18), position.y + (size.y - fonts.tile->FontSize) * 0.5f),
        color::text, "SETTINGS");
    // Large faded wheel in the corner, like the icons on the HUB's tiles.
    const ImVec2 centre(end.x - S(44), position.y + size.y * 0.5f);
    if (tile_icon(draw, g_icon_settings, centre, S(58), hovered)) return pressed;
    const ImU32 ink = rgba(255, 255, 255, hovered ? 0.55f : 0.3f);
    for (int tooth = 0; tooth < 8; ++tooth) {
        const float angle = static_cast<float>(tooth) * 0.7853982f;
        draw->AddLine(ImVec2(centre.x + std::cos(angle) * S(14), centre.y + std::sin(angle) * S(14)),
                      ImVec2(centre.x + std::cos(angle) * S(22), centre.y + std::sin(angle) * S(22)), ink, S(6));
    }
    draw->AddCircle(centre, S(15), ink, 0, S(4));
    draw->AddCircle(centre, S(6), ink, 0, S(3));
    return pressed;
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

// The STATUS tile: what ReSkate is doing, or what went wrong and what to do
// about it. Laid out like the HUB's BOUNTIES tile, with the state in its edge.
void status_tile(const Fonts& fonts, const State& state, ImVec2 position, float width, float time,
                 const fs::path& logs, bool modal) {
    auto* draw = ImGui::GetWindowDrawList();
    const bool failed = state.phase == Phase::failed;
    const auto problem = failed ? launcher_problem::explain(state.status) : launcher_problem::Problem{};
    const std::string headline = failed ? problem.headline : state.status;
    const std::string detail = failed ? problem.advice : state.detail;

    Icon icon = Icon::busy;
    ImU32 accent = color::blue;
    const char* pill = "WORKING";
    switch (state.phase) {
    case Phase::ready: icon = Icon::check; accent = color::good; pill = "READY"; break;
    case Phase::failed: icon = Icon::fail; accent = color::danger; pill = "PROBLEM"; break;
    case Phase::update_available:
    case Phase::game_missing:
    case Phase::game_outdated:
    case Phase::mods_broken: icon = Icon::warning; accent = color::warning; pill = "ACTION NEEDED"; break;
    default: break;
    }

    // The two things worth knowing at a glance, under the rule.
    struct Fact { Icon icon; std::string text; };
    std::vector<Fact> facts;
    if (state.config) {
        const auto build = state.config->game.build_id.empty() ? state.config->game.manifest_id
                                                               : state.config->game.build_id;
        const bool game_ok = state.phase != Phase::game_missing && state.phase != Phase::game_outdated &&
                             state.phase != Phase::downloading;
        facts.push_back({game_ok ? Icon::check : Icon::warning, "skate. build " + build});
        if (update::binary_updates_enabled())
            facts.push_back({state.phase == Phase::update_available ? Icon::warning : Icon::check,
                "ReSkate " + state.config->runtime.version});
    }

    const float pad = S(20), gutter = S(28);
    const float text_x = position.x + pad + gutter;
    const float text_width = width - pad * 2 - gutter;
    const auto headline_size = fonts.heading->CalcTextSizeA(fonts.heading->FontSize, FLT_MAX, text_width, headline.c_str());
    const auto detail_size = detail.empty() ? ImVec2(0, 0)
        : fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, text_width, detail.c_str());

    float height = S(64) + headline_size.y;
    if (!detail.empty()) height += S(8) + detail_size.y;
    if (failed) height += S(12) + S(30);
    if (state.progress >= 0) height += S(22);
    if (!facts.empty()) height += S(18) + static_cast<float>(facts.size()) * (fonts.body->FontSize + S(12));
    height += facts.empty() ? S(20) : S(8);

    const ImVec2 end(position.x + width, position.y + height);
    rough_rect(draw, position, end, color::tile, 5);
    // The state is readable before a word of it is: a coloured edge and a pill.
    draw->AddRectFilled(position, ImVec2(position.x + S(4), end.y), accent);
    draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(position.x + pad, position.y + S(14)), color::text, "STATUS");
    badge(draw, fonts, ImVec2(end.x - pad - badge_width(fonts, pill),
        position.y + S(14) + (fonts.tile->FontSize - fonts.caption->FontSize - S(8)) * 0.5f), pill, accent, color::ink);

    float y = position.y + S(64);
    skate_theme::status_icon(draw, ImVec2(position.x + pad + S(10), y + fonts.heading->FontSize * 0.5f), icon, time, g_scale);
    draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(text_x, y), failed ? color::danger : color::text,
        headline.c_str(), nullptr, text_width);
    y += headline_size.y;
    if (!detail.empty()) {
        y += S(8);
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, y), color::muted, detail.c_str(), nullptr,
            text_width);
        y += detail_size.y;
    }
    if (failed) {
        y += S(12);
        ImGui::BeginDisabled(modal);
        ImGui::SetCursorScreenPos(ImVec2(text_x, y));
        if (ImGui::Button("Open logs", ImVec2(S(104), S(30)))) open_path(logs);
        ImGui::SameLine(0, S(8));
        if (ImGui::Button("Copy details", ImVec2(S(116), S(30)))) {
            // The raw message, not the friendly one: this is for the Discord.
            std::string report = "ReSkate launcher\n" + state.status + "\n";
            for (const auto& fact : facts) report += fact.text + "\n";
            ImGui::SetClipboardText(report.c_str());
        }
        ImGui::EndDisabled();
        y += S(30);
    }
    if (state.progress >= 0) {
        progress_bar(draw, ImVec2(position.x + pad, y + S(6)), ImVec2(end.x - pad, y + S(18)), state.progress, time);
        y += S(22);
    }
    if (!facts.empty()) {
        y += S(10);
        draw->AddLine(ImVec2(position.x + pad, y), ImVec2(end.x - pad, y), rgba(255, 255, 255, 0.09f), S(1));
        y += S(8);
        for (const auto& fact : facts) {
            skate_theme::status_icon(draw, ImVec2(position.x + pad + S(10), y + fonts.body->FontSize * 0.5f),
                fact.icon, time, g_scale);
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, y), color::muted, fact.text.c_str());
            y += fonts.body->FontSize + S(12);
        }
    }
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
    // A mod dropped on the window opens the Mods panel and installs it.
    {
        std::lock_guard lock(g_dropped_mutex);
        if (!g_dropped.empty() && !ui.settings && !ui.sign_in && !qr_open && !state.prompt && !mods_panel.installing) {
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
        ui.steam_checked = time;
        if (!ui.steam_offline_seen) {
            ui.steam_offline_seen = true;
            ui.steam_offline = !launcher.settings().offline && !launcher_app::steam_signed_in();
        }
    }
    const bool modal = ui.settings || ui.mods || ui.sign_in || qr_open || state.prompt.has_value() ||
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
    draw_plate(draw, fonts, ImVec2(S(50), S(40)), greet ? "Welcome, " + ui.steam_name : std::string("ReSkate"), version);
    page_title(draw, fonts, ImVec2(S(52), S(88)), "RESKATE");

    window_buttons(draw, window, size);

    ImGui::BeginDisabled(modal);
    const float column = S(360);
    const ImVec2 primary_size(column, S(160));
    const ImVec2 primary(size.x - S(64) - column, size.y - S(56) - primary_size.y);
    // MODS sits directly under PLAY, where the eye already is, and is taller
    // than SETTINGS so it reads as a place to go rather than a toggle.
    const ImVec2 mods_size(column, S(116));
    const ImVec2 mods_position(primary.x, primary.y - S(14) - mods_size.y);
    const ImVec2 settings(primary.x, mods_position.y - S(14) - S(84));
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
        detail = !state.config ? std::string("Offline")
               : !update::binary_updates_enabled() ? std::string("ReSkate development build")
               : state.config->runtime.version.empty() ? std::string("ReSkate")
               : "ReSkate " + state.config->runtime.version;
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
    if (settings_tile(draw, fonts, settings, ImVec2(column, S(84)), !modal)) ui.settings = true;
    if (time - ui.mods_checked > 5 && !ui.mods) {
        std::size_t enabled_mods = 0, installed = 0;
        const auto list = mods::scan_mods(launcher_mods::mods_root(launcher.session().paths.directory).parent_path());
        for (const auto& entry : list.entries) {
            ++installed;
            if (entry.enabled) ++enabled_mods;
        }
        // A mod the game threw out of the merge is named here, so nobody has
        // to wonder which one stopped working.
        std::vector<std::string> dropped;
        for (const auto& entry : list.entries)
            if (entry.enabled && (list.excluded.contains(entry.mod.name) || !entry.mod.outdated.empty()))
                dropped.push_back(entry.mod.title);
        const auto pending = updates(mods_panel.store, installed_versions(list, true)).size();
        ui.mods_pending = pending;
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

    if (enabled && !secondary && !modal &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) act();
    ImGui::EndDisabled();

    status_tile(fonts, state, ImVec2(S(60), S(200)), S(430), time, launcher.session().paths.logs, modal);

    if (modal) draw->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 9, 0.72f));
    g_drag_allowed = !modal && !ImGui::IsAnyItemHovered();
    ImGui::End();

    if (state.prompt) prompt_window(launcher, fonts, size, *state.prompt, ui);
    else if (qr_open) qr_window(launcher, fonts, size, state.qr);
    else if (ui.sign_in) sign_in_window(launcher, fonts, size, ui);
    else if (ui.settings) settings_window(launcher, fonts, size, ui, window);
    else if (ui.mods) mods_window(launcher, fonts, size, ui, mods_panel, window);
    else if (state.phase == Phase::mods_broken)
        mods_broken_window(launcher, fonts, size, ui, mods_panel, state.mod_problems);
    else if (ui.mods_update_prompt) mods_outdated_window(launcher, fonts, size, ui, mods_panel);
    else if (ui.steam_offline) steam_offline_window(fonts, size, ui);
}

} // namespace dingosdk::launcher_gui::detail
