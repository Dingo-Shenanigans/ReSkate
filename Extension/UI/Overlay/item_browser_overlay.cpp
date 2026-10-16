#include "overlay_internal.h"
#include "input_capture.h"
#include "tab_icons.h"
#include <optional>
#include <imgui_internal.h>

// The skater item grids' search bar and key prompts, drawn over the game's own menu in its style.
// The grids themselves are the game's (Extension/Customization/item_browser.cpp); this only shows
// what is going on and takes the typing.
namespace dingosdk::overlay {
namespace {
std::atomic<const ItemBrowserHost*> item_browser_host{};
}
void set_item_browser(const ItemBrowserHost* host) noexcept { item_browser_host.store(host); }
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
namespace {
using Clock = std::chrono::steady_clock;
struct Panel {
    ItemBrowserView view;
    Clock::time_point next_poll{};
    std::array<char, 96> input{};
    bool stick_down{};
    std::string pushed; // the search text the host was last given
    // The tabs' panel.
    int shown_panel{};        // the mode it was last drawn in (0 when closed)
    int cursor{};             // the tile the arrow keys are on
    int edit{-1};             // the tab being changed
    std::array<char, 48> name{};
    bool name_focus{}, delete_armed{};
    // The controller: its buttons last frame and those pressed this one, whether it was the last thing
    // used (its buttons are shown in the prompts then), its kind, and the View button's hold.
    unsigned pad_last{}, pad_pressed{};
    bool pad_mode{}, pad_connected{};
    std::atomic<bool> keyboard_used{};
    ControllerStyle pad_style{};
    Clock::time_point view_down{};
    bool view_held{}, view_long{};
    int icon_cursor{-1};    // in the tab editor, the icon the D-pad is on (-1: on the tabs)
    std::size_t name_hint{}; // the next name Y gives a tab
    Clock::time_point r3_down{};
    bool r3_long{};
};
Panel& panel() { static auto* value = new Panel; return *value; }
const ItemBrowserHost* host() { return item_browser_host.load(); }

// A five-pointed star, the mark favorites carry on their tiles.
void star(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour) {
    std::array<ImVec2, 10> points;
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + static_cast<float>(i) * 0.6283185f, r = i % 2 ? radius * 0.42f : radius;
        points[static_cast<std::size_t>(i)] = ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r);
    }
    // Concave: fan it from the centre.
    for (int i = 0; i < 10; ++i)
        draw->AddTriangleFilled(centre, points[static_cast<std::size_t>(i)], points[static_cast<std::size_t>((i + 1) % 10)], colour);
}

// A controller button as the game draws its prompts: white, the button's mark in black, then what it
// does. Like theme::keycap, 20 high at the given scale; returns the width drawn.
enum class Pad { a, b, x, y, lb, rb, ls, rs, view };
float pad_glyph(ImDrawList* draw, ImFont* font, float scale, ImVec2 at, Pad button, ControllerStyle style, const char* label) {
    constexpr ImU32 white = IM_COL32(255, 255, 255, 255), ink = IM_COL32(18, 18, 18, 255);
    if (!draw) { // only its width
        const bool ps = style != ControllerStyle::xbox;
        if (button == Pad::lb || button == Pad::rb) return 30 * scale;
        if (button == Pad::view)
            return ps ? font->CalcTextSizeA(11 * scale, FLT_MAX, 0, style == ControllerStyle::dualsense ? "CREATE" : "SHARE").x + 12 * scale
                      : 24 * scale;
        return 20 * scale;
    }
    const bool ps = style != ControllerStyle::xbox;
    const float h = 20 * scale, r = h * 0.5f;
    const ImVec2 c(at.x + r, at.y + r);
    float w = h;
    const auto text_in = [&](const char* text, float left, float width) {
        const auto size = font->CalcTextSizeA(11 * scale, FLT_MAX, 0, text);
        draw->AddText(font, 11 * scale, ImVec2(left + (width - size.x) * 0.5f, at.y + (h - size.y) * 0.5f), ink, text);
    };
    switch (button) {
    case Pad::a: case Pad::b: case Pad::x: case Pad::y:
        draw->AddCircleFilled(c, r, white, 24);
        if (!ps) {
            const char* letter = button == Pad::a ? "A" : button == Pad::b ? "B" : button == Pad::x ? "X" : "Y";
            text_in(letter, at.x, h);
        } else if (button == Pad::a) { // cross
            const float d = r * 0.42f;
            draw->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), ink, 1.8f * scale);
            draw->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), ink, 1.8f * scale);
        } else if (button == Pad::b) { // circle
            draw->AddCircle(c, r * 0.48f, ink, 20, 1.7f * scale);
        } else if (button == Pad::x) { // square
            const float d = r * 0.42f;
            draw->AddRect(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), ink, 0, 0, 1.7f * scale);
        } else { // triangle
            const float d = r * 0.5f;
            draw->AddTriangle(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y + d * 0.75f), ImVec2(c.x - d, c.y + d * 0.75f), ink, 1.7f * scale);
        }
        break;
    case Pad::lb: case Pad::rb: {
        const char* text = button == Pad::lb ? (ps ? "L1" : "LB") : (ps ? "R1" : "RB");
        w = 30 * scale;
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), white, 6 * scale);
        text_in(text, at.x, w);
        break;
    }
    case Pad::ls: case Pad::rs: {
        // A stick pressed in: the stick's letter on its cap, a dot under it for the click.
        draw->AddCircleFilled(c, r, white, 24);
        const char* letter = button == Pad::ls ? "L" : "R";
        const auto size = font->CalcTextSizeA(12 * scale, FLT_MAX, 0, letter);
        draw->AddText(font, 12 * scale, ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f - 1.5f * scale), ink, letter);
        draw->AddCircleFilled(ImVec2(c.x, c.y + r * 0.62f), 1.6f * scale, ink, 8);
        break;
    }
    case Pad::view:
        if (ps) {
            const char* text = style == ControllerStyle::dualsense ? "CREATE" : "SHARE";
            w = font->CalcTextSizeA(11 * scale, FLT_MAX, 0, text).x + 12 * scale;
            draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), white, 6 * scale);
            text_in(text, at.x, w);
        } else { // the View button: two overlapping windows
            w = 24 * scale;
            draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), white, 6 * scale);
            const ImVec2 o(at.x + 6 * scale, at.y + 5 * scale);
            draw->AddRect(ImVec2(o.x + 3 * scale, o.y), ImVec2(o.x + 12 * scale, o.y + 7 * scale), ink, 0, 0, 1.3f * scale);
            draw->AddRectFilled(ImVec2(o.x, o.y + 3 * scale), ImVec2(o.x + 9 * scale, o.y + 10 * scale), white);
            draw->AddRect(ImVec2(o.x, o.y + 3 * scale), ImVec2(o.x + 9 * scale, o.y + 10 * scale), ink, 0, 0, 1.3f * scale);
        }
        break;
    }
    if (!label || !*label) return w;
    const auto text = font->CalcTextSizeA(14 * scale, FLT_MAX, 0, label);
    draw->AddText(font, 14 * scale, ImVec2(at.x + w + 8 * scale, at.y + 2 * scale), white, label);
    return w + 8 * scale + text.x;
}

