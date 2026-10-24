#include "overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include <cmath>
#include <map>

// ReSkate's S.K.A.T.E. throwdown HUD, in place of the game's debug text. The game side reads
// what that debug HUD lays out each frame (its messages and every player's letters) and
// hands it over here, where it is drawn in skate.'s menu style: the messages on a plate at
// the top of the screen, and a scoreboard with each player's S K A T E tiles on the left,
// the tricks already set this game under it.
// Background draw list, under ReSkate's own menus and chat; it takes no input.

namespace dingosdk::overlay {
namespace {
std::atomic<SkateHudFeed> skate_hud_feed{};
}
void set_skate_hud_feed(SkateHudFeed feed) noexcept { skate_hud_feed.store(feed); }
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
namespace theme = dingosdk::skate_theme;
using Clock = std::chrono::steady_clock;
constexpr std::string_view skate_word = "SKATE";

struct HudState {
    SkateHud hud;
    // When the top message last changed (it slides in), and each player's letters with
    // when the last one was earned (that tile flashes), by name.
    std::string headline;
    Clock::time_point headline_at{};
    std::map<std::string, std::pair<int, Clock::time_point>, std::less<>> letters;
};
HudState &hud_state() {
    static HudState value;
    return value;
}

ImU32 with_alpha(ImU32 colour, float alpha) {
    const auto a = static_cast<unsigned>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
float seconds_since(Clock::time_point at) { return std::chrono::duration<float>(Clock::now() - at).count(); }
float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
ImU32 message_colour(SkateHudMessage::Kind kind) {
    switch (kind) {
    case SkateHudMessage::Kind::trick: return theme::white;
    case SkateHudMessage::Kind::success: return theme::good;
    case SkateHudMessage::Kind::failure: return theme::danger;
    default: return theme::bar;
    }
}
std::string upper(std::string text) {
    for (auto &c : text)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}
// `text` cut with "..." to fit `width`, on a UTF-8 character boundary.
std::string fit(ImFont *font, float size, std::string text, float width) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= width) return text;
    while (!text.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.0f, (text + "...").c_str()).x > width) {
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty()) text.pop_back();
    }
    return text + "...";
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    const float offset = std::max(1.0f, size / 16.0f);
    const auto alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), with_alpha(IM_COL32(0, 0, 0, 255), alpha * 0.7f),
                  text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}

// The messages, centred on a plate at the top: the first one large (it slides in when it
// changes), a trick to copy in white, the rest smaller. Sizes stay near the fonts' baked
// sizes (title 44, heading 22, bold 17) so the text stays sharp.
void draw_messages(ImDrawList *draw, const SkateHud &hud, float scale, float alpha_in, float slide, ImU32 accent) {
    auto &s = state();
    if (hud.messages.empty()) return;
    auto *title = s.menu.title ? s.menu.title : ImGui::GetFont();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    struct Line {
        ImFont *font;
        float size;
        ImU32 colour;
        std::string text;
        ImVec2 extent;
    };
    std::vector<Line> lines;
    const float max_width = ImGui::GetIO().DisplaySize.x * 0.7f;
    for (std::size_t i = 0; i < hud.messages.size() && i < 6; ++i) {
        const auto &message = hud.messages[i];
        Line line{};
        if (i == 0 && message.kind != SkateHudMessage::Kind::trick) {
            line = {title, 40.0f * scale, message.kind == SkateHudMessage::Kind::prompt ? accent : message_colour(message.kind), upper(message.text), {}};
        } else if (message.kind == SkateHudMessage::Kind::trick) {
            line = {heading, 26.0f * scale, theme::white, message.text, {}};
        } else {
            line = {bold, 20.0f * scale, message_colour(message.kind), message.text, {}};
        }
        line.text = fit(line.font, line.size, line.text, max_width);
        line.extent = line.font->CalcTextSizeA(line.size, FLT_MAX, 0.0f, line.text.c_str());
        lines.push_back(std::move(line));
    }
    const float pad_x = 26.0f * scale, pad_y = 12.0f * scale, gap = 4.0f * scale;
    float width{}, height{};
    for (const auto &line : lines) {
        width = std::max(width, line.extent.x);
        height += line.extent.y;
    }
    height += gap * static_cast<float>(lines.size() - 1);
    const float centre = ImGui::GetIO().DisplaySize.x * 0.5f;
    const ImVec2 min(centre - width * 0.5f - pad_x, 34.0f * scale + slide);
    const ImVec2 max(centre + width * 0.5f + pad_x, min.y + height + pad_y * 2.0f);
    theme::rough_rect(draw, min, max, with_alpha(theme::tile, 0.9f * alpha_in), 71u, scale);
    // The plate's stripe takes the first message's colour: orange to act, green or red after.
    const auto kind = hud.messages.front().kind;
    const auto stripe = kind == SkateHudMessage::Kind::trick || kind == SkateHudMessage::Kind::prompt ? accent : message_colour(kind);
    draw->AddRectFilled(ImVec2(min.x, max.y - 4.0f * scale), ImVec2(max.x, max.y), with_alpha(stripe, alpha_in));
    float y = min.y + pad_y;
    for (const auto &line : lines) {
        shadowed(draw, line.font, line.size, ImVec2(centre - line.extent.x * 0.5f, y), with_alpha(line.colour, alpha_in),
                 line.text);
        y += line.extent.y + gap;
    }
}

