#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "Extension/Skater/drone_flight.h"
#include "skate_menu.h"
#include "modding_menu.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"

#include <Windows.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <array>
#include <cstring>
#include <initializer_list>
#include <string>

namespace dingosdk::overlay {
using namespace menu;
namespace menu {
using namespace theme;

// Set once per frame in draw_skate_menu, then read through px() while the menu
// and its pages draw.
namespace { float ui_scale = default_menu_scale; }
float px(float value) { return value * ui_scale; }

// Heavy uppercase header over a thin rule, like the titles on skate.'s HUB tiles.
void section(SkateMenu& menu, const char* text) {
    ImGui::Spacing();
    ImGui::PushFont(menu.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const auto at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + px(2)), skate_theme::tile_light);
    draw->AddRectFilled(at, ImVec2(at.x + px(34), at.y + px(3)), blue);
    ImGui::Dummy(ImVec2(0, px(8)));
}
float page_body_height(const SkateMenu& menu) {
    const bool feedback_shown = !menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until;
    return std::max(80.0f, ImGui::GetContentRegionAvail().y - (feedback_shown ? px(48) : 0.0f));
}
void feedback(SkateMenu& menu, const char* text) {
    // An accepted request has nothing to say; only problems and results show.
    if (!text || !*text) return;
    menu.feedback = text;
    menu.feedback_until = ImGui::GetTime() + 6.0;
}
void send_console(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) {
        feedback(menu, "The game command dispatcher is unavailable.");
        return;
    }
    std::array<char, 512> result{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
    result.back() = 0;
    feedback(menu, result.data());
}
void debug_request(SkateMenu& menu, const CallbacksV3& callbacks, DebugRequest request) {
    std::array<char, 512> result{};
    if (!callbacks.queue_debug) return;
    const bool queued = callbacks.queue_debug(callbacks.user, request, result.data(), result.size());
    result.back() = '\0';
    feedback(menu, result[0] ? result.data() : (queued ? "" : "Change unavailable."));
}

// ---------------------------------------------------------------- full screen controls
// Full screen is made for a controller, so the shared controls draw differently there: tall
// rows, on/off as a switch, a few options as "< option >" and a number as "< value >" with a bar,
// both changed with left / right (which then never moves the focus). What the focused control
// does shows in the bottom bar (focus_hint): a controller has no hover tooltips.
namespace {
bool console_mode = false;
bool tab_switched = false; // a page or tab changed this frame (the full screen menu's tab sound)
bool console_playstation = false; // the pad in use names LT / RT as L2 / R2
std::string focus_hint, pending_hint;
ImFont* console_font = nullptr; // bold, for the controls drawn by hand
double next_step = 0; // set_console_step: the next stepper's step (0: a fortieth of its range)
// Left / right this frame. A list of options moves one per press (`repeat` off); a number, held,
// runs on after half a second, as the pause menu's sliders do. (ImGui's own repeat starts so soon
// that an unhurried press counted as two or three.)
bool sideways_pressed(std::initializer_list<ImGuiKey> keys, bool repeat) {
    for (const auto key : keys)
        if (repeat ? ImGui::GetKeyPressedAmount(key, 0.5f, 0.07f) > 0 : ImGui::IsKeyPressed(key, false)) return true;
    return false;
}
bool left_pressed(bool repeat = false) {
    return sideways_pressed({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow}, repeat);
}
bool right_pressed(bool repeat = false) {
    return sideways_pressed({ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow}, repeat);
}
bool sideways_held() {
    return ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight) ||
           ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight) ||
           ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow);
}
float held_for() {
    float longest = 0;
    for (const auto key : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                           ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
        longest = std::max(longest, ImGui::GetKeyData(key)->DownDuration);
    return longest;
}
// The row a full screen control sits on: its fill, and a white edge while focused.
void console_frame(ImDrawList* draw, ImVec2 at, ImVec2 end, bool focused, bool hovered, bool disabled) {
    // As the pause menu's tiles: rough-cut, blue under the focus or the pointer.
    skate_theme::rough_rect(draw, at, end, disabled ? IM_COL32(26, 26, 26, 160) : focused || hovered ? blue : IM_COL32(26, 26, 26, 235),
                            static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, ui_scale);
}
void arrows(ImDrawList* draw, ImVec2 at, ImVec2 end, bool lit) {
    const float h = end.y - at.y, s = px(7), mid = at.y + h * .5f;
    const ImU32 colour = lit ? paper : muted;
    draw->AddTriangleFilled(ImVec2(at.x + px(16), mid), ImVec2(at.x + px(16) + s, mid - s), ImVec2(at.x + px(16) + s, mid + s), colour);
    draw->AddTriangleFilled(ImVec2(end.x - px(16), mid), ImVec2(end.x - px(16) - s, mid - s), ImVec2(end.x - px(16) - s, mid + s), colour);
}
// The stepper behind a full screen slider. A step reports the control active and edited, and
// letting go of left / right deactivates it, so callers that save on IsItemDeactivatedAfterEdit
// (as they do when a slider is let go) save once a run of steps ends.
template<class Value>
bool stepper(const char* id, Value* value, Value low, Value high, const char* format, ImGuiSliderFlags flags) {
    ImGuiContext& g = *GImGui;
    const double unit = std::exchange(next_step, 0.0);
    const float width = ImGui::CalcItemWidth(), height = px(46);
    ImGui::PushID(id);
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##stepper", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const ImGuiID item = ImGui::GetItemID();
    const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered();
    const bool disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    // Most settings go to the game and come back a few frames later: held, each step would start
    // again from the value the game has not changed yet. So the stepped value is kept, and shown,
    // while left / right is held and for a second after; then the game's own value shows again.
    static ImGuiID kept{};
    static Value kept_value{};
    static double kept_until{};
    if (kept == item && g.Time < kept_until) *value = kept_value;
    else if (kept == item) kept = 0;
    int direction = 0;
    if (clicked && !disabled && hovered && g.IO.MouseReleased[0]) direction = g.IO.MousePos.x < at.x + width * .5f ? -1 : 1;
    const bool holding = focused && !disabled && sideways_held();
    if (focused && !disabled) {
        if (left_pressed(true)) direction = -1;
        if (right_pressed(true)) direction = 1;
        if (holding || direction) ImGui::NavMoveRequestCancel();
        if (!pending_hint.empty()) focus_hint = pending_hint;
    }
    bool changed = false;
    if (direction) {
        const bool logarithmic = (flags & ImGuiSliderFlags_Logarithmic) && low > 0 && high > low;
        const double speed = held_for() > 1.2f ? 4.0 : 1.0;
        double next = static_cast<double>(*value);
        if (unit > 0) next += direction * speed * unit;
        else if (logarithmic) next *= std::pow(static_cast<double>(high) / static_cast<double>(low), direction * speed / 40.0);
        else if constexpr (std::is_integral_v<Value>) next += direction * speed * std::max(1.0, std::round((static_cast<double>(high) - low) / 40.0));
        else next += direction * speed * (static_cast<double>(high) - low) / 40.0;
        next = std::clamp(next, static_cast<double>(low), static_cast<double>(high));
        Value stepped;
        if constexpr (std::is_integral_v<Value>) stepped = static_cast<Value>(std::lround(next));
        else stepped = static_cast<Value>(next);
        if (stepped != *value) {
            *value = stepped;
            changed = true;
        }
        kept = item, kept_value = *value, kept_until = g.Time + 1.0;
    }
    if (kept == item && sideways_held() && focused) kept_until = g.Time + 1.0;
    static ImGuiID stepping{};
    static bool stepped_edit{};
    if (direction || (holding && stepping == item)) {
        if (stepping != item) stepping = item, stepped_edit = false;
        ImGui::SetActiveID(item, g.CurrentWindow);
        stepped_edit |= changed;
        if (stepped_edit) ImGui::MarkItemEdited(item);
    } else if (stepping == item && !holding) {
        stepping = 0;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    console_frame(draw, at, end, focused, hovered, disabled);
    arrows(draw, at, end, focused);
    char text[96]{};
    ImFormatString(text, sizeof(text), format && *format ? format : (std::is_integral_v<Value> ? "%d" : "%.2f"), *value);
    ImFont* font = console_font ? console_font : ImGui::GetFont();
    const auto size = font->CalcTextSizeA(px(17), FLT_MAX, 0, text);
    draw->AddText(font, px(17), ImVec2(at.x + (width - size.x) * .5f, at.y + (height - size.y) * .5f - px(4)), disabled ? muted : paper, text);
    // Where the value sits in its range, in steps.
    double fraction = 0;
    if (high > low) {
        if ((flags & ImGuiSliderFlags_Logarithmic) && low > 0)
            fraction = std::log(static_cast<double>(*value) / low) / std::log(static_cast<double>(high) / low);
        else fraction = (static_cast<double>(*value) - low) / (static_cast<double>(high) - low);
    }
    constexpr int ticks = 20;
    const float bar_left = at.x + px(48), bar_width = width - px(96), tick = bar_width / ticks;
    for (int i = 0; i < ticks && (unit <= 0 || (static_cast<double>(high) - low) / unit < 2000); ++i) {
        const bool lit = (i + 0.5) / ticks <= std::clamp(fraction, 0.0, 1.0);
        draw->AddRectFilled(ImVec2(bar_left + tick * i + px(1), end.y - px(10)), ImVec2(bar_left + tick * (i + 1) - px(1), end.y - px(6)),
                            lit ? (focused || hovered ? paper : blue) : IM_COL32(255, 255, 255, 40));
    }
    ImGui::PopID();
    return changed;
}
}
bool console_style() { return console_mode; }
int console_step(bool focused) {
    if (!focused) return 0;
    const int direction = left_pressed() ? -1 : right_pressed() ? 1 : 0;
    if (direction || sideways_held()) ImGui::NavMoveRequestCancel();
    return direction;
}
void set_console_hint(const char* hint) { focus_hint = hint ? hint : ""; }
void set_console_step(double step) { next_step = step; }
const std::string& console_hint() { return focus_hint; }
void begin_console_frame(bool full, ImFont* bold, bool playstation) {
    console_mode = full;
    console_playstation = playstation;
    console_font = bold;
    focus_hint.clear();
    pending_hint.clear();
    next_step = 0;
}

bool toggle_row(SkateMenu& menu, const char* label, const char* hint, bool& value, bool available,
                const char* unavailable) {
    if (console_mode) {
        // Full screen: a tall row, the switch at its right end.
        ImGui::PushID(label);
        ImGui::BeginDisabled(!available);
        const auto at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = px(54);
        const bool clicked = ImGui::Selectable("##toggle", false, ImGuiSelectableFlags_None, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        if (clicked) value = !value;
        if (focused || hovered) focus_hint = hint;
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + width, at.y + height);
        console_frame(draw, at, end, focused, hovered, !available);
        const auto text = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, label);
        draw->PushClipRect(at, ImVec2(end.x - px(110), end.y), true);
        draw->AddText(menu.bold, px(17), ImVec2(at.x + px(18), at.y + (height - text.y) * .5f), available ? paper : muted, label);
        draw->PopClipRect();
        // What it is now on the right, as the pause menu's settings show it: blue, white under the focus.
        const char* state = available ? (value ? "ON" : "OFF") : unavailable;
        const auto size = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, state);
        const ImU32 state_ink = !available ? muted : focused || hovered ? paper : value ? blue : muted;
        draw->AddText(menu.bold, px(17), ImVec2(end.x - px(24) - size.x, at.y + (height - size.y) * .5f), state_ink, state);
        if (hovered) ImGui::SetTooltip("%s", hint);
        ImGui::EndDisabled();
        ImGui::PopID();
        return clicked;
    }
    ImGui::PushID(label);
    ImGui::BeginDisabled(!available);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = px(36);
    const bool clicked = ImGui::Selectable("##toggle", false, ImGuiSelectableFlags_None, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    if (clicked) value = !value;
    // A dark tile per option with an ON/OFF block, like the game's option rows.
    auto* draw = ImGui::GetWindowDrawList();
    const unsigned seed = static_cast<unsigned>(ImGui::GetItemID());
    skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height - px(3)),
        hovered ? skate_theme::tile_light : skate_theme::tile_grey, seed, ui_scale);
    if (!available) skate_theme::scribble(draw, at, ImVec2(at.x + width * 0.6f, at.y + height - px(3)), seed, ui_scale * 0.5f);
    if (focused) draw->AddRect(at, ImVec2(at.x + width, at.y + height - px(3)), paper, 0, 0, px(2));
    const auto color = available ? paper : muted;
    const float block = px(58);
    draw->PushClipRect(at, ImVec2(at.x + width - block - px(12), at.y + height), true);
    draw->AddText(menu.bold, px(16), ImVec2(at.x + px(12), at.y + px(8)), color, label);
    draw->PopClipRect();
    const char* state = available ? (value ? "ON" : "OFF") : unavailable;
    const ImVec2 box(at.x + width - block - px(6), at.y + px(6));
    const bool on = available && value;
    draw->AddRectFilled(box, ImVec2(box.x + block, at.y + height - px(9)), on ? blue : IM_COL32(0, 0, 0, 90));
    const auto text = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, state);
    draw->AddText(menu.bold, px(14), ImVec2(box.x + (block - text.x) * 0.5f, box.y + (height - px(15) - text.y) * 0.5f),
        on ? skate_theme::black : muted, state);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

