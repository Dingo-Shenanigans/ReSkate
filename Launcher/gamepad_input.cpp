#include "gamepad_input.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace dingosdk::launcher_gui {
namespace {

// XINPUT_GAMEPAD_* bits, spelled out so this file needs no Windows headers.
constexpr std::uint16_t dpad_up = 0x0001, dpad_down = 0x0002, dpad_left = 0x0004, dpad_right = 0x0008;
constexpr std::uint16_t right_thumb = 0x0080, button_a = 0x1000, button_b = 0x2000;

struct Button { std::uint16_t bit; ImGuiKey key; };
// Only what moves and presses the focus. X/Y and the shoulders stay out: ImGui
// reads them as its window switcher and tweak modifiers, which the launcher has
// no use for.
constexpr Button focus_buttons[]{
    {dpad_up, ImGuiKey_GamepadDpadUp}, {dpad_down, ImGuiKey_GamepadDpadDown},
    {dpad_left, ImGuiKey_GamepadDpadLeft}, {dpad_right, ImGuiKey_GamepadDpadRight},
    {button_a, ImGuiKey_GamepadFaceDown}};
constexpr std::uint16_t focus_bits = dpad_up | dpad_down | dpad_left | dpad_right | button_a;

// XInput's left stick dead zone.
constexpr float left_dead_zone = 7849.0f / 32767.0f;
// Scrolling at full tilt, in design pixels a second.
constexpr float scroll_speed = 1200.0f;

// How the right stick drives the pointer.
struct PointerFeel {
    float dead_zone;
    float speed;      // design pixels a second at full tilt
    bool squared;     // a small tilt is slow, for placing the pointer precisely
};
// A thumbstick sets a speed. A resting thumb is never quite centred, so the
// dead zone is large.
constexpr PointerFeel stick_feel{0.2f, 1400.0f, true};
// The Steam Deck's trackpad, in Steam's gamepad layouts, tilts the stick by how
// fast the thumb swipes, and a quick swipe already tilts it all the way. The
// tilt drops back to rest the moment the thumb lifts. Small swipes must move
// the pointer and a quick one must cross the window.
constexpr PointerFeel trackpad_feel{0.06f, 2600.0f, false};
// The right stick never rests quite at the middle, and Steam's trackpad rests
// a little off it too, somewhere new after each touch. A reading that holds
// this still, this near the middle, for this long is where it rests.
constexpr float rest_jitter = 0.01f;
constexpr float rest_limit = 0.12f;
constexpr float rest_seconds = 0.3f;
// A mouse move this close to the last one is the same place reported again.
constexpr float pointer_slack = 2.0f;

float axis(std::int16_t value) { return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f); }

// The left stick scrolls whatever the user is looking at: the panel holding
// the pad's focus while the focus is shown, else the one under the pointer.
// ImGui's own stick scrolling only ever moves the focused window, which under a
// pointer is usually the page itself, with nothing to scroll. It scrolls the
// panel directly: wheel events would queue behind the pointer's moves and lag.
void scroll(const ImGuiIO& io, float tilt, float scale) {
    const float amount = std::copysign(std::clamp((std::abs(tilt) - left_dead_zone) / (1 - left_dead_zone), 0.0f, 1.0f), tilt);
    if (amount == 0.0f) return;
    const ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.NavCursorVisible && g.NavHighlightItemUnderNav ? g.NavWindow : g.HoveredWindow;
    // The nearest panel around it that can scroll.
    while (window && window->ScrollMax.y <= 0 && (window->Flags & ImGuiWindowFlags_ChildWindow)) window = window->ParentWindow;
    if (window && window->ScrollMax.y > 0)
        ImGui::SetScrollY(window, window->Scroll.y - amount * scroll_speed * scale * io.DeltaTime);
}

