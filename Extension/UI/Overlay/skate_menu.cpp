#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "skate_menu.h"
#include "modding_menu.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"

#include <Windows.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>
#include <array>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::overlay {
using namespace menu;
namespace menu {
using namespace theme;

namespace {
// Set once per frame in draw_skate_menu, then read through px() while the menu and its pages draw.
float ui_scale = default_menu_scale;
int shown_layout = menu_layout_sheet; // the layout this frame is drawn in
// The menu's own greys, from its mock-ups: a near-black sheet, dark tiles for cards and rows a step
// lighter, and the side sheet's focus: white, with dark text on it.
constexpr ImU32 sheet_fill = IM_COL32(16, 16, 16, 242);
constexpr ImU32 band_fill = IM_COL32(11, 11, 11, 255);
constexpr ImU32 window_fill = IM_COL32(20, 20, 20, 242);
constexpr ImU32 rail_fill = IM_COL32(17, 17, 17, 255);
// Cards and rows: the menu's greys on its own dark sheet; a page drawn in the kit over the game world
// (draw_in_kit: a plugin's page in the pause menu) swaps in the pause pages' near black for its time.
ImU32 card_fill = IM_COL32(29, 29, 31, 250);
constexpr ImU32 inner_card_fill = IM_COL32(37, 37, 40, 250);
ImU32 row_fill = IM_COL32(40, 40, 43, 255);
constexpr ImU32 row_hover = IM_COL32(56, 57, 61, 255);
constexpr ImU32 tab_fill = IM_COL32(35, 35, 37, 255);
constexpr ImU32 rule_ink = IM_COL32(45, 45, 47, 255);
constexpr ImU32 focus_ink = IM_COL32(11, 11, 11, 255);
constexpr ImU32 quiet_ink = IM_COL32(138, 140, 146, 255);
// Rows are this tall at 1080 lines; FramePadding is set so ImGui's own widgets are as tall.
constexpr float row_size = 48, text_size = 17;
float row_height() { return ui_scale * row_size; }
}
float px(float value) { return value * ui_scale; }

// Heavy uppercase header over a thin rule, like the titles on skate.'s HUB tiles.
void section(SkateMenu& menu, const char* text) {
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::PushFont(menu.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const auto at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + px(2)), rule_ink);
    draw->AddRectFilled(at, ImVec2(at.x + px(34), at.y + px(3)), blue);
    ImGui::Dummy(ImVec2(0, px(6)));
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

// ---------------------------------------------------------------- the controls
// The menu is made to be used with a controller as much as a mouse, in both layouts: tall rows, on/off
// as a block at the row's end, a few options as "< option >" and a number as "< value >" over a bar,
// both changed with left / right (which then never moves the focus) or a click on either half. What
// the focused control does shows in the menu (focus_hint): a controller has no hover tooltips.
namespace {
bool tab_switched = false; // a page or tab changed this frame (the menu's tab sound)
// The page's first row of tabs this frame (category_tabs): which is open and its name, for the
// pages opened lately (SkateMenu::recent). -1: the page has none.
int primary_tab = -1;
std::string primary_tab_name;
// What A, X and Y do on the page drawn this frame (page_prompts), for the footer; null for the usual.
const char* prompt_a = nullptr;
const char* prompt_x = nullptr;
const char* prompt_y = nullptr;
// X on the controller went down this frame. The menu keeps X from ImGui (held, ImGui would take
// it for window switching), so it is read from the pad itself (draw_skate_menu).
bool pad_x_down = false;
std::string focus_hint, pending_hint;
ImFont* control_font = nullptr; // bold, for the controls drawn by hand
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
// The item just drawn has the controller's or the keyboard's focus, shown. (After a click ImGui
// keeps its focus on the item too, but hidden: the pointer then lights it as hovered instead.)
bool nav_lit() { return ImGui::IsItemFocused() && ImGui::GetIO().NavVisible; }
// The tile under a row: white under the focus, a lighter grey under the pointer, rough-cut like the
// game's tiles.
void row_frame(ImDrawList* draw, ImVec2 at, ImVec2 end, bool lit, bool hovered, bool disabled) {
    const ImU32 fill = lit ? paper : disabled ? IM_COL32(40, 40, 43, 130) : hovered ? row_hover : row_fill;
    skate_theme::rough_rect(draw, at, end, fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, ui_scale);
}
ImU32 row_ink(bool lit, bool enabled) { return lit ? focus_ink : enabled ? paper : muted; }
void arrows(ImDrawList* draw, ImVec2 at, ImVec2 end, ImU32 colour) {
    const float h = end.y - at.y, s = px(7), mid = at.y + h * .5f;
    draw->AddTriangleFilled(ImVec2(at.x + px(16), mid), ImVec2(at.x + px(16) + s, mid - s), ImVec2(at.x + px(16) + s, mid + s), colour);
    draw->AddTriangleFilled(ImVec2(end.x - px(16), mid), ImVec2(end.x - px(16) - s, mid - s), ImVec2(end.x - px(16) - s, mid + s), colour);
}
// Which of `count` options is chosen, as a row of small dashes along the bottom of a row.
void option_dots(ImDrawList* draw, float centre, float bottom, int count, int selected, bool lit) {
    if (count < 2 || count > 12) return;
    const float dot = px(6), spacing = px(5), dots = count * dot + (count - 1) * spacing, left = centre - dots * .5f;
    for (int i = 0; i < count; ++i)
        draw->AddRectFilled(ImVec2(left + i * (dot + spacing), bottom - px(10)), ImVec2(left + i * (dot + spacing) + dot, bottom - px(6)),
                            i == selected ? (lit ? focus_ink : blue) : lit ? IM_COL32(0, 0, 0, 50) : IM_COL32(255, 255, 255, 50));
}
// A value as a bar: left / right (or a click on either half, or a press and slide with the mouse)
// step it. A step reports the control active and edited, and letting go of left / right deactivates
// it, so callers that save on IsItemDeactivatedAfterEdit (as they do when a slider is let go) save
// once a run of steps ends.
template<class Value>
bool stepper(const char* id, Value* value, Value low, Value high, const char* format, ImGuiSliderFlags flags) {
    ImGuiContext& g = *GImGui;
    const double unit = std::exchange(next_step, 0.0);
    const float width = ImGui::CalcItemWidth(), height = row_height();
    ImGui::PushID(id);
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##stepper", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const ImGuiID item = ImGui::GetItemID();
    const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered(), lit = nav_lit();
    const bool disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    // Most settings go to the game and come back a few frames later: held, each step would start
    // again from the value the game has not changed yet. So the stepped value is kept, and shown,
    // while left / right is held and for a second after; then the game's own value shows again.
    static ImGuiID kept{};
    static Value kept_value{};
    static double kept_until{};
    if (kept == item && g.Time < kept_until) *value = kept_value;
    else if (kept == item) kept = 0;
    bool changed = false;
    int direction = 0;
    const bool logarithmic = (flags & ImGuiSliderFlags_Logarithmic) && low > 0 && high > low;
    const float bar_left = at.x + px(48), bar_width = width - px(96);
    // Where a value sits along the bar (0 low, 1 high; outside for a typed value past the ends), and back.
    const auto fraction_of = [&](double v) {
        if (!(high > low)) return 0.0;
        if (logarithmic) return v > 0 ? std::log(v / low) / std::log(static_cast<double>(high) / low) : 0.0;
        return (v - low) / (static_cast<double>(high) - low);
    };
    const auto value_at = [&](double f) {
        return logarithmic ? low * std::pow(static_cast<double>(high) / low, f) : low + f * (static_cast<double>(high) - low);
    };
    // The mouse: a click steps (left half down, right half up, as the arrows show); press and move left or right
    // to slide it instead. From a typed value past an end the slide starts there and is not pulled back first.
    static ImGuiID dragging{};
    static double drag_from{}, drag_value{};
    static float drag_x{};
    static bool slid_far{};
    if (!disabled && ImGui::IsItemActivated() && g.ActiveIdSource == ImGuiInputSource_Mouse) {
        dragging = item, drag_x = g.IO.MousePos.x, drag_value = static_cast<double>(*value), drag_from = fraction_of(drag_value), slid_far = false;
    }
    if (dragging == item && std::abs(g.IO.MousePos.x - drag_x) > px(4)) slid_far = true;
    if (clicked && !disabled && hovered && !slid_far) direction = g.IO.MousePos.x < at.x + width * .5f ? -1 : 1;
    if (dragging == item && (!ImGui::IsItemActive() || !g.IO.MouseDown[0])) dragging = 0;
    if (dragging == item && slid_far && bar_width > 0) {
        double next = value_at(drag_from + (g.IO.MousePos.x - drag_x) / bar_width);
        if (flags & ImGuiSliderFlags_AlwaysClamp) next = std::clamp(next, static_cast<double>(low), static_cast<double>(high));
        else next = std::clamp(next, std::min(static_cast<double>(low), drag_value), std::max(static_cast<double>(high), drag_value));
        if (unit > 0) next = drag_value + std::round((next - drag_value) / unit) * unit;
        Value slid;
        if constexpr (std::is_integral_v<Value>) slid = static_cast<Value>(std::lround(next));
        else slid = static_cast<Value>(next);
        if (slid != *value) {
            *value = slid;
            changed = true;
            ImGui::MarkItemEdited(item);
        }
        kept = item, kept_value = *value, kept_until = g.Time + 1.0;
    }
    const bool holding = focused && !disabled && sideways_held();
    if ((focused || hovered) && !pending_hint.empty()) focus_hint = pending_hint;
    if (focused && !disabled) {
        if (left_pressed(true)) direction = -1;
        if (right_pressed(true)) direction = 1;
        if (holding || direction) ImGui::NavMoveRequestCancel();
        if (!pending_hint.empty()) focus_hint = pending_hint;
    }
    if (direction) {
        const double speed = held_for() > 1.2f ? 4.0 : 1.0;
        double next = static_cast<double>(*value);
        if (unit > 0) next += direction * speed * unit;
        else if (logarithmic) next *= std::pow(static_cast<double>(high) / static_cast<double>(low), direction * speed / 40.0);
        else if constexpr (std::is_integral_v<Value>) next += direction * speed * std::max(1.0, std::round((static_cast<double>(high) - low) / 40.0));
        else next += direction * speed * (static_cast<double>(high) - low) / 40.0;
        // A typed value past an end stays where it is until a step brings it back inside.
        if (flags & ImGuiSliderFlags_AlwaysClamp) next = std::clamp(next, static_cast<double>(low), static_cast<double>(high));
        else next = std::clamp(next, std::min(static_cast<double>(low), static_cast<double>(*value)), std::max(static_cast<double>(high), static_cast<double>(*value)));
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
    row_frame(draw, at, end, lit, hovered, disabled);
    arrows(draw, at, end, lit ? focus_ink : hovered ? paper : muted);
    char text[96]{};
    ImFormatString(text, sizeof(text), format && *format ? format : (std::is_integral_v<Value> ? "%d" : "%.2f"), *value);
    ImFont* font = control_font ? control_font : ImGui::GetFont();
    const auto size = font->CalcTextSizeA(px(text_size), FLT_MAX, 0, text);
    draw->AddText(font, px(text_size), ImVec2(at.x + (width - size.x) * .5f, at.y + (height - size.y) * .5f - px(4)), row_ink(lit, !disabled), text);
    // Where the value sits in its range, in steps.
    const double fraction = fraction_of(static_cast<double>(*value));
    constexpr int ticks = 20;
    const float tick = bar_width / ticks;
    for (int i = 0; i < ticks && (unit <= 0 || (static_cast<double>(high) - low) / unit < 2000); ++i) {
        const bool on = (i + 0.5) / ticks <= std::clamp(fraction, 0.0, 1.0);
        draw->AddRectFilled(ImVec2(bar_left + tick * i + px(1), end.y - px(10)), ImVec2(bar_left + tick * (i + 1) - px(1), end.y - px(6)),
                            on ? (lit ? focus_ink : blue) : lit ? IM_COL32(0, 0, 0, 40) : IM_COL32(255, 255, 255, 40));
    }
    ImGui::PopID();
    return changed;
}
void begin_controls_frame(ImFont* bold) {
    control_font = bold;
    focus_hint.clear();
    pending_hint.clear();
    next_step = 0;
}
}
bool console_style() { return true; }
int console_step(bool focused) {
    if (!focused) return 0;
    const int direction = left_pressed() ? -1 : right_pressed() ? 1 : 0;
    if (direction || sideways_held()) ImGui::NavMoveRequestCancel();
    return direction;
}
void set_console_hint(const char* hint) { focus_hint = hint ? hint : ""; }
void set_console_step(double step) { next_step = step; }
const std::string& console_hint() { return focus_hint; }

bool toggle_row(SkateMenu& menu, const char* label, const char* hint, bool& value, bool available,
                const char* unavailable) {
    // A row a setting, what it is now in a block at the right end: blue for ON.
    ImGui::PushID(label);
    ImGui::BeginDisabled(!available);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row_height();
    const bool clicked = ImGui::InvisibleButton("##toggle", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), lit = nav_lit();
    if (clicked) value = !value;
    if ((lit || hovered) && hint) focus_hint = hint;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    row_frame(draw, at, end, lit, hovered, !available);
    if (!available) skate_theme::scribble(draw, at, ImVec2(at.x + width * 0.6f, end.y), static_cast<unsigned>(ImGui::GetItemID()), ui_scale * 0.5f);
    const float block = px(62);
    const auto text = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, label);
    draw->PushClipRect(at, ImVec2(end.x - block - px(20), end.y), true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + px(16), at.y + (height - text.y) * .5f), row_ink(lit, available), label);
    draw->PopClipRect();
    const char* state = available ? (value ? "ON" : "OFF") : unavailable;
    const bool on = available && value;
    const ImVec2 box(end.x - px(8) - block, at.y + px(8)), box_end(end.x - px(8), end.y - px(8));
    if (available) draw->AddRectFilled(box, box_end, on ? blue : lit ? IM_COL32(0, 0, 0, 38) : IM_COL32(0, 0, 0, 110));
    const auto size = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, state);
    draw->AddText(menu.bold, px(14), ImVec2(box.x + (block - size.x) * 0.5f, box.y + (box_end.y - box.y - size.y) * 0.5f),
                  on ? paper : lit ? IM_COL32(70, 70, 74, 255) : muted, state);
    if (hint && *hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

// ---------------------------------------------------------------- cards
// A card is a group, not a child window: ImGui leaves out a child window that is all below the
// screen, controls and all, so the D-pad could not move down into it (and scroll it into view). A
// group's controls can always be reached. Its tile is drawn under it once its height is known, on a
// draw list channel split off for it, and its controls keep inside its padding because the window's
// work rectangle is narrowed to the card while it draws (ImGui sizes, wraps and fills to that).
namespace {
constexpr float card_pad_x = 16, card_pad_y = 14, card_gap = 10;
// A window this wide, or wider, lays a page's cards out in two columns.
constexpr float two_column_width = 780;
// A run of one card spans the page in a wide window, but no wider than this: across a wider card a
// row's label and its control drift too far apart to read as one.
constexpr float single_card_width = 900;
// A longer run than this goes one card at a time under the shorter column: working out the best split
// tries every way of sharing the cards between the two columns.
constexpr std::size_t planned_run_cards = 12;
struct CardFrame {
    ImGuiWindow* window{};
    ImDrawListSplitter splitter;
    float left{}, right{}, top{};
    float work_max{}, content_max{};
    unsigned seed{};
    ImGuiID id{};
    int column = -1; // its column in a two-column run (2 for a run's only card, across the page), -1 for none
};
std::array<CardFrame, 6>& card_frames() {
    static auto* frames = new std::array<CardFrame, 6>;
    return *frames;
}
int card_depth = 0;
// A run of cards as drawn: each card's id and its height with the gap under it, and the frame.
struct CardRun {
    std::vector<ImGuiID> cards;
    std::vector<float> heights;
    int frame = 0;
};
// The runs drawn lately, by the id of their first card. A card's height is only known once it has
// drawn, so a run is laid out from how tall its cards were the frame before.
std::map<ImGuiID, CardRun>& card_runs() {
    static auto* runs = new std::map<ImGuiID, CardRun>;
    return *runs;
}
// The run of cards one after another on a wide page, shared between two columns so that they end up
// about as tall as each other. Anything drawn between two cards ends the run, so a page's own layout
// is kept.
struct CardFlow {
    ImGuiWindow* window{};
    int frame = -1;
    ImVec2 next{};   // where a card leaves the cursor: the next card carries the run on only from there
    float left{}, width{};
    std::array<float, 2> bottom{};
    ImGuiID first{};       // the run's first card, which keys its record
    CardRun drawn;         // this frame's run so far
    std::vector<int> plan; // each card's column from last frame's heights; empty: each under the shorter column
    bool single = false;   // the run was one card last frame, which then spans the page
};
CardFlow card_flow;

// Columns for a run's cards, the first top left, that leave the taller column as short as they can.
// Used only when that beats putting each card under the shorter column by more than a row, so a page
// keeps the usual flow unless the split is clearly better. Empty for the usual flow.
std::vector<int> plan_card_columns(const std::vector<float>& heights) {
    const std::size_t count = heights.size();
    if (count < 3 || count > planned_run_cards) return {};
    std::array<float, 2> greedy{};
    for (const float height : heights) greedy[greedy[1] < greedy[0] ? 1 : 0] += height;
    unsigned best = 0;
    float best_tallest = FLT_MAX, best_spread = FLT_MAX;
    // Bit i of a split puts card i in the right column; the first card always heads the left one.
    for (unsigned split = 0; split < (1u << count); split += 2) {
        std::array<float, 2> sum{};
        for (std::size_t i = 0; i < count; ++i) sum[(split >> i) & 1u] += heights[i];
        const float tallest = std::max(sum[0], sum[1]), spread = std::abs(sum[0] - sum[1]);
        if (tallest < best_tallest || (tallest == best_tallest && spread < best_spread)) best = split, best_tallest = tallest, best_spread = spread;
    }
    if (best_tallest >= std::max(greedy[0], greedy[1]) - px(36)) return {};
    std::vector<int> columns(count);
    for (std::size_t i = 0; i < count; ++i) columns[i] = static_cast<int>((best >> i) & 1u);
    return columns;
}
}
void begin_card(SkateMenu& menu, const char* id, const char* title, const char* subtitle) {
    ImGui::PushID(id);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const auto cursor = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImGuiID card_id = ImGui::GetID("##card");
    float left = cursor.x, right = cursor.x + avail, top = cursor.y;
    int column = -1;
    if (card_depth == 0 && shown_layout == menu_layout_hub && avail >= px(two_column_width)) {
        auto& flow = card_flow;
        const int frame_count = ImGui::GetFrameCount();
        const bool carries_on = flow.window == window && flow.frame == frame_count && std::abs(cursor.x - flow.next.x) < 0.5f &&
                                std::abs(cursor.y - flow.next.y) < 0.5f;
        if (!carries_on) {
            flow = CardFlow{window, frame_count, {}, cursor.x, avail, {cursor.y, cursor.y}, card_id, {}, {}, false};
            flow.drawn.frame = frame_count;
            auto& runs = card_runs();
            if (const auto found = runs.find(card_id); found != runs.end()) {
                flow.single = found->second.cards.size() == 1;
                flow.plan = plan_card_columns(found->second.heights);
                // Last frame's cards, to check the run is still the same cards as it draws.
                if (!flow.plan.empty()) flow.drawn.cards = found->second.cards;
            }
            // Runs not drawn for a minute or so, of pages long closed, are let go.
            if (runs.size() > 256) std::erase_if(runs, [&](const auto& run) { return frame_count - run.second.frame > 3600; });
        }
        const std::size_t index = flow.drawn.heights.size();
        if (!flow.plan.empty() && (index >= flow.plan.size() || flow.drawn.cards[index] != card_id)) flow.plan.clear();
        if (flow.single && index == 0) {
            column = 2;
            right = left + std::min(flow.width, px(single_card_width));
        } else {
            column = !flow.plan.empty() ? flow.plan[index] : flow.bottom[1] < flow.bottom[0] ? 1 : 0;
            const float half = (flow.width - px(card_gap)) * .5f;
            left = flow.left + static_cast<float>(column) * (half + px(card_gap));
            right = left + half;
            top = flow.bottom[static_cast<std::size_t>(column)];
        }
        if (flow.drawn.cards.size() <= index) flow.drawn.cards.resize(index + 1);
        flow.drawn.cards[index] = card_id;
    }
    if (card_depth < static_cast<int>(card_frames().size())) {
        auto& frame = card_frames()[static_cast<std::size_t>(card_depth)];
        frame.window = window;
        frame.left = left, frame.right = right, frame.top = top, frame.column = column;
        frame.id = card_id;
        frame.seed = static_cast<unsigned>(card_id) & 0xfff;
        frame.work_max = window->WorkRect.Max.x;
        frame.content_max = window->ContentRegionRect.Max.x;
        frame.splitter.Split(window->DrawList, 2);
        frame.splitter.SetCurrentChannel(window->DrawList, 1);
        window->WorkRect.Max.x = window->ContentRegionRect.Max.x = right - px(card_pad_x);
    }
    ++card_depth;
    ImGui::SetCursorScreenPos(ImVec2(left + px(card_pad_x), top + px(card_pad_y)));
    ImGui::BeginGroup();
    if (!title) return;
    ImGui::PushFont(menu.heading);
    const auto title_at = ImGui::GetCursorScreenPos();
    ImGui::TextUnformatted(title);
    const float title_width = ImGui::GetItemRectSize().x, title_height = ImGui::GetItemRectSize().y;
    ImGui::PopFont();
    if (subtitle && *subtitle) {
        // Beside the title at the card's right end when it fits there, under it otherwise.
        const float room = ImGui::GetContentRegionAvail().x;
        const auto size = menu.body->CalcTextSizeA(px(15), FLT_MAX, 0, subtitle);
        if (title_width + px(16) + size.x <= room) {
            ImGui::GetWindowDrawList()->AddText(menu.body, px(15), ImVec2(title_at.x + room - size.x, title_at.y + title_height - size.y - px(1)),
                                                muted, subtitle);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }
    ImGui::Dummy(ImVec2(0, px(2)));
}
void end_card() {
    // The bottom padding, inside the group so the group (and the tile) covers it.
    ImGui::Dummy(ImVec2(0, std::max(0.0f, px(card_pad_y) - ImGui::GetStyle().ItemSpacing.y)));
    ImGui::EndGroup();
    --card_depth;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (card_depth >= 0 && card_depth < static_cast<int>(card_frames().size())) {
        auto& frame = card_frames()[static_cast<std::size_t>(card_depth)];
        const float bottom = ImGui::GetItemRectMax().y;
        if (frame.window == window) {
            window->WorkRect.Max.x = frame.work_max;
            window->ContentRegionRect.Max.x = frame.content_max;
            frame.splitter.SetCurrentChannel(window->DrawList, 0);
            skate_theme::rough_rect(window->DrawList, ImVec2(frame.left, frame.top), ImVec2(frame.right, bottom), card_depth ? inner_card_fill : card_fill,
                                    frame.seed, ui_scale);
            frame.splitter.Merge(window->DrawList);
        }
        // What follows starts under the card, or under the taller column of a run.
        float next = bottom + px(card_gap), x = frame.left;
        if (frame.column >= 0) {
            auto& flow = card_flow;
            if (frame.column == 2) flow.bottom = {next, next};
            else flow.bottom[static_cast<std::size_t>(frame.column)] = next;
            // The run as drawn so far is what lays it out next frame.
            const std::size_t index = flow.drawn.heights.size();
            if (index < flow.drawn.cards.size() && flow.drawn.cards[index] == frame.id) {
                flow.drawn.heights.push_back(next - frame.top);
                auto& record = card_runs()[flow.first];
                record.cards.assign(flow.drawn.cards.begin(), flow.drawn.cards.begin() + static_cast<std::ptrdiff_t>(index + 1));
                record.heights = flow.drawn.heights;
                record.frame = flow.drawn.frame;
            }
            next = std::max(flow.bottom[0], flow.bottom[1]);
            x = flow.left;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, next - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        if (frame.column >= 0) card_flow.next = ImGui::GetCursorScreenPos();
    }
    card_depth = std::max(card_depth, 0);
    ImGui::PopID();
}
namespace {
struct Folds {
    std::map<std::string, bool, std::less<>> open;
    std::vector<std::pair<std::string, bool>> changed;
};
Folds& folds() {
    static auto* value = new Folds;
    return *value;
}
}
bool fold_open(std::string_view id, bool fallback) {
    const auto& open = folds().open;
    const auto found = open.find(id);
    return found == open.end() ? fallback : found->second;
}
void set_fold(std::string_view id, bool open) { folds().open[std::string(id)] = open; }
bool take_fold_change(std::string& id, bool& open) {
    auto& changed = folds().changed;
    if (changed.empty()) return false;
    id = std::move(changed.back().first);
    open = changed.back().second;
    changed.pop_back();
    return true;
}
void fold_arrow(ImVec2 at, float row_height, bool open, ImU32 colour) {
    const float r = ImGui::GetFontSize() * 0.26f;
    const ImVec2 c(at.x + r + px(2), at.y + row_height * 0.5f);
    auto* draw = ImGui::GetWindowDrawList();
    if (open) draw->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.7f), ImVec2(c.x + r, c.y - r * 0.7f), ImVec2(c.x, c.y + r * 0.8f), colour);
    else draw->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.7f, c.y + r), ImVec2(c.x + r * 0.8f, c.y), colour);
}
bool fold_card(SkateMenu& menu, const char* id, const char* title, const char* subtitle, bool open_by_default) {
    begin_card(menu, id);
    bool open = fold_open(id, open_by_default);
    const auto at = ImGui::GetCursorScreenPos();
    const float height = px(40), width = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##fold", ImVec2(width, height), ImGuiButtonFlags_EnableNav)) {
        open = !open;
        set_fold(id, open);
        folds().changed.emplace_back(id, open);
    }
    const bool hovered = ImGui::IsItemHovered(), lit = nav_lit();
    auto* draw = ImGui::GetWindowDrawList();
    if (lit || hovered)
        skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height), lit ? paper : row_hover, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff,
                                ui_scale);
    if (hovered) ImGui::SetTooltip(open ? "Click to fold this card away." : "Click to open this card.");
    if (lit || hovered) focus_hint = open ? "A folds this card away." : "A opens this card.";
    fold_arrow(ImVec2(at.x + px(6), at.y), height, open, lit ? focus_ink : muted);
    const float x = at.x + px(6) + ImGui::GetFontSize() * 0.9f + px(6);
    const float size = px(22);
    draw->AddText(menu.heading, size, ImVec2(x, at.y + (height - size) * .5f), lit ? focus_ink : paper, title);
    if (subtitle) {
        const float after = menu.heading->CalcTextSizeA(size, FLT_MAX, 0, title).x + px(12);
        draw->PushClipRect(at, ImVec2(at.x + width, at.y + height), true);
        draw->AddText(menu.body, px(15), ImVec2(x + after, at.y + (height - px(15)) * .5f), lit ? IM_COL32(70, 70, 74, 255) : muted, subtitle);
        draw->PopClipRect();
    }
    if (open) ImGui::Dummy(ImVec2(0, px(2)));
    return open;
}