namespace {
// The label column of field() and info(), and the width below which they stack.
float label_column() { return px(console_mode ? 300.0f : 180.0f); }
bool narrow() { return ImGui::GetContentRegionAvail().x < px(440); }
}
void begin_card(SkateMenu& menu, const char* id, const char* title, const char* subtitle) {
    if (console_mode) {
        // Full screen a card is a group, not a child window: ImGui leaves out a child window that is
        // all below the screen, controls and all, so the D-pad could not move down into it (and
        // scroll it into view). A group's controls can always be reached.
        ImGui::PushID(id);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, px(2)));
        if (title) {
            ImGui::PushFont(menu.heading);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            if (subtitle) {
                ImGui::SameLine(0, px(10));
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, muted);
                ImGui::TextUnformatted(subtitle);
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, px(4)));
        }
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, console_mode ? IM_COL32(0, 0, 0, 0) : skate_theme::tile);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, console_mode ? ImVec2(0, px(10)) : ImVec2(px(16), px(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    // A card never scrolls itself; the wheel keeps scrolling the page around it.
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const auto at = ImGui::GetWindowPos();
    if (!console_mode) ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + px(4), at.y + ImGui::GetWindowHeight()), blue);
    if (title) {
        ImGui::PushFont(console_mode ? menu.heading : menu.bold);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (subtitle) {
            ImGui::SameLine(0, px(10));
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, px(4)));
    }
}
void end_card() {
    if (console_mode) {
        ImGui::Dummy(ImVec2(0, px(10)));
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, px(8)));
        return;
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, px(8)));
}
void field(SkateMenu&, const char* label, const char* tooltip) {
    pending_hint = tooltip ? tooltip : "";
    const float start = ImGui::GetCursorPosX();
    const bool stacked = narrow();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    if (!stacked) ImGui::SameLine(start + label_column());
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void info(SkateMenu&, const char* label, const std::string& value) {
    const float start = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (!narrow()) ImGui::SameLine(start + label_column());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
}
void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void warn(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::warning);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
bool choice(SkateMenu& menu, const char* id, int& selected, std::initializer_list<const char*> options, bool enabled,
            float total, std::initializer_list<const char*> tooltips) {
    if (console_mode) {
        // Full screen: one row, "< option >", left / right (or A) to change it.
        ImGui::PushID(id);
        ImGui::BeginDisabled(!enabled);
        const int count = static_cast<int>(options.size());
        const float width = total > 0 ? total : ImGui::GetContentRegionAvail().x, height = px(46);
        const auto at = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##cycle", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        int step = 0;
        if (clicked && enabled) step = hovered && ImGui::GetIO().MouseReleased[0] && ImGui::GetIO().MousePos.x < at.x + width * .5f ? -1 : 1;
        if (focused && enabled) {
            if (left_pressed()) step = -1;
            if (right_pressed()) step = 1;
            if (step || sideways_held()) ImGui::NavMoveRequestCancel();
        }
        bool changed = false;
        if (step && count > 0) {
            selected = ((selected < 0 ? (step > 0 ? -1 : 0) : selected) + step + count) % count;
            changed = true;
        }
        const char* tooltip = selected >= 0 && selected < static_cast<int>(tooltips.size()) ? tooltips.begin()[selected] : nullptr;
        if (focused || hovered) focus_hint = tooltip ? tooltip : pending_hint;
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + width, at.y + height);
        console_frame(draw, at, end, focused, hovered, !enabled);
        arrows(draw, at, end, focused);
        const char* label = selected >= 0 && selected < count ? options.begin()[selected] : "Choose";
        const auto size = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, label);
        draw->AddText(menu.bold, px(17), ImVec2(at.x + (width - size.x) * .5f, at.y + (height - size.y) * .5f - px(4)),
                      enabled ? paper : muted, label);
        // Which of the options this is.
        const float dot = px(6), spacing = px(5), dots = count * dot + (count - 1) * spacing;
        for (int i = 0; i < count; ++i)
            draw->AddRectFilled(ImVec2(at.x + (width - dots) * .5f + i * (dot + spacing), end.y - px(10)),
                                ImVec2(at.x + (width - dots) * .5f + i * (dot + spacing) + dot, end.y - px(6)),
                                i == selected ? (focused || hovered ? paper : blue) : IM_COL32(255, 255, 255, 50));
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    ImGui::PushID(id);
    const auto count = static_cast<int>(options.size());
    const float gap = px(4);
    const float width = ((total > 0 ? total : ImGui::GetContentRegionAvail().x) - gap * (count - 1)) / count;
    const float height = px(32);
    auto* draw = ImGui::GetWindowDrawList();
    bool changed{};
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::InvisibleButton("##choice", ImVec2(width, height), ImGuiButtonFlags_EnableNav) && selected != i) {
            selected = i;
            changed = true;
        }
        ImGui::EndDisabled();
        const bool hovered = enabled && ImGui::IsItemHovered();
        const char* tooltip = i < static_cast<int>(tooltips.size()) ? tooltips.begin()[i] : nullptr;
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            draw->AddRect(at, ImVec2(at.x + width, at.y + height), paper, 0, 0, px(2));
        ImGui::PopID();
        const bool on = selected == i;
        const ImU32 fill = on ? (enabled ? blue : IM_COL32(1, 131, 255, 120))
                         : hovered ? skate_theme::tile_light : skate_theme::tile_grey;
        skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height), fill, static_cast<unsigned>(i + 29), ui_scale);
        const char* label = options.begin()[i];
        const auto size = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, label);
        draw->PushClipRect(at, ImVec2(at.x + width, at.y + height), true);
        draw->AddText(menu.bold, px(14), ImVec2(at.x + std::max(px(6), (width - size.x) * .5f), at.y + (height - size.y) * .5f),
            on ? skate_theme::black : enabled ? paper : muted, label);
        draw->PopClipRect();
    }
    ImGui::PopID();
    return changed;
}
void tag(SkateMenu& menu, const char* text, ImU32 colour) {
    const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, text);
    const ImVec2 pad(px(7), px(3));
    const auto at = ImGui::GetCursorScreenPos();
    // Centred on a widget row, so it lines up with text beside it that was
    // aligned to frame padding (table rows, buttons).
    const float line = ImGui::GetFrameHeight();
    const float top = at.y + std::max(0.0f, (line - size.y - pad.y * 2) * .5f);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, std::max(line, size.y + pad.y * 2)));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x + pad.x * 2, top + size.y + pad.y * 2), colour);
    draw->AddText(menu.bold, px(12), ImVec2(at.x + pad.x, top + pad.y), skate_theme::black, text);
}
bool primary_button(SkateMenu& menu, const char* label, bool enabled) {
    if (console_mode) return button_row(menu, label, {{label, enabled, true}}) == 0;
    ImGui::BeginDisabled(!enabled);
    skate_theme::push_primary_button();
    ImGui::PushFont(menu.bold);
    const bool clicked = ImGui::Button(label, ImVec2(-FLT_MIN, px(40)));
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    ImGui::EndDisabled();
    return clicked;
}