// One tab as the game draws its own: a dark tile with a white icon, or a light one with a blue icon
// for the one that is open.
void tab_tile(ImDrawList* draw, ImVec2 centre, float size, std::string_view icon, bool open, float scale) {
    const ImVec2 a(centre.x - size * 0.5f, centre.y - size * 0.5f), b(centre.x + size * 0.5f, centre.y + size * 0.5f);
    skate_theme::rough_rect(draw, a, b, open ? IM_COL32(226, 226, 226, 255) : IM_COL32(22, 22, 22, 245), 31, scale);
    TabIcon image;
    const float inset = size * 0.2f;
    if (tab_icon(icon, image) || tab_icon("folder", image))
        draw->AddImage(image.texture, ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), image.uv0, image.uv1,
                       open ? theme::blue : IM_COL32(255, 255, 255, 255));
}
// The "+" that makes a tab.
void plus_tile(ImDrawList* draw, ImVec2 centre, float size, float scale) {
    const ImVec2 a(centre.x - size * 0.5f, centre.y - size * 0.5f), b(centre.x + size * 0.5f, centre.y + size * 0.5f);
    skate_theme::rough_rect(draw, a, b, IM_COL32(22, 22, 22, 245), 37, scale);
    const float arm = size * 0.26f, thick = size * 0.07f;
    draw->AddRectFilled(ImVec2(centre.x - arm, centre.y - thick), ImVec2(centre.x + arm, centre.y + thick), IM_COL32(255, 255, 255, 255));
    draw->AddRectFilled(ImVec2(centre.x - thick, centre.y - arm), ImVec2(centre.x + thick, centre.y + arm), IM_COL32(255, 255, 255, 255));
}

// Where the game draws its tab row (measured on its 1440-line layout, which scales with the height):
// the row of tiles, its Q and E keys and the title under the open tab.
struct Row {
    float k{};
    float left{}, top{}, bottom{}, first{}, step{}, y{};
};
Row tab_row() {
    Row r;
    r.k = ImGui::GetIO().DisplaySize.y / 1440.0f;
    r.left = 268 * r.k;
    r.top = 226 * r.k;
    r.bottom = 404 * r.k;
    r.first = 412 * r.k;
    r.step = 120 * r.k;
    r.y = 292 * r.k;
    return r;
}

// The player's own tabs, drawn where the game's are and over them, the way the game draws its own;
// the game's row and title go under a panel of the menu's colour. Returns where the row ends.
float draw_custom_row(ImDrawList* draw, ImFont* heading, ImFont* body, const ItemBrowserView& view, float scale) {
    const auto r = tab_row();
    const auto& p = panel();
    const auto n = view.collections.size();
    // Nine to a page: the grid's width. Q and E (LB and RB) go on to the next page past the last.
    constexpr std::size_t per_page = 9;
    const std::size_t page = view.tab / per_page, pages = (n + per_page - 1) / per_page;
    const std::size_t first = page * per_page, shown = std::min(per_page, n - std::min(first, n));
    const float last = r.first + static_cast<float>(shown ? shown - 1 : 0) * r.step;
    // As far as the game's own row reaches (five tabs and its E key) or ours, whichever is longer, and no
    // further: the item browser also holds grids of other screens (Appearance), so their number says
    // nothing of this row, and a cover running on under the skater would smear it.
    const float right = std::clamp(last + 150 * r.k, 1044 * r.k, 1540 * r.k);
    // (The panel below hides the game's own row: nothing of the frame is copied over it.)
    // The row sits on a panel of its own: plain and dark, a fine light edge with a soft shadow under it
    // and a light shade inside its top.
    {
        // Wide enough for a controller's LB and RB, which are wider than Q and E.
        const ImVec2 a(r.left - 20 * r.k, r.top + 6 * r.k), b(right + 16 * r.k, r.bottom - 2 * r.k);
        const float round = 6 * r.k;
        for (int i = 5; i >= 1; --i) { // the drop shadow, a little lower than the panel
            const float grow = static_cast<float>(i) * 1.6f * r.k;
            draw->AddRectFilled(ImVec2(a.x - grow, a.y - grow + 3 * r.k), ImVec2(b.x + grow, b.y + grow + 3 * r.k), IM_COL32(0, 0, 0, 12),
                                round + grow);
        }
        draw->AddRectFilled(a, b, IM_COL32(17, 16, 16, 255), round);
        // A light shade inside the top edge.
        const float depth = 7 * r.k;
        draw->AddRectFilledMultiColor(ImVec2(a.x + round, a.y + 1), ImVec2(b.x - round, a.y + depth), IM_COL32(0, 0, 0, 70),
                                      IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        draw->AddRect(a, b, IM_COL32(255, 255, 255, 34), round, 0, 1.0f);
    }
    if (p.pad_mode) pad_glyph(draw, body, scale, ImVec2(276 * r.k, r.y - 10 * scale), Pad::lb, p.pad_style, "");
    else theme::keycap(draw, body, scale, ImVec2(288 * r.k, r.y - 10 * scale), "Q", "");
    for (std::size_t i = 0; i < shown; ++i) {
        const auto index = first + i;
        const bool open = index == view.tab;
        const auto icon = index < view.icons.size() ? view.icons[index] : std::string{};
        tab_tile(draw, ImVec2(r.first + static_cast<float>(i) * r.step, r.y), (open ? 104 : 92) * r.k, icon, open, scale);
        if (!open) continue;
        // Its title under it, as the game puts "Tops" under its open tab.
        const auto& title = view.collections[index];
        const float size = 30 * r.k;
        const auto extent = heading->CalcTextSizeA(size, FLT_MAX, 0, title.c_str());
        const float width = std::max(104 * r.k, extent.x + 24 * r.k), x = r.first + static_cast<float>(i) * r.step - 52 * r.k;
        draw->AddRectFilled(ImVec2(x, 350 * r.k), ImVec2(x + width, 392 * r.k), IM_COL32(22, 22, 22, 240), 3 * r.k);
        draw->AddText(heading, size, ImVec2(x + (width - extent.x) * 0.5f, 371 * r.k - extent.y * 0.5f), IM_COL32(255, 255, 255, 255),
                      title.c_str());
    }
    if (p.pad_mode) pad_glyph(draw, body, scale, ImVec2(last + 66 * r.k, r.y - 10 * scale), Pad::rb, p.pad_style, "");
    else theme::keycap(draw, body, scale, ImVec2(last + 72 * r.k, r.y - 10 * scale), "E", "");
    if (pages > 1) {
        // Which page, under the last key: "2 / 3".
        const auto text = std::to_string(page + 1) + " / " + std::to_string(pages);
        draw->AddText(body, 20 * r.k, ImVec2(last + 66 * r.k, r.y + 40 * r.k), IM_COL32(222, 222, 222, 255), text.c_str());
    }
    return right;
}
} // namespace