// Every player's row on the left: whose turn it is marked in blue, eliminated players
// dimmed, and five S K A T E tiles with the earned ones in red. Returns the panel's bottom
// and width.
std::pair<float, float> draw_scoreboard(ImDrawList *draw, HudState &h, float scale, std::string_view title, std::string_view penalties, bool lower_left = false) {
    auto &s = state();
    const auto &players = h.hud.players;
    if (players.empty()) return {120.0f * scale, 0.0f};
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const float name_size = 19.0f * scale, tile = 28.0f * scale, tile_gap = 4.0f * scale;
    const float row = 42.0f * scale, row_gap = 6.0f * scale, pad = 12.0f * scale;
    float name_width = 90.0f * scale;
    for (const auto &player : players)
        name_width = std::max(name_width, bold->CalcTextSizeA(name_size, FLT_MAX, 0.0f, player.name.c_str()).x);
    name_width = std::min(name_width, 240.0f * scale);
    const float tiles_width = tile * static_cast<float>(penalties.size()) + tile_gap * static_cast<float>(penalties.size() - 1);
    const float width = pad * 2.0f + 14.0f * scale + name_width + 14.0f * scale + tiles_width;
    const float left = (lower_left ? 64.0f : 32.0f) * scale;
    // Reserve the trick history as well as the line score and multiplier.
    float top = lower_left ? ImGui::GetIO().DisplaySize.y - 430.0f * scale -
        26.0f * scale - static_cast<float>(players.size()) * (row + row_gap) : 120.0f * scale;
    shadowed(draw, heading, 18.0f * scale, ImVec2(left + 2.0f * scale, top), theme::grey_text, std::string(title));
    top += 26.0f * scale;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto &player = players[i];
        const float alpha = player.out ? 0.5f : 1.0f;
        const ImVec2 min(left, top + static_cast<float>(i) * (row + row_gap)), max(left + width, min.y + row);
        theme::rough_rect(draw, min, max, with_alpha(player.up ? theme::tile_light : theme::tile, 0.9f * alpha),
                          static_cast<unsigned>(i * 13 + 5), scale);
        if (player.up) {
            draw->AddRectFilled(min, ImVec2(min.x + 5.0f * scale, max.y), theme::blue);
            const float mid = (min.y + max.y) * 0.5f, x = min.x + pad;
            draw->AddTriangleFilled(ImVec2(x, mid - 6.0f * scale), ImVec2(x + 9.0f * scale, mid),
                                    ImVec2(x, mid + 6.0f * scale), theme::blue);
        }
        const auto name = fit(bold, name_size, player.name.empty() ? "Player " + std::to_string(i + 1) : player.name,
                              name_width);
        const float name_x = min.x + pad + 14.0f * scale;
        const float text_y = (min.y + max.y) * 0.5f - bold->CalcTextSizeA(name_size, FLT_MAX, 0.0f, "Ag").y * 0.5f;
        shadowed(draw, bold, name_size, ImVec2(name_x, text_y),
                 with_alpha(player.out ? theme::grey_text : theme::white, alpha), name);
        if (player.out) {
            const float strike = (min.y + max.y) * 0.5f;
            const float end = name_x + bold->CalcTextSizeA(name_size, FLT_MAX, 0.0f, name.c_str()).x;
            draw->AddLine(ImVec2(name_x - 2.0f * scale, strike), ImVec2(end + 2.0f * scale, strike),
                          with_alpha(theme::danger, 0.9f), std::max(1.0f, 2.0f * scale));
        }
        // A letter earned since the last frame flashes and pops in.
        const auto key = player.name.empty() ? "#" + std::to_string(i) : player.name;
        auto &seen = h.letters[key];
        if (player.letters > seen.first) seen.second = Clock::now();
        seen.first = player.letters;
        const float flash = 1.0f - ease_out(seconds_since(seen.second) / 0.7f);
        float x = name_x + name_width + 14.0f * scale;
        const float tile_top = (min.y + max.y) * 0.5f - tile * 0.5f;
        for (int letter = 0; letter < static_cast<int>(penalties.size()); ++letter, x += tile + tile_gap) {
            const bool earned = letter < player.letters;
            const bool newest = earned && letter == player.letters - 1 && flash > 0.0f;
            const float grow = newest ? tile * 0.18f * flash : 0.0f;
            const ImVec2 a(x - grow, tile_top - grow), b(x + tile + grow, tile_top + tile + grow);
            if (lower_left) {
                // Three separate torn-paper stamps, in the mode's blue palette.
                // Keep unearned letters legible without looking like penalties.
                theme::rough_rect(draw, ImVec2(a.x + 2 * scale, a.y + 2 * scale),
                    ImVec2(b.x + 2 * scale, b.y + 2 * scale), with_alpha(theme::tile, alpha), 33u + letter, scale);
                theme::rough_rect(draw, a, b, with_alpha(earned ? theme::blue : theme::white,
                    earned ? alpha : .24f * alpha), 17u + letter * 7u, scale);
            } else draw->AddRectFilled(a, b, with_alpha(earned ? theme::danger : theme::tile_grey, alpha), 3.0f * scale);
            if (newest)
                draw->AddRectFilled(a, b, with_alpha(theme::white, 0.6f * flash), 3.0f * scale);
            const char text[2]{penalties[static_cast<std::size_t>(letter)], 0};
            const float letter_size = 19.0f * scale;
            const auto extent = heading->CalcTextSizeA(letter_size, FLT_MAX, 0.0f, text);
            draw->AddText(heading, letter_size,
                          ImVec2((a.x + b.x - extent.x) * 0.5f, (a.y + b.y - extent.y) * 0.5f),
                          with_alpha(lower_left ? (earned ? theme::tile : theme::white) :
                              (earned ? theme::white : theme::grey_text), earned ? alpha : 0.65f * alpha), text);
        }
    }
    return {top + static_cast<float>(players.size()) * (row + row_gap), width};
}

