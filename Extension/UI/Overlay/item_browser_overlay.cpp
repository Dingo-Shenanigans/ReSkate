#include "overlay_internal.h"
#include "Engine/Core/Platform/path_text.h"
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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
namespace {
struct Panel {
    ItemBrowserView view;
    std::array<char, 96> input{};
    bool stick_down{};
    std::string pushed; // the search text the host was last given
};
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
    const float width = 350.0f * scale, height = 42.0f * scale, margin = 0.074f * display.y;
    // At the right of the game's menu, which stops widening at 20:9.
    const auto [frame_left, frame_width] = menu_frame(display);
    const ImVec2 at(frame_left + frame_width - margin - width, margin);
    const bool typing = s.item_search_visible.load();

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
                std::snprintf(p.input.data(), p.input.size(), "%s", view.search.c_str());
                const auto id = ImGui::GetID("##reskate_item_search_input");
                if (auto* active = ImGui::GetInputTextState(id); active && ImGui::GetActiveID() == id)
                    active->ReloadUserBufAndMoveToEnd();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = ImGui::InputTextWithHint("##reskate_item_search_input", "Type to search", p.input.data(),
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

    // Key prompts, like the game's "Esc Back".
    float x = at.x;
    const float y = at.y + height + 12 * scale;
    const auto prompt = [&](const char* key, const std::string& label) {
        x += theme::keycap(draw, body, scale, ImVec2(x, y), key, label.c_str()) + 22 * scale;
    };
    if (typing) {
        prompt("Enter", "Done");
        prompt("Esc", "Clear");
    } else {
        prompt("F", view.focused_favorite ? "Unfavorite" : "Favorite");
        prompt("-", "Search");
        prompt("X", view.filter);
    }
    if (view.focused_favorite && !typing)
        star(draw, ImVec2(at.x - 22 * scale, at.y + height * 0.5f), 13 * scale, skate_theme::bar);
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