bool item_browser_open() { return panel().view.open; }
bool item_browser_custom() { return panel().view.open && panel().view.custom; }
void item_browser_keyboard_used() { panel().keyboard_used.store(true, std::memory_order_relaxed); }

void item_browser_key(ItemBrowserKey key) {
    const auto* h = host();
    if (!h) return;
    panel().next_poll = {}; // what it did shows at the next frame
    if (key == ItemBrowserKey::favorite && h->toggle_favorite) h->toggle_favorite();
    if (key == ItemBrowserKey::filter && h->cycle_filter) h->cycle_filter();
    if (key == ItemBrowserKey::layer && h->toggle_layer) h->toggle_layer();
    if (key == ItemBrowserKey::clear_layers && h->clear_layers) h->clear_layers();
    if (key == ItemBrowserKey::previous_tab && h->step_tab) h->step_tab(-1);
    if (key == ItemBrowserKey::next_tab && h->step_tab) h->step_tab(1);
    if (key == ItemBrowserKey::custom) {
        // With no tabs of the player's yet, Tab opens the panel that makes them.
        if (panel().view.collections.empty()) state().item_panel.store(2);
        else if (h->toggle_custom) h->toggle_custom();
    }
}

void item_browser_search_cleared() {
    auto& p = panel();
    p.input.fill(0);
    // Esc in the collection box or the tabs' panel only closes it; in the search box it clears the search.
    if (state().item_panel.exchange(0)) return;
    if (state().item_picker.exchange(false)) return;
    if (const auto* h = host(); h && h->set_search) h->set_search("");
}