namespace {
// The number a slider's format shows ("x %.2f", "%.0f%%", "%d degrees"), as the box beside it
// types it; a format with none ("Everyone") falls back to a plain number.
std::string box_format(const char* format, bool integer) {
    for (const char* at = format; at && *at; ++at) {
        if (*at != '%') continue;
        if (at[1] == '%') { ++at; continue; }
        const char* end = at + 1;
        while (*end && !std::strchr("dfgeiu", *end)) ++end;
        if (!*end) break;
        return std::string(at, end + 1);
    }
    return integer ? "%d" : "%.2f";
}
// A slider with a number box beside it for an exact value. Drawn as one group, so a caller's
// IsItemActive / IsItemDeactivatedAfterEdit cover both: a typed value counts once the box is left.
template<class Value, class Slide, class Type>
bool slider_with_box(const char* id, Value* value, Value low, Value high, ImGuiSliderFlags flags, const char* format, const char* box,
                     Slide slide, Type type) {
    if (console_mode) {
        ImGui::BeginGroup();
        const bool changed = stepper(id, value, low, high, format, flags);
        ImGui::EndGroup();
        return changed;
    }
    next_step = 0;
    const float total = ImGui::CalcItemWidth();
    const bool boxed = !(flags & ImGuiSliderFlags_NoInput);
    const float box_width = px(78), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(boxed ? std::max(px(60), total - box_width - gap) : total);
    bool changed = slide(id, value);
    if (boxed) {
        ImGui::SameLine(0, gap);
        ImGui::SetNextItemWidth(box_width);
        Value typed = *value;
        type("##exact", &typed, box);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type an exact value");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            *value = (flags & ImGuiSliderFlags_AlwaysClamp) ? std::clamp(typed, low, high) : typed;
            changed = true;
        }
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}
}
bool slider_float(const char* id, float* value, float low, float high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, false).c_str(),
        [&](const char* label, float* v) { return ImGui::SliderFloat(label, v, low, high, format, flags); },
        [](const char* label, float* v, const char* box) { return ImGui::InputFloat(label, v, 0, 0, box); });
}
bool slider_int(const char* id, int* value, int low, int high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, true).c_str(),
        [&](const char* label, int* v) { return ImGui::SliderInt(label, v, low, high, format, flags); },
        [](const char* label, int* v, const char*) { return ImGui::InputInt(label, v, 0, 0); });
}
bool slider_double(const char* id, double* value, double low, double high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, false).c_str(),
        [&](const char* label, double* v) { return ImGui::SliderScalar(label, ImGuiDataType_Double, v, &low, &high, format, flags); },
        [](const char* label, double* v, const char* box) { return ImGui::InputDouble(label, v, 0, 0, box); });
}

// ---------------------------------------------------------------- rows for a controller
int button_row(SkateMenu& menu, const char* id, std::initializer_list<RowButton> buttons) {
    int pressed = -1;
    ImGui::PushID(id);
    if (!console_mode) {
        // A window keeps ImGui's buttons, side by side at their own width.
        int i = 0;
        for (const auto& button : buttons) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::BeginDisabled(!button.enabled);
            if (button.on) ImGui::PushStyleColor(ImGuiCol_Button, blue);
            if (ImGui::Button(button.label)) pressed = i;
            if (button.on) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (button.hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", button.hint);
            ImGui::PopID();
            ++i;
        }
        ImGui::PopID();
        return pressed;
    }
    // Full screen: one row of equal tiles as the pause menu's buttons, blue under the focus, white
    // for the one that is on.
    const int count = static_cast<int>(buttons.size());
    const float gap = px(10), height = px(50);
    const float width = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    auto* draw = ImGui::GetWindowDrawList();
    int i = 0;
    for (const auto& button : buttons) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!button.enabled);
        if (ImGui::InvisibleButton("##button", ImVec2(width, height), ImGuiButtonFlags_EnableNav)) pressed = i;
        ImGui::EndDisabled();
        const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, hovered = button.enabled && ImGui::IsItemHovered();
        if ((focused || hovered) && button.hint) focus_hint = button.hint;
        const ImVec2 end(at.x + width, at.y + height);
        const bool lit = button.enabled && (focused || hovered);
        const ImU32 fill = lit ? blue : button.on ? paper : button.enabled ? IM_COL32(26, 26, 26, 235) : IM_COL32(26, 26, 26, 140);
        skate_theme::rough_rect(draw, at, end, fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, ui_scale);
        const char* shown_end = ImGui::FindRenderedTextEnd(button.label);
        const auto size = menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, button.label, shown_end);
        draw->PushClipRect(at, end, true);
        draw->AddText(menu.bold, px(16), ImVec2(at.x + std::max(px(10), (width - size.x) * .5f), at.y + (height - size.y) * .5f),
                      !button.enabled ? muted : button.on && !lit ? skate_theme::black : paper, button.label, shown_end);
        draw->PopClipRect();
        ImGui::PopID();
        ++i;
    }
    ImGui::PopID();
    return pressed;
}

