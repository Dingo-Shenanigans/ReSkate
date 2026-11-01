#include "overlay_internal.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
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
// The question (ask_question): what it says and its answers, under the mutex; whether it is up and
// which answer is picked.
struct Question {
    std::string label, title, text, yes, no;
    std::function<void(bool)> answer;
    bool back_is_yes{};
};
Question question; // under the mutex
std::atomic<bool> question_up{}, question_yes{true};
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

void ask_question(std::string label, std::string title, std::string text, std::string yes, std::string no,
                  std::function<void(bool)> answer, bool back_is_yes) noexcept {
    try {
        std::lock_guard lock(mutex);
        question = {std::move(label), std::move(title), std::move(text), std::move(yes), std::move(no), std::move(answer), back_is_yes};
        question_yes.store(true);
        question_up.store(true);
    } catch (...) {}
}
bool question_asking() noexcept { return question_up.load(); }
bool question_back() noexcept {
    std::lock_guard lock(mutex);
    return question.back_is_yes;
}
void question_pick(bool yes) noexcept { question_yes.store(yes); }
bool question_picked() noexcept { return question_yes.load(); }
void question_answer(bool yes) noexcept {
    std::function<void(bool)> answer;
    {
        std::lock_guard lock(mutex);
        if (!question_up.exchange(false)) return;
        answer = std::move(question.answer);
        question.answer = nullptr;
    }
    try { if (answer) answer(yes); } catch (...) {}
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
bool message_pending() { return up.load() || question_up.load(); }
namespace {
// The question (ask_question), drawn as the message is, with its two answers side by side: the
// picked one blue, as the map download's card does it.
void draw_question() {
    Question shown;
    {
        std::lock_guard lock(mutex);
        shown = {question.label, question.title, question.text, question.yes, question.no, nullptr, question.back_is_yes};
    }
    auto& s = state();
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    const auto width_of = [](ImFont* font, float size, const std::string& what) { return font->CalcTextSizeA(size, FLT_MAX, 0, what.c_str()).x; };
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    const float key_scale = u * 2;
    const float inset = 56 * u, label_size = 28 * u, title_size = 64 * u, text_size = 38 * u;
    const float width = std::min(1600 * u, display.x - 200 * u), wrap = width - inset * 2;
    const float text_height = body->CalcTextSizeA(text_size, FLT_MAX, wrap, shown.text.c_str()).y;
    const float button_width = 520 * u, button_height = 96 * u, between = 30 * u;
    const float under = 34 * u + 20 * key_scale;
    const float fixed_height = inset + label_size + 20 * u + title_size + 34 * u + 48 * u + button_height + inset;
    const float max_card_height = std::max(fixed_height, display.y - under - 24 * u);
    const float text_view_height = std::min(text_height, max_card_height - fixed_height);
    const float height = fixed_height + text_view_height;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), std::max(8 * u, std::floor((display.y - height - under) * 0.5f))),
        max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 170));
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 250), 0x5e3, u * 2);
    draw->AddRectFilled(min, ImVec2(min.x + 10 * u, max.y), theme::blue);
    const float left = min.x + inset;
    float y = min.y + inset;
    draw->AddText(bold, label_size, ImVec2(left, y), theme::blue, shown.label.c_str());
    y += label_size + 20 * u;
    draw->AddText(heading, title_size, ImVec2(left, y), theme::paper, shown.title.c_str());
    y += title_size + 34 * u;
    static std::string last_text;
    static float text_scroll{};
    if (shown.text != last_text) {
        last_text = shown.text;
        text_scroll = 0;
    }
    const ImVec2 text_min(left, y), text_max(left + wrap, y + text_view_height);
    const float max_scroll = std::max(0.0f, text_height - text_view_height);
    if (max_scroll > 0) {
        if (ImGui::IsMouseHoveringRect(text_min, text_max, false))
            text_scroll = std::clamp(text_scroll - ImGui::GetIO().MouseWheel * text_size * 3, 0.0f, max_scroll);
        const bool scroll_up = (pad.available && (pad.buttons & 0x0001)) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
        const bool scroll_down = (pad.available && (pad.buttons & 0x0002)) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        static auto next_pad_scroll = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (scroll_up != scroll_down && now >= next_pad_scroll) {
            text_scroll = std::clamp(text_scroll + (scroll_up ? -text_size * 3 : text_size * 3), 0.0f, max_scroll);
            next_pad_scroll = now + std::chrono::milliseconds(100);
        } else if (!scroll_up && !scroll_down) next_pad_scroll = {};
    }
    draw->PushClipRect(text_min, text_max, true);
    draw->AddText(body, text_size, ImVec2(left, y - text_scroll), theme::paper, shown.text.c_str(), nullptr, wrap);
    draw->PopClipRect();
    if (max_scroll > 0) {
        const float track_x = max.x - 7 * u;
        const float thumb_height = std::max(12 * u, text_view_height * text_view_height / text_height);
        const float thumb_top = y + (text_view_height - thumb_height) * text_scroll / max_scroll;
        draw->AddRectFilled(ImVec2(track_x, y), ImVec2(track_x + 4 * u, y + text_view_height), IM_COL32(255, 255, 255, 55), 2 * u);
        draw->AddRectFilled(ImVec2(track_x, thumb_top), ImVec2(track_x + 4 * u, thumb_top + thumb_height), theme::blue, 2 * u);
        draw->AddText(body, 12 * u, ImVec2(left, y + text_view_height + 14 * u), theme::muted,
                      pad.available ? "D-pad or wheel to read" : "Up / Down or wheel to read");
    }
    y += text_view_height + 48 * u;
    // The two answers: the picked one blue. Pointing at one picks it, a click answers with it.
    const float row = button_width * 2 + between;
    const ImVec2 yes_min(std::floor(min.x + (width - row) * 0.5f), y), yes_max(yes_min.x + button_width, y + button_height);
    const ImVec2 no_min(yes_max.x + between, y), no_max(no_min.x + button_width, y + button_height);
    const bool over_yes = ImGui::IsMouseHoveringRect(yes_min, yes_max, false), over_no = ImGui::IsMouseHoveringRect(no_min, no_max, false);
    if (over_yes) question_pick(true);
    else if (over_no) question_pick(false);
    const bool yes = question_picked();
    const float size = 40 * u;
    const auto answer_button = [&](ImVec2 a, ImVec2 b, const std::string& text, bool picked, unsigned seed) {
        skate_theme::rough_rect(draw, a, b, picked ? theme::blue : skate_theme::tile_light, seed, u * 2);
        draw->AddText(bold, size, ImVec2(a.x + (button_width - width_of(bold, size, text)) * 0.5f, a.y + (button_height - size) * 0.5f),
                      picked ? skate_theme::black : theme::paper, text.c_str());
    };
    answer_button(yes_min, yes_max, shown.yes, yes, 0x5e4);
    answer_button(no_min, no_max, shown.no, !yes, 0x5e5);
    const auto take = pad.available ? dingosdk::controller_combo_label(0x1000, pad.style) : std::string("Enter");
    const auto back = pad.available ? dingosdk::controller_combo_label(0x2000, pad.style) : std::string("Esc");
    const auto& back_label = shown.back_is_yes ? shown.yes : shown.no;
    const float hints = width_of(bold, 12 * key_scale, take) + width_of(bold, 14 * key_scale, "Choose") + width_of(bold, 12 * key_scale, back) +
                        width_of(bold, 14 * key_scale, back_label) + 80 * key_scale;
    float x = min.x + (width - hints) * 0.5f;
    x += theme::keycap(draw, bold, key_scale, ImVec2(x, max.y + 34 * u), take.c_str(), "Choose") + 40 * key_scale;
    theme::keycap(draw, bold, key_scale, ImVec2(x, max.y + 34 * u), back.c_str(), back_label.c_str());
    if ((over_yes || over_no) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) question_answer(over_yes);
}
}
void draw_message() {
    if (!up.load()) {
        if (question_up.load()) draw_question();
        return;
    }
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