// B goes to ImGui's own cancel while ImGui has something to cancel: a combo or
// menu to close, or a text field to leave. Otherwise it is Escape, the
// launcher's way back out of a page or a modal.
bool imgui_cancels() {
    const ImGuiContext& g = *GImGui;
    if (ImGui::IsAnyItemActive()) return true;
    if (g.OpenPopupStack.Size == 0) return false;
    const ImGuiWindow* top = g.OpenPopupStack.back().Window;
    return top && !(top->Flags & ImGuiWindowFlags_Modal);
}

bool moved_away(ImVec2 position, ImVec2 before) {
    return std::abs(position.x - before.x) > pointer_slack || std::abs(position.y - before.y) > pointer_slack;
}

} // namespace

void PadFeed::update(ImGuiIO& io, const PadState& input, float scale) {
    // A pad that went away lets go of everything it held.
    const PadState pad = input.connected ? input : PadState{};
    const auto changed = static_cast<std::uint16_t>(pad.buttons ^ buttons_);
    const auto pressed = static_cast<std::uint16_t>(pad.buttons & changed);
    if (pad.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

    // Only changes are sent: the keyboard shares Escape and the mouse shares the
    // left button, and a pad repeating "up" every frame would cancel them.
    for (const auto& button : focus_buttons)
        if (changed & button.bit) io.AddKeyEvent(button.key, (pad.buttons & button.bit) != 0);
    if (changed & button_b) {
        const bool down = (pad.buttons & button_b) != 0;
        // The release goes to whichever key the press went to.
        if (down) back_key_ = imgui_cancels() ? ImGuiKey_GamepadFaceRight : ImGuiKey_Escape;
        io.AddKeyEvent(back_key_, down);
    }
    scroll(io, axis(pad.left_y), scale);
    buttons_ = pad.buttons;

    // Moving the focus hands the screen back to it: ImGui hides the focus while
    // a pointer moves, so the pointer goes away instead. Scrolling keeps both.
    if (pressed & focus_bits) pointing_ = false;

    const PointerFeel& feel = trackpad_ ? trackpad_feel : stick_feel;
    const float raw_x = axis(pad.right_x), raw_y = -axis(pad.right_y);   // screen Y grows downward
    if (!pad.connected) {
        // No pad is being read (none there, or the launcher is in the background, which reads
        // as none): that is not the stick at rest. Taking the middle for its rest here made a
        // stick that rests off it, or a wheel or throttle on that axis, steer the pointer for
        // good once the launcher had the focus again, and the mouse with it.
        steady_ = 0;
        rest_known_ = false;
        pointing_ = false;
    } else if (std::abs(raw_x - last_x_) < rest_jitter && std::abs(raw_y - last_y_) < rest_jitter &&
        std::sqrt(raw_x * raw_x + raw_y * raw_y) < rest_limit) {
        steady_ += io.DeltaTime;
        if (steady_ >= rest_seconds) {
            rest_ = ImVec2(raw_x, raw_y);
            rest_known_ = true;
        }
    } else {
        steady_ = 0;
    }
    last_x_ = raw_x;
    last_y_ = raw_y;
    // Measured from where it rests, and not at all until that is known: a
    // pointer creeping off on its own would take the screen from the focus.
    const float x = raw_x - rest_.x, y = raw_y - rest_.y;
    const float tilt = std::min(1.0f, std::sqrt(x * x + y * y));
    const bool steering = rest_known_ && tilt > feel.dead_zone;
    // The first R3 only shows the pointer: a stray click must not press
    // whatever the mouse last rested on.
    const bool click = pointing_ && (pressed & right_thumb);
    if (!pointing_ && (steering || (pressed & right_thumb))) {
        pointing_ = true;
        // From wherever the mouse left it, which may be outside the window.
        pointer_ = ImGui::IsMousePosValid(&io.MousePos) ? io.MousePos
                                                        : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    }
    if (pointing_ && steering) {
        const float speed = (tilt - feel.dead_zone) / (1 - feel.dead_zone);
        const float step = (feel.squared ? speed * speed : speed) * feel.speed * scale * io.DeltaTime / tilt;
        pointer_.x += x * step;
        pointer_.y += y * step;
    }
    if (pointing_) {
        pointer_.x = std::clamp(pointer_.x, 0.0f, std::max(0.0f, io.DisplaySize.x - 1));
        pointer_.y = std::clamp(pointer_.y, 0.0f, std::max(0.0f, io.DisplaySize.y - 1));
        // Every frame, after the backend's own report of the system cursor,
        // which stays where the mouse left it.
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(pointer_.x, pointer_.y);
    }
    if (click || (clicking_ && (changed & right_thumb))) {
        clicking_ = click;
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, click);
    }
    io.MouseDrawCursor = pointing_;
}