namespace {
// field() and info() put the label beside what it names on a wide page, in a column this wide.
bool wide_rows() { return ImGui::GetContentRegionAvail().x >= px(560); }
float label_column() { return std::min(px(240), ImGui::GetContentRegionAvail().x * .4f); }
}
void field(SkateMenu&, const char* label, const char* tooltip) {
    pending_hint = tooltip ? tooltip : "";
    const float start = ImGui::GetCursorPosX(), column = label_column();
    // A label too long for its column goes above the control instead of under it.
    const bool beside = wide_rows() && ImGui::CalcTextSize(label).x <= column - ImGui::GetStyle().ItemSpacing.x;
    if (beside) ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    if (beside) ImGui::SameLine(start + column);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void info(SkateMenu&, const char* label, const std::string& value) {
    const float start = ImGui::GetCursorPosX(), column = label_column();
    // A value is text that wraps, not a control that needs the width, so it stays beside its label in
    // narrower cards than a field does (a half-width card in the window, say), where a label alone on
    // its line over its value reads as two loose lines.
    const bool beside = ImGui::GetContentRegionAvail().x >= px(300) && ImGui::CalcTextSize(label).x <= column - ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (beside) ImGui::SameLine(start + column);
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
    // One row, "< option >": left / right, A, or a click on either half changes it.
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const int count = static_cast<int>(options.size());
    const float width = total > 0 ? total : ImGui::GetContentRegionAvail().x, height = row_height();
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##cycle", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused(), lit = nav_lit();
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
    if (lit || hovered) focus_hint = tooltip ? tooltip : pending_hint;
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    row_frame(draw, at, end, lit, hovered, !enabled);
    arrows(draw, at, end, lit ? focus_ink : hovered ? paper : muted);
    const char* label = selected >= 0 && selected < count ? options.begin()[selected] : "Choose";
    const auto size = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, label);
    draw->PushClipRect(ImVec2(at.x + px(34), at.y), ImVec2(end.x - px(34), end.y), true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + std::max(px(36), (width - size.x) * .5f), at.y + (height - size.y) * .5f - px(4)),
                  row_ink(lit, enabled), label);
    draw->PopClipRect();
    option_dots(draw, at.x + width * .5f, end.y, count, selected, lit);
    ImGui::EndDisabled();
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
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row_height();
    const bool clicked = ImGui::InvisibleButton("##primary", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool lit = nav_lit(), hovered = enabled && ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    skate_theme::rough_rect(draw, at, end, !enabled ? IM_COL32(1, 131, 255, 90) : lit ? paper : hovered ? skate_theme::blue_hover : blue,
                            static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, ui_scale);
    const char* shown_end = ImGui::FindRenderedTextEnd(label);
    const auto size = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, label, shown_end);
    draw->PushClipRect(at, end, true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + std::max(px(10), (width - size.x) * .5f), at.y + (height - size.y) * .5f),
                  lit ? focus_ink : enabled ? paper : IM_COL32(245, 245, 245, 140), label, shown_end);
    draw->PopClipRect();
    ImGui::PopID();
    return clicked && enabled;
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
// The stepper, and a number box beside it for a mouse and keyboard (the controller's focus skips
// it). Drawn as one group, so a caller's IsItemActive / IsItemDeactivatedAfterEdit cover both: a
// typed value counts once the box is left, and may go past the stepper's ends unless the caller clamps.
template<class Value, class Type>
bool slider_with_box(const char* id, Value* value, Value low, Value high, ImGuiSliderFlags flags, const char* format, const char* box, Type type) {
    const bool boxed = !(flags & ImGuiSliderFlags_NoInput);
    const float total = ImGui::CalcItemWidth(), box_width = px(96), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(boxed ? std::max(px(120), total - box_width - gap) : total);
    bool changed = stepper(id, value, low, high, format, flags);
    if (boxed) {
        ImGui::SameLine(0, gap);
        ImGui::PushID(id);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetNextItemWidth(box_width);
        Value typed = *value;
        type("##exact", &typed, box);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type an exact value and press Enter");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            *value = (flags & ImGuiSliderFlags_AlwaysClamp) ? std::clamp(typed, low, high) : typed;
            changed = true;
        }
        ImGui::PopItemFlag();
        ImGui::PopID();
    }
    ImGui::EndGroup();
    return changed;
}
}
bool slider_float(const char* id, float* value, float low, float high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, false).c_str(),
        [](const char* label, float* v, const char* box) { return ImGui::InputFloat(label, v, 0, 0, box); });
}
bool slider_int(const char* id, int* value, int low, int high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, true).c_str(),
        [](const char* label, int* v, const char*) { return ImGui::InputInt(label, v, 0, 0); });
}
bool slider_double(const char* id, double* value, double low, double high, const char* format, ImGuiSliderFlags flags) {
    return slider_with_box(id, value, low, high, flags, format, box_format(format, false).c_str(),
        [](const char* label, double* v, const char* box) { return ImGui::InputDouble(label, v, 0, 0, box); });
}

