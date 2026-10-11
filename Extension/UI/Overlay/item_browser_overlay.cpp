#include "overlay_internal.h"
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
} // namespace

bool item_browser_open() { return panel().view.open; }

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
    if (const auto now = Clock::now(); now >= p.next_poll) {
        p.next_poll = now + std::chrono::milliseconds(50);
        try { p.view = h->view(); } catch (...) { p.view = {}; }
    }
    if (!p.view.open) {
        s.item_search_visible.store(false);
        p.stick_down = false;
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
    const ImVec2 at(display.x - margin - width, margin);
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
}
} // namespace dingosdk::overlay::detail
