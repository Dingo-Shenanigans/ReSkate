#include "startup_window.h"
#include "Engine/Core/Platform/wide_text.h"

#include <Windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dingosdk::startup {
namespace {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr auto fade_in = 150ms;
constexpr auto fade_out = 250ms;
// How often the window looks for the game's own.
constexpr auto window_check = 200ms;
// A startup that never shows a window still takes this one down.
constexpr auto longest_open = 180s;

// The overlay's palette (ui/overlay/skate_style.h) as GDI colours.
constexpr COLORREF ink = RGB(19, 20, 22);
constexpr COLORREF paper = RGB(241, 240, 232);
constexpr COLORREF blue = RGB(0, 145, 255);
constexpr COLORREF shade = RGB(50, 52, 56);
constexpr COLORREF soft = RGB(205, 206, 208);
constexpr COLORREF track = RGB(30, 31, 34);

// In 96-dpi pixels: the size of the game's own launch picture. A picture of any
// other shape is scaled to cover it and cropped evenly, never stretched.
constexpr int window_width = 640;
constexpr int window_height = 380;
constexpr int text_left = 16;
constexpr int bar_height = 4;
constexpr int shadow_height = 120;

constexpr wchar_t class_name[] = L"ReSkateStartupWindow";
constexpr const wchar_t* art_files[] = {L"ReSkate.Splash.png", L"ReSkate.Splash.jpg"};
constexpr wchar_t art_resource[] = L"STARTUP_SPLASH";

struct Shared {
    std::mutex mutex;
    std::wstring heading{L"Starting ReSkate\x2026"}, detail;
    float progress{-1.0f};
    bool opened{}, closing{};
    std::atomic<bool> shown{};
};

Shared& shared() {
    // Kept for the life of the process: the window thread can outlive static teardown.
    static auto* value = new Shared;
    return *value;
}

void fill(HDC dc, RECT area, COLORREF colour) {
    SetDCBrushColor(dc, colour);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

HMODULE this_module() {
    static const char anchor{};
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&anchor), &module);
    return module;
}

std::filesystem::path module_path(HMODULE module) {
    std::wstring name(32768, L'\0');
    const auto length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    if (!length || length >= name.size()) return {};
    name.resize(length);
    return name;
}

// Windows 10 1607 and later; resolved so an older system just gets an unscaled window.
template<class Function> Function user32(const char* name) {
    const auto module = GetModuleHandleW(L"user32.dll");
#pragma warning(push)
#pragma warning(disable: 4191)
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
#pragma warning(pop)
}

HBITMAP make_dib(int width, int height, void** bits) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, bits, nullptr, 0);
}

// The game shows a window of its own once it starts rendering; that is this
// one's cue to go.
struct Search {
    HWND own;
    bool found;
};

BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || window == search.own || !IsWindowVisible(window)) return TRUE;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds) || bounds.right - bounds.left < 200 || bounds.bottom - bounds.top < 150)
        return TRUE;
    search.found = true;
    return FALSE;
}