// ---------------------------------------------------------------- rows of buttons and lists
namespace {
// A button tile of button_row and button_grid: white under the focus, blue for the one that is on.
bool button_tile(SkateMenu& menu, const char* label, ImVec2 size, bool enabled, bool on, const char* hint) {
    const auto at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool lit = nav_lit(), hovered = enabled && ImGui::IsItemHovered();
    if ((lit || hovered) && hint) focus_hint = hint;
    if (hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    const ImU32 fill = lit ? paper : !enabled ? IM_COL32(40, 40, 43, 130) : on ? blue : hovered ? row_hover : row_fill;
    skate_theme::rough_rect(draw, at, end, fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, ui_scale);
    const char* shown_end = ImGui::FindRenderedTextEnd(label);
    const auto text = menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, label, shown_end);
    draw->PushClipRect(at, end, true);
    draw->AddText(menu.bold, px(16), ImVec2(at.x + std::max(px(10), (size.x - text.x) * .5f), at.y + (size.y - text.y) * .5f),
                  lit ? focus_ink : enabled ? paper : muted, label, shown_end);
    draw->PopClipRect();
    return pressed && enabled;
}
}
int button_row(SkateMenu& menu, const char* id, std::initializer_list<RowButton> buttons) {
    int pressed = -1;
    const int count = static_cast<int>(buttons.size());
    if (count == 0) return pressed;
    ImGui::PushID(id);
    // Equal tiles across the row; when the widest name would not fit, fewer to a row and more rows.
    const float gap = px(6), room = ImGui::GetContentRegionAvail().x;
    float widest = 0;
    for (const auto& button : buttons)
        widest = std::max(widest, menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, button.label, ImGui::FindRenderedTextEnd(button.label)).x);
    const int across = std::clamp(static_cast<int>((room + gap) / (widest + px(28) + gap)), 1, count);
    const float width = (room - gap * static_cast<float>(across - 1)) / static_cast<float>(across);
    int i = 0;
    for (const auto& button : buttons) {
        if (i % across) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        if (button_tile(menu, button.label, ImVec2(width, row_height()), button.enabled, button.on, button.hint)) pressed = i;
        ImGui::PopID();
        ++i;
    }
    ImGui::PopID();
    return pressed;
}

int button_grid(SkateMenu& menu, const char* id, const std::vector<std::string>& names, int lit) {
    int pressed = -1;
    ImGui::PushID(id);
    const int count = static_cast<int>(names.size());
    // At least 150 px wide, as many to a row as fit.
    const float gap = px(6), room = ImGui::GetContentRegionAvail().x;
    const int across = std::max(1, std::min(count, static_cast<int>((room + gap) / (px(150) + gap))));
    const float width = (room - gap * static_cast<float>(across - 1)) / static_cast<float>(across);
    for (int i = 0; i < count; ++i) {
        if (i % across) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        if (button_tile(menu, names[static_cast<std::size_t>(i)].c_str(), ImVec2(width, px(44)), true, i == lit, nullptr)) pressed = i;
        ImGui::PopID();
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
    // The name on the left, "< option >" on the right; left / right change it, one per press. A steps
    // on too, or, for a row that applies on A (`pressed`), applies it.
    ImGui::BeginDisabled(!enabled);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row_height();
    const bool clicked = ImGui::InvisibleButton("##option", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool focused = ImGui::IsItemFocused(), lit = nav_lit(), hovered = enabled && ImGui::IsItemHovered();
    const float split = at.x + width * .45f;
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
    if (lit || hovered) focus_hint = hint ? hint : "";
    if (hint && *hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    row_frame(draw, at, end, lit, hovered, !enabled);
    const auto text = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, name.c_str());
    draw->PushClipRect(at, ImVec2(split - px(8), end.y), true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + px(16), at.y + (height - text.y) * .5f), row_ink(lit, enabled), name.c_str());
    draw->PopClipRect();
    arrows(draw, ImVec2(split, at.y), end, lit ? focus_ink : hovered ? paper : muted);
    const char* value = selected >= 0 && selected < count ? options[static_cast<std::size_t>(selected)].c_str() : "Choose";
    const float room = end.x - split - px(80);
    const auto size = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, value);
    draw->PushClipRect(ImVec2(split + px(34), at.y), ImVec2(end.x - px(34), end.y), true);
    draw->AddText(menu.bold, px(text_size),
                  ImVec2(split + px(40) + std::max(0.0f, (room - size.x) * .5f), at.y + (height - size.y) * .5f - (count > 1 && count <= 12 ? px(4) : 0)),
                  !enabled ? muted : lit ? focus_ink : blue, value);
    draw->PopClipRect();
    option_dots(draw, split + (end.x - split) * .5f, end.y, count, selected, lit);
    ImGui::PopID();
    return changed;
}

