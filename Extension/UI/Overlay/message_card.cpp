#include "overlay_internal.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <cmath>
#include <mutex>
#include <string>

// A message the player has to see and say OK to, in the middle of the screen: that they were
// kicked or banned from a server, and why (Runtime/client_tick.cpp shows it when a session
// ends that way). In the look of the map download's card and the vote for the next map
// (map_download_card.cpp), and answered as they are: A or B on a controller, Enter or Esc, or
// a click on OK. The overlay has the pointer while it is up, and the runtime holds the game's
// input, so the button that answers is not also a press in the game.
namespace dingosdk::overlay {
namespace {
std::mutex mutex;
std::string title_text, body_text; // under the mutex
std::atomic<bool> up{};
}

void show_message(std::string title, std::string text) noexcept {
    try {
        std::lock_guard lock(mutex);
        title_text = std::move(title);
        body_text = std::move(text);
        up.store(true);
    } catch (...) {}
}
bool message_asking() noexcept { return up.load(); }
void message_dismiss() noexcept { up.store(false); }
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
bool message_pending() { return up.load(); }
void draw_message() {
    if (!up.load()) return;
    std::string title, text;
    {
        std::lock_guard lock(mutex);
        title = title_text;
        text = body_text;
    }
    auto& s = state();
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    const auto display = ImGui::GetIO().DisplaySize;
    // (The unit the pause menu's pages are laid out in: one of the game's 3840 by 2160.)
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    const auto width_of = [](ImFont* font, float size, const std::string& what) { return font->CalcTextSizeA(size, FLT_MAX, 0, what.c_str()).x; };

    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    const auto key = pad.available ? dingosdk::controller_combo_label(0x1000, pad.style) : std::string("Enter");
    const float key_scale = u * 2;
    const float hint_width = width_of(bold, 12 * key_scale, key) + 20 * key_scale + width_of(bold, 14 * key_scale, "OK");

    const float inset = 56 * u, label_size = 28 * u, title_size = 64 * u, text_size = 38 * u;
    const float width = std::min(1500 * u, display.x - 200 * u), wrap = width - inset * 2;
    const float text_height = body->CalcTextSizeA(text_size, FLT_MAX, wrap, text.c_str()).y;
    const float button_width = 420 * u, button_height = 96 * u;
    const float height = inset + label_size + 20 * u + title_size + 34 * u + text_height + 48 * u + button_height + inset;
    const float under = 34 * u + 20 * key_scale;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), std::floor((display.y - height - under) * 0.5f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 170));
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 250), 0x5e1, u * 2);
    draw->AddRectFilled(min, ImVec2(min.x + 10 * u, max.y), skate_theme::danger);
    const float left = min.x + inset;
    float y = min.y + inset;
    draw->AddText(bold, label_size, ImVec2(left, y), skate_theme::danger, "MULTIPLAYER");
    y += label_size + 20 * u;
    draw->AddText(heading, title_size, ImVec2(left, y), theme::paper, title.c_str());
    y += title_size + 34 * u;
    draw->AddText(body, text_size, ImVec2(left, y), theme::paper, text.c_str(), nullptr, wrap);
    y += text_height + 48 * u;

    // OK: the one thing there is to press, so it is always the pick.
    const ImVec2 a(std::floor(min.x + (width - button_width) * 0.5f), y), b(a.x + button_width, a.y + button_height);
    const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
    skate_theme::rough_rect(draw, a, b, hovered ? skate_theme::blue_hover : theme::blue, 0x5e2, u * 2);
    const float ok_size = 44 * u;
    const auto ok = bold->CalcTextSizeA(ok_size, FLT_MAX, 0, "OK");
    draw->AddText(bold, ok_size, ImVec2(a.x + (button_width - ok.x) * 0.5f, a.y + (button_height - ok.y) * 0.5f), skate_theme::black, "OK");
    theme::keycap(draw, bold, key_scale, ImVec2(min.x + (width - hint_width) * 0.5f, max.y + 34 * u), key.c_str(), "OK");
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) message_dismiss();
}
} // namespace dingosdk::overlay::detail