bool option_row(SkateMenu& menu, const char* label, const char* hint, int& selected, const std::vector<std::string>& options, bool enabled,
                bool* pressed) {
    const int count = static_cast<int>(options.size());
    // "Name###id": the name is shown, the id names the control (so a name can change under the focus).
    const char* id = std::strstr(label, "###");
    const std::string name(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PushID(id ? id : label);
    bool changed = false;
    if (!console_mode) {
        field(menu, name.c_str(), hint);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::BeginCombo("##option", selected >= 0 && selected < count ? options[static_cast<std::size_t>(selected)].c_str() : "Choose")) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(options[static_cast<std::size_t>(i)].c_str(), i == selected) && i != selected) selected = i, changed = true;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    // Full screen: the name on the left, "< option >" on the right; left / right change it, one
    // per press. A steps on too, or, for a row that applies on A (`pressed`), applies it.
    ImGui::BeginDisabled(!enabled);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(54);
    const bool clicked = ImGui::InvisibleButton("##option", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool focused = ImGui::IsItemFocused(), hovered = enabled && ImGui::IsItemHovered();
    const float split = at.x + width * .48f;
    int step = 0;
    const bool mouse = clicked && ImGui::GetIO().MouseReleased[0] && hovered;
    if (enabled && clicked) {
        if (mouse && ImGui::GetIO().MousePos.x >= split) step = ImGui::GetIO().MousePos.x < split + (at.x + width - split) * .5f ? -1 : 1;
        else if (pressed) *pressed = true;
        else step = 1;
    }
    if (focused && enabled) {
        if (left_pressed()) step = -1;
        if (right_pressed()) step = 1;
        if (step || sideways_held()) ImGui::NavMoveRequestCancel();
    }
    if (step && count > 0) {
        selected = ((selected < 0 ? (step > 0 ? -1 : 0) : selected) + step + count) % count;
        changed = true;
    }
    if ((focused && ImGui::GetIO().NavVisible) || hovered) focus_hint = hint ? hint : "";
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    const bool lit = (focused && ImGui::GetIO().NavVisible) || hovered;
    console_frame(draw, at, end, lit, false, !enabled);
    const auto text = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, name.c_str());
    draw->PushClipRect(at, ImVec2(split - px(10), end.y), true);
    draw->AddText(menu.bold, px(17), ImVec2(at.x + px(18), at.y + (height - text.y) * .5f), enabled ? paper : muted, name.c_str());
    draw->PopClipRect();
    arrows(draw, ImVec2(split, at.y), end, lit);
    const char* value = selected >= 0 && selected < count ? options[static_cast<std::size_t>(selected)].c_str() : "Choose";
    const float room = end.x - split - px(80);
    const auto size = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, value);
    draw->PushClipRect(ImVec2(split + px(36), at.y), ImVec2(end.x - px(36), end.y), true);
    draw->AddText(menu.bold, px(17), ImVec2(split + px(40) + std::max(0.0f, (room - size.x) * .5f), at.y + (height - size.y) * .5f - (count <= 12 ? px(4) : 0)),
                  !enabled ? muted : lit ? paper : blue, value);
    draw->PopClipRect();
    if (count > 1 && count <= 12) {
        const float dot = px(6), spacing = px(5), dots = count * dot + (count - 1) * spacing, left = split + (end.x - split - dots) * .5f;
        for (int i = 0; i < count; ++i)
            draw->AddRectFilled(ImVec2(left + i * (dot + spacing), end.y - px(10)), ImVec2(left + i * (dot + spacing) + dot, end.y - px(6)),
                                i == selected ? (lit ? paper : blue) : IM_COL32(255, 255, 255, 50));
    }
    ImGui::PopID();
    return changed;
}

bool list_row(SkateMenu& menu, const char* id, const std::string& text, const std::string& side, bool current, bool enabled) {
    ImGui::PushID(id);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(48);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool lit = enabled && ((ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) || ImGui::IsItemHovered());
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    skate_theme::rough_rect(draw, at, end, lit ? blue : current ? paper : IM_COL32(26, 26, 26, 200), static_cast<unsigned>(ImGui::GetItemID()) & 0xfff,
                            ui_scale);
    const ImU32 text_ink = lit ? paper : current ? skate_theme::black : enabled ? paper : muted;
    const auto side_size = menu.body->CalcTextSizeA(px(15), FLT_MAX, 0, side.c_str());
    draw->PushClipRect(at, ImVec2(end.x - side_size.x - px(36), end.y), true);
    draw->AddText(menu.bold, px(17), ImVec2(at.x + px(18), at.y + (height - px(17)) * .5f), text_ink, text.c_str());
    draw->PopClipRect();
    draw->AddText(menu.body, px(15), ImVec2(end.x - side_size.x - px(18), at.y + (height - side_size.y) * .5f),
                  lit ? paper : current ? IM_COL32(40, 40, 40, 255) : muted, side.c_str());
    ImGui::PopID();
    return clicked && enabled;
}

void category_tabs(SkateMenu& menu, int& selected, std::initializer_list<const char*> tabs, const char* id) {
    // A row of tiles like skate.'s tab bar: the selected tab is blue with black text.
    selected = std::clamp(selected, 0, static_cast<int>(tabs.size()) - 1);
    // LT / RT on a controller step the page's first row of tabs (the one drawn first each frame),
    // not one nested in it.
    static int stepped_frame = -1;
    const bool primary = stepped_frame != ImGui::GetFrameCount();
    if (stepped_frame != ImGui::GetFrameCount()) {
        stepped_frame = ImGui::GetFrameCount();
        const int count = static_cast<int>(tabs.size());
        if (!ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput && !menu.recording_bind && !menu.voice_bind_capture) {
            const int was = selected;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) selected = (selected + count - 1) % count;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) selected = (selected + 1) % count;
            if (selected != was) menu.feedback.clear(), tab_switched = true;
        }
    }
    // Full screen, the page's first row of tabs looks as the pause menu's do: rough tiles, the open one
    // white with the game's blue line under it, LT and RT at the ends (they step through them).
    if (primary && menu.fullscreen_drawn) {
        ImGui::PushID(id);
        auto* draw = ImGui::GetWindowDrawList();
        // In the pause menu's own units (hub_page.cpp), so these are the ONLINE page's tabs exactly.
        const auto display = ImGui::GetIO().DisplaySize;
        const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
        const float height = 84 * u, size = 44 * u;
        const auto start = ImGui::GetCursorScreenPos();
        float x = start.x;
        const auto cap = [&](const char* name) {
            x += keycap(draw, menu.bold, u * 2.2f, ImVec2(x, start.y + 20 * u), name, "") + 4 * u;
        };
        cap(console_playstation ? "L2" : "LT");
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const char* label = tabs.begin()[i];
            const float width = menu.bold->CalcTextSizeA(size, FLT_MAX, 0, label).x + 56 * u;
            ImGui::SetCursorScreenPos(ImVec2(x, start.y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##section", ImVec2(width, height), ImGuiButtonFlags_EnableNav) && selected != i) {
                selected = i;
                menu.feedback.clear();
                tab_switched = true;
            }
            const bool lit = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
            const unsigned seed = static_cast<unsigned>(ImGui::GetItemID());
            ImGui::PopID();
            const bool on = selected == i;
            const ImVec2 p(x, start.y), q(x + width, start.y + height);
            skate_theme::rough_rect(draw, p, q, lit && !on ? blue : on ? paper : IM_COL32(26, 26, 26, 200), seed & 0xfff, u * 2);
            draw->AddText(menu.bold, size, ImVec2(p.x + 28 * u, p.y + (height - size) * .5f), on ? skate_theme::black : paper, label);
            if (on) draw->AddRectFilled(ImVec2(p.x + 6 * u, q.y + 4 * u), ImVec2(q.x - 6 * u, q.y + 14 * u), blue, 5 * u);
            x = q.x + 20 * u;
        }
        cap(console_playstation ? "R2" : "RT");
        ImGui::PopID();
        // The page under them where the ONLINE page's starts.
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (84 + 64) * u));
        ImGui::Dummy(ImVec2(0, 0));
        return;
    }
    ImGui::PushID(id);
    const float gap = px(6);
    const float tab_width = (ImGui::GetContentRegionAvail().x - gap * (tabs.size() - 1)) / tabs.size();
    const float height = px(34);
    auto* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_width, height), ImGuiButtonFlags_EnableNav) && selected != i) {
            selected = i;
            menu.feedback.clear();
        }
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            draw->AddRect(at, ImVec2(at.x + tab_width, at.y + height), paper, 0, 0, px(2));
        ImGui::PopID();
        const bool on = selected == i;
        skate_theme::rough_rect(draw, at, ImVec2(at.x + tab_width, at.y + height),
            on ? blue : hovered ? skate_theme::tile_light : skate_theme::tile_grey, static_cast<unsigned>(i + 11), ui_scale);
        const char* label = tabs.begin()[i];
        const auto size = menu.bold->CalcTextSizeA(px(15), FLT_MAX, 0, label);
        draw->PushClipRect(at, ImVec2(at.x + tab_width, at.y + height), true);
        draw->AddText(menu.bold, px(15), ImVec2(at.x + std::max(px(6), (tab_width - size.x) * .5f), at.y + (height - size.y) * .5f),
            on ? skate_theme::black : paper, label);
        draw->PopClipRect();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, px(6)));
}
}