bool list_row(SkateMenu& menu, const char* id, const std::string& text, const std::string& side, bool current, bool enabled) {
    ImGui::PushID(id);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row_height();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool lit = enabled && nav_lit(), hovered = enabled && ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    skate_theme::rough_rect(draw, at, end, lit ? paper : current ? blue : hovered ? row_hover : row_fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff,
                            ui_scale);
    const ImU32 text_ink = lit ? focus_ink : enabled ? paper : muted;
    const auto side_size = menu.body->CalcTextSizeA(px(15), FLT_MAX, 0, side.c_str());
    draw->PushClipRect(at, ImVec2(end.x - side_size.x - px(36), end.y), true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + px(16), at.y + (height - px(text_size)) * .5f), text_ink, text.c_str());
    draw->PopClipRect();
    draw->AddText(menu.body, px(15), ImVec2(end.x - side_size.x - px(16), at.y + (height - side_size.y) * .5f),
                  lit ? IM_COL32(70, 70, 74, 255) : current ? paper : muted, side.c_str());
    ImGui::PopID();
    return clicked && enabled;
}

void category_tabs(SkateMenu& menu, int& selected, std::span<const char* const> tabs, const char* id) {
    const int count = static_cast<int>(tabs.size());
    if (count == 0) return;
    selected = std::clamp(selected, 0, count - 1);
    // LT / RT on a controller step the page's first row of tabs (the one drawn first each frame),
    // not one nested in it.
    static int stepped_frame = -1;
    const bool primary = stepped_frame != ImGui::GetFrameCount();
    if (primary) {
        stepped_frame = ImGui::GetFrameCount();
        if (!ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput && !menu.recording_bind && !menu.voice_bind_capture) {
            const int was = selected;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) selected = (selected + count - 1) % count;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) selected = (selected + 1) % count;
            if (selected != was) menu.feedback.clear(), tab_switched = true;
        }
        primary_tab = selected;
        primary_tab_name = tabs[static_cast<std::size_t>(selected)];
    }
    const auto pick = [&](int i) {
        if (selected == i) return;
        selected = i;
        menu.feedback.clear();
        tab_switched = true;
    };
    ImGui::PushID(id);
    auto* draw = ImGui::GetWindowDrawList();
    const auto start = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (primary && shown_layout == menu_layout_hub) {
        // The window: the names in a row on a thin rule, the open one white over a blue brush stroke.
        const float size = px(18), height = px(36), gap = px(26);
        float x = start.x, y = start.y;
        ImVec2 open_at{}, open_end{};
        for (int i = 0; i < count; ++i) {
            const char* label = tabs[static_cast<std::size_t>(i)];
            const float w = menu.bold->CalcTextSizeA(size, FLT_MAX, 0, label).x;
            if (i && x + w > start.x + avail) x = start.x, y += height + px(4);
            ImGui::SetCursorScreenPos(ImVec2(x - px(6), y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(w + px(12), height), ImGuiButtonFlags_EnableNav)) pick(i);
            const bool lit = nav_lit(), hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = selected == i;
            if (lit)
                skate_theme::rough_rect(draw, ImVec2(x - px(6), y), ImVec2(x + w + px(6), y + height - px(7)), paper, static_cast<unsigned>(i + 7), ui_scale);
            draw->AddText(menu.bold, size, ImVec2(x, y + (height - px(7) - size) * .5f), lit ? focus_ink : on || hovered ? paper : quiet_ink, label);
            if (on) open_at = ImVec2(x - px(4), y + height - px(5)), open_end = ImVec2(x + w + px(4), y + height);
            x += w + gap;
        }
        const float bottom = y + height;
        draw->AddRectFilled(ImVec2(start.x, bottom - px(2)), ImVec2(start.x + avail, bottom), rule_ink);
        // The stroke under the open tab, cut a little crooked as a brush leaves it.
        draw->AddQuadFilled(ImVec2(open_at.x, open_at.y + px(1.5f)), ImVec2(open_end.x, open_at.y), ImVec2(open_end.x - px(1), open_end.y),
                            ImVec2(open_at.x + px(1), open_end.y - px(1)), blue);
        ImGui::SetCursorScreenPos(ImVec2(start.x, bottom));
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PopID();
        return;
    }
    // A segmented strip, as many rows as it takes for every label to fit (two rows of three, say),
    // the tabs of each row as wide as each other: the open one white with dark text.
    const float gap = px(4), height = px(34), size = px(14);
    float widest = 0;
    for (const char* label : tabs) widest = std::max(widest, menu.bold->CalcTextSizeA(size, FLT_MAX, 0, label).x);
    int columns = count;
    for (int rows = 1; rows <= count; ++rows) {
        columns = (count + rows - 1) / rows;
        if ((avail - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns) >= widest + px(16)) break;
    }
    const int rows = (count + columns - 1) / columns;
    const float tab_width = (avail - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (int i = 0; i < count; ++i) {
        const ImVec2 at(start.x + static_cast<float>(i % columns) * (tab_width + gap), start.y + static_cast<float>(i / columns) * (height + gap));
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_width, height), ImGuiButtonFlags_EnableNav)) pick(i);
        const bool lit = nav_lit(), hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = selected == i;
        const ImVec2 end(at.x + tab_width, at.y + height);
        skate_theme::rough_rect(draw, at, end, on ? paper : hovered ? row_hover : tab_fill, static_cast<unsigned>(i + 11), ui_scale);
        // The focus on another tab is a white edge (white is the open tab's); on the open one, a blue line under it.
        if (lit && !on) draw->AddRect(at, end, paper, 0, 0, px(2));
        if (lit && on) draw->AddRectFilled(ImVec2(at.x, end.y - px(3)), end, blue);
        const char* label = tabs[static_cast<std::size_t>(i)];
        const auto text = menu.bold->CalcTextSizeA(size, FLT_MAX, 0, label);
        draw->PushClipRect(at, end, true);
        draw->AddText(menu.bold, size, ImVec2(at.x + std::max(px(6), (tab_width - text.x) * .5f), at.y + (height - text.y) * .5f),
                      on ? focus_ink : hovered || lit ? paper : quiet_ink, label);
        draw->PopClipRect();
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + static_cast<float>(rows) * height + static_cast<float>(rows - 1) * gap));
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::PopID();
}

// ---------------------------------------------------------------- rows whose actions open under them
namespace {
// One row of the whole menu is open at a time (a list's rows are looked at one by one), so its state
// is kept here rather than by each page. `refocus` is a row B closed with the focus inside it, which
// takes the focus back as it draws; `to_actions` asks for the focus on the first action, as A opens it.
struct OpenRowState {
    ImGuiID open{}, refocus{};
    int drawn = -10;          // the frame the open row last drew: one not drawn is gone (another tab, a list that changed)
    bool to_actions{}, focus_inside{};
    ImVec2 top_left{};
    float right{};
};
OpenRowState open_rows;
}
bool open_row(SkateMenu& menu, const char* id, const OpenRow& row) {
    ImGui::PushID(id);
    auto& state = open_rows;
    const ImGuiID self = ImGui::GetID("##open-row");
    const int frame = ImGui::GetFrameCount();
    if (state.open == self && state.drawn < frame - 1) state.open = 0;
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row.detail.empty() ? row_height() : px(62);
    bool clicked = false;
    if (row.openable) clicked = ImGui::InvisibleButton("##open-row", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    else ImGui::Dummy(ImVec2(width, height));
    const bool lit = row.openable && nav_lit(), hovered = row.openable && ImGui::IsItemHovered();
    if (clicked) {
        state.to_actions = state.open != self && lit; // lit: A on the controller or the keyboard, not a click
        state.open = state.open == self ? 0 : self;
    }
    if (state.refocus == self) {
        ImGui::FocusItem();
        state.refocus = 0;
    }
    const bool open = state.open == self;
    if (lit || hovered) focus_hint = row.hint ? row.hint : open ? "A or B closes this." : "A opens this.";
    // Off screen (a long list scrolled away) it only takes its place.
    if (ImGui::IsItemVisible()) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + width, at.y + height);
        skate_theme::rough_rect(draw, at, end, lit ? paper : open || hovered ? row_hover : row_fill, static_cast<unsigned>(self) & 0xfff, ui_scale);
        if (row.edge && !open) draw->AddRectFilled(at, ImVec2(at.x + px(4), end.y), row.edge);
        float x = at.x + px(16);
        if (row.swatch) {
            const float s = px(26), y = at.y + (height - s) * .5f;
            skate_theme::rough_rect(draw, ImVec2(x, y), ImVec2(x + s, y + s), row.swatch, static_cast<unsigned>(self >> 12) & 0xfff, ui_scale);
            x += s + px(12);
        }
        // Right to left: the side text, then the tags after the name, then the name in what is left.
        float right = end.x - px(16);
        if (!row.side.empty()) {
            const auto size = menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, row.side.c_str());
            right -= size.x;
            draw->AddText(menu.bold, px(16), ImVec2(right, at.y + (height - size.y) * .5f),
                          lit ? focus_ink : row.side_colour ? row.side_colour : muted, row.side.c_str());
            right -= px(14);
        }
        float tags = 0;
        for (const auto& tag_shown : row.tags) tags += menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, tag_shown.text).x + px(14) + px(6);
        const float line = row.detail.empty() ? at.y + (height - px(text_size)) * .5f : at.y + px(10);
        const auto name = menu.bold->CalcTextSizeA(px(text_size), FLT_MAX, 0, row.name.c_str());
        const float name_right = std::max(x + px(40), right - tags);
        draw->PushClipRect(ImVec2(x, at.y), ImVec2(name_right, end.y), true);
        draw->AddText(menu.bold, px(text_size), ImVec2(x, line), lit ? focus_ink : row.name_colour ? row.name_colour : paper, row.name.c_str());
        draw->PopClipRect();
        float tag_x = std::min(x + name.x, name_right) + px(10);
        draw->PushClipRect(ImVec2(x, at.y), ImVec2(right, end.y), true);
        for (const auto& tag_shown : row.tags) {
            const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, tag_shown.text);
            const float top = line + (px(text_size) - size.y - px(6)) * .5f;
            draw->AddRectFilled(ImVec2(tag_x, top), ImVec2(tag_x + size.x + px(14), top + size.y + px(6)), tag_shown.colour);
            draw->AddText(menu.bold, px(12), ImVec2(tag_x + px(7), top + px(3)), skate_theme::black, tag_shown.text);
            tag_x += size.x + px(14) + px(6);
        }
        if (!row.detail.empty())
            draw->AddText(menu.body, px(15), ImVec2(x, line + px(text_size) + px(6)), lit ? IM_COL32(70, 70, 74, 255) : muted, row.detail.c_str());
        draw->PopClipRect();
    }
    if (!open) {
        ImGui::PopID();
        return false;
    }
    state.drawn = frame;
    state.top_left = at;
    state.right = at.x + width;
    ImGui::Indent(px(18));
    ImGui::Dummy(ImVec2(0, px(2)));
    if (std::exchange(state.to_actions, false)) ImGui::SetKeyboardFocusHere();
    return true;
}
void end_open_row() {
    auto& state = open_rows;
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::Unindent(px(18));
    // A blue stripe down the open row and its actions, so they read as one.
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddRectFilled(state.top_left, ImVec2(state.top_left.x + px(4), bottom), blue);
    // Whether the focus is on the row or one of its actions, for B (close_open_row).
    ImGuiContext& g = *GImGui;
    state.focus_inside = false;
    if (g.NavWindow && g.NavId && g.NavWindow->RootWindow == ImGui::GetCurrentWindow()->RootWindow) {
        const ImRect focus = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
        state.focus_inside = ImRect(state.top_left, ImVec2(state.right, bottom)).Contains(focus.GetCenter());
    }
    ImGui::PopID();
}
int action_chips(SkateMenu& menu, const char* id, std::initializer_list<RowButton> chips) {
    int pressed = -1;
    const int count = static_cast<int>(chips.size());
    if (count == 0) return pressed;
    ImGui::PushID(id);
    const float gap = px(6), room = ImGui::GetContentRegionAvail().x;
    float widest = 0;
    for (const auto& chip : chips)
        widest = std::max(widest, menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, chip.label, ImGui::FindRenderedTextEnd(chip.label)).x);
    const int across = std::clamp(static_cast<int>((room + gap) / (widest + px(24) + gap)), 1, count);
    const float width = (room - gap * static_cast<float>(across - 1)) / static_cast<float>(across);
    int i = 0;
    for (const auto& chip : chips) {
        if (i % across) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        if (button_tile(menu, chip.label, ImVec2(width, px(40)), chip.enabled, chip.on, chip.hint)) pressed = i;
        ImGui::PopID();
        ++i;
    }
    ImGui::PopID();
    return pressed;
}
bool close_open_row() {
    auto& state = open_rows;
    if (!state.open || state.drawn < ImGui::GetFrameCount() - 1 || !state.focus_inside) return false;
    state.refocus = state.open;
    state.open = 0;
    return true;
}