// The tricks already set this game, under the scoreboard: none of them can be set again.
void draw_done_tricks(ImDrawList *draw, const SkateHud &hud, float scale, float top, float min_width) {
    auto &s = state();
    if (hud.done_tricks.empty()) return;
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    constexpr std::size_t shown = 8;
    const float left = 32.0f * scale, pad = 10.0f * scale, size = 16.0f * scale, line = size + 5.0f * scale;
    top += 10.0f * scale;
    shadowed(draw, heading, 16.0f * scale, ImVec2(left + 2.0f * scale, top), theme::grey_text,
             "TRICKS DONE (" + std::to_string(hud.done_tricks.size()) + ")");
    top += 22.0f * scale;
    // The newest at the top; older ones beyond the first few are counted.
    const auto count = std::min(hud.done_tricks.size(), shown);
    const bool more = hud.done_tricks.size() > shown;
    const float width = std::max(min_width, 260.0f * scale);
    const float height = pad * 2.0f + line * static_cast<float>(count + (more ? 1 : 0)) - 5.0f * scale;
    theme::rough_rect(draw, ImVec2(left, top), ImVec2(left + width, top + height), with_alpha(theme::tile, 0.85f), 29u,
                      scale);
    float y = top + pad;
    for (std::size_t i = 0; i < count; ++i, y += line) {
        const auto &trick = hud.done_tricks[hud.done_tricks.size() - 1 - i];
        shadowed(draw, body, size, ImVec2(left + pad, y), i == 0 ? theme::white : with_alpha(theme::white, 0.75f),
                 fit(body, size, trick, width - pad * 2.0f));
    }
    if (more)
        shadowed(draw, body, size, ImVec2(left + pad, y), theme::grey_text,
                 "+" + std::to_string(hud.done_tricks.size() - shown) + " earlier");
}
} // namespace

bool skate_hud_pending() {
    auto &h = hud_state();
    h.hud = {};
    if (const auto feed = skate_hud_feed.load()) {
        try { h.hud = feed(); } catch (...) { h.hud = {}; }
    }
    if (h.hud.messages.empty() && h.hud.players.empty()) {
        // The throwdown is over: the next one starts fresh.
        h.headline.clear();
        h.letters.clear();
        return false;
    }
    return true;
}

void draw_throwdown_hud(HudState &h, std::string_view title, std::string_view penalties, ImU32 accent) {
    if (h.hud.messages.empty() && h.hud.players.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.8f, 2.0f);
    auto *draw = ImGui::GetBackgroundDrawList();
    const auto headline = h.hud.messages.empty() ? std::string{} : h.hud.messages.front().text;
    if (headline != h.headline) {
        h.headline = headline;
        h.headline_at = Clock::now();
    }
    const float shown = ease_out(seconds_since(h.headline_at) / 0.2f);
    draw_messages(draw, h.hud, scale, shown, (1.0f - shown) * -12.0f * scale, accent);
    const auto [bottom, width] = draw_scoreboard(draw, h, scale, title, penalties);
    draw_done_tricks(draw, h.hud, scale, bottom, width);
}
void draw_skate_hud() { draw_throwdown_hud(hud_state(), "S.K.A.T.E.", skate_word, theme::bar); }
} // namespace dingosdk::overlay::detail