bool game_window_open(HWND own) {
    Search search{own, false};
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

ComPtr<IWICImagingFactory> imaging() {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

// The splash picture (PNG or JPEG), as premultiplied BGRA: ReSkate.Splash.png or
// .jpg beside the DLL, else the copy built into it. Null when there is neither.
ComPtr<IWICBitmapSource> open_art() {
    const auto factory = imaging();
    if (!factory) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    const auto directory = module_path(this_module()).parent_path();
    for (const auto* name : art_files) {
        if (SUCCEEDED(factory->CreateDecoderFromFilename((directory / name).c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &decoder)))
            break;
        decoder.Reset();
    }
    if (!decoder) {
        const auto module = this_module();
        const auto found = FindResourceW(module, art_resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        const auto loaded = found ? LoadResource(module, found) : nullptr;
        auto* bytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
        const auto size = found ? SizeofResource(module, found) : 0;
        ComPtr<IWICStream> stream;
        if (!bytes || !size || FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(bytes, size)) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
            return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT source_width{}, source_height{};
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&source_width, &source_height)) || !source_width || !source_height)
        return nullptr;
    return converter;
}

// `art` cover-fitted to width x height, as a top-down DIB.
HBITMAP scale_art(IWICBitmapSource* art, int width, int height) {
    const auto factory = imaging();
    UINT source_width{}, source_height{};
    if (!factory || FAILED(art->GetSize(&source_width, &source_height)) || !source_width || !source_height)
        return nullptr;
    const double scale = std::max(static_cast<double>(width) / source_width,
                                  static_cast<double>(height) / source_height);
    const auto scaled_width = std::max(static_cast<UINT>(width), static_cast<UINT>(std::ceil(source_width * scale)));
    const auto scaled_height = std::max(static_cast<UINT>(height), static_cast<UINT>(std::ceil(source_height * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler))) return nullptr;
    if (FAILED(scaler->Initialize(art, scaled_width, scaled_height, WICBitmapInterpolationModeHighQualityCubic)) &&
        FAILED(scaler->Initialize(art, scaled_width, scaled_height, WICBitmapInterpolationModeFant)))
        return nullptr;
    void* bits{};
    const auto bitmap = make_dib(width, height, &bits);
    if (!bitmap) return nullptr;
    const WICRect crop{static_cast<INT>(scaled_width - static_cast<UINT>(width)) / 2,
                       static_cast<INT>(scaled_height - static_cast<UINT>(height)) / 2, width, height};
    if (FAILED(scaler->CopyPixels(&crop, static_cast<UINT>(width) * 4,
                                  static_cast<UINT>(width) * static_cast<UINT>(height) * 4,
                                  static_cast<BYTE*>(bits)))) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

struct Window {
    HWND window{};
    UINT dpi{96};
    int width{}, height{};
    HFONT title{}, heading{}, body{};
    HBITMAP background{}, shadow{};
    HICON large_icon{}, small_icon{};
    ComPtr<IWICBitmapSource> art;
    Clock::time_point opened_at{}, leave_at{}, checked_at{};
    bool leaving{};

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

    void layout() {
        width = px(window_width);
        height = px(window_height);
        for (auto* font : {&title, &heading, &body})
            if (*font) { DeleteObject(*font); *font = nullptr; }
        const auto font = [&](int size, int weight, bool italic) {
            return CreateFontW(-px(size), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Arial");
        };
        title = font(84, FW_BOLD, true);
        heading = font(16, FW_BOLD, false);
        body = font(12, FW_NORMAL, false);
        make_background();
        make_shadow();
    }

    // The picture, or ReSkate's own card when there is none.
    void make_background() {
        if (background) DeleteObject(background);
        void* bits{};
        background = make_dib(width, height, &bits);
        if (!background) return;
        const auto dc = CreateCompatibleDC(nullptr);
        const auto previous = SelectObject(dc, background);
        fill(dc, {0, 0, width, height}, ink);
        if (const auto picture = art ? scale_art(art.Get(), width, height) : nullptr) {
            const auto source = CreateCompatibleDC(nullptr);
            const auto kept = SelectObject(source, picture);
            const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, 0, 0, width, height, source, 0, 0, width, height, blend);
            SelectObject(source, kept);
            DeleteDC(source);
            DeleteObject(picture);
        } else {
            draw_card(dc);
        }
        SelectObject(dc, previous);
        DeleteDC(dc);
    }

    void draw_card(HDC dc) const {
        // Halftone washing in from two corners, as on the game's own picture.
        SelectObject(dc, GetStockObject(NULL_PEN));
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, shade);
        const int step = px(8);
        const float reach = static_cast<float>(px(260));
        for (int y = 0; y < height; y += step) {
            for (int x = (y / step % 2) * step / 2; x < width; x += step) {
                const float corner = std::min(std::hypot(static_cast<float>(width - x), static_cast<float>(y)),
                                              std::hypot(static_cast<float>(x), static_cast<float>(height - y)));
                const float radius = (1.0f - corner / reach) * static_cast<float>(px(3));
                if (radius < 0.6f) continue;
                const int r = std::max(1, static_cast<int>(std::lround(radius)));
                Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
            }
        }
        // The wordmark and the blue torn rule under it.
        SelectObject(dc, title);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, paper);
        constexpr wchar_t wordmark[] = L"RESKATE";
        SIZE extent{};
        GetTextExtentPoint32W(dc, wordmark, static_cast<int>(std::size(wordmark) - 1), &extent);
        const int left = (width - extent.cx) / 2, top = height * 2 / 5 - extent.cy / 2;
        TextOutW(dc, left, top, wordmark, static_cast<int>(std::size(wordmark) - 1));
        const float rule = static_cast<float>(extent.cx) * 0.55f;
        const float x0 = static_cast<float>(left) + static_cast<float>(extent.cx) * 0.06f;
        const float y0 = static_cast<float>(top + extent.cy + px(2));
        const float unit = static_cast<float>(px(2));
        // A torn-paper rule: fractions along the rule, and heights.
        const std::array<std::pair<float, float>, 8> torn{{{0.0f, 2.0f}, {0.23f, 0.0f}, {0.57f, 1.0f},
                                                           {1.0f, -1.0f}, {0.99f, 3.0f}, {0.68f, 4.0f},
                                                           {0.31f, 3.0f}, {0.004f, 5.0f}}};
        std::array<POINT, torn.size()> points{};
        for (std::size_t i = 0; i < torn.size(); ++i)
            points[i] = {static_cast<LONG>(x0 + torn[i].first * rule), static_cast<LONG>(y0 + torn[i].second * unit)};
        SetDCBrushColor(dc, blue);
        Polygon(dc, points.data(), static_cast<int>(points.size()));
    }

    // Darkens the bottom of the picture so the status stays readable on any art.
    void make_shadow() {
        if (shadow) DeleteObject(shadow);
        void* bits{};
        constexpr int rows = 64;
        shadow = make_dib(1, rows, &bits);
        if (!shadow) return;
        auto* pixels = static_cast<std::uint32_t*>(bits);
        for (int row = 0; row < rows; ++row) {
            const auto t = static_cast<float>(row) / (rows - 1);
            pixels[row] = static_cast<std::uint32_t>(t * t * 200.0f) << 24;  // premultiplied black
        }
    }

    void tick() {
        auto& state = shared();
        const auto now = Clock::now();
        bool closing;
        {
            std::lock_guard lock(state.mutex);
            closing = state.closing;
        }
        if (!closing && now - checked_at >= window_check) {
            checked_at = now;
            closing = game_window_open(window) || now - opened_at >= longest_open;
            if (closing) {
                std::lock_guard lock(state.mutex);
                state.closing = true;
            }
        }
        float alpha = std::min(1.0f, std::chrono::duration<float>(now - opened_at) / fade_in);
        if (closing && !leaving) {
            leaving = true;
            leave_at = now;
        }
        if (leaving) {
            const auto gone = std::chrono::duration<float>(now - leave_at) / fade_out;
            if (gone >= 1.0f) {
                DestroyWindow(window);
                return;
            }
            alpha = std::min(alpha, 1.0f - gone);
        }
        SetLayeredWindowAttributes(window, 0, static_cast<BYTE>(255.0f * std::clamp(alpha, 0.0f, 1.0f)), LWA_ALPHA);
        InvalidateRect(window, nullptr, FALSE);
    }

    void paint() {
        PAINTSTRUCT paint{};
        const auto target = BeginPaint(window, &paint);
        const auto dc = CreateCompatibleDC(target);
        const auto frame = CreateCompatibleBitmap(target, width, height);
        const auto previous = SelectObject(dc, frame);

        std::wstring status, detail;
        float progress;
        {
            auto& state = shared();
            std::lock_guard lock(state.mutex);
            status = state.heading;
            detail = state.detail;
            progress = state.closing ? 1.0f : state.progress;
        }

        const auto source = CreateCompatibleDC(dc);
        if (background) {
            const auto kept = SelectObject(source, background);
            BitBlt(dc, 0, 0, width, height, source, 0, 0, SRCCOPY);
            SelectObject(source, kept);
        } else {
            fill(dc, {0, 0, width, height}, ink);
        }
        if (shadow) {
            const auto kept = SelectObject(source, shadow);
            const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, 0, height - px(shadow_height), width, px(shadow_height), source, 0, 0, 1, 64, blend);
            SelectObject(source, kept);
        }
        DeleteDC(source);

        SetBkMode(dc, TRANSPARENT);
        const int bar = height - px(bar_height);
        const auto line = [&](HFONT font, COLORREF colour, const std::wstring& text, int top, int line_height) {
            SelectObject(dc, font);
            RECT row{px(text_left), top, width - px(text_left), top + line_height};
            RECT behind = row;
            OffsetRect(&behind, 1, 1);
            SetTextColor(dc, RGB(0, 0, 0));
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &behind,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetTextColor(dc, colour);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &row,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        };
        if (detail.empty()) {
            line(heading, paper, status, bar - px(30), px(22));
        } else {
            line(heading, paper, status, bar - px(46), px(22));
            line(body, soft, detail, bar - px(24), px(17));
        }

        fill(dc, {0, bar, width, height}, track);
        if (progress >= 0.0f) {
            fill(dc, {0, bar, static_cast<int>(width * std::min(progress, 1.0f)), height}, blue);
        } else {
            const auto seconds = std::chrono::duration<float>(Clock::now() - opened_at).count();
            const int length = width / 4;
            const int start = -length + static_cast<int>(std::fmod(seconds * 0.7f, 1.0f) * (width + length));
            fill(dc, {std::max(start, 0), bar, std::min(start + length, width), height}, blue);
        }

        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(frame);
        DeleteDC(dc);
        EndPaint(window, &paint);
    }

    void release() {
        for (auto* font : {&title, &heading, &body})
            if (*font) { DeleteObject(*font); *font = nullptr; }
        for (auto* bitmap : {&background, &shadow})
            if (*bitmap) { DeleteObject(*bitmap); *bitmap = nullptr; }
        for (auto* icon : {&large_icon, &small_icon})
            if (*icon) { DestroyIcon(*icon); *icon = nullptr; }
        art.Reset();  // before COM goes away on this thread
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_TIMER:
        if (self) self->tick();
        return 0;
    case WM_PAINT:
        if (self) {
            self->paint();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // the game's window gets the focus, not this one
    case WM_DPICHANGED:
        if (self) {
            self->dpi = HIWORD(wparam);
            self->layout();
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, self->width, self->height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void run() {
    auto& state = shared();
    // A late start, with the game already on screen, has nothing to cover.
    if (game_window_open(nullptr)) {
        std::lock_guard lock(state.mutex);
        state.closing = true;
        return;
    }
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Windows this thread makes scale themselves; the game's DPI handling is untouched.
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using WindowDpi = UINT(WINAPI*)(HWND);
    if (const auto set_awareness = user32<SetAwareness>("SetThreadDpiAwarenessContext"))
        set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto instance = this_module();
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.style = CS_DROPSHADOW;
    description.lpfnWndProc = window_proc;
    description.hInstance = instance;
    description.lpszClassName = class_name;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&description);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);

    Window self;
    // Never focused, so the game's window takes the focus when it comes; on the
    // taskbar, so it reads as the game starting rather than a stray popup.
    self.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                  class_name, L"ReSkate", WS_POPUP, monitor.rcWork.left, monitor.rcWork.top, 1, 1,
                                  nullptr, nullptr, instance, &self);
    if (!self.window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    if (const auto window_dpi = user32<WindowDpi>("GetDpiForWindow"))
        if (const auto dpi = window_dpi(self.window)) self.dpi = dpi;
    self.art = open_art();
    self.layout();
    const auto game = module_path(nullptr);
    if (!game.empty() && ExtractIconExW(game.c_str(), 0, &self.large_icon, &self.small_icon, 1)) {
        SendMessageW(self.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(self.large_icon));
        SendMessageW(self.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(self.small_icon));
    }
    SetLayeredWindowAttributes(self.window, 0, 0, LWA_ALPHA);
    const auto& work = monitor.rcWork;
    // Shown with SetWindowPos rather than ShowWindow, which would spend the
    // game's STARTUPINFO show command on this window instead of its own.
    SetWindowPos(self.window, HWND_TOPMOST, work.left + (work.right - work.left - self.width) / 2,
                 work.top + (work.bottom - work.top - self.height) / 2, self.width, self.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    self.opened_at = self.checked_at = Clock::now();
    state.shown = true;
    SetTimer(self.window, 1, 16, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    self.release();
    if (SUCCEEDED(com)) CoUninitialize();
}
} // namespace

void open() noexcept {
    auto& state = shared();
    {
        std::lock_guard lock(state.mutex);
        if (state.opened) return;
        state.opened = true;
    }
    try {
        std::thread(run).detach();
    } catch (...) {
        std::lock_guard lock(state.mutex);
        state.closing = true;  // no window is no harm
    }
}

void status(std::string_view heading, std::string_view detail, float progress) noexcept {
    try {
        auto wide_heading = utf8_to_wide(heading);
        auto wide_detail = utf8_to_wide(detail);
        auto& state = shared();
        std::lock_guard lock(state.mutex);
        state.heading = std::move(wide_heading);
        state.detail = std::move(wide_detail);
        state.progress = progress;
    } catch (...) {
    }
}

void close() noexcept {
    auto& state = shared();
    std::lock_guard lock(state.mutex);
    state.closing = true;
}

bool shown() noexcept { return shared().shown.load(); }

} // namespace dingosdk::startup