// ---------------------------------------------------------------- rows that open
namespace {
// The last frame a page had a row open (expand_row): B then closes the row instead of leaving the page.
int back_held_frame = -10;
}
bool page_holds_back() { return back_held_frame >= ImGui::GetFrameCount() - 1; }

bool expand_row(SkateMenu& menu, const char* id, const std::string& title, const std::string& detail, const std::string& side,
                std::string& open, bool current) {
    ImGui::PushID(id);
    bool is_open = !open.empty() && open == id;
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float side_width = side.empty() ? 0.0f : menu.body->CalcTextSizeA(px(15), FLT_MAX, 0, side.c_str()).x + px(16);
    const float text_left = px(40), wrap = std::max(px(80), width - text_left - side_width - px(16));
    const float detail_height = detail.empty() ? 0.0f : menu.body->CalcTextSizeA(px(15), FLT_MAX, wrap, detail.c_str()).y + px(4);
    const float height = detail.empty() ? row_height() : std::max(row_height(), px(26) + px(text_size) + detail_height);
    const bool clicked = ImGui::InvisibleButton("##open", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool lit = nav_lit(), hovered = ImGui::IsItemHovered();
    if (clicked) {
        is_open = !is_open;
        open = is_open ? std::string(id) : std::string();
    } else if (is_open && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::GetIO().WantTextInput) {
        // B closes it, and the focus comes back to the row from the actions that are going away.
        is_open = false;
        open.clear();
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    }
    if (is_open) back_held_frame = ImGui::GetFrameCount();
    if (lit || hovered) focus_hint = is_open ? "A or B closes it." : "A shows what you can do with it.";
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    skate_theme::rough_rect(draw, at, end, lit ? paper : is_open || hovered ? row_hover : row_fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff,
                            ui_scale);
    if (current) draw->AddRectFilled(at, ImVec2(at.x + px(4), end.y), lit ? focus_ink : blue);
    const float title_y = detail.empty() ? at.y + (height - px(text_size)) * .5f : at.y + px(12);
    fold_arrow(ImVec2(at.x + px(14), title_y - (row_height() - px(text_size)) * .5f), row_height(), is_open, lit ? focus_ink : muted);
    draw->PushClipRect(at, ImVec2(end.x - side_width - px(8), end.y), true);
    draw->AddText(menu.bold, px(text_size), ImVec2(at.x + text_left, title_y), lit ? focus_ink : paper, title.c_str());
    if (!detail.empty())
        draw->AddText(menu.body, px(15), ImVec2(at.x + text_left, title_y + px(text_size) + px(4)), lit ? IM_COL32(70, 70, 74, 255) : muted,
                      detail.c_str(), nullptr, wrap);
    draw->PopClipRect();
    if (!side.empty())
        draw->AddText(menu.body, px(15), ImVec2(end.x - side_width, title_y + (px(text_size) - px(15)) * .5f),
                      lit ? IM_COL32(70, 70, 74, 255) : current ? paper : muted, side.c_str());
    ImGui::PopID();
    return is_open;
}

void hold_back() { back_held_frame = ImGui::GetFrameCount(); }
bool pad_x_pressed() { return pad_x_down; }
void page_prompts(const char* a, const char* x, const char* y) {
    prompt_a = a;
    prompt_x = x;
    prompt_y = y;
}

RowPress library_row(SkateMenu& menu, const char* id, const LibraryRow& row) {
    ImGui::PushID(id);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = row.detail.empty() ? row_height() : px(58);
    const bool clicked = ImGui::InvisibleButton("##library-row", ImVec2(width, height), ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_MouseButtonLeft |
                                                ImGuiButtonFlags_MouseButtonRight);
    const bool focused = ImGui::IsItemFocused(), lit = nav_lit(), hovered = ImGui::IsItemHovered();
    if ((lit || hovered) && row.hint) focus_hint = row.hint;
    const ImVec2 end(at.x + width, at.y + height);
    // Right to left: the tag, then the side text, then the name and its line in what is left.
    float right = end.x - px(14);
    ImVec2 tag_at{}, tag_end{};
    if (row.tag && *row.tag) {
        const auto size = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, row.tag);
        tag_end = ImVec2(right, at.y + (height + size.y) * .5f + px(3));
        tag_at = ImVec2(right - size.x - px(16), at.y + (height - size.y) * .5f - px(3));
        right = tag_at.x - px(12);
    }
    RowPress result = RowPress::none;
    const auto& io = ImGui::GetIO();
    if (clicked) {
        // A click on the tag is X's, a right click Y's, anything else A's.
        const bool on_tag = row.tag && io.MousePos.x >= tag_at.x && io.MousePos.x <= tag_end.x && io.MousePos.y >= at.y && io.MousePos.y <= end.y;
        result = ImGui::IsMouseReleased(ImGuiMouseButton_Right) ? RowPress::y : hovered && on_tag && io.MouseReleased[0] ? RowPress::x : RowPress::a;
    } else if (focused && !io.WantTextInput) {
        if (pad_x_down) result = RowPress::x;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) result = RowPress::y;
    }
    if (ImGui::IsItemVisible()) {
        auto* draw = ImGui::GetWindowDrawList();
        skate_theme::rough_rect(draw, at, end, lit ? paper : row.open || hovered ? row_hover : row_fill, static_cast<unsigned>(ImGui::GetItemID()) & 0xfff,
                                ui_scale);
        if (row.edge) draw->AddRectFilled(at, ImVec2(at.x + px(4), end.y), row.edge);
        if (row.open) draw->AddRectFilled(at, ImVec2(at.x + px(4), end.y), blue);
        float x = at.x + px(14);
        if (row.swatch || row.image) {
            const float s = std::min(px(34), height - px(16)), y = at.y + (height - s) * .5f;
            if (row.image) draw->AddImage(row.image, ImVec2(x, y), ImVec2(x + s, y + s));
            else skate_theme::rough_rect(draw, ImVec2(x, y), ImVec2(x + s, y + s), row.swatch, static_cast<unsigned>(ImGui::GetItemID() >> 12) & 0xfff, ui_scale);
            x += s + px(12);
        }
        if (row.tag && *row.tag) {
            draw->AddRectFilled(tag_at, tag_end, row.tag_fill ? row.tag_fill : IM_COL32(61, 62, 66, 255));
            draw->AddText(menu.bold, px(14), ImVec2(tag_at.x + px(8), tag_at.y + px(3)), row.tag_ink ? row.tag_ink : IM_COL32(185, 187, 192, 255), row.tag);
        }
        if (!row.side.empty()) {
            const auto size = menu.body->CalcTextSizeA(px(15), FLT_MAX, 0, row.side.c_str());
            right -= size.x;
            draw->AddText(menu.body, px(15), ImVec2(right, at.y + (height - size.y) * .5f),
                          lit ? IM_COL32(70, 70, 74, 255) : row.side_colour ? row.side_colour : muted, row.side.c_str());
            right -= px(12);
        }
        const float line = row.detail.empty() ? at.y + (height - px(text_size)) * .5f : at.y + px(10);
        draw->PushClipRect(ImVec2(x, at.y), ImVec2(std::max(x + px(40), right), end.y), true);
        draw->AddText(menu.bold, px(text_size), ImVec2(x, line), lit ? focus_ink : paper, row.name.c_str());
        if (!row.detail.empty())
            draw->AddText(menu.body, px(14), ImVec2(x, line + px(text_size) + px(5)),
                          lit ? IM_COL32(70, 70, 74, 255) : row.detail_colour ? row.detail_colour : muted, row.detail.c_str());
        draw->PopClipRect();
    }
    ImGui::PopID();
    return result;
}

int row_actions(SkateMenu& menu, const char* id, std::initializer_list<RowButton> actions) {
    int pressed = -1;
    const int count = static_cast<int>(actions.size());
    if (count == 0) return pressed;
    ImGui::PushID(id);
    // Under the row's title, past its arrow; smaller tiles than button_row's, as many to a row as fit.
    const float indent = px(40), gap = px(6);
    ImGui::Indent(indent);
    const float room = ImGui::GetContentRegionAvail().x;
    float widest = 0;
    for (const auto& action : actions)
        widest = std::max(widest, menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, action.label, ImGui::FindRenderedTextEnd(action.label)).x);
    const int across = std::clamp(static_cast<int>((room + gap) / (widest + px(28) + gap)), 1, count);
    const float width = (room - gap * static_cast<float>(across - 1)) / static_cast<float>(across);
    int i = 0;
    for (const auto& action : actions) {
        if (i % across) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        if (button_tile(menu, action.label, ImVec2(width, px(38)), action.enabled, action.on, action.hint)) pressed = i;
        ImGui::PopID();
        ++i;
    }
    ImGui::Unindent(indent);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::PopID();
    return pressed;
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
    "HOME", "MAP", "BUILD", "SKATER", "SANDBOX", "PLUGINS", "ONLINE", "PROGRESS", "MODS", "SETTINGS", "DEVELOPER"};
constexpr std::array<const char*, page_count> page_subtitles{
    "Everything, a button away.", "Pick your spot. Set the vibe.", "Make the park yours.", "Ride it your way.",
    "Tune it. Drill it.", "What your plugins add.", "Bring your crew.", "Pick up where you want.",
    "Bring your own.", "Your controls, your screen, your sound.", "Under the hood."};
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
    static const ImWchar digits[]{'0','9',0};
    menu.countdown=embedded_font(L"FONT_BRUSH",240.f,digits);
    menu.mono = font(nullptr, "consola.ttf", 15);
    // The larger set, for a menu scaled up past what the set above draws sharply (a big screen grows
    // the menu with it). The brushed titles are page and section names: plain Latin is enough.
    static const ImWchar latin[]{0x0020, 0x00FF, 0};
    menu.body_large = font(L"FONT_BODY", "arial.ttf", 17 * large_font_ratio);
    menu.bold_large = font(L"FONT_HEADING", "arialbd.ttf", 17 * large_font_ratio);
    menu.heading_large = font(L"FONT_HEADING", "arialbd.ttf", 22 * large_font_ratio);
    menu.title_large = font(L"FONT_BRUSH", "arialbi.ttf", 44 * large_font_ratio, latin);
}

namespace {
// A console line of the menu's own, its reply kept off the page (the menu shows what it did).
void quietly(const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) return;
    std::array<char, 256> reply{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), reply.data(), reply.size());
}
}