namespace {
// The menu fonts are built into ReSkate.dll (External/fonts, shared with the
// launcher); Windows fonts are the fallback.
ImFont* embedded_font(const wchar_t* name, float size, const ImWchar* ranges) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&embedded_font), &module)) return nullptr;
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // the resource lives as long as the DLL
    config.OversampleH = size >= 32 ? 1 : 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(module, resource)), size,
        &config, ranges);
}

constexpr std::array<const char*, page_count> page_names{
    "HOME", "MAP", "BUILD", "SKATER", "SANDBOX", "ADDONS", "ONLINE", "PROGRESS", "MODS", "SETTINGS", "DEVELOPER"};
constexpr std::array<const char*, page_count> page_subtitles{
    "Everything, a button away.", "Pick your spot. Set the vibe.", "Make the park yours.", "Ride it your way.",
    "Tune it. Drill it.", "What ReSkate adds to skate.", "Bring your crew.", "Pick up where you want.",
    "Bring your own.", "Your controls, your screen.", "Under the hood."};
constexpr std::array<const char*, addon_count> addon_names{"ADDONS", "DRONE", "BOARD BREAK", "ROAD RASH", "HALL OF MEAT", "GRIND EXITS", "MUSIC"};
constexpr std::array<const char*, addon_count> addon_subtitles{
    "What ReSkate adds to skate.", "Take it to the sky.", "Slams snap your board.", "Bails leave their mark.", "Every bone counts.", "Pick the side you pop out.",
    "Your sound, in the game."};
// The side rail's groups: the heading above the page that starts each.
const char* group_of(int page) {
    switch (page) {
    case page_map: return "PLAY";
    case page_skater: return "RIDE";
    case page_addons: return "EXTRAS";
    case page_online: return "CREW";
    case page_mods: return "SYSTEM";
    default: return nullptr;
    }
}
}

void load_skate_fonts(SkateMenu& menu) {
    auto* atlas = ImGui::GetIO().Fonts;
    atlas->TexGlyphPadding = 2;
    // Latin, Cyrillic and general punctuation cover player and lobby names.
    static const ImWchar ranges[]{0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2190, 0x21FF, 0};
    std::array<char, MAX_PATH> windows{};
    const auto length = GetWindowsDirectoryA(windows.data(), static_cast<UINT>(windows.size()));
    const auto font = [&](const wchar_t* resource, const char* filename, float size, const ImWchar* glyphs = ranges) -> ImFont* {
        if (resource)
            if (auto* loaded = embedded_font(resource, size, glyphs)) return loaded;
        if (length && length < windows.size()) {
            const auto path = std::string(windows.data()) + "\\Fonts\\" + filename;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                if (auto* loaded = atlas->AddFontFromFileTTF(path.c_str(), size)) return loaded;
        }
        ImFontConfig config;
        config.SizePixels = size;
        return atlas->AddFontDefault(&config);
    };
    // Every role has a Windows font and then ImGui's own font as fallbacks.
    atlas->AddFontDefault();
    menu.body = font(L"FONT_BODY", "arial.ttf", 17);
    menu.bold = font(L"FONT_HEADING", "arialbd.ttf", 17);
    menu.heading = font(L"FONT_HEADING", "arialbd.ttf", 22);
    menu.title = font(L"FONT_BRUSH", "arialbi.ttf", 44);
    menu.mono = font(nullptr, "consola.ttf", 15);
    // Full screen's larger set. The brushed titles are page and section names: plain Latin is enough.
    static const ImWchar latin[]{0x0020, 0x00FF, 0};
    menu.body_large = font(L"FONT_BODY", "arial.ttf", 17 * large_font_ratio);
    menu.bold_large = font(L"FONT_HEADING", "arialbd.ttf", 17 * large_font_ratio);
    menu.heading_large = font(L"FONT_HEADING", "arialbd.ttf", 22 * large_font_ratio);
    menu.title_large = font(L"FONT_BRUSH", "arialbi.ttf", 44 * large_font_ratio, latin);
}

namespace menu {
bool menu_fullscreen(const SkateMenu& menu, const Model& model) { return menu.fullscreen_pending.value_or(model.menu_fullscreen); }
void set_menu_fullscreen(SkateMenu& menu, const CallbacksV3& callbacks, bool fullscreen) {
    menu.fullscreen_pending = fullscreen;
    menu.fullscreen_until = ImGui::GetTime() + 3.0;
    if (!callbacks.queue_console_command) return;
    std::array<char, 256> result{};
    callbacks.queue_console_command(callbacks.user, fullscreen ? "ui fullscreen 1" : "ui fullscreen 0", result.data(), result.size());
}
void open_page(SkateMenu& menu, int page, int addon) {
    if (menu.page != page || (addon >= 0 && addon != menu.addon)) tab_switched = true;
    if (menu.page != page) {
        menu.recording_bind = 0;
        menu.voice_bind_capture = 0;
        menu.feedback.clear();
    }
    menu.page = page;
    if (addon >= 0) menu.addon = addon;
}
}

namespace {
bool page_hidden(const SkateMenu& menu, const Model& model, int page) {
    // Developer (the network view) is hidden for now; offline there is no Online. HOME is the
    // full screen menu's: a window has its side rail instead.
    return page == page_developer || (page == page_home && !menu.fullscreen_drawn) || (model.steam_offline && page == page_online);
}
const char* page_title(const SkateMenu& menu) {
    return menu.page == page_addons && menu.addon > addon_all ? addon_names[static_cast<std::size_t>(menu.addon)] : page_names[static_cast<std::size_t>(menu.page)];
}
const char* page_subtitle(const SkateMenu& menu) {
    return menu.page == page_addons ? addon_subtitles[static_cast<std::size_t>(menu.addon)] : page_subtitles[static_cast<std::size_t>(menu.page)];
}
void step_page(SkateMenu& menu, const Model& model, int step) {
    int page = menu.page;
    for (int tries = 0; tries < page_count; ++tries) {
        page = (page + step + page_count) % page_count;
        if (!page_hidden(menu, model, page)) break;
    }
    open_page(menu, page);
}

// The brushed title, tilted like the HUB's, with its subtitle beside it.
void brushed_title(ImDrawList* draw, SkateMenu& menu, ImVec2 at, const char* text, const char* subtitle, float size) {
    const int start = draw->VtxBuffer.Size;
    draw->AddText(menu.title, size, ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), text);
    draw->AddText(menu.title, size, at, paper, text);
    const auto extent = menu.title->CalcTextSizeA(size, FLT_MAX, 0, text);
    skate_theme::rotate_since(draw, start, -3.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
    if (subtitle) draw->AddText(menu.body, px(14), ImVec2(at.x + extent.x + px(18), at.y + size * 0.5f), muted, subtitle);
}

// A controller button as the pad in use shows it: Xbox's coloured letters, or PlayStation's shapes
// (drawn: the menu font has no ✕ ○ □ △), and the shoulders, triggers and middle buttons by name.
enum class Glyph { a, b, x, y, lb, rb, lt, rt, view, rs };
float glyph(ImDrawList* draw, SkateMenu& menu, ImVec2 at, Glyph which, bool playstation) {
    const float size = px(22);
    const ImVec2 centre(at.x + size * .5f, at.y + size * .5f);
    const auto face = [&](ImU32 xbox_fill, const char* letter, ImU32 shape) {
        if (!playstation) {
            draw->AddCircleFilled(centre, size * .5f, xbox_fill);
            const auto text = menu.bold->CalcTextSizeA(px(13), FLT_MAX, 0, letter);
            draw->AddText(menu.bold, px(13), ImVec2(centre.x - text.x * .5f, centre.y - text.y * .5f), skate_theme::black, letter);
            return size;
        }
        draw->AddCircleFilled(centre, size * .5f, IM_COL32(32, 32, 34, 255));
        const float r = size * .24f, w = px(2);
        switch (which) {
        case Glyph::a:
            draw->AddLine(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), shape, w);
            draw->AddLine(ImVec2(centre.x - r, centre.y + r), ImVec2(centre.x + r, centre.y - r), shape, w);
            break;
        case Glyph::b: draw->AddCircle(centre, r, shape, 0, w); break;
        case Glyph::x: draw->AddRect(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), shape, 0, 0, w); break;
        default:
            draw->AddTriangle(ImVec2(centre.x, centre.y - r * 1.1f), ImVec2(centre.x - r * 1.1f, centre.y + r * .8f),
                              ImVec2(centre.x + r * 1.1f, centre.y + r * .8f), shape, w);
            break;
        }
        return size;
    };
    switch (which) {
    case Glyph::a: return face(IM_COL32(95, 211, 106, 255), "A", IM_COL32(159, 185, 255, 255));
    case Glyph::b: return face(IM_COL32(255, 107, 107, 255), "B", IM_COL32(255, 154, 168, 255));
    case Glyph::x: return face(IM_COL32(77, 163, 255, 255), "X", IM_COL32(240, 166, 224, 255));
    case Glyph::y: return face(IM_COL32(255, 210, 77, 255), "Y", IM_COL32(127, 224, 200, 255));
    default: break;
    }
    static constexpr std::array<const char*, 6> xbox{"LB", "RB", "LT", "RT", "VIEW", "RS"};
    static constexpr std::array<const char*, 6> sony{"L1", "R1", "L2", "R2", "CREATE", "R"};
    const auto index = static_cast<std::size_t>(which) - static_cast<std::size_t>(Glyph::lb);
    const char* name = (playstation ? sony : xbox)[index];
    const auto text = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, name);
    const float width = text.x + px(14);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + size), IM_COL32(45, 45, 47, 255), px(4));
    draw->AddText(menu.bold, px(12), ImVec2(at.x + px(7), at.y + (size - text.y) * .5f), paper, name);
    return width;
}
// Buttons, then what they do, as one prompt. Returns the width drawn.
float prompt(ImDrawList* draw, SkateMenu& menu, ImVec2 at, std::initializer_list<Glyph> glyphs, const char* label, bool playstation) {
    float x = at.x;
    for (const auto which : glyphs) x += glyph(draw, menu, ImVec2(x, at.y), which, playstation) + px(4);
    draw->AddText(menu.bold, px(14), ImVec2(x + px(4), at.y + px(3)), paper, label);
    return x + px(4) + menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, label).x - at.x;
}

