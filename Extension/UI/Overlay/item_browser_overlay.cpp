#include "overlay_internal.h"
#include "Engine/Core/Platform/path_text.h"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

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
ImVec2 window_to_buffer_scale(); // overlay_render.cpp (fit_display_to_buffer)
namespace {
struct Panel {
    ItemBrowserView view;
    std::array<char, 96> input{};
    bool stick_down{};
    std::string pushed; // the search text the host was last given
    // The pickers' filters (draw_filters): the row the keys are on, the row whose list is open (-1:
    // none), the list's highlighted choice and its first one in sight, and a click that leaves them
    // when it is let go (so the game does not get its release).
    // `lit` and `scroll` count in the open list's matches for `query`, what is typed in its search.
    int row{}, open{-1}, lit{}, scroll{};
    std::string query;
    bool leaving{};
    bool search_leaving{}; // a click away from the search field, which leaves it when let go
    ImVec2 last_mouse{};
    unsigned releases{}; // of the watched mouse (Watched), as last seen
};
// The mouse over the game's window while the game has it, from its messages (item_browser_mouse):
// where it is, where the left button last went down and where it was last let go, and how many
// times it has been.
struct Watched {
    std::mutex mutex;
    ImVec2 at{-FLT_MAX, -FLT_MAX}, pressed{-FLT_MAX, -FLT_MAX}, released{-FLT_MAX, -FLT_MAX};
    unsigned releases{};
    ULONGLONG released_when{};
};
Watched& watched() { static auto* value = new Watched; return *value; }
Panel& panel() { static auto* value = new Panel; return *value; }
const ItemBrowserHost* host() { return item_browser_host.load(); }

// The icon of the mod the highlighted item is from: read and decoded off this thread, uploaded by
// the render thread to the SRV heap's fourth slot (overlay_render.cpp, upload_mod_icon).
struct ModIcon {
    std::mutex mutex;
    std::string wanted, tried;  // the icon asked for, and the last one the loader finished
    bool loading{};
    // Icons decoded lately (a mod with none as no pixels), so going back to a mod's item shows its
    // icon at once: at most `kept`, 64 KB each.
    static constexpr std::size_t kept = 32;
    std::vector<std::pair<std::string, CardPixels>> decoded;
    CardPixels current;         // kept to upload again after a device reset
    std::string current_path;
    bool waiting{};
    unsigned serial{};
    ImTextureID texture{};
    std::string ready_path;     // the icon the texture holds
};
ModIcon& mod_icon() { static auto* value = new ModIcon; return *value; } // process lifetime

void load_mod_icons() {
    auto& m = mod_icon();
    for (;;) {
        std::string path;
        {
            std::lock_guard lock(m.mutex);
            if (m.wanted == m.tried) {
                m.loading = false;
                return;
            }
            path = m.wanted;
        }
        CardPixels pixels;
        bool ok{};
        try {
            std::ifstream input(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
            const std::string png{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            ok = !png.empty() && png.size() <= 4 * 1024 * 1024 && decode_picture(png, pixels, 128);
        } catch (...) {}
        if (!ok) pixels = {};
        std::lock_guard lock(m.mutex);
        m.tried = path;
        if (m.decoded.size() >= ModIcon::kept) m.decoded.erase(m.decoded.begin());
        m.decoded.emplace_back(path, pixels);
        if (ok && path == m.wanted) {
            pixels.serial = ++m.serial;
            m.current = std::move(pixels);
            m.current_path = path;
            m.waiting = true;
        }
    }
}
// Asks for this icon (empty: none); the icon's texture once it is on the GPU, else 0.
ImTextureID want_mod_icon(const std::string& path) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (path.empty()) return {};
    if (m.wanted != path) {
        m.wanted = path;
        // One decoded lately goes straight to the GPU (in the next frame).
        const auto known = std::ranges::find(m.decoded, path, &std::pair<std::string, CardPixels>::first);
        if (known != m.decoded.end()) {
            m.tried = path;
            if (!known->second.rgba.empty() && m.ready_path != path) {
                m.current = known->second;
                m.current.serial = ++m.serial;
                m.current_path = path;
                m.waiting = true;
            }
        } else if (!m.loading) {
            try {
                std::thread(load_mod_icons).detach();
                m.loading = true;
            } catch (...) {}
        }
    }
    return m.ready_path == path ? m.texture : ImTextureID{};
}

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
// Where the game's menu is across the screen: its left edge and width. The full width up to 21:9;
// on a wider screen a 21:9 frame in the middle.
std::pair<float, float> menu_frame(ImVec2 display) {
    const float width = std::min(display.x, display.y * 21.0f / 9.0f);
    return {(display.x - width) * 0.5f, width};
}

// `text` cut short with "..." to fit `width`.
std::string fitted(ImFont* font, float size, const std::string& text, float width) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty()) {
        do cut.pop_back(); while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xc0) == 0x80);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        if (font->CalcTextSizeA(size, FLT_MAX, 0, (cut + "...").c_str()).x <= width) break;
    }
    return cut + "...";
}