bool item_browser_pending() {
    auto& s = state();
    auto& p = panel();
    const auto* h = host();
    if (!h || !h->view) return false;
    if (const auto now = Clock::now(); now >= p.next_poll) {
        p.next_poll = now + std::chrono::milliseconds(50);
        try { p.view = h->view(); } catch (...) { p.view = {}; }
    }
    if (!p.view.open) {
        s.item_search_visible.store(false);
        s.item_panel.store(0);
        p.stick_down = false;
        p.view_held = false;
        set_withheld_keys({});
        set_withheld_pad_buttons(0);
        return false;
    }
    // While the player's own tabs are shown the game does not see Q, E and Enter (Tab is always ours
    // over an item grid); with one of our boxes or panels open it sees no keys at all anyway. The same
    // for the controller: the stick clicks and View are always ours over a grid, LB, RB and A too while
    // our tabs are shown (A there wears the item on top, as Enter does).
    if (p.view.custom) set_withheld_keys({VK_TAB, 'Q', 'E', VK_RETURN});
    else set_withheld_keys({VK_TAB});
    set_withheld_pad_buttons(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK |
                             (p.view.custom ? XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A : 0u));
    // The controller, read here once a presented frame while a grid is open.
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    const unsigned buttons = pad.available ? (pad.buttons & 0xffffu) : 0u;
    p.pad_pressed = buttons & ~p.pad_last;
    p.pad_last = buttons;
    // The prompts follow what was used last: a key or mouse button, or a controller's button; a
    // controller that has just been connected counts as used. (RESKATE_ITEM_PAD_PROMPTS=1, for looking
    // at them without one: always the controller's.)
    static const bool forced = [] {
        wchar_t value[4]{};
        return GetEnvironmentVariableW(L"RESKATE_ITEM_PAD_PROMPTS", value, 4) == 1 && value[0] == L'1';
    }();
    if (p.keyboard_used.exchange(false, std::memory_order_relaxed)) p.pad_mode = false;
    if (pad.available && !p.pad_connected) p.pad_mode = true;
    p.pad_connected = pad.available;
    if (p.pad_pressed || (pad.available && p.pad_mode)) p.pad_style = pad.style;
    if (p.pad_pressed) p.pad_mode = true;
    if (forced) p.pad_mode = true;
    const bool panel_open = s.item_panel.load() != 0, typing = s.item_search_visible.load();
    if (!panel_open && !typing) {
        if (p.pad_pressed & XINPUT_GAMEPAD_LEFT_THUMB) item_browser_key(ItemBrowserKey::favorite);
        // R3: a tap wears the item on top (or takes it off), holding it takes every item off the top.
        if (p.pad_pressed & XINPUT_GAMEPAD_RIGHT_THUMB) p.r3_down = Clock::now(), p.r3_long = false;
        if (p.view.custom && (p.pad_pressed & XINPUT_GAMEPAD_A)) item_browser_key(ItemBrowserKey::layer);
        if (p.view.custom && (p.pad_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)) item_browser_key(ItemBrowserKey::previous_tab);
        if (p.view.custom && (p.pad_pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)) item_browser_key(ItemBrowserKey::next_tab);
    }
    if (p.r3_down != Clock::time_point{}) {
        const bool r3 = (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        if (r3 && !p.r3_long && Clock::now() - p.r3_down >= std::chrono::milliseconds(600)) {
            p.r3_long = true;
            item_browser_key(ItemBrowserKey::clear_layers);
        } else if (!r3) {
            if (!p.r3_long && !panel_open && !typing) item_browser_key(ItemBrowserKey::layer);
            p.r3_down = {};
        }
    }
    // View: a tap swaps the game's tabs and the player's (in the tabs' panel: adding and changing),
    // holding it opens the panel.
    const auto now = Clock::now();
    const bool view_down = (buttons & XINPUT_GAMEPAD_BACK) != 0;
    if (view_down && !p.view_held) {
        p.view_held = true;
        p.view_long = false;
        p.view_down = now;
    } else if (view_down && !p.view_long && now - p.view_down >= std::chrono::milliseconds(450)) {
        p.view_long = true;
        if (!panel_open && !typing) s.item_panel.store(1);
    } else if (!view_down && p.view_held) {
        p.view_held = false;
        if (!p.view_long && !typing) {
            if (panel_open) s.item_panel.store(s.item_panel.load() == 1 ? 2 : 1);
            else item_browser_key(ItemBrowserKey::custom);
        }
    }
    return true;
}

// The tabs' panel over the item grid. Adding (V): the tabs as tiles, the ones the highlighted item is
// in lit; a click or Enter puts it in or takes it out, "+" makes a tab with it in. Changing (R): a click
// picks a tab; its name, its icon (the game's own category icons first, then ReSkate's), its place,
// or away with it. Arrow keys and Enter work too; Esc closes it.
void draw_tab_panel(const ItemBrowserView& view, const ItemBrowserHost* h, ImFont* heading, ImFont* body, float scale) {
    auto& s = state();
    auto& p = panel();
    int mode = s.item_panel.load();
    const auto r = tab_row();
    const auto n = static_cast<int>(view.collections.size());
    if (p.shown_panel != mode) {
        // Just opened (or switched): start on the open tab.
        p.shown_panel = mode;
        p.cursor = view.custom ? static_cast<int>(view.tab) : 0;
        p.delete_armed = false;
        if (mode == 2 && p.edit < 0 && n) p.edit = p.cursor;
        if (mode == 2 && p.edit >= 0 && p.edit < n) std::snprintf(p.name.data(), p.name.size(), "%s", view.collections[static_cast<std::size_t>(p.edit)].c_str());
        if (mode == 2 && !n) p.edit = -1;
    }
    if (p.edit >= n) p.edit = n ? n - 1 : -1;
    const auto select = [&](int index) {
        p.edit = index;
        p.delete_armed = false;
        if (index >= 0 && index < n) std::snprintf(p.name.data(), p.name.size(), "%s", view.collections[static_cast<std::size_t>(index)].c_str());
    };
    const auto make = [&] {
        // A new tab, named so it can be told apart until it is renamed.
        int number = n + 1;
        std::string name;
        do name = "Tab " + std::to_string(number++);
        while (std::any_of(view.collections.begin(), view.collections.end(), [&](const std::string& c) { return c == name; }));
        const int index = h->make_tab ? h->make_tab(name.c_str(), "folder") : -1;
        if (index < 0) return;
        if (mode == 1 && h->pick_collection) h->pick_collection(name.c_str());
        s.item_panel.store(2);
        mode = 2;
        p.shown_panel = 2;
        p.edit = index;
        p.cursor = index;
        std::snprintf(p.name.data(), p.name.size(), "%s", name.c_str());
        p.name_focus = true;
    };

    ImGui::SetNextWindowPos(ImVec2(290 * r.k, 420 * r.k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1180 * r.k, 0), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 21, 246));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * r.k, 24 * r.k));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * r.k);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##reskate_item_tabs", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        ImGui::PushFont(heading);
        ImGui::SetWindowFontScale(34 * r.k / heading->FontSize);
        ImGui::TextUnformatted(mode == 1 ? "ADD TO YOUR TABS" : "YOUR TABS");
        ImGui::PopFont();
        ImGui::PushFont(body);
        ImGui::SetWindowFontScale(22 * r.k / body->FontSize);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::muted);
        if (mode == 1)
            ImGui::TextUnformatted(view.focused_item ? ("Pick the tabs for " + view.focused_title + ". + makes a new one.").c_str()
                                                     : "Highlight an item in the grid first.");
        else
            ImGui::TextUnformatted(n ? "Pick a tab to change it, or + to make one. Tab shows your tabs in the menu."
                                     : "No tabs yet: + makes your first one.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10 * r.k));

        // The tiles, nine to a row, then "+".
        const float tile = 92 * r.k, step = 120 * r.k, label = 26 * r.k;
        const int per_row = 9;
        const auto origin = ImGui::GetCursorScreenPos();
        const bool typing = ImGui::IsAnyItemActive();
        // The controller: the D-pad moves over the tabs and, in the editor, on down into the icons; A
        // picks, B closes, and in the editor Y gives the tab a name, X deletes it, LB and RB move it.
        const unsigned pressed = typing ? 0u : p.pad_pressed;
        const auto names = mode == 2 ? tab_icon_names() : std::vector<std::string>{};
        constexpr int per_icon_row = 14;
        if (mode != 2) p.icon_cursor = -1;
        const bool left = (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) || (pressed & XINPUT_GAMEPAD_DPAD_LEFT);
        const bool right = (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) || (pressed & XINPUT_GAMEPAD_DPAD_RIGHT);
        const bool up = (!typing && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) || (pressed & XINPUT_GAMEPAD_DPAD_UP);
        const bool down = (!typing && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) || (pressed & XINPUT_GAMEPAD_DPAD_DOWN);
        if (p.icon_cursor < 0) {
            if (left) p.cursor = std::max(0, p.cursor - 1);
            if (right) p.cursor = std::min(n, p.cursor + 1);
            if (up) p.cursor = std::max(0, p.cursor - per_row);
            if (down) {
                if (mode == 2 && p.edit >= 0 && !names.empty() && p.cursor / per_row == n / per_row) p.icon_cursor = 0;
                else p.cursor = std::min(n, p.cursor + per_row);
            }
        } else {
            const int count = static_cast<int>(names.size());
            if (left) p.icon_cursor = std::max(0, p.icon_cursor - 1);
            if (right) p.icon_cursor = std::min(count - 1, p.icon_cursor + 1);
            if (up) p.icon_cursor = p.icon_cursor < per_icon_row ? -1 : p.icon_cursor - per_icon_row;
            if (down) p.icon_cursor = std::min(count - 1, p.icon_cursor + per_icon_row);
        }
        const bool pad_pick = (pressed & XINPUT_GAMEPAD_A) != 0;
        if (pressed & XINPUT_GAMEPAD_B) s.item_panel.store(0);
        if (mode == 2 && p.edit >= 0 && p.edit < n) {
            if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) && h->move_tab && p.edit > 0) h->move_tab(p.edit, -1), --p.edit, p.cursor = p.edit;
            if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && h->move_tab && p.edit + 1 < n) h->move_tab(p.edit, 1), ++p.edit, p.cursor = p.edit;
            if ((pressed & XINPUT_GAMEPAD_X) && h->remove_tab) {
                h->remove_tab(p.edit);
                p.edit = -1;
            }
            if ((pressed & XINPUT_GAMEPAD_Y) && h->rename_tab) {
                // A name without a keyboard: the next of these that no tab has yet.
                constexpr std::array<std::string_view, 22> suggestions{
                    "Jewelry", "Rings", "Earrings", "Piercings", "Necklaces", "Chains", "Bracelets", "Watches", "Gloves", "Scarves", "Hats",
                    "Glasses", "Bags", "Masks", "Random", "Fits", "Goth", "Summer", "Winter", "Mods", "Favourites", "Costumes"};
                const auto folded = [](std::string text) {
                    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return text;
                };
                for (std::size_t tried = 0; tried < suggestions.size(); ++tried) {
                    const std::string name(suggestions[p.name_hint++ % suggestions.size()]);
                    if (std::any_of(view.collections.begin(), view.collections.end(),
                                    [&](const std::string& c) { return folded(c) == folded(name); }))
                        continue;
                    h->rename_tab(p.edit, name.c_str());
                    std::snprintf(p.name.data(), p.name.size(), "%s", name.c_str());
                    break;
                }
            }
        }
        const bool enter = (!typing && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ||
                           (pad_pick && p.icon_cursor < 0);
        for (int i = 0; i <= n; ++i) {
            const ImVec2 centre(origin.x + tile * 0.5f + static_cast<float>(i % per_row) * step,
                                origin.y + tile * 0.5f + static_cast<float>(i / per_row) * (tile + label + 16 * r.k));
            ImGui::SetCursorScreenPos(ImVec2(centre.x - tile * 0.5f, centre.y - tile * 0.5f));
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tile, tile + label));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered) p.cursor = i;
            const bool chosen = clicked || (enter && p.cursor == i);
            if (i == n) {
                plus_tile(draw, centre, tile, scale);
                if (chosen) make();
            } else {
                const auto index = static_cast<std::size_t>(i);
                const bool lit = mode == 1 ? index < view.focused_in.size() && view.focused_in[index] : i == p.edit;
                tab_tile(draw, centre, tile, index < view.icons.size() ? view.icons[index] : std::string{}, lit, scale);
                const auto& name = view.collections[index];
                const auto extent = body->CalcTextSizeA(20 * r.k, FLT_MAX, 0, name.c_str());
                draw->PushClipRect(ImVec2(centre.x - step * 0.5f, centre.y), ImVec2(centre.x + step * 0.5f, centre.y + tile + label), true);
                draw->AddText(body, 20 * r.k, ImVec2(centre.x - std::min(extent.x, step) * 0.5f, centre.y + tile * 0.5f + 6 * r.k),
                              lit ? IM_COL32(255, 255, 255, 255) : theme::muted, name.c_str());
                draw->PopClipRect();
                if (chosen) {
                    if (mode == 1 && view.focused_item && h->pick_collection) h->pick_collection(name.c_str());
                    if (mode == 2) select(i);
                }
            }
            if (p.cursor == i && p.icon_cursor < 0)
                draw->AddRect(ImVec2(centre.x - tile * 0.5f - 4 * r.k, centre.y - tile * 0.5f - 4 * r.k),
                              ImVec2(centre.x + tile * 0.5f + 4 * r.k, centre.y + tile * 0.5f + 4 * r.k), theme::blue, 4 * r.k, 0, 3 * r.k);
        }
        const int rows = n / per_row + 1;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * (tile + label + 16 * r.k)));
        ImGui::Dummy(ImVec2(per_row * step, 4 * r.k));

        // Changing the picked tab.
        if (mode == 2 && p.edit >= 0 && p.edit < n) {
            const auto index = static_cast<std::size_t>(p.edit);
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 6 * r.k));
            ImGui::TextUnformatted("Name");
            ImGui::SameLine(140 * r.k);
            ImGui::SetNextItemWidth(420 * r.k);
            if (p.name_focus) {
                ImGui::SetKeyboardFocusHere();
                p.name_focus = false;
            }
            const bool renamed = ImGui::InputText("##reskate_tab_name", p.name.data(), p.name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if ((renamed || ImGui::IsItemDeactivatedAfterEdit()) && h->rename_tab) h->rename_tab(p.edit, p.name.data());
            ImGui::Dummy(ImVec2(0, 6 * r.k));
            ImGui::TextUnformatted("Icon");
            const float chip = 60 * r.k, gap = 74 * r.k;
            const auto icons_at = ImGui::GetCursorScreenPos();
            const int per = per_icon_row;
            for (std::size_t i = 0; i < names.size(); ++i) {
                const ImVec2 centre(icons_at.x + chip * 0.5f + static_cast<float>(i % per) * gap,
                                    icons_at.y + chip * 0.5f + static_cast<float>(i / per) * gap);
                ImGui::SetCursorScreenPos(ImVec2(centre.x - chip * 0.5f, centre.y - chip * 0.5f));
                ImGui::PushID(1000 + static_cast<int>(i));
                const bool picked = ImGui::InvisibleButton("icon", ImVec2(chip, chip));
                const bool over = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool current = index < view.icons.size() && view.icons[index] == names[i];
                tab_tile(draw, centre, chip, names[i], current, scale);
                if (over || p.icon_cursor == static_cast<int>(i)) draw->AddRect(ImVec2(centre.x - chip * 0.5f - 3 * r.k, centre.y - chip * 0.5f - 3 * r.k),
                                        ImVec2(centre.x + chip * 0.5f + 3 * r.k, centre.y + chip * 0.5f + 3 * r.k), theme::blue, 3 * r.k, 0, 2 * r.k);
                if ((picked || (pad_pick && p.icon_cursor == static_cast<int>(i))) && h->set_tab_icon) h->set_tab_icon(p.edit, names[i].c_str());
            }
            const auto icon_rows = static_cast<int>((names.size() + per - 1) / per);
            ImGui::SetCursorScreenPos(ImVec2(icons_at.x, icons_at.y + static_cast<float>(std::max(icon_rows, 1)) * gap));
            if (names.empty()) ImGui::TextUnformatted("(the icons are still loading)");
            ImGui::Dummy(ImVec2(0, 8 * r.k));
        }
        // The panel's actions, as the item grid's prompts: a key (or the controller's button) and what it
        // does, in a row; each can be clicked, or its key pressed.
        ImGui::Dummy(ImVec2(0, 10 * r.k));
        auto at = ImGui::GetCursorScreenPos();
        const float k = scale * 0.82f, row_h = 25 * k;
        const auto action = [&](const char* key, ImGuiKey shortcut, std::optional<Pad> button, const char* label) {
            const bool pad = p.pad_mode;
            if (pad && !button) return false;
            const float cell = 40 * k;
            const ImVec2 centre(at.x + cell * 0.5f, at.y + row_h * 0.5f);
            if (pad) {
                const float w = pad_glyph(nullptr, body, k, {}, *button, p.pad_style, "");
                pad_glyph(draw, body, k, ImVec2(centre.x - w * 0.5f, centre.y - 10 * k), *button, p.pad_style, "");
            } else {
                const float h = 19 * k, size = 12 * k;
                const auto text = body->CalcTextSizeA(size, FLT_MAX, 0, key);
                const float w = std::max(h, text.x + 10 * k);
                draw->AddRectFilled(ImVec2(centre.x - w * 0.5f, centre.y - h * 0.5f), ImVec2(centre.x + w * 0.5f, centre.y + h * 0.5f),
                                    IM_COL32(255, 255, 255, 255), 3 * k);
                draw->AddText(body, size, ImVec2(centre.x - text.x * 0.5f, centre.y - text.y * 0.5f), skate_theme::black, key);
            }
            const float size = 15 * k, label_w = body->CalcTextSizeA(size, FLT_MAX, 0, label).x;
            const ImVec2 text_at(at.x + cell + 2 * k, centre.y - 8 * k);
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(label);
            const bool clicked = ImGui::InvisibleButton("action", ImVec2(cell + 2 * k + label_w, row_h));
            const bool hover = ImGui::IsItemHovered();
            ImGui::PopID();
            draw->AddText(body, size, text_at, hover ? theme::blue : IM_COL32(240, 240, 240, 255), label);
            at.x += cell + 2 * k + label_w + 22 * k;
            return clicked || (!pad && !typing && shortcut != ImGuiKey_None && ImGui::IsKeyPressed(shortcut, false));
        };
        if (mode == 2 && p.edit >= 0 && p.edit < n) {
            if (action("Q", ImGuiKey_Q, std::nullopt, "Move left") && h->move_tab && p.edit > 0)
                h->move_tab(p.edit, -1), p.edit = std::max(0, p.edit - 1), p.cursor = p.edit;
            if (action("E", ImGuiKey_E, std::nullopt, "Move right") && h->move_tab && p.edit + 1 < n)
                h->move_tab(p.edit, 1), p.edit = std::min(n - 1, p.edit + 1), p.cursor = p.edit;
            if (action("S", ImGuiKey_S, std::nullopt, "Show it") && h->select_tab) {
                h->select_tab(p.edit);
                if (!view.custom && h->toggle_custom) h->toggle_custom();
                s.item_panel.store(0);
            }
            if (action("Del", ImGuiKey_Delete, std::nullopt, "Delete tab") && h->remove_tab) {
                h->remove_tab(p.edit);
                p.edit = -1;
            }
        }
        if (p.pad_mode) {
            action("", ImGuiKey_None, Pad::a, mode == 1 ? "Add / remove" : "Pick");
            action("", ImGuiKey_None, Pad::view, mode == 1 ? "Edit tabs" : "Add items");
            if (mode == 2 && p.edit >= 0) {
                action("", ImGuiKey_None, Pad::y, "Name");
                action("", ImGuiKey_None, Pad::x, "Delete tab");
                action("", ImGuiKey_None, Pad::lb, "Move left");
                action("", ImGuiKey_None, Pad::rb, "Move right");
            }
        }
        if (action("Esc", ImGuiKey_None, Pad::b, "Done")) s.item_panel.store(0);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetWindowPos().x, at.y + row_h));
        ImGui::Dummy(ImVec2(0, 4 * r.k));
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void draw_item_browser() {
    auto& s = state();
    auto& p = panel();
    const auto* h = host();
    if (!h || !p.view.open) return;
    const auto& view = p.view;
    auto* heading = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    // Sized like the game's own item card and prompts, which are laid out for a 2160-line screen.
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 3.0f) * 1.35f;
    const float width = 350.0f * scale, height = 42.0f * scale, margin = 0.074f * display.y;
    const ImVec2 at(display.x - margin - width, margin);
    const bool typing = s.item_search_visible.load();
    const bool picking = typing && s.item_picker.load();

    // The search field: a dark tile like the game's item card, blue-edged while it takes the keys.
    auto* draw = ImGui::GetBackgroundDrawList();
    skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height), IM_COL32(18, 18, 19, 235), 71, scale);
    if (typing) draw->AddRect(ImVec2(at.x - scale, at.y - scale), ImVec2(at.x + width + scale, at.y + height + scale), theme::blue, 0, 0, 2.5f * scale);
    const ImVec2 lens(at.x + 24 * scale, at.y + height * 0.46f);
    draw->AddCircle(lens, 8 * scale, theme::paper, 0, 2.2f * scale);
    draw->AddLine(ImVec2(lens.x + 6 * scale, lens.y + 6 * scale), ImVec2(lens.x + 12 * scale, lens.y + 12 * scale), theme::paper, 2.6f * scale);
    const std::string count = view.shown == view.total ? std::to_string(view.total)
                                                        : std::to_string(view.shown) + " / " + std::to_string(view.total);
    const float count_size = 15 * scale;
    const auto count_extent = body->CalcTextSizeA(count_size, FLT_MAX, 0, count.c_str());
    draw->AddText(body, count_size, ImVec2(at.x + width - count_extent.x - 14 * scale, at.y + (height - count_extent.y) * 0.5f),
                  theme::muted, count.c_str());

    const float text_x = at.x + 46 * scale, text_width = width - 46 * scale - count_extent.x - 28 * scale;
    // What Enter picks in the collection box: the collection named or numbered, or the only one whose
    // name holds what is typed; else a new collection by that name.
    const auto folded = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string typed = p.input.data(), lowered = folded(typed);
    int target = -1, contained = -1;
    unsigned containing{};
    for (std::size_t i = 0; i < view.collections.size(); ++i) {
        const auto name = folded(view.collections[i]);
        if (!lowered.empty() && (name == lowered || lowered == std::to_string(i + 1))) target = static_cast<int>(i);
        else if (!lowered.empty() && name.find(lowered) != std::string::npos) ++containing, contained = static_cast<int>(i);
    }
    if (target < 0 && containing == 1) target = contained;
    if (typing) {
        ImGui::SetNextWindowPos(ImVec2(text_x, at.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(text_width, height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (height - 20 * scale) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::paper);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(1, 131, 255, 110));
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##reskate_item_search", nullptr, flags)) {
            ImGui::PushFont(heading);
            ImGui::SetWindowFontScale(20 * scale / heading->FontSize);
            if (s.item_search_focus_requested.exchange(false)) {
                std::snprintf(p.input.data(), p.input.size(), "%s", picking ? "" : view.search.c_str());
                const auto id = ImGui::GetID("##reskate_item_search_input");
                if (auto* active = ImGui::GetInputTextState(id); active && ImGui::GetActiveID() == id)
                    active->ReloadUserBufAndMoveToEnd();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = ImGui::InputTextWithHint("##reskate_item_search_input", picking ? "Collection name or number" : "Type to search",
                                                          p.input.data(), p.input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (picking) {
                // Enter puts the item into the collection typed (a new name makes one) or takes it out.
                if (entered) {
                    if (h->pick_collection && p.input[0])
                        h->pick_collection(target >= 0 ? view.collections[static_cast<std::size_t>(target)].c_str() : p.input.data());
                    p.input.fill(0);
                    s.item_picker.store(false);
                    s.item_search_visible.store(false);
                }
            } else {
                // Every letter filters the grid at once.
                if (h->set_search && p.pushed != p.input.data()) {
                    p.pushed = p.input.data();
                    h->set_search(p.pushed.c_str());
                }
                if (entered) s.item_search_visible.store(false);
            }
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    } else {
        p.pushed = view.search;
        const float size = 20 * scale;
        const auto text = view.search.empty() ? std::string("Search") : view.search;
        draw->PushClipRect(ImVec2(text_x, at.y), ImVec2(text_x + text_width, at.y + height), true);
        draw->AddText(heading, size, ImVec2(text_x, at.y + (height - size) * 0.5f - scale),
                      view.search.empty() ? theme::muted : theme::paper, text.c_str());
        draw->PopClipRect();
    }

    // The prompts: one action a line under the search field, the key or button in a cell of its own
    // and what it does after it, smaller than the game's own and the same for the keyboard, an Xbox
    // pad and a PlayStation pad. A button that is held shows a ring filling round it as it is.
    const float ps = scale * 0.82f;            // the prompts' size
    const float cell = 40 * ps, pitch = 25 * ps;
    float y = at.y + height + 12 * scale;
    const auto label_text = [&](ImVec2 where, const char* text) {
        const float size = 15 * ps;
        draw->AddText(body, size, ImVec2(where.x + 1.2f * ps, where.y + 1.2f * ps), IM_COL32(0, 0, 0, 120), text);
        draw->AddText(body, size, where, IM_COL32(240, 240, 240, 255), text);
    };
    // A small keycap, as the game draws its own, centred in the cell.
    const auto key_cell = [&](ImVec2 centre, const char* key) {
        const float h = 19 * ps, size = 12 * ps;
        const auto text = body->CalcTextSizeA(size, FLT_MAX, 0, key);
        const float w = std::max(h, text.x + 10 * ps);
        draw->AddRectFilled(ImVec2(centre.x - w * 0.5f, centre.y - h * 0.5f), ImVec2(centre.x + w * 0.5f, centre.y + h * 0.5f),
                            IM_COL32(255, 255, 255, 255), 3 * ps);
        draw->AddText(body, size, ImVec2(centre.x - text.x * 0.5f, centre.y - text.y * 0.5f), skate_theme::black, key);
    };
    // The ring of a held button: grey, filling white clockwise from the top as it is held.
    const auto hold_ring = [&](ImVec2 centre, float progress) {
        const float radius = 13.5f * ps;
        draw->AddCircle(centre, radius, IM_COL32(255, 255, 255, 70), 32, 2.2f * ps);
        if (progress <= 0) return;
        constexpr float top = -1.5707963f;
        draw->PathArcTo(centre, radius, top, top + 6.2831853f * std::min(progress, 1.0f), 32);
        draw->PathStroke(IM_COL32(255, 255, 255, 255), 0, 2.6f * ps);
    };
    const auto now = Clock::now();
    const auto held = [&](Clock::time_point since, bool active, float seconds) {
        return active ? std::chrono::duration<float>(now - since).count() / seconds : 0.0f;
    };
    const float view_hold = held(p.view_down, p.view_held && !p.view_long, 0.45f);
    const float r3_hold = held(p.r3_down, p.r3_down != Clock::time_point{} && !p.r3_long, 0.6f);
    // Prompts side by side in rows, each the keyboard's key or with a controller in use its button (one
    // only the keyboard has is left out then); `hold` is how far a held button is (negative: not one).
    float x = at.x;
    const auto next_row = [&] {
        if (x > at.x) y += pitch + 4 * ps;
        x = at.x;
    };
    const auto prompt = [&](const char* key, std::optional<Pad> button, const std::string& label, float hold = -1) {
        const bool pad = p.pad_mode && !typing;
        if (pad && !button) return;
        const ImVec2 centre(x + cell * 0.5f, y + pitch * 0.5f);
        if (pad) {
            // pad_glyph draws from its top left at a 20-high size: scaled to fit the cell.
            const float w = pad_glyph(nullptr, body, ps, {}, *button, p.pad_style, "");
            pad_glyph(draw, body, ps, ImVec2(centre.x - w * 0.5f, centre.y - 10 * ps), *button, p.pad_style, "");
            if (hold >= 0) hold_ring(centre, hold);
        } else {
            key_cell(centre, key);
        }
        label_text(ImVec2(x + cell + 2 * ps, centre.y - 8 * ps), label.c_str());
        x += cell + 2 * ps + body->CalcTextSizeA(15 * ps, FLT_MAX, 0, label.c_str()).x + 22 * ps;
    };
    if (picking) {
        prompt("Enter", std::nullopt, "Add / remove");
        prompt("Esc", std::nullopt, "Close");
    } else if (typing) {
        prompt("Enter", std::nullopt, "Done");
        prompt("Esc", std::nullopt, "Clear");
    } else {
        // The item grid's own: favorite, search, what it shows.
        prompt("F", Pad::ls, view.focused_favorite ? "Unfavorite" : "Favorite");
        prompt("-", std::nullopt, "Search");
        if (!view.custom) prompt("X", std::nullopt, view.filter);
        next_row();
        // The player's tabs.
        prompt("V", Pad::view, p.pad_mode ? "Add to tab (hold)" : "Add to tab", p.pad_mode ? view_hold : -1);
        prompt("R", std::nullopt, "Edit tabs");
        next_row();
        // Wearing items on top.
        if (view.focused_item)
            prompt(view.custom ? "Enter" : "B", view.custom ? Pad::a : Pad::rs, view.focused_layer ? "Take item off" : "Wear item");
        if (view.layers) prompt("Del", Pad::rs, p.pad_mode ? "Take all off (hold)" : "Take all off", p.pad_mode ? r3_hold : -1);
    }
    next_row();
    if (view.focused_favorite && !typing)
        star(draw, ImVec2(at.x - 22 * scale, at.y + height * 0.5f), 13 * scale, skate_theme::bar);
    float row = y + 2 * ps;
    const float line = 13.5f * scale;
    // A line of text as the game writes over its pictures: with a soft shadow, readable over the skater.
    const auto say = [&](ImVec2 where, ImU32 colour, const char* text) {
        if (colour == theme::muted) colour = IM_COL32(222, 222, 222, 255); // grey is lost over the skater
        for (const auto& offset : {ImVec2(1.5f, 1.5f), ImVec2(0, 1.5f), ImVec2(1.5f, 0), ImVec2(2.5f, 2.5f)})
            draw->AddText(body, line, ImVec2(where.x + offset.x * scale, where.y + offset.y * scale), IM_COL32(0, 0, 0, 110), text);
        draw->AddText(body, line, where, colour, text);
    };
    // Which collections the highlighted item is in (all of them, numbered, while the box is open).
    std::string in;
    for (std::size_t i = 0; i < view.collections.size(); ++i)
        if (i < view.focused_in.size() && view.focused_in[i]) in += (in.empty() ? "" : ", ") + view.collections[i];
    if (picking) {
        // Every collection that fits what is typed, numbered; a star on those the item is in.
        for (std::size_t i = 0; i < view.collections.size(); ++i) {
            const auto name = folded(view.collections[i]);
            if (!lowered.empty() && name.find(lowered) == std::string::npos && lowered != std::to_string(i + 1)) continue;
            const bool member = i < view.focused_in.size() && view.focused_in[i];
            const auto text = std::to_string(i + 1) + "  " + view.collections[i];
            say(ImVec2(at.x + 26 * scale, row),
                          static_cast<int>(i) == target ? theme::blue : member ? theme::paper : theme::muted, text.c_str());
            if (member) star(draw, ImVec2(at.x + 12 * scale, row + line * 0.55f), 6 * scale, skate_theme::bar);
            row += line * 1.25f;
        }
        bool number{!lowered.empty()};
        for (const char c : lowered) number &= c >= '0' && c <= '9';
        std::string hint;
        if (target >= 0) {
            const bool member = static_cast<std::size_t>(target) < view.focused_in.size() && view.focused_in[static_cast<std::size_t>(target)];
            hint = (member ? "Enter: take it out of " : "Enter: add it to ") + view.collections[static_cast<std::size_t>(target)];
        } else if (!lowered.empty() && !number) hint = "Enter: new collection \"" + typed + "\"";
        if (!hint.empty()) {
            say(ImVec2(at.x + 26 * scale, row), theme::blue, hint.c_str());
            row += line * 1.25f;
        }
        if (view.collections.empty() && lowered.empty()) {
            say(ImVec2(at.x + 26 * scale, row), theme::muted, "No collections yet: type a name, then Enter");
            row += line * 1.25f;
        }
    } else if (!typing && !in.empty()) {
        const auto text = "In: " + in;
        say(ImVec2(at.x, row), theme::muted, text.c_str());
        row += line * 1.25f;
    }
    if (!typing && view.layers) {
        // The menus show the outfit alone: the layers go on the skater in the world.
        auto text = std::to_string(view.layers) + (view.layers == 1 ? " item" : " items") + " worn on top";
        say(ImVec2(at.x, row), theme::muted, text.c_str());
        row += line * 1.25f;
        if (!view.layers_note.empty()) say(ImVec2(at.x, row), theme::muted, view.layers_note.c_str());
    }

    // The player's own tabs in the game's place, or the key that brings them, right of the game's row.
    const auto r = tab_row();
    if (view.custom) {
        const float end = draw_custom_row(draw, heading, body, view, scale);
        if (p.pad_mode) pad_glyph(draw, body, scale, ImVec2(end + 40 * r.k, r.y - 10 * scale), Pad::view, p.pad_style, "Game tabs");
        else theme::keycap(draw, body, scale, ImVec2(end + 40 * r.k, r.y - 10 * scale), "Tab", "Game tabs");
        if (view.tab_empty) {
            // The game keeps one tile in the grid to hold its highlight: it goes under a card.
            const ImVec2 a(300 * r.k, 420 * r.k), b(1480 * r.k, 670 * r.k);
            draw->AddRectFilled(a, b, IM_COL32(28, 28, 29, 255), 6 * r.k);
            TabIcon icon;
            const auto name = view.tab < view.icons.size() ? view.icons[view.tab] : std::string{};
            if (tab_icon(name, icon) || tab_icon("folder", icon))
                draw->AddImage(icon.texture, ImVec2(a.x + 40 * r.k, a.y + 45 * r.k), ImVec2(a.x + 200 * r.k, a.y + 205 * r.k), icon.uv0, icon.uv1,
                               IM_COL32(255, 255, 255, 70));
            draw->AddText(heading, 34 * r.k, ImVec2(a.x + 240 * r.k, a.y + 60 * r.k), IM_COL32(255, 255, 255, 255), "NOTHING HERE YET");
            draw->AddText(body, 24 * r.k, ImVec2(a.x + 240 * r.k, a.y + 115 * r.k), theme::muted,
                          p.pad_mode ? "Press View for the game's tabs, highlight an item and hold View"
                                     : "Press Tab for the game's tabs, highlight an item and press V");
            draw->AddText(body, 24 * r.k, ImVec2(a.x + 240 * r.k, a.y + 150 * r.k), theme::muted, "to put it in this tab.");
        }
    } else {
        const char* label = view.collections.empty() ? "Make your own tabs" : "My tabs";
        if (p.pad_mode) pad_glyph(draw, body, scale, ImVec2(1060 * r.k, r.y - 10 * scale), Pad::view, p.pad_style, label);
        else theme::keycap(draw, body, scale, ImVec2(1060 * r.k, r.y - 10 * scale), "Tab", label);
    }
    if (s.item_panel.load()) draw_tab_panel(view, h, heading, body, scale);
    else p.shown_panel = 0;
}
} // namespace dingosdk::overlay::detail