void draw_page_body(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    switch (menu.page) {
    case page_home: home_page(menu, model, callbacks); break;
    case page_map: map_page(menu, model, callbacks); break;
    case page_build: build_page(menu, model, callbacks); break;
    case page_skater: skater_page(menu, model, callbacks); break;
    case page_sandbox: trainer_page(menu, model, callbacks); break;
    case page_addons: addons_page(menu, model, callbacks); break;
    case page_online:
        if (console_style()) {
            const auto at = ImGui::GetCursorScreenPos();
            const auto room = ImGui::GetContentRegionAvail();
            detail::draw_online_embedded(ImGui::GetWindowDrawList(), at, ImVec2(at.x + room.x, at.y + room.y), model, callbacks, true);
            ImGui::Dummy(room);
        } else {
            multiplayer_page(menu, model, callbacks);
        }
        break;
    case page_progress: progression_page(menu, model, callbacks); break;
    case page_mods:
        // The SCRIPTS tab (custom Lua scripts, modding_menu.cpp) is not offered for now:
        // nothing uses them. The page is the installed mods.
        menu.mods_tab = 0;
        ImGui::PushID(menu.mods_tab);
        ImGui::BeginChild("mods-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
        draw_modding_menu(menu, menu.mods_tab, model, callbacks);
        ImGui::EndChild();
        ImGui::PopID();
        break;
    case page_settings: settings_page(menu, model, callbacks); break;
    case page_developer: developer_page(menu, model, callbacks); break;
    }
    if (menu.rail_indent > 0) {
        ImGui::Unindent(menu.rail_indent);
        menu.rail_indent = 0;
    }
    if (!menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until) {
        // The feedback line sits in a dark strip with a blue edge, like the game's toasts.
        ImGui::Spacing();
        const bool debug_page = menu.page == page_skater || menu.page == page_sandbox || (menu.page == page_settings && menu.settings_tab == 1);
        const auto& result = debug_page && !model.debug.status.empty() && ImGui::GetTime() > menu.feedback_until - 5.75 ? model.debug.status
                                                                                                                       : menu.feedback;
        const auto at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float text_height = ImGui::CalcTextSize(result.c_str(), nullptr, false, width - px(24)).y;
        auto* page = ImGui::GetWindowDrawList();
        page->AddRectFilled(at, ImVec2(at.x + width, at.y + text_height + px(12)), skate_theme::tile);
        page->AddRectFilled(at, ImVec2(at.x + px(4), at.y + text_height + px(12)), blue);
        ImGui::SetCursorScreenPos(ImVec2(at.x + px(14), at.y + px(6)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - px(24));
        ImGui::TextUnformatted(result.c_str());
        ImGui::PopTextWrapPos();
    }
}

void wordmark(ImDrawList* draw, SkateMenu& menu, ImVec2 at, float size) {
    const int start = draw->VtxBuffer.Size;
    draw->AddText(menu.title, size, ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), "RESKATE");
    draw->AddText(menu.title, size, at, paper, "RESKATE");
    const auto extent = menu.title->CalcTextSizeA(size, FLT_MAX, 0, "RESKATE");
    skate_theme::rotate_since(draw, start, -4.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
}

// A window over the game, moved by dragging and resized from its edges: made for a mouse. The
// side rail lists the pages in their groups, and every addon in EXTRAS in place of an ADDONS page:
// the window has no overview of them, full screen's ALL ADDONS tab is that.
void draw_windowed(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool& visible, bool appearing) {
    auto& io = ImGui::GetIO();
    const ImVec2 maximum(std::max(px(400.0f), io.DisplaySize.x - 24), std::max(px(320.0f), io.DisplaySize.y - 24));
    ImGui::SetNextWindowSize(ImVec2(std::min(px(1000.0f), maximum.x), std::min(px(700.0f), maximum.y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_FirstUseEver, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(px(680.0f), maximum.x), std::min(px(480.0f), maximum.y)), maximum);
    if (appearing) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("ReSkate###skate-menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        const auto size = ImGui::GetWindowSize();
        const auto current = ImGui::GetWindowPos();
        const ImVec2 bounded(std::clamp(current.x, 0.0f, std::max(0.0f, io.DisplaySize.x - size.x)),
            std::clamp(current.y, 0.0f, std::max(0.0f, io.DisplaySize.y - size.y)));
        if (bounded.x != current.x || bounded.y != current.y) ImGui::SetWindowPos(bounded);
        const auto origin = ImGui::GetWindowPos();
        auto* draw = ImGui::GetWindowDrawList();
        const float sidebar = px(190.0f);
        if (menu.page == page_addons && menu.addon == addon_all) menu.addon = addon_drone;

        // Side rail: brushed wordmark, then the pages in their groups like the HUB's tab icons.
        draw->AddRectFilled(origin, ImVec2(origin.x + sidebar, origin.y + size.y), IM_COL32(10, 10, 11, 250));
        wordmark(draw, menu, ImVec2(origin.x + px(16), origin.y + px(14)), px(40));
        draw->AddText(menu.body, px(12), ImVec2(origin.x + px(18), origin.y + px(66)), muted, "YOUR SESSION. YOUR RULES.");
        ImGui::SetCursorPos(ImVec2(px(10), px(90)));
        const float rail_height = size.y - px(134);
        ImGui::BeginChild("navigation", ImVec2(sidebar - px(20), rail_height), ImGuiChildFlags_NavFlattened);
        const float rail = sidebar - px(20);
        // Every row is as tall as fits the rail without scrolling, from 28 to 36.
        float row_height = px(36);
        {
            int rows = 0, groups = 0;
            for (int i = 0; i < page_count; ++i) {
                if (page_hidden(menu, model, i)) continue;
                rows += i == page_addons ? addon_count - 1 : 1;
                if (group_of(i)) ++groups;
            }
            const float spacing = ImGui::GetStyle().ItemSpacing.y;
            ImGui::PushFont(menu.bold);
            const float heading = px(2) + spacing + ImGui::GetTextLineHeightWithSpacing();
            ImGui::PopFont();
            if (rows) row_height = std::clamp((rail_height - groups * heading) / rows - spacing, px(28), px(36));
        }
        const auto row = [&](const char* id, const char* label, bool selected, float height, float indent, ImFont* font, float text) {
            const auto at = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(rail, height), ImGuiButtonFlags_EnableNav) && !selected;
            const bool hovered = ImGui::IsItemHovered();
            auto* nav = ImGui::GetWindowDrawList();
            const ImVec2 end(at.x + rail, at.y + height - px(3));
            if (selected || hovered)
                skate_theme::rough_rect(nav, ImVec2(at.x + indent, at.y), end, selected ? blue : skate_theme::tile_grey,
                                        static_cast<unsigned>(ImGui::GetItemID()), px(1));
            if (ImGui::IsItemFocused() && io.NavVisible) nav->AddRect(at, end, paper, 0, 0, px(2));
            nav->AddText(font, text, ImVec2(at.x + indent + px(12), at.y + (height - px(3) - text) * .5f),
                selected ? skate_theme::black : paper, label);
            return pressed;
        };
        for (int i = 0; i < page_count; ++i) {
            if (page_hidden(menu, model, i)) continue;
            if (const char* group = group_of(i)) {
                ImGui::Dummy(ImVec2(0, px(2)));
                ImGui::PushStyleColor(ImGuiCol_Text, muted);
                ImGui::PushFont(menu.bold);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(10));
                ImGui::TextUnformatted(group);
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
            ImGui::PushID(i);
            if (i == page_addons) {
                for (int a = addon_drone; a < addon_count; ++a) {
                    ImGui::PushID(a);
                    if (row("##addon", addon_names[static_cast<std::size_t>(a)], menu.page == page_addons && menu.addon == a, row_height, 0,
                            menu.bold, px(16)))
                        open_page(menu, page_addons, a);
                    ImGui::PopID();
                }
            } else if (row("##page", page_names[static_cast<std::size_t>(i)], menu.page == i, row_height, 0, menu.bold, px(16))) {
                open_page(menu, i, -1);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        {
            static const auto label = dingosdk::launcher::key_cap(dingosdk::launcher::overlay_keys().menu);
            keycap(draw, menu.bold, px(1), ImVec2(origin.x + px(16), origin.y + size.y - px(34)), label.c_str(), "Close");
        }

        brushed_title(draw, menu, ImVec2(origin.x + sidebar + px(22), origin.y + px(12)), page_title(menu), page_subtitle(menu), px(44));
        // Close, and beside it the full screen menu.
        const float switch_width = ImGui::CalcTextSize("SWITCH TO FULLSCREEN").x + px(20);
        ImGui::SetCursorPos(ImVec2(size.x - px(44) - px(6) - switch_width, px(16)));
        if (ImGui::Button("SWITCH TO FULLSCREEN##fullscreen", ImVec2(switch_width, px(28)))) set_menu_fullscreen(menu, callbacks, true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Full screen, made for a controller (F11)");
        ImGui::SameLine(0, px(6));
        if (ImGui::Button("X", ImVec2(px(28), px(28)))) visible = false;
        ImGui::SetCursorPos(ImVec2(sidebar + px(20), px(76)));
        ImGui::BeginChild("page", ImVec2(size.x - sidebar - px(40), size.y - px(96)), ImGuiChildFlags_NavFlattened);
        draw_page_body(menu, model, callbacks);
        ImGui::EndChild();
    }
    ImGui::End();
}

// Full screen, scrolling a page by pad: the right stick scrolls it, and up / down past its first or
// last control scrolls on to what is above or below (text, a disabled control) that the focus
// cannot land on. The page is the smallest scrolling part of the menu that holds the focus.
void pad_scroll(ImGuiWindow* root) {
    ImGuiContext& g = *GImGui;
    ImRect focus;
    const bool has_focus = g.NavWindow && g.NavId && g.NavWindow->RootWindow == root;
    if (has_focus) focus = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
    ImGuiWindow* page = nullptr;
    for (ImGuiWindow* window : g.Windows) {
        if (!window->Active || window->RootWindow != root || window == root || window->ScrollMax.y <= 0) continue;
        const bool holds = !has_focus || window->InnerRect.Contains(focus.GetCenter());
        if (holds && (!page || window->InnerRect.GetHeight() < page->InnerRect.GetHeight())) page = window;
    }
    // An edge press: the focus did not move on the frame after a press up / down.
    static ImGuiID pressed_on{};
    static int pressed_frame{-10}, pressed_dir{};
    const int frame = ImGui::GetFrameCount();
    if (page && pressed_frame == frame - 1 && pressed_dir && g.NavId == pressed_on && !ImGui::IsAnyItemActive())
        ImGui::SetScrollY(page, std::clamp(page->Scroll.y + pressed_dir * px(220), 0.0f, page->ScrollMax.y));
    pressed_dir = 0;
    const auto down = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    if (down(ImGuiKey_GamepadDpadDown) || down(ImGuiKey_GamepadLStickDown) || down(ImGuiKey_DownArrow)) pressed_dir = 1;
    else if (down(ImGuiKey_GamepadDpadUp) || down(ImGuiKey_GamepadLStickUp) || down(ImGuiKey_UpArrow)) pressed_dir = -1;
    pressed_on = g.NavId;
    pressed_frame = frame;
    if (!page) return;
    const float stick = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
    // Gentle near the middle, a page a second or so at full tilt.
    if (std::abs(stick) > 0.01f)
        ImGui::SetScrollY(page, std::clamp(page->Scroll.y + stick * std::abs(stick) * px(650) * ImGui::GetIO().DeltaTime, 0.0f, page->ScrollMax.y));
}

// Fills the screen, with the pages across the top and button prompts at the bottom: made for a
// controller. LB / RB change page, LT / RT the page's tabs, View goes back to a window.
void draw_fullscreen(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool& visible, bool appearing, bool pad,
                     bool playstation) {
    auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8, 9, 11, 238));
    const bool began = ImGui::Begin("ReSkate###skate-menu-full", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    if (began) {
        {
            ImGuiContext& g = *GImGui;
            ImGuiWindow* self = ImGui::GetCurrentWindow();
            bool pad_pressed = false;
            for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown && !pad_pressed; ++key)
                pad_pressed = ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false);
            const bool ours = g.NavWindow && g.NavWindow->RootWindow == self;
            if (!ours && (pad_pressed || !g.NavWindow) && !io.WantTextInput) ImGui::FocusWindow(self);
        }
        const auto size = ImGui::GetWindowSize();
        const auto origin = ImGui::GetWindowPos();
        auto* draw = ImGui::GetWindowDrawList();
        const float bar = px(68), footer = px(56);
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + bar), IM_COL32(10, 10, 11, 255));
        draw->AddRectFilled(ImVec2(origin.x, origin.y + bar - px(1)), ImVec2(origin.x + size.x, origin.y + bar), skate_theme::tile_grey);
        wordmark(draw, menu, ImVec2(origin.x + px(26), origin.y + px(16)), px(34));

        // The pages across the top, LB and RB at their ends.
        const float tab_height = px(42), tab_pad = px(16);
        float tabs_width = 0;
        for (int i = 0; i < page_count; ++i)
            if (!page_hidden(menu, model, i))
                tabs_width += menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, page_names[static_cast<std::size_t>(i)]).x + tab_pad * 2 + px(2);
        const float glyphs = pad ? px(36) * 2 : 0;
        float x = origin.x + std::max(px(220), (size.x - tabs_width - glyphs) * .5f);
        const float tab_y = origin.y + (bar - tab_height) * .5f;
        if (pad) x += glyph(draw, menu, ImVec2(x, tab_y + (tab_height - px(22)) * .5f), Glyph::lb, playstation) + px(12);
        for (int i = 0; i < page_count; ++i) {
            if (page_hidden(menu, model, i)) continue;
            const char* name = page_names[static_cast<std::size_t>(i)];
            const auto text = menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, name);
            const float width = text.x + tab_pad * 2;
            ImGui::SetCursorScreenPos(ImVec2(x, tab_y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##top-page", ImVec2(width, tab_height), ImGuiButtonFlags_EnableNav)) open_page(menu, i, i == page_addons ? addon_all : -1);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemFocused() && io.NavVisible) draw->AddRect(ImVec2(x, tab_y), ImVec2(x + width, tab_y + tab_height), paper, 0, 0, px(2));
            ImGui::PopID();
            const bool on = menu.page == i;
            if (on || hovered) draw->AddRectFilled(ImVec2(x, tab_y), ImVec2(x + width, tab_y + tab_height), on ? blue : skate_theme::tile_grey);
            draw->AddText(menu.bold, px(16), ImVec2(x + tab_pad, tab_y + (tab_height - text.y) * .5f), on ? skate_theme::black : paper, name);
            x += width + px(2);
        }
        if (pad) glyph(draw, menu, ImVec2(x + px(10), tab_y + (tab_height - px(22)) * .5f), Glyph::rb, playstation);

        // Back to a window, and close.
        const float switch_width = ImGui::CalcTextSize("SWITCH TO WINDOWED").x + px(28);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - px(26) - px(36) - px(8) - switch_width, origin.y + (bar - px(36)) * .5f));
        if (ImGui::Button("SWITCH TO WINDOWED##windowed", ImVec2(switch_width, px(36)))) set_menu_fullscreen(menu, callbacks, false);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A window over the game, made for a mouse (F11)");
        ImGui::SameLine(0, px(8));
        if (ImGui::Button("X##close", ImVec2(px(36), px(36)))) visible = false;

        // The page, in a column that keeps lines readable on a wide screen.
        const float width = std::min(size.x - px(80), px(1360));
        const float left = (size.x - width) * .5f;
        if (menu.page != page_home)
            brushed_title(draw, menu, ImVec2(origin.x + left, origin.y + bar + px(16)), page_title(menu), page_subtitle(menu), px(44));
        const float top = bar + (menu.page == page_home ? px(24) : px(84));
        ImGui::SetCursorPos(ImVec2(left, top));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(16), px(13)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(12), px(12)));
        ImGui::BeginChild("page", ImVec2(width, size.y - top - footer - px(12)), ImGuiChildFlags_NavFlattened);
        draw_page_body(menu, model, callbacks);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        if (menu.page != page_online && !menu.recording_bind) pad_scroll(ImGui::GetCurrentWindow());

        // What the buttons do: the pad's, or the keys when there is none.
        const float foot = origin.y + size.y - footer;
        draw->AddRectFilled(ImVec2(origin.x, foot), ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(10, 10, 11, 255));
        draw->AddRectFilled(ImVec2(origin.x, foot), ImVec2(origin.x + size.x, foot + px(1)), skate_theme::tile_grey);
        const ImVec2 at(origin.x + left, foot + (footer - px(22)) * .5f);
        float across = 0;
        if (pad) {
            across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::a}, "Select", playstation) + px(26);
            across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::b}, menu.page == page_home ? "Close" : "Back", playstation) + px(26);
            across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::lb, Glyph::rb}, "Pages", playstation) + px(26);
            across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::lt, Glyph::rt}, "Sections", playstation) + px(26);
            across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::view}, "Windowed", playstation) + px(26);
            if (menu.page != page_online) across += prompt(draw, menu, ImVec2(at.x + across, at.y), {Glyph::rs}, "Scroll", playstation) + px(40);
            else across += px(14);
            if (!console_hint().empty()) {
                draw->PushClipRect(ImVec2(at.x + across, foot), ImVec2(origin.x + size.x - px(20), origin.y + size.y), true);
                draw->AddText(menu.body, px(15), ImVec2(at.x + across, at.y + px(3)), muted, console_hint().c_str());
                draw->PopClipRect();
            }
        } else {
            static const auto label = dingosdk::launcher::key_cap(dingosdk::launcher::overlay_keys().menu);
            across += keycap(draw, menu.bold, px(1), ImVec2(at.x + across, at.y), "Esc", "Close") + px(26);
            across += keycap(draw, menu.bold, px(1), ImVec2(at.x + across, at.y), "F11", "Windowed") + px(26);
            keycap(draw, menu.bold, px(1), ImVec2(at.x + across, at.y), label.c_str(), "Close");
        }
    }
    ImGui::End();
}
}