// Which mod the highlighted item is from, inside the game's own item card (the rarity tag, the
// name and the description at the bottom of the screen): the mod's icon, what it is to the item,
// its name, who made it, its version and how many items it has. The card grows upward from a
// fixed bottom edge as its text wraps; a mod's item has three empty description lines
// (Customization/local_cosmetic_catalog.cpp), so its last three lines are always the same place.
// As measured on 1280x720 and on 2000 wide at nine heights: the game lays its menu out across
// the screen's width up to 21:9, centred past that (menu_frame); it sizes it by the smaller of its
// two scales (`u`); the prompt row's bottom margin goes with the screen's height, the item card
// sits on the prompt row, its left edge is 45.25% of the frame and it is 319 wide.
void draw_mod_card(ImDrawList* draw, const ItemBrowserView& view, ImFont* heading, ImFont* body) {
    if (view.mod_title.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto [left, width] = menu_frame(display);
    const float u = std::min(width / 1280.0f, display.y / 720.0f);
    const float prompts_top = display.y * (1.0f - 35.0f / 720.0f) - 37 * u;   // Hide Outfit, Equip, Back
    const float card_bottom = prompts_top - 6 * u;                             // the game's item card
    const float card_left = left + width * 0.4525f, card_width = 319 * u;
    // Its last three lines (20 each, over an 8 bottom margin), inset like its text. The first line
    // keeps the info sign the game's notice shows there (item_browser.cpp, quiet_notices); this
    // goes under it, over the other two.
    const float pad = 6 * u, icon = 40 * u;
    const ImVec2 a(card_left + 12 * u - pad, card_bottom - 68 * u), b(card_left + card_width - 12 * u, card_bottom - 8 * u);
    const float top = a.y + 20 * u;
    // The icon, or the title's first letter on a plain tile when the mod has none.
    const ImVec2 i0(a.x + pad, top), i1(i0.x + icon, i0.y + icon);
    if (const auto texture = want_mod_icon(view.mod_icon)) {
        draw->AddImage(texture, i0, i1);
    } else {
        draw->AddRectFilled(i0, i1, skate_theme::tile_light);
        const char letter[2]{static_cast<char>(std::toupper(static_cast<unsigned char>(view.mod_title.front()))), 0};
        const float size = 24 * u;
        const auto extent = heading->CalcTextSizeA(size, FLT_MAX, 0, letter);
        draw->AddText(heading, size, ImVec2(i0.x + (icon - extent.x) * 0.5f, i0.y + (icon - extent.y) * 0.5f), theme::muted, letter);
    }
    const float text_x = i1.x + 10 * u, right = b.x - pad;
    // The tag, like the game's rarity tag, then the name beside it.
    const char* tag = view.mod_changed ? "CHANGED BY MOD" : "MOD";
    const float tag_size = 10 * u, title_size = 15 * u, line_size = 12 * u;
    const auto tag_extent = heading->CalcTextSizeA(tag_size, FLT_MAX, 0, tag);
    const float row = top + (icon - title_size - 5 * u - line_size) * 0.5f;
    const ImVec2 t0(text_x, row + (title_size - tag_extent.y - 4 * u) * 0.5f);
    const ImVec2 t1(t0.x + tag_extent.x + 10 * u, t0.y + tag_extent.y + 4 * u);
    draw->AddRectFilled(t0, t1, view.mod_changed ? skate_theme::bar : skate_theme::blue);
    draw->AddText(heading, tag_size, ImVec2(t0.x + 5 * u, t0.y + 2 * u), view.mod_changed ? skate_theme::black : theme::paper, tag);
    const float name_x = t1.x + 7 * u;
    draw->AddText(heading, title_size, ImVec2(name_x, row), theme::paper,
                  fitted(heading, title_size, view.mod_title, right - name_x).c_str());
    std::string line = view.mod_author.empty() ? std::string() : "by " + view.mod_author;
    const auto add = [&](const std::string& part) { if (!part.empty()) line += (line.empty() ? "" : "  |  ") + part; };
    if (!view.mod_version.empty()) add("v" + view.mod_version);
    if (view.mod_items) add(std::to_string(view.mod_items) + (view.mod_items == 1 ? " item" : " items"));
    draw->AddText(body, line_size, ImVec2(text_x, row + title_size + 5 * u), theme::muted,
                  fitted(body, line_size, line, right - text_x).c_str());
}

// The pickers' Author, Mod pack, Sort and Show: four small lists in a line right of the search
// field, in its look, each a list to pick from that opens downward, can be searched, and stops above
// the item card. Clicked, or V then the arrow keys, Enter and Esc; an open list also takes Home,
// End, Page Up, Page Down and the wheel.
std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

struct Choice {
    std::string label, value;
};
struct Row {
    const char* name;
    std::vector<Choice> choices;
    int chosen{};
    bool enabled() const { return choices.size() > 1; }
};
std::array<Row, 4> filter_rows(const ItemBrowserView& view) {
    std::array<Row, 4> rows{Row{"Author", {}}, Row{"Mod pack", {}}, Row{"Sort", {}}, Row{"Show", {}}};
    rows[0].choices.push_back({"Any", ""});
    std::vector<std::string> authors;
    for (const auto& pack : view.packs)
        if (!pack[2].empty() && std::ranges::find(authors, pack[2]) == authors.end()) authors.push_back(pack[2]);
    std::ranges::sort(authors, [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
    });
    for (const auto& author : authors) rows[0].choices.push_back({author, author});
    // A chosen one this grid has none of stays in the list, so it shows and can be taken back.
    if (!view.author.empty() && std::ranges::find(authors, view.author) == authors.end())
        rows[0].choices.push_back({view.author, view.author});
    rows[1].choices.push_back({"Any", ""});
    for (const auto& pack : view.packs)
        if (view.author.empty() || pack[2] == view.author) rows[1].choices.push_back({pack[1], pack[0]});
    if (!view.pack.empty() && std::ranges::none_of(rows[1].choices, [&](const Choice& c) { return c.value == view.pack; }))
        rows[1].choices.push_back({view.pack, view.pack});
    for (std::size_t i = 0; i < view.sorts.size(); ++i) rows[2].choices.push_back({view.sorts[i], std::to_string(i)});
    // Show: All, Favorites, the game's own items ("skate."), then the mods'.
    for (const std::size_t i : {0u, 1u, 3u, 2u})
        if (i < view.filters.size()) rows[3].choices.push_back({view.filters[i], std::to_string(i)});
    const auto chosen = [](Row& row, const std::string& value) {
        for (std::size_t i = 0; i < row.choices.size(); ++i)
            if (row.choices[i].value == value) row.chosen = static_cast<int>(i);
    };
    chosen(rows[0], view.author);
    chosen(rows[1], view.pack);
    chosen(rows[2], std::to_string(view.sort));
    chosen(rows[3], std::to_string(view.filter_index));
    return rows;
}
void take_choice(const ItemBrowserView& view, int row, const Choice& choice) {
    const auto* h = host();
    if (!h) return;
    if (row == 3) {
        if (h->set_filter) h->set_filter(static_cast<unsigned>(std::stoul(choice.value)));
        return;
    }
    if (!h->narrow) return;
    auto author = view.author, pack = view.pack;
    auto sort = view.sort;
    if (row == 0) {
        author = choice.value;
        // The pack goes when it is another author's.
        const auto of = std::ranges::find_if(view.packs, [&](const auto& p) { return p[0] == pack; });
        if (!author.empty() && (of == view.packs.end() || (*of)[2] != author)) pack.clear();
    }
    if (row == 1) pack = choice.value;
    if (row == 2) sort = static_cast<unsigned>(std::stoul(choice.value));
    h->narrow(author.c_str(), pack.c_str(), sort);
}

// The pointer for a frame of the item browser. The overlay's (`owned`) while the filters or the
// search field have the keys. Else, while nothing else of ours is open, it is only watched, from
// the game window's own mouse messages, and nothing is taken from the game: then `released` is a
// click let go this frame, with where it went down (`pressed_at`), so a drag is no click.
struct Pointer {
    ImVec2 mouse{-FLT_MAX, -FLT_MAX}, pressed_at{-FLT_MAX, -FLT_MAX};
    bool clicked{}, released{}, moved{};
    float wheel{};
    bool others{}, owned{}, watching{};
    bool on(const ImRect& box) const { return mouse.x > -FLT_MAX / 2 && box.Contains(mouse); }
};
Pointer read_pointer() {
    auto& s = state();
    auto& p = panel();
    auto& io = ImGui::GetIO();
    Pointer ptr;
    ptr.others = s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load() ||
                 s.plugin_screen_visible.load() || s.plugin_dialog_visible.load() || s.prompt_pointer.load();
    ptr.owned = !ptr.others && (s.item_filters_visible.load() || s.item_search_visible.load());
    ptr.watching = !ptr.others && !ptr.owned;
    if (ptr.owned) {
        ptr.mouse = io.MousePos;
        ptr.clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ptr.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ptr.wheel = io.MouseWheel;
    } else if (ptr.watching) {
        auto& m = watched();
        std::lock_guard lock(m.mutex);
        ptr.mouse = m.at;
        // (One from before the item browser was on screen is none.)
        if (m.releases != p.releases && GetTickCount64() - m.released_when < 250) {
            ptr.released = true;
            ptr.pressed_at = m.pressed;
            ptr.mouse = m.released;
        }
        p.releases = m.releases;
        // The messages place it in the window's client pixels; everything here is laid out in the
        // back buffer's, which differ when the window is scaled for the display or stretched.
        // ImGui's own mouse gets the same mapping (fit_display_to_buffer), done on this thread.
        const ImVec2 scale = window_to_buffer_scale();
        for (auto* at : {&ptr.mouse, &ptr.pressed_at})
            if (at->x > -FLT_MAX / 2) *at = ImVec2(at->x * scale.x, at->y * scale.y);
    }
    ptr.moved = ptr.mouse.x != p.last_mouse.x || ptr.mouse.y != p.last_mouse.y;
    p.last_mouse = ptr.mouse;
    return ptr;
}

// The pickers' filters: a line of small lists right of the search field, as tall as it and level
// with it, `gap` apart, from `left`; `widths` is each one's. An open list drops from its control's
// left edge, as wide (wider only for its search field), and is drawn over the key prompts.
// Returns whether they have the keys.
bool draw_filters(ImDrawList* draw, ImFont* heading, ImFont* body, float scale, const Pointer& ptr, const std::array<float, 4>& widths,
                  float left, float top, float h, float gap) {
    auto& s = state();
    auto& p = panel();
    const auto& view = p.view;
    auto& io = ImGui::GetIO();
    const auto display = io.DisplaySize;
    const float u = std::min(menu_frame(display).second / 1280.0f, display.y / 720.0f);
    const float line = 27 * u, inset = 16 * scale, head = 34 * u;
    // An open list stops above where the item card's top usually is.
    const float limit = display.y * (1.0f - 35.0f / 720.0f) - 37 * u - 6 * u - 160 * u;
    auto rows = filter_rows(view);
    const auto box_of = [&](int i) {
        float x0 = left;
        for (int j = 0; j < i; ++j) x0 += widths[static_cast<std::size_t>(j)] + gap;
        return ImRect(ImVec2(x0, top), ImVec2(x0 + widths[static_cast<std::size_t>(i)], top + h));
    };
    // The open list's choices that have what is typed in its search field, in their order.
    std::vector<int> matches;
    const auto match = [&] {
        matches.clear();
        if (p.open < 0) return;
        const auto query = folded(p.query);
        const auto& choices = rows[static_cast<std::size_t>(p.open)].choices;
        for (std::size_t i = 0; i < choices.size(); ++i)
            if (query.empty() || folded(choices[i].label).find(query) != std::string::npos) matches.push_back(static_cast<int>(i));
    };
    match();

    // The pointer, as read once for the frame (read_pointer); the filters' only while they have
    // the keys or it is only watched, not while the search field has them.
    bool active = s.item_filters_visible.load() && !ptr.others;
    const bool use = active || ptr.watching;
    const ImVec2 mouse = use ? ptr.mouse : ImVec2(-FLT_MAX, -FLT_MAX), pressed_at = ptr.pressed_at;
    const bool released = use && ptr.released, clicked = use && ptr.clicked, moved = ptr.moved;
    const float wheel = use ? ptr.wheel : 0;
    const auto close_list = [&] {
        p.open = -1;
        p.query.clear();
        matches.clear();
    };
    const auto leave = [&] {
        s.item_filters_visible.store(false);
        close_list();
        active = false;
    };
    const auto open_list = [&](int i) {
        p.row = i;
        p.open = i;
        p.query.clear();
        p.scroll = 0;
        match();
        p.lit = static_cast<int>(std::ranges::find(matches, rows[static_cast<std::size_t>(i)].chosen) - matches.begin());
        // Characters typed before the list opened are not its search.
        io.InputQueueCharacters.resize(0);
    };
    const auto step_row = [&](int by) {
        for (int n = 0; n < 4; ++n) {
            p.row = (p.row + by + 4) % 4;
            if (rows[static_cast<std::size_t>(p.row)].enabled()) break;
        }
    };
    if (!rows[static_cast<std::size_t>(std::clamp(p.row, 0, 3))].enabled()) step_row(1);
    // The open list: its search field, then as many of its choices as fit above the card, eight at
    // most ("No matches" when none has what is typed).
    const auto list_size = [&]() {
        const int count = std::max(1, static_cast<int>(matches.size()));
        const int fit = std::max(1, static_cast<int>((limit - box_of(p.open).Max.y - gap - head - 8 * u) / line));
        return std::min({count, 8, fit});
    };
    const auto list_box = [&]() {
        const auto at = box_of(p.open);
        const float y0 = at.Max.y + gap;
        return ImRect(ImVec2(at.Min.x, y0),
                      ImVec2(at.Min.x + std::max(at.GetWidth(), 150 * scale), y0 + head + static_cast<float>(list_size()) * line + 8 * u));
    };
    const bool pointing = mouse.x > -FLT_MAX / 2;
    const auto row_under = [&](ImVec2 at) {
        for (int i = 0; i < 4; ++i)
            if (box_of(i).Contains(at)) return i;
        return -1;
    };
    const auto slot_under = [&](ImVec2 at) { return static_cast<int>(std::floor((at.y - (list_box().Min.y + head)) / line)); };

    // Keys and clicks.
    if (!active) {
        close_list();
        p.leaving = false;
        s.item_filters_back.store(0);
    }
    if (pointing) {
        const bool in_list = p.open >= 0 && list_box().Contains(mouse);
        if (in_list && moved) {
            const int slot = slot_under(mouse);
            if (slot >= 0 && slot < list_size() && p.scroll + slot < static_cast<int>(matches.size())) p.lit = p.scroll + slot;
        }
        if (in_list && wheel != 0)
            p.scroll = std::clamp(p.scroll - static_cast<int>(wheel), 0, std::max(0, static_cast<int>(matches.size()) - list_size()));
        // While only watched, a row opens when the click is let go: the game has had the whole of
        // it (on nothing of its own), and none of its buttons is left held when the keys come here.
        if (active ? clicked : released) {
            if (in_list) {
                const int slot = slot_under(mouse);
                if (slot < 0) {
                    // On the list's search field: nothing.
                } else {
                    if (slot < list_size() && p.scroll + slot < static_cast<int>(matches.size()))
                        take_choice(view, p.open, rows[static_cast<std::size_t>(p.open)].choices[static_cast<std::size_t>(matches[static_cast<std::size_t>(p.scroll + slot)])]);
                    close_list();
                }
            } else if (const int i = row_under(mouse); i >= 0 && rows[static_cast<std::size_t>(i)].enabled() &&
                                                       (active || row_under(pressed_at) == i)) {
                if (p.open == i) close_list();
                else open_list(i);
                if (!active) {
                    s.item_search_visible.store(false);
                    s.item_filters_visible.store(true);
                    active = true;
                }
            } else if (active) p.leaving = true;
        }
        if (p.leaving && released) {
            p.leaving = false;
            leave();
        }
    }
    if (active) {
        const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
        // Esc: the list's search text first, then the list, then the filters.
        for (auto back = s.item_filters_back.exchange(0); back && active; --back) {
            if (p.open >= 0 && !p.query.empty()) {
                p.query.clear();
                match();
                p.lit = p.scroll = 0;
            } else if (p.open >= 0) close_list();
            else leave();
        }
        if (active && p.open < 0) {
            io.InputQueueCharacters.resize(0);
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) leave();
            else {
                if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_UpArrow)) step_row(-1);
                if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_DownArrow)) step_row(1);
                if ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space)) &&
                    rows[static_cast<std::size_t>(p.row)].enabled())
                    open_list(p.row);
            }
        } else if (active) {
            // Typing searches the open list; every key is the list's, none the game's or a hotkey.
            const auto before = p.query;
            for (const auto c : io.InputQueueCharacters)
                if (c >= 32 && c != 127 && p.query.size() < 48) {
                    char text[5]{};
                    ImTextCharToUtf8(text, c);
                    p.query += text;
                }
            io.InputQueueCharacters.resize(0);
            if (pressed(ImGuiKey_Backspace) && !p.query.empty()) {
                // The whole of the last character, when it is more than one byte.
                auto end = p.query.size() - 1;
                while (end > 0 && (static_cast<unsigned char>(p.query[end]) & 0xC0) == 0x80) --end;
                p.query.erase(end);
            }
            if (p.query != before) {
                match();
                p.lit = p.scroll = 0;
            }
            const int count = static_cast<int>(matches.size()), page = list_size();
            if (pressed(ImGuiKey_UpArrow)) p.lit = std::max(0, p.lit - 1);
            if (pressed(ImGuiKey_DownArrow)) p.lit = std::min(count - 1, p.lit + 1);
            if (pressed(ImGuiKey_PageUp)) p.lit = std::max(0, p.lit - page);
            if (pressed(ImGuiKey_PageDown)) p.lit = std::min(count - 1, p.lit + page);
            if (pressed(ImGuiKey_Home)) p.lit = 0;
            if (pressed(ImGuiKey_End)) p.lit = count - 1;
            p.lit = std::clamp(p.lit, 0, std::max(0, count - 1));
            if (p.lit < p.scroll) p.scroll = p.lit;
            if (p.lit >= p.scroll + page) p.scroll = p.lit - page + 1;
            if ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) && count) {
                take_choice(view, p.open, rows[static_cast<std::size_t>(p.open)].choices[static_cast<std::size_t>(matches[static_cast<std::size_t>(p.lit)])]);
                close_list();
            }
        }
    }
    if (p.open >= 0) p.scroll = std::clamp(p.scroll, 0, std::max(0, static_cast<int>(matches.size()) - list_size()));

    // The controls: dark tiles like the search field, lighter under the pointer, blue-edged where
    // the keys are, as the search field is while it takes them; the name and the choice at the
    // search field's icon's inset, the arrow in the middle of the height.
    const int hovered = pointing && p.open < 0 ? row_under(mouse) : -1;
    for (int i = 0; i < 4; ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];
        const auto box = box_of(i);
        const ImVec2 a = box.Min, b = box.Max;
        const bool lit = hovered == i && row.enabled();
        skate_theme::rough_rect(draw, a, b, lit ? skate_theme::tile_grey : IM_COL32(18, 18, 19, 235), 73 + static_cast<unsigned>(i), scale);
        if (active && p.row == i)
            draw->AddRect(ImVec2(a.x - scale, a.y - scale), ImVec2(b.x + scale, b.y + scale), theme::blue, 0, 0, 2.5f * scale);
        const float name_size = 10 * scale, size = 15 * scale;
        const float name_y = a.y + (h - name_size - 3 * scale - size) * 0.5f;
        draw->AddText(heading, name_size, ImVec2(a.x + inset, name_y), theme::muted, row.name);
        const auto& value = row.choices[static_cast<std::size_t>(row.chosen)].label;
        draw->AddText(heading, size, ImVec2(a.x + inset, name_y + name_size + 3 * scale), row.enabled() ? theme::paper : theme::muted,
                      fitted(heading, size, value, box.GetWidth() - inset - 26 * scale).c_str());
        // A small down arrow: the control is a list.
        const ImVec2 arrow(b.x - inset + 1 * scale - 5 * scale, a.y + h * 0.5f);
        draw->AddTriangleFilled(ImVec2(arrow.x - 5 * scale, arrow.y - 3 * scale), ImVec2(arrow.x + 5 * scale, arrow.y - 3 * scale),
                                ImVec2(arrow.x, arrow.y + 3 * scale), row.enabled() ? theme::paper : theme::muted);
    }

    // The open list, over everything of the item browser's (the key prompts too): a search field
    // like the item search, then the choices that have what is typed; the highlighted one on a
    // lighter tile, the chosen one marked in blue at its left.
    if (p.open >= 0) {
        auto* over = ImGui::GetForegroundDrawList();
        const auto& row = rows[static_cast<std::size_t>(p.open)];
        const auto box = list_box();
        skate_theme::rough_rect(over, box.Min, box.Max, IM_COL32(18, 18, 19, 250), 79, scale);
        over->AddRect(ImVec2(box.Min.x - scale, box.Min.y - scale), ImVec2(box.Max.x + scale, box.Max.y + scale), theme::blue, 0, 0, 2.5f * scale);
        const ImRect field(ImVec2(box.Min.x + 6 * u, box.Min.y + 6 * u), ImVec2(box.Max.x - 6 * u, box.Min.y + head - 4 * u));
        skate_theme::rough_rect(over, field.Min, field.Max, IM_COL32(30, 30, 32, 255), 80, scale);
        const ImVec2 lens(field.Min.x + 14 * u, field.Min.y + field.GetHeight() * 0.46f);
        over->AddCircle(lens, 5 * u, theme::paper, 0, 1.6f * u);
        over->AddLine(ImVec2(lens.x + 3.8f * u, lens.y + 3.8f * u), ImVec2(lens.x + 7.5f * u, lens.y + 7.5f * u), theme::paper, 1.8f * u);
        const float search_size = 14 * u, text_x = lens.x + 13 * u;
        const auto shown = p.query.empty() ? std::string("Search") : p.query + (static_cast<int>(ImGui::GetTime() * 2) % 2 ? "" : "|");
        over->PushClipRect(ImVec2(text_x, field.Min.y), ImVec2(field.Max.x - 6 * u, field.Max.y), true);
        over->AddText(heading, search_size, ImVec2(text_x, field.Min.y + (field.GetHeight() - search_size) * 0.5f),
                      p.query.empty() ? theme::muted : theme::paper, shown.c_str());
        over->PopClipRect();
        const int size_shown = list_size(), count = static_cast<int>(matches.size());
        const float size = 14 * u, y0 = box.Min.y + head;
        if (!count)
            over->AddText(body, size, ImVec2(box.Min.x + 15 * u, y0 + (line - size) * 0.5f), theme::muted, "No matches");
        for (int slot = 0; slot < size_shown && p.scroll + slot < count; ++slot) {
            const int k = p.scroll + slot, i = matches[static_cast<std::size_t>(k)];
            const float y = y0 + static_cast<float>(slot) * line;
            if (k == p.lit) over->AddRectFilled(ImVec2(box.Min.x + 6 * u, y), ImVec2(box.Max.x - 6 * u, y + line), skate_theme::tile_light);
            if (i == row.chosen) over->AddRectFilled(ImVec2(box.Min.x + 6 * u, y), ImVec2(box.Min.x + 9 * u, y + line), theme::blue);
            over->AddText(heading, size, ImVec2(box.Min.x + 15 * u, y + (line - size) * 0.5f), theme::paper,
                          fitted(heading, size, row.choices[static_cast<std::size_t>(i)].label, box.GetWidth() - 36 * u).c_str());
        }
        if (count > size_shown) {
            // Where the choices in sight are in the whole list.
            const float track = static_cast<float>(size_shown) * line;
            const float bar = std::max(12 * u, track * static_cast<float>(size_shown) / static_cast<float>(count));
            const float at = y0 + (track - bar) * static_cast<float>(p.scroll) / static_cast<float>(count - size_shown);
            over->AddRectFilled(ImVec2(box.Max.x - 5 * u, at), ImVec2(box.Max.x - 2 * u, at + bar), theme::muted, 2 * u);
        }
    }
    return active;
}
} // namespace