namespace {

// The list row holding the focus, as it was last drawn.
struct FocusedRow {
    ImGuiID scope{}, window{}, item{};
    ImRect rect;   // relative to its window
};
FocusedRow g_focused_row;
bool g_row_focused{};
ImGuiID g_row_scope{};

// The outermost of the flattened panels around a window: a page's rail, or
// its content with the list inside it.
ImGuiWindow* column_of(ImGuiWindow* window) {
    if (!(window->ChildFlags & ImGuiChildFlags_NavFlattened)) return nullptr;
    while (window->ParentWindow && (window->ParentWindow->ChildFlags & ImGuiChildFlags_NavFlattened))
        window = window->ParentWindow;
    return window;
}

} // namespace

void PadFeed::after_new_frame() {
    ImGuiContext& g = *GImGui;
    const ImGuiID nav_window = g.NavWindow ? g.NavWindow->ID : 0;
    // ImGui lets go of an item its panel only partly shows, to move from the
    // part in view, and falls back to the page's default item, a rail tab,
    // when that move finds nothing. The focus stays where it was instead.
    if (g.NavMoveSubmitted && !g.NavId && last_nav_id_ && nav_window == last_nav_window_) {
        g.NavId = last_nav_id_;
        g.NavInitRequest = g.NavInitRequestFromMove = false;
    }

    // Up and down from a list row leave the row, from whichever of its parts
    // has the focus: ImGui looks along the line the focus last went sideways
    // on, and finds the row's own tick or switch just above or below it.
    if (g.NavMoveSubmitted && (g.NavMoveDir == ImGuiDir_Up || g.NavMoveDir == ImGuiDir_Down) &&
        g_focused_row.scope && g.NavFocusScopeId == g_focused_row.scope)
        if (ImGuiWindow* window = ImGui::FindWindowByID(g_focused_row.window)) {
            const ImRect row = ImGui::WindowRectRelToAbs(window, g_focused_row.rect);
            g.NavScoringRect.Min.y = g.NavScoringRect.Max.y = g.NavMoveDir == ImGuiDir_Up ? row.Min.y : row.Max.y;
        }

    // ImGui looks to the side only along one line through the focus, so from a
    // list row it misses the rail's tiles above or below it. A side move that
    // found nothing looks again: across the focus's full height, for a tile
    // level with the gap between two rows; then at every height from the edge
    // of the focus's column, nearest first, for BACK far under a row.
    if (side_dir_ != ImGuiDir_None && side_tries_ < 2 && g.NavId == side_from_ && g.NavWindow && !g.NavMoveSubmitted) {
        ++side_tries_;
        ImGui::NavMoveRequestSubmit(side_dir_, side_dir_, ImGuiNavMoveFlags_None,
            ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
        ImRect from = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
        if (side_tries_ == 2) {
            if (const ImGuiWindow* column = column_of(g.NavWindow)) {
                // From the column's edge, so nothing else in it, like the tick
                // above the list, comes before the next column.
                from.Min.x = from.Max.x = side_dir_ == ImGuiDir_Left ? column->Rect().Min.x : column->Rect().Max.x;
            }
            // ImGui measures only the middle 60% of the height, which must
            // still cover the whole window.
            const float middle = from.GetCenter().y, reach = g.IO.DisplaySize.y * 2;
            from.Min.y = middle - reach;
            from.Max.y = middle + reach;
        }
        g.NavScoringRect = from;
    } else {
        side_dir_ = ImGuiDir_None;
        side_tries_ = 0;
        if (g.NavMoveSubmitted && (g.NavMoveDir == ImGuiDir_Left || g.NavMoveDir == ImGuiDir_Right)) {
            side_dir_ = g.NavMoveDir;
            side_from_ = g.NavId;
        }
    }
    last_nav_id_ = g.NavId;
    last_nav_window_ = nav_window;
}

void default_focus() {
    ImGui::SetItemDefaultFocus();
    ImGuiContext& g = *GImGui;
    // With a pad there, the focus starts on it straight away as the window
    // appears: when the launcher starts, and when the pad opened this page.
    // (Not whenever nothing has the focus: ImGui clears it for a frame while
    // the focus moves off a row wider than its panel.)
    const bool pad = (g.IO.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
    const bool pad_led = g.NavCursorVisible && g.NavHighlightItemUnderNav;
    if (!pad || !ImGui::IsWindowAppearing() || !(pad_led || g.FrameCount <= 2)) return;
    ImGui::SetFocusID(g.LastItemData.ID, g.CurrentWindow);
    g.NavCursorVisible = true;
    g.NavHighlightItemUnderNav = true;
}

void begin_row() {
    g_row_scope = ImGui::GetID("##row_focus");
    g_row_focused = GImGui->NavFocusScopeId == g_row_scope;
    ImGui::PushFocusScope(g_row_scope);
}

void row_buttons() {
    if (g_row_focused) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        g_focused_row = {g_row_scope, window->ID, GImGui->LastItemData.ID,
                         ImGui::WindowRectAbsToRel(window, GImGui->LastItemData.NavRect)};
    }
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, !g_row_focused);
}