namespace menu {
int menu_layout(const SkateMenu& menu, const Model& model) {
    const int layout = menu.layout_pending.value_or(model.menu_layout);
    return layout == menu_layout_hub ? menu_layout_hub : menu_layout_sheet;
}
void set_menu_layout(SkateMenu& menu, const CallbacksV3& callbacks, int layout) {
    layout = layout == menu_layout_hub ? menu_layout_hub : menu_layout_sheet;
    menu.layout_pending = layout;
    menu.layout_until = ImGui::GetTime() + 3.0;
    quietly(callbacks, layout == menu_layout_hub ? "ui layout hub" : "ui layout sheet");
}
void open_page(SkateMenu& menu, int page, int tab) {
    int* tabs = page == page_map ? &menu.map_tab : page == page_build ? &menu.build_tab : page == page_skater ? &menu.skater_tab
              : page == page_settings ? &menu.settings_tab : page == page_online ? &menu.multiplayer_tab
              : page == page_progress ? &menu.progression_tab : nullptr;
    if (!tabs) tab = -1;
    if (menu.page != page || (tab >= 0 && tab != *tabs)) tab_switched = true;
    if (menu.page != page) {
        menu.recording_bind = 0;
        menu.voice_bind_capture = 0;
        menu.feedback.clear();
    }
    menu.page = page;
    if (tab >= 0) *tabs = tab;
}
}

namespace {
bool page_hidden(const Model& model, int page) {
    // Developer (the network view) is hidden for now; offline there is no Online.
    return page == page_developer || (model.steam_offline && page == page_online);
}
// A plugin's page opened from the PLUGINS list takes its plugin's name for the title.
const plugin_ui::PageEntry* drilled_plugin_page(SkateMenu& menu) {
    return menu.page == page_plugins && menu.plugin_page != plugin_manager_entry ? shown_plugin_page(menu) : nullptr;
}
const char* page_title(SkateMenu& menu) {
    if (const auto* plugin = drilled_plugin_page(menu)) {
        static std::string title;
        title = plugin->title;
        for (auto& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return title.c_str();
    }
    return page_names[static_cast<std::size_t>(menu.page)];
}
const char* page_subtitle(SkateMenu& menu) {
    if (drilled_plugin_page(menu)) return "A plugin's page, opened from PLUGINS.";
    return page_subtitles[static_cast<std::size_t>(menu.page)];
}
void step_page(SkateMenu& menu, const Model& model, int step) {
    int page = menu.page;
    for (int tries = 0; tries < page_count; ++tries) {
        page = (page + step + page_count) % page_count;
        if (!page_hidden(model, page)) break;
    }
    open_page(menu, page);
}
// The big brushed titles draw from the larger face, so they stay sharp at their size.
ImFont* title_font(const SkateMenu& menu) { return menu.title_large ? menu.title_large : menu.title; }

// The brushed title, tilted like the HUB's, with its subtitle beside it.
void brushed_title(ImDrawList* draw, SkateMenu& menu, ImVec2 at, const char* text, const char* subtitle, float size, float right) {
    ImFont* font = title_font(menu);
    const int start = draw->VtxBuffer.Size;
    draw->AddText(font, size, ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), text);
    draw->AddText(font, size, at, paper, text);
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    skate_theme::rotate_since(draw, start, -2.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
    if (subtitle && at.x + extent.x + px(18) < right) {
        draw->PushClipRect(ImVec2(at.x, at.y), ImVec2(right, at.y + size), true);
        draw->AddText(menu.body, px(14), ImVec2(at.x + extent.x + px(18), at.y + size - px(24)), muted, subtitle);
        draw->PopClipRect();
    }
}

// A controller button as the pad in use shows it: Xbox's coloured letters, or PlayStation's shapes
// (drawn: the menu font has no ✕ ○ □ △), and the shoulders, triggers and middle buttons by name.
// Returns its width; with no draw list it only measures.
enum class Glyph { a, b, x, y, lb, rb, lt, rt, view, rs };
float glyph(ImDrawList* draw, SkateMenu& menu, ImVec2 at, Glyph which, bool playstation) {
    const float size = px(22);
    const ImVec2 centre(at.x + size * .5f, at.y + size * .5f);
    const auto face = [&](ImU32 xbox_fill, const char* letter, ImU32 shape) {
        if (!draw) return size;
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
    if (draw) {
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + size), paper, px(3));
        draw->AddText(menu.bold, px(12), ImVec2(at.x + px(7), at.y + (size - text.y) * .5f), skate_theme::black, name);
    }
    return width;
}
// Buttons, then what they do, as one prompt. Returns the width drawn.
float prompt(ImDrawList* draw, SkateMenu& menu, ImVec2 at, std::initializer_list<Glyph> glyphs, const char* label, bool playstation) {
    float x = at.x;
    for (const auto which : glyphs) x += glyph(draw, menu, ImVec2(x, at.y), which, playstation) + px(4);
    draw->AddText(menu.bold, px(14), ImVec2(x + px(4), at.y + px(3)), paper, label);
    return x + px(4) + menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, label).x - at.x;
}

// "TIME AND WEATHER" as "Time and weather", for a line that names a page and its tab.
std::string sentence_case(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(i ? std::tolower(static_cast<unsigned char>(out[i])) : std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}
// The page just drawn, at the front of the pages opened lately (HOME's PICK UP WHERE YOU LEFT OFF): a
// page once, at the tab it was last on; a plugin's page as its own.
void note_recent_page(SkateMenu& menu) {
    if (menu.page == page_home) return;
    SkateMenu::RecentPage entry;
    entry.page = menu.page;
    if (const auto* plugin = drilled_plugin_page(menu)) {
        entry.plugin = plugin->plugin;
        entry.label = plugin->title;
    } else {
        entry.tab = primary_tab;
        entry.label = sentence_case(page_names[static_cast<std::size_t>(menu.page)]);
        if (primary_tab >= 0) entry.label += " \xc2\xb7 " + sentence_case(primary_tab_name);
    }
    auto& recent = menu.recent;
    if (!recent.empty() && recent.front().page == entry.page && recent.front().plugin == entry.plugin) {
        recent.front() = std::move(entry);
        return;
    }
    std::erase_if(recent, [&](const SkateMenu::RecentPage& r) { return r.page == entry.page && r.plugin == entry.plugin; });
    recent.insert(recent.begin(), std::move(entry));
    if (recent.size() > 3) recent.resize(3);
}

void draw_page_body(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    card_depth = 0;
    primary_tab = -1;
    primary_tab_name.clear();
    page_prompts(nullptr);
    switch (menu.page) {
    case page_home: home_page(menu, model, callbacks); break;
    case page_map: map_page(menu, model, callbacks); break;
    case page_build: build_page(menu, model, callbacks); break;
    case page_skater: skater_page(menu, model, callbacks); break;
    case page_sandbox: trainer_page(menu, model, callbacks); break;
    case page_plugins: plugins_page(menu); break;
    case page_online: multiplayer_page(menu, model, callbacks); break;
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
    note_recent_page(menu);
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
        page->AddRectFilled(at, ImVec2(at.x + width, at.y + text_height + px(12)), card_fill);
        page->AddRectFilled(at, ImVec2(at.x + px(4), at.y + text_height + px(12)), blue);
        ImGui::SetCursorScreenPos(ImVec2(at.x + px(14), at.y + px(6)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - px(24));
        ImGui::TextUnformatted(result.c_str());
        ImGui::PopTextWrapPos();
    }
}

void wordmark(ImDrawList* draw, SkateMenu& menu, ImVec2 at, float size) {
    ImFont* font = title_font(menu);
    const int start = draw->VtxBuffer.Size;
    draw->AddText(font, size, ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), "RESKATE");
    draw->AddText(font, size, at, paper, "RESKATE");
    const float re = font->CalcTextSizeA(size, FLT_MAX, 0, "RE").x;
    draw->AddText(font, size, ImVec2(at.x + re, at.y), blue, "SKATE");
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, "RESKATE");
    skate_theme::rotate_since(draw, start, -4.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
}

// Scrolling a page by pad: the right stick scrolls it, and up / down past its first or last control
// scrolls on to what is above or below (text, a disabled control) that the focus cannot land on.
// The page is the smallest scrolling part of the menu that holds the focus.
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

// A press on the controller brings the focus back to the menu (after a click on the game, say), so
// the pad never drives nothing. Not while a popup of a page is open: focusing the menu would close it.
void take_focus_for_pad() {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* self = ImGui::GetCurrentWindow();
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    bool pad_pressed = false;
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown && !pad_pressed; ++key)
        pad_pressed = ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false);
    const bool ours = g.NavWindow && g.NavWindow->RootWindow == self;
    if (!ours && (pad_pressed || !g.NavWindow) && !g.IO.WantTextInput) ImGui::FocusWindow(self);
}

// What a frame of the menu needs to know about the controller: whether it is in charge (its button
// prompts show, else the keys'), and whether it names its buttons as a PlayStation pad does.
struct Pad {
    bool in_charge{};
    bool playstation{};
};

// The prompts along the bottom of either layout: what the pad's buttons do while it is in charge,
// the keys otherwise. A prompt that does something is a button for the pointer too. `wide` adds the
// ones a narrow sheet has no room for, and the focused control's hint after them.
void footer_prompts(SkateMenu& menu, const CallbacksV3& callbacks, bool& visible, ImVec2 at, float right, Pad pad, bool wide) {
    auto* draw = ImGui::GetWindowDrawList();
    float x = at.x;
    const float gap = wide ? px(26) : px(16);
    enum Action { act_none, act_back, act_close, act_layout };
    const auto act = [&](const char* id, float width, Action action) {
        if (action != act_none) {
            ImGui::SetCursorScreenPos(ImVec2(x - px(4), at.y - px(4)));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(width + px(8), px(30)));
            ImGui::PopItemFlag();
            if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(x - px(4), at.y - px(4)), ImVec2(x + width + px(4), at.y + px(26)), IM_COL32(255, 255, 255, 26));
            if (clicked && action == act_back) {
                if (menu.page != page_home) open_page(menu, page_home);
                else visible = false;
            }
            if (clicked && action == act_close) visible = false;
            if (clicked && action == act_layout) set_menu_layout(menu, callbacks, shown_layout == menu_layout_hub ? menu_layout_sheet : menu_layout_hub);
        }
        x += width + gap;
    };
    if (pad.in_charge) {
        act("##pages", prompt(draw, menu, ImVec2(x, at.y), {Glyph::lb, Glyph::rb}, "Pages", pad.playstation), act_none);
        // A page that says what its buttons do (page_prompts) shows that in place of Select; a narrow
        // sheet then leaves Tabs out for the room (those pages have none).
        const bool own = prompt_a || prompt_x || prompt_y;
        if (wide || !own) act("##tabs", prompt(draw, menu, ImVec2(x, at.y), {Glyph::lt, Glyph::rt}, "Tabs", pad.playstation), act_none);
        if (!own) act("##select", prompt(draw, menu, ImVec2(x, at.y), {Glyph::a}, "Select", pad.playstation), act_none);
        if (prompt_a) act("##a", prompt(draw, menu, ImVec2(x, at.y), {Glyph::a}, prompt_a, pad.playstation), act_none);
        if (prompt_x) act("##x", prompt(draw, menu, ImVec2(x, at.y), {Glyph::x}, prompt_x, pad.playstation), act_none);
        if (prompt_y) act("##y", prompt(draw, menu, ImVec2(x, at.y), {Glyph::y}, prompt_y, pad.playstation), act_none);
        act("##back", prompt(draw, menu, ImVec2(x, at.y), {Glyph::b}, menu.page == page_home ? "Close" : "Back", pad.playstation), act_back);
        if (wide) {
            act("##layout", prompt(draw, menu, ImVec2(x, at.y), {Glyph::view}, "Layout", pad.playstation), act_layout);
            act("##scroll", prompt(draw, menu, ImVec2(x, at.y), {Glyph::rs}, "Scroll", pad.playstation), act_none);
        }
    } else {
        // Esc and the menu's own key both close it; one prompt is enough.
        act("##escape", keycap(draw, menu.bold, ui_scale, ImVec2(x, at.y + px(1)), "Esc", "Close"), act_close);
        act("##f11", keycap(draw, menu.bold, ui_scale, ImVec2(x, at.y + px(1)), "F11", "Layout"), act_layout);
    }
    if (wide && pad.in_charge && !console_hint().empty() && x < right) {
        draw->PushClipRect(ImVec2(x, at.y - px(4)), ImVec2(right, at.y + px(26)), true);
        draw->AddText(menu.body, px(15), ImVec2(x, at.y + px(3)), muted, console_hint().c_str());
        draw->PopClipRect();
    }
}