bool take_mod_icon_pixels(CardPixels& out) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (!m.waiting) return false;
    m.waiting = false;
    out = m.current;
    return true;
}
void mod_icon_uploaded(ImTextureID texture, unsigned serial) {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    if (serial != m.serial) return;
    m.texture = texture;
    m.ready_path = m.current_path;
}
void mod_icon_lost() {
    auto& m = mod_icon();
    std::lock_guard lock(m.mutex);
    m.texture = {};
    m.ready_path.clear();
    if (!m.current.rgba.empty()) m.waiting = true;
}

bool item_browser_open() { return panel().view.open; }
void item_browser_mouse(UINT message, float x, float y) {
    auto& m = watched();
    std::lock_guard lock(m.mutex);
    m.at = ImVec2(x, y);
    if (message == WM_LBUTTONDOWN) m.pressed = m.at;
    if (message == WM_LBUTTONUP) {
        m.released = m.at;
        m.released_when = GetTickCount64();
        ++m.releases;
    }
}
bool item_browser_filters_shown() { const auto& view = panel().view; return view.open && view.picker; }

namespace {
std::array<std::atomic<float>, 4> prompts_end{}; // left, top, width, unit
void set_prompts_end(float left, float top, float width, float unit) noexcept {
    prompts_end[0].store(left, std::memory_order_relaxed);
    prompts_end[1].store(top, std::memory_order_relaxed);
    prompts_end[2].store(width, std::memory_order_relaxed);
    prompts_end[3].store(unit, std::memory_order_relaxed);
}
} // namespace

