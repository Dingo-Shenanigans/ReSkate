#include "picture_shape.h"
#include "replay_export.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/UI/game_window.h"
#include "Extension/Settings/named_settings.h"
#include <Windows.h>
#include <dxgi1_4.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>

namespace dingosdk::picture_shape {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT show_message = WM_APP + 41, hide_message = WM_APP + 42, give_back_message = WM_APP + 43;
constexpr wchar_t backdrop_class[] = L"ReSkatePictureBackdrop";
constexpr const char* width_setting = "RenderDevice.FullscreenWidth";
constexpr const char* height_setting = "RenderDevice.FullscreenHeight";

// The backdrop: a black window of its own, on a thread of its own (with its own messages), so the game's threads never
// wait on it. It goes just behind the game's window, over its monitor; and while it is up the taskbar is told the game is
// full screen (ITaskbarList2::MarkFullscreenWindow), so it keeps out of the way when the game is in front.
struct Backdrop {
    std::once_flag started;
    std::atomic<HWND> window{};
    ComPtr<ITaskbarList2> taskbar; // the backdrop's thread's only
    HWND game{};                   // the game's window it last covered for (the backdrop's thread's)
};
Backdrop& backdrop() {
    static auto* value = new Backdrop;
    return *value;
}
void cover(HWND self, HWND game) {
    if (!game || !IsWindow(game)) return;
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return;
    const RECT m = info.rcMonitor;
    auto& b = backdrop();
    b.game = game;
    const HWND before = GetForegroundWindow();
    // Just behind the game's window (inserted after it), over the whole monitor; never active.
    SetWindowPos(self, game, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    if (b.taskbar) (void)b.taskbar->MarkFullscreenWindow(game, TRUE);
    if (before == game && GetForegroundWindow() != game) SetForegroundWindow(game);
}
LRESULT CALLBACK backdrop_proc(HWND self, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ACTIVATE:
        // Activated after all (the system can, despite WS_EX_NOACTIVATE): the game's activation back, as soon as this
        // message is done with.
        if (LOWORD(w) != WA_INACTIVE) PostMessageW(self, give_back_message, 0, 0);
        return 0;
    case give_back_message: {
        const auto game = backdrop().game;
        if (game && IsWindow(game)) SetForegroundWindow(game);
        return 0;
    }
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(self, &r);
        FillRect(reinterpret_cast<HDC>(w), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(self, &paint);
        EndPaint(self, &paint);
        return 0;
    }
    case show_message: cover(self, reinterpret_cast<HWND>(l)); return 0;
    case hide_message: {
        ShowWindow(self, SW_HIDE);
        auto& b = backdrop();
        const auto game = reinterpret_cast<HWND>(l);
        if (b.taskbar && game && IsWindow(game)) (void)b.taskbar->MarkFullscreenWindow(game, FALSE);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(self, message, w, l);
}
void backdrop_thread() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto& b = backdrop();
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&b.taskbar))) && FAILED(b.taskbar->HrInit()))
        b.taskbar.Reset();
    WNDCLASSEXW type{sizeof(WNDCLASSEXW)};
    type.lpfnWndProc = &backdrop_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = backdrop_class;
    RegisterClassExW(&type);
    const HWND self = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, backdrop_class, L"ReSkate", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                                      type.hInstance, nullptr);
    b.window.store(self, std::memory_order_release);
    MSG message{};
    while (self && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    b.taskbar.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
}
HWND backdrop_window() {
    auto& b = backdrop();
    std::call_once(b.started, [] { std::thread(&backdrop_thread).detach(); });
    return b.window.load(std::memory_order_acquire);
}

// The size of the game window's monitor as the calling thread sees it (scaled units when the screen is scaled and the
// thread is not DPI aware: the game's are not), and the same in its own pixels (the thread made per-monitor DPI aware for
// the moment, and put back as it was).
Size monitor_seen(HWND game) noexcept {
    if (!game || !IsWindow(game)) return {};
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return {};
    return {static_cast<int>(info.rcMonitor.right - info.rcMonitor.left), static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top)};
}
Size monitor_pixels(HWND game) noexcept {
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static const SetAwareness set_awareness = [] {
        const auto user32 = GetModuleHandleW(L"user32.dll");
#pragma warning(push)
#pragma warning(disable : 4191)
        return user32 ? reinterpret_cast<SetAwareness>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
#pragma warning(pop)
    }();
    if (!set_awareness) return monitor_seen(game);
    const auto was = set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!was) return monitor_seen(game);
    const auto pixels = monitor_seen(game);
    set_awareness(was);
    return pixels;
}
bool same(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

struct State {
    std::mutex mutex;
    std::string owner;
    float aspect{};
    // The window as shaped (client thread only): the game's, where it was, and the render size found before.
    bool shaped{};
    HWND game{};
    RECT original{};
    std::uint64_t next_size_try{}, next_cover{};
    std::optional<std::string> found_width, found_height; // RenderDevice.Fullscreen* before (none: not changed)
    std::string ours_width, ours_height;
};
State& state() {
    static auto* value = new State;
    return *value;
}
// The render size held at `size` (its settings found first, to put back), or put back (no size). Client thread.
void hold_render_size(State& s, std::optional<Size> size) {
    if (size) {
        const auto w = std::to_string(size->width), h = std::to_string(size->height);
        if (!s.found_width) {
            s.found_width = named_setting_value(width_setting);
            s.found_height = named_setting_value(height_setting);
            if (!s.found_width || !s.found_height) {
                s.found_width.reset();
                s.found_height.reset();
                return; // no such settings: nothing changed
            }
        }
        if (named_setting_value(width_setting) != w) (void)change_named_setting(width_setting, w, false);
        if (named_setting_value(height_setting) != h) (void)change_named_setting(height_setting, h, false);
        s.ours_width = w, s.ours_height = h;
        return;
    }
    if (!s.found_width) return;
    // Put back unless changed since by something else (it stays then).
    if (named_setting_value(width_setting) == s.ours_width) (void)change_named_setting(width_setting, *s.found_width, false);
    if (named_setting_value(height_setting) == s.ours_height) (void)change_named_setting(height_setting, *s.found_height, false);
    s.found_width.reset();
    s.found_height.reset();
}
} // namespace

