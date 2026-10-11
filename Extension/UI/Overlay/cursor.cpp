#include "overlay_internal.h"
#include "cursor.h"
#include "input_capture.h"
#include "Engine/Core/Log/logging.h"

namespace dingosdk::overlay::detail {
bool game_window_foreground(HWND window) {
    if (!window || !IsWindow(window)) return false;
    const auto root = GetAncestor(window, GA_ROOT);
    return GetForegroundWindow() == (root ? root : window) && !IsIconic(root ? root : window);
}

bool owns_menu_cursor(const State& s) {
    // WM_KILLFOCUS can refer to a child control. A cached flag must not let the
    // game resume clipping/centering while its top-level window is still active.
    return s.input_attached.load() && !s.stop.load() && !s.failed.load() &&
        interactive_visible(s) && game_window_foreground(s.window.load());
}

UINT cursor_sync_message() {
    static const auto message = RegisterWindowMessageW(L"ReSkate.Overlay.CursorOwnership");
    return message;
}

void request_window_cursor_sync(HWND window) {
    const auto message = cursor_sync_message();
    if (window && message) PostMessageW(window, message, 0, 0);
}

void sync_menu_cursor(bool release) {
    auto& s = state();
    bool changed{};
    const auto window = s.window.load();
    {
        std::lock_guard lock(s.cursor_mutex);
        const auto clip = original<ClipCursorFn>(s.clip_cursor);
        if (!clip) return;
        const bool own = !release && owns_menu_cursor(s);
        changed = own != s.cursor_released;
        if (own) {
            if (!s.cursor_released) s.cursor_clip_pending = GetClipCursor(&s.cursor_clip) != FALSE;
            s.cursor_released = true;
            clip(nullptr);
        } else if (s.cursor_released) {
            s.cursor_released = false;
            // Do not restore a game's clip over another foreground application.
            clip(!release && game_window_foreground(window) && s.cursor_clip_pending ? &s.cursor_clip : nullptr);
            s.cursor_clip_pending = false;
        }
    }
    if (changed) {
        // GetCapture/ReleaseCapture only affect the calling thread's queue.
        // The present thread cannot release the window thread's game capture.
        request_window_cursor_sync(window);
        logging::write(logging::Level::info, logging::Channel::input,
            !release && owns_menu_cursor(s) ? "Input ownership: overlay." : "Input ownership: game or desktop.");
    }
}

BOOL WINAPI clip_cursor(const RECT* rectangle) {
    auto& s = state();
    std::lock_guard lock(s.cursor_mutex);
    const auto native = original<ClipCursorFn>(s.clip_cursor);
    if (owns_menu_cursor(s)) {
        // Remember the latest native request for closing the menu; never apply
        // it while either overlay surface owns the pointer.
        if (!s.cursor_released) {
            s.cursor_released = true;
            request_window_cursor_sync(s.window.load());
        }
        s.cursor_clip_pending = rectangle != nullptr;
        if (rectangle) s.cursor_clip = *rectangle;
        return native(nullptr);
    }
    s.cursor_released = false; s.cursor_clip_pending = false;
    return native(rectangle);
}

BOOL WINAPI set_cursor_pos(int x, int y) {
    if (block_polled_input()) return TRUE;
    return original<SetCursorPosFn>(state().set_cursor_pos)(x, y);
}

void window_cursor_input(HWND window, UINT message, WPARAM wp) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return;
    const auto& s = state();
    window_cursor_input(window, message, wp, message != WM_NCDESTROY && window == s.window.load() && owns_menu_cursor(s));
}

void window_cursor_input(HWND window, UINT message, WPARAM wp, bool overlay_owns_pointer) {
    auto& s = state();
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return;
    // These fields belong to the HWND thread, including when Present posts the
    // handoff after initial setup, a close button, or swapchain recreation.
    thread_local HWND owner{};
    thread_local unsigned buttons{};
    const bool active = message != WM_NCDESTROY && window == s.window.load() && overlay_owns_pointer;
    if (!active) {
        if (owner == window) {
            owner = nullptr; buttons = 0;
            OverlayInputAccess access;
            if (GetCapture() == window) ReleaseCapture();
        }
        return;
    }
    OverlayInputAccess access;
    if (owner != window) {
        const auto captured = GetCapture();
        const auto previous = owner;
        owner = window; buttons = 0;
        if (captured && (captured == previous || captured == window || IsChild(window, captured))) ReleaseCapture();
    }
    const auto button = [&]() -> unsigned {
        switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return 1;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return 2;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return 4;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP: return HIWORD(wp) == XBUTTON2 ? 16 : 8;
        default: return 0;
        }
    }();
    const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    if (button) {
        if (up) buttons &= ~button;
        else buttons |= button;
        if (buttons && GetCapture() != window) SetCapture(window);
        else if (!buttons && GetCapture() == window) ReleaseCapture();
    }
    if (message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        buttons = 0;
        if (GetCapture() == window) ReleaseCapture();
    }
}

void update_menu_pointer() {
    auto& s = state();
    auto& io = ImGui::GetIO();
    const bool active = owns_menu_cursor(s);
    // The pause menu's server browser watches the pointer while the game keeps it: the same
    // reads, with the game's own cursor on screen and nothing taken from the game.
    const bool watching = !active && s.hub_pointer.load() && s.input_attached && !s.stop && !s.failed &&
                          game_window_foreground(s.window);
    io.AddFocusEvent(active || watching);
    io.MouseDrawCursor = active;
    if (!active && !watching) {
        io.ClearInputKeys(); io.ClearInputMouse();
        s.raw_mouse_buttons.store(0);
        return;
    }
    if (watching) io.ClearInputKeys();
    OverlayInputAccess access;
    POINT point{};
    // The Win32 backend only polls when it thinks WM_MOUSEMOVE tracking ended.
    // Skate can stop legacy messages during gameplay without WM_MOUSELEAVE.
    if (GetCursorPos(&point) && ScreenToClient(s.window.load(), &point))
        io.AddMousePosEvent(static_cast<float>(point.x), static_cast<float>(point.y));
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const std::array keys{swapped ? VK_RBUTTON : VK_LBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON,
        VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    // Raw buttons fill in when Skate's controller mode leaves the legacy state stale.
    const auto time = s.raw_mouse_time.load();
    const auto raw = time && GetTickCount64() < time + 10000 ? s.raw_mouse_buttons.load() : 0u;
    const std::array<unsigned, 3> raw_bits{swapped ? 2u : 1u, swapped ? 1u : 2u, 4u};
    for (int button = 0; button < static_cast<int>(keys.size()); ++button)
        io.AddMouseButtonEvent(button, (GetAsyncKeyState(keys[button]) & 0x8000) != 0 ||
            (button < 3 && (raw & raw_bits[static_cast<std::size_t>(button)]) != 0));
}
}