void item_browser_key(ItemBrowserKey key) {
    const auto* h = host();
    if (!h) return;
    if (key == ItemBrowserKey::favorite && h->toggle_favorite) h->toggle_favorite();
    if (key == ItemBrowserKey::filter && h->cycle_filter) h->cycle_filter();
}

void item_browser_search_cleared() {
    auto& p = panel();
    p.input.fill(0);
    if (const auto* h = host(); h && h->set_search) h->set_search("");
}

bool item_browser_pending() {
    auto& s = state();
    auto& p = panel();
    const auto* h = host();
    if (!h || !h->view) return false;
    // Every frame: the card under the game's item card follows the highlight without a lag.
    try { p.view = h->view(); } catch (...) { p.view = {}; }
    if (!p.view.open || !p.view.picker) {
        s.item_filters_visible.store(false);
        p.open = -1;
    }
    if (!p.view.open) {
        s.item_search_visible.store(false);
        p.stick_down = false;
        set_prompts_end(0, 0, 0, 0);
        return false;
    }
    // Left stick click: favorite. Read here, once a presented frame, only while a grid is open.
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    const bool stick = pad.available && (pad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    if (stick && !p.stick_down) item_browser_key(ItemBrowserKey::favorite);
    p.stick_down = stick;
    return true;
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
    const float height = 42.0f * scale, margin = 0.074f * display.y, gap = 6 * scale;
    const auto [frame_left, frame_width] = menu_frame(display);
    const float u = std::min(frame_width / 1280.0f, display.y / 720.0f);
    // The search field at the top, over the empty space right of the grid (from the item card's left
    // edge, past the grid's scrollbar), its key prompts under it; in the pickers, the filters right
    // of it, `gap` on, no further than the game's menu's right less the margin (the menu stops
    // widening at 20:9). Show is as wide as its longest choice needs.
    const float right = frame_left + frame_width - margin, left = frame_left + frame_width * 0.4525f + 12 * u;
    float show = 0;
    for (const auto& name : view.filters) show = std::max(show, heading->CalcTextSizeA(15 * scale, FLT_MAX, 0, name.c_str()).x);
    std::array<float, 4> widths{100 * scale, 100 * scale, 100 * scale, std::max(70 * scale, show + 42 * scale)};
    float filters = view.picker ? widths[0] + widths[1] + widths[2] + widths[3] + 4 * gap : 0;
    // Where the screen is too narrow for both, the filters give way down to 60% and the search to 200.
    if (const float room = right - left - 200 * scale; filters > room) {
        const float fit = std::clamp((room - 4 * gap) / (filters - 4 * gap), 0.6f, 1.0f);
        for (auto& w : widths) w *= fit;
        filters = widths[0] + widths[1] + widths[2] + widths[3] + 4 * gap;
    }
    const float width = std::clamp(right - filters - left, 120 * scale, 350.0f * scale);
    const ImVec2 at(left, margin);
    // A click on the search field gives it the keys, as "-" does (closing an open filter list
    // first); a click elsewhere while it has them leaves it, keeping what is typed, once let go.
    // While the game has the pointer the click is the game's too, on nothing of its own there.
    auto ptr = read_pointer();
    const ImRect field(at, ImVec2(at.x + width, at.y + height));
    const bool start = !s.item_search_visible.load() &&
                       ((ptr.watching && ptr.released && field.Contains(ptr.pressed_at)) || (ptr.owned && ptr.clicked)) && ptr.on(field);
    if (start) {
        p.open = -1;
        p.query.clear();
        s.item_filters_visible.store(false);
        s.item_search_visible.store(true);
        s.item_search_focus_requested.store(true);
        ptr.clicked = ptr.released = false;
    } else if (s.item_search_visible.load() && ptr.owned) {
        if (ptr.clicked && !ptr.on(field)) p.search_leaving = true;
        if (p.search_leaving && ptr.released) {
            p.search_leaving = false;
            s.item_search_visible.store(false);
        }
        ptr.clicked = ptr.released = false;
    }
    if (!s.item_search_visible.load()) p.search_leaving = false;
    const bool typing = s.item_search_visible.load();
    const float inset = 16 * scale;

    // The search field: a tile like the filters', lighter under the pointer, blue-edged while it
    // takes the keys; its magnifier at their names' inset, in the middle of the height.
    auto* draw = ImGui::GetBackgroundDrawList();
    const bool hovered = !typing && (ptr.watching || ptr.owned) && ptr.on(field);
    skate_theme::rough_rect(draw, at, field.Max, hovered ? skate_theme::tile_grey : IM_COL32(18, 18, 19, 235), 71, scale);
    if (typing) draw->AddRect(ImVec2(at.x - scale, at.y - scale), ImVec2(at.x + width + scale, at.y + height + scale), theme::blue, 0, 0, 2.5f * scale);
    const float lens_radius = 7 * scale;
    const ImVec2 lens(at.x + inset + lens_radius, at.y + height * 0.5f - 2.5f * scale);
    draw->AddCircle(lens, lens_radius, theme::paper, 0, 2.2f * scale);
    draw->AddLine(ImVec2(lens.x + 5 * scale, lens.y + 5 * scale), ImVec2(lens.x + 10 * scale, lens.y + 10 * scale), theme::paper, 2.6f * scale);
    // (No count of the items found: the grid shows them. Its right end holds the favorite star.)
    const float text_size = 16 * scale, star_radius = 11 * scale;
    const float text_x = lens.x + lens_radius + 14 * scale, text_width = at.x + width - inset - 2 * star_radius - 8 * scale - text_x;
    if (typing) {
        ImGui::SetNextWindowPos(ImVec2(text_x, at.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(text_width, height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (height - text_size) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::paper);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(1, 131, 255, 110));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::muted);
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##reskate_item_search", nullptr, flags)) {
            ImGui::PushFont(heading);
            ImGui::SetWindowFontScale(text_size / heading->FontSize);
            if (s.item_search_focus_requested.exchange(false)) {
                std::snprintf(p.input.data(), p.input.size(), "%s", view.search.c_str());
                const auto id = ImGui::GetID("##reskate_item_search_input");
                if (auto* active = ImGui::GetInputTextState(id); active && ImGui::GetActiveID() == id)
                    active->ReloadUserBufAndMoveToEnd();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = ImGui::InputTextWithHint("##reskate_item_search_input", "Search", p.input.data(),
                                                          p.input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            // Every letter filters the grid at once.
            if (h->set_search && p.pushed != p.input.data()) {
                p.pushed = p.input.data();
                h->set_search(p.pushed.c_str());
            }
            if (entered) s.item_search_visible.store(false);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
    } else {
        p.pushed = view.search;
        const float size = text_size;
        const auto text = view.search.empty() ? std::string("Search") : view.search;
        draw->PushClipRect(ImVec2(text_x, at.y), ImVec2(text_x + text_width, at.y + height), true);
        draw->AddText(heading, size, ImVec2(text_x, at.y + (height - size) * 0.5f),
                      view.search.empty() ? theme::muted : theme::paper, text.c_str());
        draw->PopClipRect();
    }

    // Key prompts, like the game's "Esc Back", on one line under the search field from its left.
    float x = at.x;
    const float y = at.y + height + 12 * scale;
    const auto prompt = [&](const char* key, const std::string& label) {
        x += theme::keycap(draw, body, scale, ImVec2(x, y), key, label.c_str()) + 22 * scale;
    };
    // The pickers' filters first: while they have the keys, the prompts are theirs.
    const bool filtering = view.picker && draw_filters(draw, heading, body, scale, ptr, widths, at.x + width + gap, margin, height, gap);
    if (typing) {
        prompt("Enter", "Done");
        prompt("Esc", "Clear");
    } else if (filtering) {
        prompt("Enter", p.open < 0 ? "Open" : "Select");
        prompt("Esc", p.open < 0 ? "Done" : "Back");
    } else {
        prompt("F", view.focused_favorite ? "Unfavorite" : "Favorite");
        prompt("-", "Search");
        prompt("X", view.filter);
        if (view.picker) prompt("V", "Filters");
    }
    if (view.focused_favorite && !typing)
        star(draw, ImVec2(at.x + width - inset - star_radius, at.y + height * 0.5f), star_radius, skate_theme::bar);
    set_prompts_end(at.x, y + 30 * scale, width, scale);
    draw_mod_card(draw, view, heading, body);
}
} // namespace dingosdk::overlay::detail

namespace dingosdk::overlay {
ItemPrompts item_browser_prompts() noexcept {
    using detail::prompts_end;
    return {prompts_end[0].load(std::memory_order_relaxed), prompts_end[1].load(std::memory_order_relaxed),
            prompts_end[2].load(std::memory_order_relaxed), prompts_end[3].load(std::memory_order_relaxed)};
}
} // namespace dingosdk::overlay