Size fitted(float aspect, Size monitor) noexcept {
    if (!(aspect > 0.1f) || monitor.width <= 0 || monitor.height <= 0) return {};
    const float screen = static_cast<float>(monitor.width) / static_cast<float>(monitor.height);
    Size s;
    if (aspect <= screen) {
        s.height = monitor.height & ~1;
        s.width = std::min(static_cast<int>(std::lround(static_cast<float>(monitor.height) * aspect * 0.5f)) * 2, monitor.width & ~1);
    } else {
        s.width = monitor.width & ~1;
        s.height = std::min(static_cast<int>(std::lround(static_cast<float>(monitor.width) / aspect * 0.5f)) * 2, monitor.height & ~1);
    }
    return s;
}

Size in_pixels(Size window, Size seen, Size pixels) noexcept {
    if (window.width <= 0 || window.height <= 0 || seen.width <= 0 || seen.height <= 0 || pixels.width < 2 || pixels.height < 2 || seen == pixels)
        return window;
    const auto scaled = [](int side, int seen_side, int pixel_side) {
        const double exact = static_cast<double>(side) * static_cast<double>(pixel_side) / static_cast<double>(seen_side);
        return std::clamp(static_cast<int>(std::lround(exact * 0.5)) * 2, 2, pixel_side & ~1);
    };
    return {scaled(window.width, seen.width, pixels.width), scaled(window.height, seen.height, pixels.height)};
}

std::string set(std::string_view owner, std::optional<float> aspect) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!aspect) {
            if (s.owner == owner) s.owner.clear();
            return {};
        }
        if (!s.owner.empty() && s.owner != owner) return s.owner + " has the picture's shape";
        s.owner = std::string(owner);
        s.aspect = *aspect;
        return {};
    } catch (...) {
        return "it could not be taken";
    }
}

void clear(std::string_view owner) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.owner == owner) s.owner.clear();
}

void tick() noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        // Nothing changes under a video being exported.
        if (replay_export::running().running) return;
        const HWND game = static_cast<HWND>(game_window());
        const HWND self = backdrop_window();
        const auto now = GetTickCount64();
        if (!s.owner.empty() && game && IsWindow(game)) {
            const auto seen = monitor_seen(game);
            const auto window = fitted(s.aspect, seen);
            if (window.width <= 0) return;
            MONITORINFO info{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return;
            const RECT m = info.rcMonitor;
            RECT target{m.left + (m.right - m.left - window.width) / 2, m.top + (m.bottom - m.top - window.height) / 2, 0, 0};
            target.right = target.left + window.width;
            target.bottom = target.top + window.height;
            if (!s.shaped || s.game != game) {
                GetWindowRect(game, &s.original);
                s.shaped = true;
                s.game = game;
                s.next_size_try = 0;
                s.next_cover = 0;
            }
            RECT at{};
            GetWindowRect(game, &at);
            if (!same(at, target) && now >= s.next_size_try) {
                // Asynchronously: the game's own thread moves its window, and nothing here waits for it.
                SetWindowPos(game, nullptr, target.left, target.top, window.width, window.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                s.next_size_try = now + 500;
            }
            // The backdrop just behind the game, whenever the game is in front and it is not there.
            if (self && now >= s.next_cover && (!IsWindowVisible(self) || (GetForegroundWindow() == game && GetWindow(game, GW_HWNDNEXT) != self))) {
                PostMessageW(self, show_message, 0, reinterpret_cast<LPARAM>(game));
                s.next_cover = now + 250;
            }
            const auto render = in_pixels(window, seen, monitor_pixels(game));
            hold_render_size(s, render);
            return;
        }
        hold_render_size(s, std::nullopt);
        if (!s.shaped) return;
        s.shaped = false;
        if (s.game && IsWindow(s.game))
            SetWindowPos(s.game, nullptr, s.original.left, s.original.top, s.original.right - s.original.left, s.original.bottom - s.original.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (self) PostMessageW(self, hide_message, 0, reinterpret_cast<LPARAM>(s.game));
    } catch (...) {}
}

std::optional<Memory> memory() noexcept {
    try {
        Memory result;
        MEMORYSTATUSEX status{sizeof(MEMORYSTATUSEX)};
        if (GlobalMemoryStatusEx(&status)) result.commit_free = status.ullAvailPageFile;
        const auto luid = game_adapter();
        if (!luid) return std::nullopt;
        const auto module = GetModuleHandleW(L"dxgi.dll");
        using Create = HRESULT(WINAPI*)(REFIID, void**);
#pragma warning(push)
#pragma warning(disable : 4191)
        const auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "CreateDXGIFactory1")) : nullptr;
#pragma warning(pop)
        ComPtr<IDXGIFactory1> factory;
        if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory) return std::nullopt;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) ||
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32 | desc.AdapterLuid.LowPart) != luid)
                continue;
            ComPtr<IDXGIAdapter3> three;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter.As(&three)) && SUCCEEDED(three->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget) {
                result.budget = info.Budget;
                result.usage = info.CurrentUsage;
                return result;
            }
            return std::nullopt;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace dingosdk::picture_shape