// The side sheet: docked to the right edge at the display's full height, the game in view beside it.
// Its left edge is dragged to make it wider (saved, `ui sheetwidth`). Pages step with LB / RB in its
// header, where dots say which of them is open.
void draw_sheet(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool& visible, bool appearing, Pad pad) {
    auto& io = ImGui::GetIO();
    const auto display = io.DisplaySize;
    const float low = std::min(px(min_menu_sheet_width), display.x), high = std::max(low, display.x * .5f);
    const float saved = menu.sheet_width_pending.value_or(model.menu_sheet_width > 0 ? model.menu_sheet_width : default_menu_sheet_width);
    const float width = std::clamp(menu.sheet_dragging ? menu.sheet_drag_width : px(saved), low, high);
    ImGui::SetNextWindowPos(ImVec2(display.x - width, 0));
    ImGui::SetNextWindowSize(ImVec2(width, display.y));
    if (appearing) ImGui::SetNextWindowFocus();
    // The sheet draws its own fill, its left edge leaning a little like a cut tile's.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    const bool began = ImGui::Begin("ReSkate###skate-menu-sheet", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    if (began) {
        take_focus_for_pad();
        const auto origin = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        auto* draw = ImGui::GetWindowDrawList();
        const float lean = px(6), band = px(104), footer = px(50), pad_x = px(26);
        const auto edge_at = [&](float y) { return origin.x + lean * (1.0f - (y - origin.y) / std::max(1.0f, size.y)); };
        draw->AddQuadFilled(ImVec2(origin.x + lean, origin.y), ImVec2(origin.x + size.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y),
                            ImVec2(origin.x, origin.y + size.y), sheet_fill);
        draw->AddQuadFilled(ImVec2(origin.x + lean, origin.y), ImVec2(origin.x + size.x, origin.y), ImVec2(origin.x + size.x, origin.y + band),
                            ImVec2(edge_at(origin.y + band), origin.y + band), band_fill);
        const float foot = origin.y + size.y - footer;
        draw->AddQuadFilled(ImVec2(edge_at(foot), foot), ImVec2(origin.x + size.x, foot), ImVec2(origin.x + size.x, origin.y + size.y),
                            ImVec2(origin.x, origin.y + size.y), band_fill);

        // The left edge, held and dragged, sets the width; it is saved once let go.
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::InvisibleButton("##sheet-edge", ImVec2(px(8), size.y));
        ImGui::PopItemFlag();
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            draw->AddLine(ImVec2(edge_at(origin.y) + px(1), origin.y), ImVec2(origin.x + px(1), origin.y + size.y), blue, px(3));
        }
        if (ImGui::IsItemActive()) {
            menu.sheet_dragging = true;
            menu.sheet_drag_width = std::clamp(display.x - io.MousePos.x, low, high);
        } else if (menu.sheet_dragging) {
            menu.sheet_dragging = false;
            const float reference = std::clamp(menu.sheet_drag_width / px(1), min_menu_sheet_width, max_menu_sheet_width);
            menu.sheet_width_pending = reference;
            menu.sheet_width_until = ImGui::GetTime() + 3.0;
            quietly(callbacks, std::format("ui sheetwidth {:.0f}", reference));
        }

        // The header: LB, the page's name with a dot for each page under it, RB. The pointer clicks
        // the buttons and the dots.
        const auto page_button = [&](const char* id, ImVec2 at, Glyph which) {
            const float w = glyph(nullptr, menu, at, which, pad.playstation);
            ImGui::SetCursorScreenPos(ImVec2(at.x - px(6), at.y - px(6)));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(w + px(12), px(34)));
            ImGui::PopItemFlag();
            if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(at.x - px(4), at.y - px(4)), ImVec2(at.x + w + px(4), at.y + px(26)), IM_COL32(255, 255, 255, 30));
            glyph(draw, menu, at, which, pad.playstation);
            return clicked;
        };
        const float cap_y = origin.y + band * .5f - px(11);
        if (page_button("##previous-page", ImVec2(origin.x + pad_x, cap_y), Glyph::lb)) step_page(menu, model, -1);
        const float rb_width = glyph(nullptr, menu, ImVec2(), Glyph::rb, pad.playstation);
        if (page_button("##next-page", ImVec2(origin.x + size.x - pad_x - rb_width, cap_y), Glyph::rb)) step_page(menu, model, 1);
        // A plugin's page opened from PLUGINS is titled with the plugin's name (page_title), so the sheet says
        // whose page it is; the names row under it still marks PLUGINS, where B goes back to.
        const char* name = page_title(menu);
        ImFont* big = title_font(menu);
        const float title = px(44);
        const auto extent = big->CalcTextSizeA(title, FLT_MAX, 0, name);
        const ImVec2 name_at(origin.x + (size.x + lean - extent.x) * .5f, origin.y + px(12));
        const int start = draw->VtxBuffer.Size;
        draw->AddText(big, title, ImVec2(name_at.x + px(2), name_at.y + px(3)), IM_COL32(0, 0, 0, 160), name);
        draw->AddText(big, title, name_at, paper, name);
        skate_theme::rotate_since(draw, start, -2.0f, ImVec2(name_at.x + extent.x * .5f, name_at.y + extent.y * .5f));
        {
            // Every page by name under the title, the open one white and underlined, the row slid so
            // it sits in the middle: which pages LB and RB lead to reads at a glance (dots did not).
            const float size_text = px(13), spacing = px(16), y = origin.y + px(70);
            const float left = origin.x + pad_x + rb_width + px(14), right = origin.x + size.x - pad_x - rb_width - px(14);
            std::vector<std::pair<int, float>> names;
            float total = 0, before = 0;
            for (int i = 0; i < page_count; ++i) {
                if (page_hidden(model, i)) continue;
                const float w = menu.bold->CalcTextSizeA(size_text, FLT_MAX, 0, page_names[static_cast<std::size_t>(i)]).x;
                if (i == menu.page) before = total + w * .5f;
                names.emplace_back(i, w);
                total += w + spacing;
            }
            total -= spacing;
            const float middle = (left + right) * .5f;
            float x = total <= right - left ? middle - total * .5f : std::clamp(middle - before, right - total, left);
            draw->PushClipRect(ImVec2(left, y - px(4)), ImVec2(right, y + px(22)), true);
            for (const auto& [i, w] : names) {
                const bool open = i == menu.page;
                ImGui::SetCursorScreenPos(ImVec2(x - spacing * .5f, y - px(4)));
                ImGui::PushID(i);
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                const bool visible_name = x + w > left && x < right;
                if (visible_name && ImGui::InvisibleButton("##page-name", ImVec2(w + spacing, px(24))))
                    open_page(menu, i);
                const bool hovered = visible_name && ImGui::IsItemHovered();
                ImGui::PopItemFlag();
                ImGui::PopID();
                draw->AddText(menu.bold, size_text, ImVec2(x, y), open ? paper : hovered ? IM_COL32(210, 212, 216, 255) : skate_theme::grey_text,
                              page_names[static_cast<std::size_t>(i)]);
                if (open) draw->AddRectFilled(ImVec2(x - px(2), y + px(17)), ImVec2(x + w + px(2), y + px(20)), blue);
                x += w + spacing;
            }
            draw->PopClipRect();
        }

        // The page: its tabs, then its body, scrolling.
        const bool hint_strip = pad.in_charge; // a controller has no hover tooltips: what the focus does shows above the prompts
        const float hint = hint_strip ? px(30) : 0.0f, top = band + px(14);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad_x, origin.y + top));
        ImGui::BeginChild("page", ImVec2(size.x - pad_x * 2, std::max(px(80), size.y - top - footer - hint - px(8))), ImGuiChildFlags_NavFlattened);
        draw_page_body(menu, model, callbacks);
        ImGui::EndChild();
        if (!menu.recording_bind) pad_scroll(ImGui::GetCurrentWindow());

        if (hint_strip && !console_hint().empty()) {
            draw->PushClipRect(ImVec2(origin.x + pad_x, foot - hint), ImVec2(origin.x + size.x - pad_x, foot), true);
            draw->AddText(menu.body, px(15), ImVec2(origin.x + pad_x, foot - hint + px(6)), muted, console_hint().c_str());
            draw->PopClipRect();
        }
        footer_prompts(menu, callbacks, visible, ImVec2(origin.x + pad_x, foot + (footer - px(22)) * .5f), origin.x + size.x - pad_x, pad, false);
    }
    ImGui::End();
}