namespace {
// Full screen, the game's menu sounds as its pause menu makes them: a tab or page changing, a press
// (A, Enter, a click on a control), the focus moving. The ONLINE page is the pause menu's own and
// makes its own, so only the page changes are made for it.
void menu_sounds(bool page_makes_its_own) {
    ImGuiContext& g = *GImGui;
    static ImGuiID heard{};
    static double moved_at{};
    constexpr std::uint32_t navigate = 3, select = 7, tab = 56;
    if (menu::tab_switched) {
        play_ui_sound(tab);
    } else if (!page_makes_its_own) {
        const bool pressed = (g.NavId && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                          ImGui::IsKeyPressed(ImGuiKey_Space, false))) ||
                             (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g.HoveredId);
        if (pressed) play_ui_sound(select);
        // (Sweeping the pointer across tiles is not a sound for each one: only the D-pad's focus.)
        else if (g.NavId != heard && g.NavId && g.NavCursorVisible && ImGui::GetTime() - moved_at > 0.045) {
            play_ui_sound(navigate);
            moved_at = ImGui::GetTime();
        }
    }
    heard = g.NavId;
}
}

void draw_skate_menu(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks,
                     bool& visible) {
    menu::tab_switched = false;
    const bool on_controls = menu.page == page_settings && menu.settings_tab == 0;
    const bool on_voice = menu.page == page_online && menu.multiplayer_tab == multiplayer_voice_tab;
    const bool was_drawn = menu.last_menu_frame + 1 == ImGui::GetFrameCount();
    if (!visible || !on_controls || !was_drawn) menu.recording_bind = 0;
    if (!visible || !on_voice || !was_drawn) menu.voice_bind_capture = 0;
    auto& io = ImGui::GetIO();
    // Esc (B on a controller) closes the menu, unless it is busy with something else: typing in a
    // field, an open popup, or recording a key or controller binding. Full screen, B first goes
    // back to HOME.
    const bool idle = !io.WantTextInput && !detail::online_embedded_typing() && !menu.recording_bind && !menu.voice_bind_capture && !ImGui::IsAnyItemActive() &&
                      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (menu.fullscreen_pending && (*menu.fullscreen_pending == model.menu_fullscreen || ImGui::GetTime() > menu.fullscreen_until))
        menu.fullscreen_pending.reset();
    const bool full = menu_fullscreen(menu, model);
    const bool appearing = !was_drawn || full != menu.fullscreen_drawn;
    menu.fullscreen_drawn = full;
    menu_fullscreen_shown().store(full);
    if (visible && idle && was_drawn) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) visible = false;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (full && menu.page != page_home) open_page(menu, page_home);
            else visible = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) step_page(menu, model, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) step_page(menu, model, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F11, false))
            set_menu_fullscreen(menu, callbacks, !full);
    }
    menu.last_menu_frame = ImGui::GetFrameCount();
    // Full screen is read from the sofa: everything grows with the screen, drawn from the larger fonts.
    const float boost = full ? std::clamp(io.DisplaySize.y / 1080.0f, 1.0f, 2.0f) * 1.3f : 1.0f;
    // Each style keeps its own size; a slider held shows its size as it moves.
    const float chosen = menu.scale_editing ? menu.scale : full ? model.menu_scale_fullscreen : model.menu_scale;
    ui_scale = std::clamp(chosen, min_menu_scale, max_menu_scale) * boost;
    const bool large = full && menu.body_large && menu.bold_large && menu.heading_large && menu.title_large;
    const std::array<ImFont*, 4> small_fonts{menu.body, menu.bold, menu.heading, menu.title};
    if (large) {
        menu.body = menu.body_large;
        menu.bold = menu.bold_large;
        menu.heading = menu.heading_large;
        menu.title = menu.title_large;
    }
    const auto restore_font_scale = io.FontGlobalScale;
    // Widget text follows the global font scale; the pixel layout below follows px().
    io.FontGlobalScale = large ? ui_scale / large_font_ratio : ui_scale;
    menu.rail_indent = 0;
    if (trainer_page_wanted()) open_page(menu, page_sandbox);
    if (dingosdk::drone::menu_flag().exchange(false)) open_page(menu, page_addons, addon_drone); // `dronemenu`
    menu.page = std::clamp(menu.page, 0, static_cast<int>(page_count) - 1);
    if (page_hidden(menu, model, menu.page)) menu.page = full ? page_home : page_map;
    menu_page_owns_pad().store(full && (menu.page == page_online || menu.recording_bind));
    // The pad in use names its buttons in the full screen prompts.
    const auto pad = read_gamepad();

    ImGui::PushFont(menu.body);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), px(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0);
    int colours = skate_theme::push_widget_colours();
    // A controller's focus in white: the theme's blue vanishes on blue buttons and tabs.
    if (full) {
        ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(245, 245, 245, 255));
        ++colours;
    }
    begin_console_frame(full, menu.bold, pad.playstation);
    if (full) draw_fullscreen(menu, model, callbacks, visible, appearing, pad.valid, pad.playstation);
    else draw_windowed(menu, model, callbacks, visible, appearing);
    if (full) menu_sounds(menu.page == page_online);
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(9);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (large) {
        menu.body = small_fonts[0];
        menu.bold = small_fonts[1];
        menu.heading = small_fonts[2];
        menu.title = small_fonts[3];
    }
}
}