void end_row() {
    ImGui::PopItemFlag();
    ImGui::PopFocusScope();
}

void hold_focus(bool covered) {
    static struct {
        bool held{};
        ImGuiID id{}, window{};
        bool shown{};
    } held;
    ImGuiContext& g = *GImGui;
    if (covered && !held.held) {
        const bool in_row = g_focused_row.scope && g.NavFocusScopeId == g_focused_row.scope;
        held = {true, in_row ? g_focused_row.item : g.NavId,
                in_row ? g_focused_row.window : g.NavWindow ? g.NavWindow->ID : 0,
                g.NavCursorVisible && g.NavHighlightItemUnderNav};
    } else if (!covered && held.held) {
        held.held = false;
        ImGuiWindow* window = ImGui::FindWindowByID(held.window);
        if (!held.id || !window) return;
        ImGui::SetFocusID(held.id, window);
        g.NavCursorVisible = g.NavHighlightItemUnderNav = held.shown;
    }
}

void keep_focus_in_list() {
    ImGuiContext& g = *GImGui;
    if (!g.NavMoveScoringItems || (g.NavMoveDir != ImGuiDir_Up && g.NavMoveDir != ImGuiDir_Down)) return;
    // By the columns the list covers: its rows run on past what it shows.
    const ImRect list = g.CurrentWindow->Rect();
    const auto in_list = [&](float x) { return x >= list.Min.x && x <= list.Max.x; };
    // The focus is in the list itself, or in a panel inside it.
    ImGuiWindow* window = g.CurrentWindow;
    if (!g.NavWindow || !(g.NavWindow == window || ImGui::IsWindowChildOf(g.NavWindow, window, false))) return;
    // Everything around the list has had its chance by now: the rail comes
    // before the content, and nothing under the list takes the focus.
    const ImGuiNavItemData& best = g.NavMoveResultLocal.ID ? g.NavMoveResultLocal : g.NavMoveResultOther;
    if (best.ID && in_list(ImGui::WindowRectRelToAbs(best.Window, best.RectRel).GetCenter().x)) return;
    ImGui::NavMoveRequestCancel();
}

void PadFeed::mouse_moved(ImVec2 position) {
    // Windows repeats a mouse move where the mouse already was when a window
    // changes under it; only a move to a new place is the mouse or a finger.
    if (pointing_ && moved_away(position, mouse_)) pointing_ = false;
    mouse_ = position;
}

} // namespace dingosdk::launcher_gui