// The hub rail: a window in the middle of the screen, the pages down its left side in the brushed
// font. Dragging an edge or a corner resizes it (saved, `ui hubsize`); it reopens centred.
void draw_hub(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool& visible, bool appearing, Pad pad) {
    auto& io = ImGui::GetIO();
    const auto display = io.DisplaySize;
    const ImVec2 maximum(std::max(px(400.0f), display.x - 24), std::max(px(320.0f), display.y - 24));
    const ImVec2 minimum(std::min(px(min_menu_hub_width), maximum.x), std::min(px(min_menu_hub_height), maximum.y));
    const bool saved = model.menu_hub_width > 0 && model.menu_hub_height > 0;
    const ImVec2 reference = menu.hub_size_pending.value_or(saved ? ImVec2(model.menu_hub_width, model.menu_hub_height)
                                                                  : ImVec2(default_menu_hub_width, default_menu_hub_height));
    const ImVec2 wanted(std::clamp(px(reference.x), minimum.x, maximum.x), std::clamp(px(reference.y), minimum.y, maximum.y));
    // The saved size is given to ImGui on opening and when it changes (another scale, the saved size
    // arriving); between those the window keeps the size the player drags it to.
    const bool differs = std::abs(wanted.x - menu.hub_size_applied.x) > 1 || std::abs(wanted.y - menu.hub_size_applied.y) > 1;
    if (appearing || (differs && !io.MouseDown[0])) {
        ImGui::SetNextWindowSize(wanted, ImGuiCond_Always);
        menu.hub_size_applied = wanted;
    }
    ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), appearing ? ImGuiCond_Always : ImGuiCond_FirstUseEver, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, window_fill);
    const bool began = ImGui::Begin("ReSkate###skate-menu-hub", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    if (began) {
        take_focus_for_pad();
        const auto size = ImGui::GetWindowSize();
        const auto current = ImGui::GetWindowPos();
        const ImVec2 bounded(std::clamp(current.x, 0.0f, std::max(0.0f, display.x - size.x)),
            std::clamp(current.y, 0.0f, std::max(0.0f, display.y - size.y)));
        if (bounded.x != current.x || bounded.y != current.y) ImGui::SetWindowPos(bounded);
        // A size other than the one given is the player's drag: saved once the mouse lets go.
        if ((std::abs(size.x - menu.hub_size_applied.x) > 1 || std::abs(size.y - menu.hub_size_applied.y) > 1) && !io.MouseDown[0]) {
            const ImVec2 dragged(std::clamp(size.x / px(1), min_menu_hub_width, max_menu_hub_side),
                                 std::clamp(size.y / px(1), min_menu_hub_height, max_menu_hub_side));
            menu.hub_size_pending = dragged;
            menu.hub_size_until = ImGui::GetTime() + 3.0;
            menu.hub_size_applied = size;
            quietly(callbacks, std::format("ui hubsize {:.0f}x{:.0f}", dragged.x, dragged.y));
        }
        const auto origin = ImGui::GetWindowPos();
        auto* draw = ImGui::GetWindowDrawList();

        // The rail: the wordmark, then the pages in the brushed font, the open one white on a blue stroke.
        const float rail = px(250);
        draw->AddRectFilled(origin, ImVec2(origin.x + rail, origin.y + size.y), rail_fill);
        wordmark(draw, menu, ImVec2(origin.x + px(28), origin.y + px(20)), px(42));
        {
            const float top = px(100), list_height = size.y - top - px(20);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + px(18), origin.y + top));
            ImGui::BeginChild("pages", ImVec2(rail - px(18), list_height), ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            int rows = 0;
            for (int i = 0; i < page_count; ++i) rows += page_hidden(model, i) ? 0 : 1;
            const float row = std::clamp(list_height / static_cast<float>(std::max(rows, 1)) - ImGui::GetStyle().ItemSpacing.y, px(26), px(38));
            auto* list = ImGui::GetWindowDrawList();
            ImFont* font = title_font(menu);
            for (int i = 0; i < page_count; ++i) {
                if (page_hidden(model, i)) continue;
                const auto at = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("##page", ImVec2(rail - px(36), row), ImGuiButtonFlags_EnableNav)) open_page(menu, i);
                const bool lit = nav_lit(), hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = menu.page == i;
                if (on || lit) {
                    // A brush stroke, cut crooked at its ends.
                    const float x0 = at.x, x1 = at.x + px(200), y0 = at.y + row * .08f, y1 = at.y + row * .92f, h = y1 - y0;
                    list->AddQuadFilled(ImVec2(x0 + px(4), y0 + h * .12f), ImVec2(x1, y0), ImVec2(x1 - px(8), y1), ImVec2(x0, y1 - h * .12f), lit ? paper : blue);
                }
                const float text = std::min(px(24), row * .8f);
                list->AddText(font, text, ImVec2(at.x + px(10), at.y + (row - text) * .5f), lit ? focus_ink : on || hovered ? paper : quiet_ink,
                              page_names[static_cast<std::size_t>(i)]);
            }
            ImGui::EndChild();
        }

        // The page: its title, its tabs and body, the prompts under it.
        const float left = rail + px(34), right_pad = px(34), footer = px(52);
        const float main_width = size.x - left - right_pad;
        brushed_title(draw, menu, ImVec2(origin.x + left, origin.y + px(18)), page_title(menu), page_subtitle(menu), px(60),
                      origin.x + size.x - right_pad - px(40));
        {
            // Close, for the pointer.
            const ImVec2 at(origin.x + size.x - px(44), origin.y + px(16));
            ImGui::SetCursorScreenPos(at);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::InvisibleButton("##close", ImVec2(px(28), px(28)))) visible = false;
            ImGui::PopItemFlag();
            const ImU32 cross = ImGui::IsItemHovered() ? paper : muted;
            draw->AddLine(ImVec2(at.x + px(8), at.y + px(8)), ImVec2(at.x + px(20), at.y + px(20)), cross, px(2.5f));
            draw->AddLine(ImVec2(at.x + px(20), at.y + px(8)), ImVec2(at.x + px(8), at.y + px(20)), cross, px(2.5f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close");
        }
        const float top = px(96);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + left, origin.y + top));
        ImGui::BeginChild("page", ImVec2(main_width, std::max(px(80), size.y - top - footer - px(8))), ImGuiChildFlags_NavFlattened);
        draw_page_body(menu, model, callbacks);
        ImGui::EndChild();
        if (!menu.recording_bind) pad_scroll(ImGui::GetCurrentWindow());

        const float foot = origin.y + size.y - footer;
        draw->AddRectFilled(ImVec2(origin.x + left, foot), ImVec2(origin.x + size.x - right_pad, foot + px(2)), rule_ink);
        footer_prompts(menu, callbacks, visible, ImVec2(origin.x + left, foot + (footer - px(22)) * .5f), origin.x + size.x - right_pad, pad, true);
    }
    ImGui::End();
}

// The game's menu sounds as its pause menu makes them: a tab or page changing, a press (A, Enter, a
// click on a control), the focus moving.
void menu_sounds() {
    ImGuiContext& g = *GImGui;
    static ImGuiID heard{};
    static double moved_at{};
    constexpr std::uint32_t navigate = 3, select = 7, tab = 56;
    if (menu::tab_switched) {
        play_ui_sound(tab);
    } else {
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

// The kit's look over ImGui's own, for the menu and for a page drawn in the kit outside it (draw_in_kit).
// px() must already be at the scale drawn at. Returns how many colours it pushed, for pop_kit_style.
int push_kit_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    // ImGui's own widgets on a page as tall as the kit's rows.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(14), (row_height() - px(text_size)) * .5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, px(10));
    int colours = skate_theme::push_widget_colours();
    // Raw ImGui widgets on a page in the kit's greys, and a controller's focus in white: the theme's
    // blue vanishes on blue buttons and tabs.
    const std::pair<ImGuiCol, ImU32> kit_colours[]{
        {ImGuiCol_FrameBg, row_fill}, {ImGuiCol_FrameBgHovered, row_hover}, {ImGuiCol_FrameBgActive, IM_COL32(66, 67, 72, 255)},
        {ImGuiCol_Button, row_fill}, {ImGuiCol_ButtonHovered, row_hover}, {ImGuiCol_ButtonActive, skate_theme::blue_active},
        {ImGuiCol_Header, row_hover}, {ImGuiCol_HeaderHovered, IM_COL32(66, 67, 72, 255)}, {ImGuiCol_HeaderActive, skate_theme::blue_active},
        {ImGuiCol_PopupBg, IM_COL32(22, 22, 24, 252)}, {ImGuiCol_Border, rule_ink}, {ImGuiCol_Separator, rule_ink},
        {ImGuiCol_TableHeaderBg, row_fill}, {ImGuiCol_NavCursor, paper}};
    for (const auto& [id, colour] : kit_colours) ImGui::PushStyleColor(id, colour);
    return colours + static_cast<int>(std::size(kit_colours));
}
void pop_kit_style(int colours) {
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(10);
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
    // Esc closes the menu, and B goes back to HOME first, unless it is busy with something else:
    // typing in a field, an open popup, or recording a key or controller binding.
    const bool idle = !io.WantTextInput && !detail::online_embedded_typing() && !menu.recording_bind && !menu.voice_bind_capture && !ImGui::IsAnyItemActive() &&
                      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // What was just chosen or dragged shows until the saved setting comes back through the model.
    const double now = ImGui::GetTime();
    if (menu.layout_pending && (*menu.layout_pending == model.menu_layout || now > menu.layout_until)) menu.layout_pending.reset();
    if (menu.sheet_width_pending && (std::abs(*menu.sheet_width_pending - model.menu_sheet_width) < 0.5f || now > menu.sheet_width_until))
        menu.sheet_width_pending.reset();
    if (menu.hub_size_pending && ((std::abs(menu.hub_size_pending->x - model.menu_hub_width) < 0.5f &&
                                   std::abs(menu.hub_size_pending->y - model.menu_hub_height) < 0.5f) ||
                                  now > menu.hub_size_until))
        menu.hub_size_pending.reset();
    const int layout = menu_layout(menu, model);
    const bool appearing = !was_drawn || layout != menu.layout_drawn;
    menu.layout_drawn = layout;
    shown_layout = layout;
    if (visible && idle && was_drawn) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) visible = false;
        // (B closes an open row first: one whose actions have the focus, or a page's expand_row.)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !close_open_row() && !page_holds_back()) {
            if (menu.page != page_home) open_page(menu, page_home);
            else visible = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) step_page(menu, model, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) step_page(menu, model, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F11, false))
            set_menu_layout(menu, callbacks, layout == menu_layout_hub ? menu_layout_sheet : menu_layout_hub);
    }
    menu.last_menu_frame = ImGui::GetFrameCount();
    // One size for both layouts, measured at 1080 lines: it grows with a bigger screen, so it reads
    // the same at 1440p or 4K. A size slider held shows its size as it moves.
    // The kit's rows and text are a size larger than the old window's at the same setting: 0.85 brings
    // the default back to about what it was.
    const float display = std::clamp(io.DisplaySize.y / 1080.0f, 1.0f, 2.0f) * 0.85f;
    const float chosen = menu.scale_editing ? menu.scale : model.menu_scale;
    ui_scale = std::clamp(chosen, min_menu_scale, max_menu_scale) * display;
    // Past this the regular faces would be stretched: the larger set draws instead.
    const bool large = ui_scale > 1.25f && menu.body_large && menu.bold_large && menu.heading_large && menu.title_large;
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
    if (trainer_page_wanted()) open_page(menu, page_sandbox);
    if (trainer_cheats_wanted()) {
        open_page(menu, page_skater);
        menu.skater_tab = 2;
    }
    menu.page = std::clamp(menu.page, 0, static_cast<int>(page_count) - 1);
    if (page_hidden(model, menu.page)) menu.page = page_home;
    menu_page_owns_pad().store(menu.recording_bind != 0);
    // The pad in use names its buttons in the prompts, which show while it is in charge.
    const auto gamepad = read_gamepad();
    const Pad pad{menu_pad_in_charge().load(), gamepad.playstation};
    // X (Square on a PlayStation pad), which pages use as a row's second action (pad_x_pressed).
    constexpr unsigned pad_x_bit = 0x4000; // XINPUT_GAMEPAD_X
    static unsigned pad_before{};
    const unsigned pad_now = gamepad.valid ? gamepad.buttons : 0;
    pad_x_down = (pad_now & pad_x_bit) && !(pad_before & pad_x_bit) && !io.WantTextInput;
    pad_before = pad_now;

    ImGui::PushFont(menu.body);
    const int colours = push_kit_style();
    begin_controls_frame(menu.bold);
    if (layout == menu_layout_hub) draw_hub(menu, model, callbacks, visible, appearing, pad);
    else draw_sheet(menu, model, callbacks, visible, appearing, pad);
    menu_sounds();
    pop_kit_style(colours);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (large) {
        menu.body = small_fonts[0];
        menu.bold = small_fonts[1];
        menu.heading = small_fonts[2];
        menu.title = small_fonts[3];
    }
}

namespace menu {
void draw_in_kit(SkateMenu& menu, float scale, const std::function<void()>& body) {
    if (!menu.body || !menu.bold || !menu.heading || !menu.title) return;
    // What the menu's own frame set, put back after: it may have drawn earlier this frame, and its
    // footer and its record of the pages lately opened read these.
    const float kept_scale = ui_scale;
    const int kept_layout = shown_layout, kept_primary_tab = primary_tab;
    const bool kept_tab_switched = tab_switched;
    ImFont* const kept_control_font = control_font;
    const std::string kept_focus_hint = focus_hint, kept_pending_hint = pending_hint, kept_primary_name = primary_tab_name;
    const double kept_step = next_step;
    // The wide layout's cards, two columns of them in a box wide enough, as on the menu's own window.
    ui_scale = scale;
    shown_layout = menu_layout_hub;
    auto& io = ImGui::GetIO();
    const bool large = ui_scale > 1.25f && menu.body_large && menu.bold_large && menu.heading_large && menu.title_large;
    const std::array<ImFont*, 4> small_fonts{menu.body, menu.bold, menu.heading, menu.title};
    if (large) {
        menu.body = menu.body_large;
        menu.bold = menu.bold_large;
        menu.heading = menu.heading_large;
        menu.title = menu.title_large;
    }
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = large ? ui_scale / large_font_ratio : ui_scale;
    // Over the game world the menu's greys read as grey cards: the pause pages' near black instead.
    const ImU32 kept_card = card_fill, kept_row = row_fill;
    card_fill = IM_COL32(16, 16, 16, 242), row_fill = IM_COL32(29, 29, 31, 250);
    ImGui::PushFont(menu.body);
    const int colours = push_kit_style();
    begin_controls_frame(menu.bold);
    body();
    pop_kit_style(colours);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (large) {
        menu.body = small_fonts[0];
        menu.bold = small_fonts[1];
        menu.heading = small_fonts[2];
        menu.title = small_fonts[3];
    }
    card_fill = kept_card, row_fill = kept_row;
    ui_scale = kept_scale;
    shown_layout = kept_layout;
    primary_tab = kept_primary_tab;
    tab_switched = kept_tab_switched;
    control_font = kept_control_font;
    focus_hint = kept_focus_hint;
    pending_hint = kept_pending_hint;
    primary_tab_name = kept_primary_name;
    next_step = kept_step;
}
}
}
