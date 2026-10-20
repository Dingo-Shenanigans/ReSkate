#include "gui.h"

#include "gui_internal.h"
#include "gui_renderer.h"
#include "Engine/Core/Platform/path_text.h"

#include "Engine/Core/Log/logging.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>

#include <imgui_internal.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace dingosdk::launcher_gui {
namespace detail {

namespace {

// Keeps a panel or page in front, but not over its own open combo or popup, and
// not while the focus is already inside it: focusing the window again every
// frame would pull a controller's focus out of its child panels.
void keep_in_front(const char* id) {
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
    const ImGuiWindow* window = ImGui::FindWindowByName(id);
    const ImGuiWindow* focused = GImGui->NavWindow;
    if (window && focused && focused->RootWindow == window) return;
    // The install progress covers the Mods page and holds the focus; taking it
    // back every frame would flash the page's own default item.
    if (focused && focused->RootWindow == ImGui::FindWindowByName("##installing")) return;
    ImGui::SetNextWindowFocus();
}

} // namespace

void panel_title(const Fonts& fonts, const char* text) {
    ImGui::PushFont(fonts.tile);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

ImVec2 begin_panel(const char* id, ImVec2 size, ImVec2 panel) {
    ImGui::SetNextWindowPos(ImVec2((size.x - panel.x) * 0.5f, (size.y - panel.y) * 0.5f));
    ImGui::SetNextWindowSize(panel);
    keep_in_front(id);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    return panel;
}

ImVec2 begin_page(const char* id, ImVec2 size) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    keep_in_front(id);
    // Opaque, and darker than a tile: a page covers the window, so the main
    // screen must not show through, and its tiles need something to sit on.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(rgba(14, 15, 18)));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    return size;
}

bool tile_hit(const char* id, ImVec2 position, ImVec2 size, bool enabled, bool& hovered) {
    ImGui::SetCursorScreenPos(position);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    hovered = enabled && ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

bool nav_tile(const Fonts& fonts, float width, const char* label, bool selected, const std::string& count,
              bool accent, const std::string& tip) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, S(54));
    bool hovered{};
    const bool pressed = tile_hit(label, position, size, true, hovered);
    const ImVec2 end(position.x + size.x, position.y + size.y);
    // Each tile gets its own hand-cut edge, so no two wobble the same way.
    unsigned seed = 7;
    for (const char* letter = label; *letter; ++letter) seed = seed * 31u + static_cast<unsigned>(*letter);
    skate_theme::rough_rect(draw, position, end,
        selected ? color::blue : hovered ? color::tile_grey : color::tile, seed, g_scale);
    draw->AddText(fonts.heading, fonts.heading->FontSize,
        ImVec2(position.x + S(16), position.y + (size.y - fonts.heading->FontSize) * 0.5f),
        selected ? color::ink : color::text, label);
    if (!count.empty()) {
        const float height = fonts.caption->FontSize + S(8);
        badge(draw, fonts, ImVec2(end.x - S(14) - badge_width(fonts, count), position.y + (size.y - height) * 0.5f),
            count, selected ? rgba(0, 0, 0, 0.32f) : accent ? color::blue : rgba(255, 255, 255, 0.14f),
            !selected && accent ? color::ink : color::text);
    }
    if (hovered && !tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    ImGui::SetCursorScreenPos(ImVec2(position.x, end.y + S(8)));
    return pressed;
}

float badge_width(const Fonts& fonts, const std::string& text) {
    return fonts.caption->CalcTextSizeA(fonts.caption->FontSize, FLT_MAX, 0, text.c_str()).x + S(18);
}

void badge(ImDrawList* draw, const Fonts& fonts, ImVec2 position, const std::string& text, ImU32 fill, ImU32 ink) {
    const float height = fonts.caption->FontSize + S(8);
    const ImVec2 end(position.x + badge_width(fonts, text), position.y + height);
    draw->AddRectFilled(position, end, fill, height * 0.5f);
    draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(position.x + S(9), position.y + S(4)), ink, text.c_str());
}

float text_height(ImFont* font, float wrap, const std::string& text) {
    return text.empty() ? 0 : font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap, text.c_str()).y;
}

void page_banner(const Fonts& fonts, const char* heading, const char* pill, const std::string& about,
                 const std::string& status, ImU32 status_fill, const std::string& caption) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = S(20), side = S(230);
    const float wrap = width - pad * 2 - side - S(20);
    const float tall = std::max(S(100), pad + fonts.heading->FontSize + S(10) + text_height(fonts.body, wrap, about) + pad);
    draw->AddRectFilled(top, ImVec2(top.x + width, top.y + tall), color::tile, S(6));
    draw->AddRectFilled(top, ImVec2(top.x + S(5), top.y + tall), color::blue, S(6), ImDrawFlags_RoundCornersLeft);
    const float text_x = top.x + pad + S(4);
    draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(text_x, top.y + pad), color::text, heading);
    if (pill && *pill) {
        const float heading_width = fonts.heading->CalcTextSizeA(fonts.heading->FontSize, FLT_MAX, 0, heading).x;
        badge(draw, fonts, ImVec2(text_x + heading_width + S(12), top.y + pad + (fonts.heading->FontSize - fonts.caption->FontSize - S(8)) * 0.5f),
            pill, color::blue, color::ink);
    }
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, top.y + pad + fonts.heading->FontSize + S(10)), color::muted,
        about.c_str(), nullptr, wrap);
    const float right = top.x + width - pad;
    const float block = fonts.caption->FontSize + S(8) + (caption.empty() ? 0 : S(8) + fonts.caption->FontSize);
    const float block_y = top.y + (tall - block) * 0.5f;
    if (!status.empty()) badge(draw, fonts, ImVec2(right - badge_width(fonts, status), block_y), status, status_fill, color::ink);
    if (!caption.empty()) {
        const float caption_width = fonts.caption->CalcTextSizeA(fonts.caption->FontSize, FLT_MAX, 0, caption.c_str()).x;
        draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(right - caption_width, block_y + fonts.caption->FontSize + S(16)),
            color::muted, caption.c_str());
    }
    ImGui::Dummy(ImVec2(width, tall));
    ImGui::Dummy(ImVec2(0, S(6)));
}

void card(ImDrawList* draw, ImVec2 start, ImVec2 size, bool marked) {
    const ImVec2 end(start.x + size.x, start.y + size.y);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(start, end);
    draw->AddRectFilled(start, end, color::tile, S(6));
    draw->AddRect(start, end, hovered ? rgba(255, 255, 255, 0.22f) : color::outline, S(6));
    if (marked) draw->AddRectFilled(start, ImVec2(start.x + S(5), end.y), color::blue, S(6), ImDrawFlags_RoundCornersLeft);
}

void field(const Fonts& fonts, const char* name, const std::string& value) {
    if (value.empty()) return;
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("%s", name);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
}

bool list_row(const char* id, float width, float height, bool ticked) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + width, start.y + height);
    // A card, as the OFFICIAL and PROFILES pages draw theirs: its own hover, so the corners stay round.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(start, end);
    draw->AddRectFilled(start, end, ticked ? rgba(28, 38, 52) : color::tile, S(6));
    if (hovered) draw->AddRectFilled(start, end, rgba(255, 255, 255, 0.035f), S(6));
    draw->AddRect(start, end, hovered ? rgba(255, 255, 255, 0.22f) : color::outline, S(6));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.05f)));
    const bool pressed = ImGui::Selectable(id, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, height));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ticked) draw->AddRectFilled(start, ImVec2(start.x + S(5), end.y), color::blue, S(6), ImDrawFlags_RoundCornersLeft);
    return pressed;
}

bool toggle(const char* id, bool* on) {
    const ImVec2 size(S(42), S(22));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    if (pressed) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    auto* draw = ImGui::GetWindowDrawList();
    const float radius = size.y * 0.5f;
    draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
        *on ? color::blue : rgba(255, 255, 255, hovered ? 0.2f : 0.13f), radius);
    draw->AddCircleFilled(ImVec2(start.x + (*on ? size.x - radius : radius), start.y + radius), radius - S(3),
        *on ? color::ink : rgba(196, 202, 212));
    return pressed;
}

bool more_button(const char* id, float size) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    auto* draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(start, ImVec2(start.x + size, start.y + size), rgba(255, 255, 255, 0.1f), S(4));
    for (int dot = -1; dot <= 1; ++dot)
        draw->AddCircleFilled(ImVec2(start.x + size * 0.5f + static_cast<float>(dot) * S(6), start.y + size * 0.5f), S(1.8f),
            hovered ? color::text : color::muted);
    return pressed;
}

void open_path(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_url(std::string_view url) {
    if (!url.starts_with("https://")) return;
    ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace {

// The window resizes from its edges, no smaller than it opens. While an edge is dragged Windows
// runs its own message loop, so the launcher draws from WM_SIZE through g_paint; g_painting keeps
// that from starting a frame inside one.
std::function<void()> g_paint;
bool g_painting{};
POINT g_min_size{};

// How far a maximised borderless window reaches past the screen on each side.
int frame_overhang(HWND window) {
    const auto dpi = GetDpiForWindow(window);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_capturing_key && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        // Tell left and right modifiers apart so they can be refused by name.
        auto key = static_cast<unsigned>(wparam);
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU)
            key = MapVirtualKeyW((static_cast<UINT>(lparam) >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
        g_captured_key = key;
        g_capturing_key = false;
        return 0;
    }
    if (g_capturing_key && (message == WM_CHAR || message == WM_SYSCHAR || message == WM_KEYUP || message == WM_SYSKEYUP))
        return 0;
    if (message == WM_MOUSEMOVE && g_pad_feed)
        g_pad_feed->mouse_moved(ImVec2(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam))));
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_NCCALCSIZE:
        // No frame: the client is the whole window (the frame style is only there for resizing,
        // snapping and the shadow). Maximised, Windows puts the frame past the screen's edge.
        if (wparam) {
            if (IsZoomed(window)) {
                auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
                const int overhang = frame_overhang(window);
                rect.left += overhang;
                rect.top += overhang;
                rect.right -= overhang;
                rect.bottom -= overhang;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        RECT client{};
        GetClientRect(window, &client);
        // The edges and corners resize it.
        if (!IsZoomed(window)) {
            const int edge = static_cast<int>(S(6));
            const bool left = point.x < edge, right = point.x >= client.right - edge;
            const bool top = point.y < edge, bottom = point.y >= client.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        // The top strip drags the borderless window, except over its buttons.
        if (g_drag_allowed && point.y >= 0 && static_cast<float>(point.y) < S(44) &&
            static_cast<float>(point.x) < static_cast<float>(client.right) - S(100)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO:
        if (g_min_size.x) reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = g_min_size;
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && g_paint && !g_painting) g_paint();
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        {
            std::lock_guard lock(g_dropped_mutex);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L' ');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                g_dropped.emplace_back(path);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// The glyphs every font loads: Latin with its extensions (Turkish, Polish...), Cyrillic,
// punctuation and the arrows.
const ImWchar font_ranges[]{0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x2193, 0};

// Fonts are embedded as RCDATA (External/fonts); Segoe UI is the fallback. `merge` adds only the glyphs the
// font loaded before it lacks.
ImFont* embedded_font(const wchar_t* name, float size, bool merge = false) {
    const auto instance = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(instance, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!resource) return nullptr;
    const auto loaded = LoadResource(instance, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // the resource lives as long as the process
    config.OversampleH = size >= 48 ? 1 : 2;
    config.MergeMode = merge;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(instance, resource)),
        S(size), &config, font_ranges);
}

ImFont* system_font(const wchar_t* file, float size) {
    std::array<wchar_t, MAX_PATH> windows{};
    GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    const auto path = fs::path(windows.data()) / L"Fonts" / file;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return nullptr;
    // (ImGui reads UTF-8)
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path_utf8(path).c_str(), S(size), nullptr, font_ranges);
}

Fonts load_fonts() {
    auto& io = ImGui::GetIO();
    io.Fonts->TexGlyphPadding = 3;  // large glyphs bleed into neighbours with 1 px
    const auto font = [](const wchar_t* name, const wchar_t* fallback, float size) {
        if (auto* loaded = embedded_font(name, size)) return loaded;
        if (auto* loaded = system_font(fallback, size)) return loaded;
        return ImGui::GetIO().Fonts->AddFontDefault();
    };
    Fonts fonts;
    fonts.body = font(L"FONT_BODY", L"segoeui.ttf", 15);
    fonts.caption = font(L"FONT_BODY", L"segoeui.ttf", 12);
    fonts.bold = font(L"FONT_HEADING", L"segoeuib.ttf", 16);
    fonts.heading = font(L"FONT_HEADING", L"segoeuib.ttf", 22);
    fonts.tile = font(L"FONT_HEADING", L"seguibl.ttf", 30);
    fonts.action = font(L"FONT_HEADING", L"seguibl.ttf", 50);
    fonts.title = font(L"FONT_BRUSH", L"seguibl.ttf", 86);
    // Permanent Marker has no Ğ, İ or Ş, which some languages' titles need.
    embedded_font(L"FONT_HEADING", 86, true);
    io.FontDefault = fonts.body;
    // Built here rather than on the first frame, so the log says whether it
    // worked. A window with no atlas draws nothing but its clear colour.
    const bool built = io.Fonts->Build();
    logging::log(built ? logging::Level::info : logging::Level::error, logging::Channel::launcher,
        "Launcher fonts: atlas {}x{} {}.", io.Fonts->TexWidth, io.Fonts->TexHeight,
        built ? "built" : "COULD NOT BE BUILT, so the window will be empty");
    return fonts;
}

void apply_style() {
    auto& style = ImGui::GetStyle();
    // skate.'s menus are square-cornered, flat and high-contrast.
    style.WindowRounding = 0;
    style.FrameRounding = 0;
    style.GrabRounding = 0;
    style.WindowBorderSize = 0;
    style.FrameBorderSize = 0;
    style.WindowPadding = ImVec2(S(28), S(24));
    style.FramePadding = ImVec2(S(12), S(8));
    style.ItemSpacing = ImVec2(S(10), S(10));
    style.ScrollbarSize = S(10);
    auto* colours = style.Colors;
    const auto rgb = [](ImU32 colour) { return ImGui::ColorConvertU32ToFloat4(colour); };
    colours[ImGuiCol_Text] = rgb(color::text);
    colours[ImGuiCol_TextDisabled] = rgb(color::muted);
    colours[ImGuiCol_WindowBg] = rgb(color::panel);
    colours[ImGuiCol_Border] = rgb(color::outline);
    colours[ImGuiCol_FrameBg] = rgb(rgba(45, 45, 47));
    colours[ImGuiCol_FrameBgHovered] = rgb(rgba(61, 62, 66));
    colours[ImGuiCol_FrameBgActive] = rgb(rgba(72, 73, 78));
    colours[ImGuiCol_Button] = rgb(rgba(45, 45, 47));
    colours[ImGuiCol_ButtonHovered] = rgb(rgba(61, 62, 66));
    colours[ImGuiCol_ButtonActive] = rgb(rgba(72, 73, 78));
    colours[ImGuiCol_CheckMark] = rgb(color::blue);
    colours[ImGuiCol_SliderGrab] = rgb(color::blue);
    colours[ImGuiCol_Header] = rgb(rgba(45, 45, 47));
    colours[ImGuiCol_HeaderHovered] = rgb(rgba(1, 131, 255, 0.35f));
    colours[ImGuiCol_HeaderActive] = rgb(rgba(1, 131, 255, 0.5f));
    colours[ImGuiCol_PopupBg] = rgb(rgba(26, 26, 26));
    colours[ImGuiCol_ModalWindowDimBg] = rgb(rgba(4, 6, 9, 0.72f));
    colours[ImGuiCol_Separator] = rgb(color::outline);
    colours[ImGuiCol_TextSelectedBg] = rgb(rgba(1, 131, 255, 0.45f));
    // Only shown once a controller moves the focus: the outline of the item A presses.
    colours[ImGuiCol_NavCursor] = rgb(color::blue);
}


// True once `process` shows its startup splash (ReSkateStartupWindow) or, if the
// splash is disabled, the game's own window.
bool game_window_shown(DWORD process) {
    struct Search { DWORD process; bool found; } search{process, false};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& target = *reinterpret_cast<Search*>(parameter);
        DWORD owner{};
        GetWindowThreadProcessId(window, &owner);
        if (owner != target.process || !IsWindowVisible(window)) return TRUE;
        std::array<wchar_t, 64> name{};
        GetClassNameW(window, name.data(), static_cast<int>(name.size()));
        const std::wstring_view kind(name.data());
        if (kind == L"ReSkateStartupWindow" || kind == L"Skate") { target.found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}


} // namespace
} // namespace detail

int run(const launcher_app::Session& session, const std::vector<std::wstring>& arguments) {
    using namespace detail;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    // The window opens at the design size, which is also the smallest it can be
    // made, so that has to fit the screen: scale down for a small laptop, or a
    // high DPI that would push the window past the taskbar. Fonts and the style
    // are built from the final scale.
    const auto work_width = static_cast<float>(work.right - work.left);
    const auto work_height = static_cast<float>(work.bottom - work.top);
    if (work_width > 0 && work_height > 0)
        g_scale = std::clamp(std::min(work_width * 0.98f / design_width, work_height * 0.96f / design_height),
            0.62f, g_scale);

    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    window_class.lpszClassName = L"ReSkateLauncher";
    RegisterClassExW(&window_class);

    const int width = static_cast<int>(S(design_width));
    const int height = static_cast<int>(S(design_height));
    logging::log(logging::Level::info, logging::Channel::launcher,
        "Launcher window: {}x{} at scale {:.2f} (system DPI {}), work area {}x{}.", width, height, g_scale,
        GetDpiForSystem(), work.right - work.left, work.bottom - work.top);
    const HWND window = CreateWindowExW(WS_EX_APPWINDOW, window_class.lpszClassName, L"ReSkate",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) throw std::runtime_error("Cannot create the launcher window");
    g_min_size = {width, height};
    // The frame style changed what the client is: have Windows ask again (WM_NCCALCSIZE).
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const DWORD corners = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    const auto fonts = load_fonts();
    apply_style();
    ImGui_ImplWin32_Init(window);
    Renderer renderer;
    if (!renderer.init(window)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        // (Shown translated by main.)
        throw std::runtime_error(translatable("This PC's graphics driver cannot draw the launcher's window. Update your "
            "graphics driver and try again. If that does not help, the graphics card is below what ReSkate and "
            "skate. need. The log names the card it tried to use."));
    }
    g_renderer = &renderer;
    {
        const auto bytes = background_bytes(session.self.parent_path());
        std::vector<unsigned char> pixels;
        UINT image_width{}, image_height{};
        if (!bytes.empty() && decode_image(bytes, ImVec2(static_cast<float>(width) * 1.1f, static_cast<float>(height) * 1.1f),
                pixels, image_width, image_height)) {
            g_background.id = renderer.upload_texture(pixels, image_width, image_height);
            g_background.width = static_cast<float>(image_width);
            g_background.height = static_cast<float>(image_height);
        }
    }
    for (auto [name, icon] : {std::pair{L"LAUNCHER_ICON_MODS", &g_icon_mods},
                              std::pair{L"LAUNCHER_ICON_SETTINGS", &g_icon_settings},
                              std::pair{L"LAUNCHER_ICON_THUNDERSTORE", &g_icon_thunderstore}}) {
        std::vector<unsigned char> pixels;
        UINT icon_width{}, icon_height{};
        const auto bytes = resource_bytes(name);
        if (!bytes.empty() && decode_image(bytes, ImVec2(S(96), S(96)), pixels, icon_width, icon_height)) {
            icon->id = renderer.upload_texture(pixels, icon_width, icon_height);
            icon->width = static_cast<float>(icon_width);
            icon->height = static_cast<float>(icon_height);
        }
    }
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    int result = 0;
    {
        // On the heap: crash dumps hold thread stacks, and both keep the Steam password while it is typed.
        const auto launcher_storage = std::make_unique<Launcher>(session, arguments);
        auto& launcher = *launcher_storage;
        launcher.check();
        const auto ui_storage = std::make_unique<Ui>();
        auto& ui = *ui_storage;
        ModsPanel mods_panel;
        // The Steam Deck's right trackpad reaches the launcher as the right stick
        // (Steam sets SteamDeck=1 for what it starts there), which wants a
        // trackpad's feel rather than a thumbstick's.
        PadFeed pad_feed(GetEnvironmentVariableW(L"SteamDeck", nullptr, 0) > 0);
        g_pad_feed = &pad_feed;
        bool running = true;
        HANDLE game{};
        DWORD game_id{};
        bool hidden{};
        bool drew{};
        UINT drawn_width = static_cast<UINT>(width), drawn_height = static_cast<UINT>(height);
        const auto paint = [&] {
            RECT client{};
            GetClientRect(window, &client);
            const auto client_width = static_cast<UINT>(client.right - client.left);
            const auto client_height = static_cast<UINT>(client.bottom - client.top);
            if (!client_width || !client_height) return;
            if (client_width != drawn_width || client_height != drawn_height) {
                renderer.resize(client_width, client_height);
                drawn_width = client_width;
                drawn_height = client_height;
            }
            g_painting = true;
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // A pad pressed while another window has the focus is not meant for us;
            // it reads as let go, so nothing stays held while we are in the background.
            const bool foreground = GetForegroundWindow() == window;
            pad_feed.update(ImGui::GetIO(), foreground ? read_pad() : PadState{}, g_scale);
            ImGui::NewFrame();
            pad_feed.after_new_frame();
            frame(launcher, fonts, window, ui, mods_panel);
            // While the pad moves the focus, the mouse cursor (the Deck's trackpad in its
            // desktop layout) hides; the next mouse move hides the focus and brings it back.
            if (GImGui->NavCursorVisible && GImGui->NavHighlightItemUnderNav && !pad_feed.pointing())
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            ImGui::Render();
            renderer.render();
            g_painting = false;
            if (!drew) {
                drew = true;
                logging::write(logging::Level::info, logging::Channel::launcher, "Launcher drew its first frame.");
            }
        };
        g_paint = paint;
        while (running) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT) running = false;
            }
            if (!running) break;
            if (launcher.restart_requested()) {
                launcher.restart();
                break;
            }
            // A successful launch closes immediately unless the user keeps the
            // launcher alive. In that mode it sleeps hidden while the game runs.
            if (launcher.launched() && !launcher.settings().keep_open_after_launch) break;
            if (!game && launcher.game()) {
                game_id = launcher.game();
                game = OpenProcess(SYNCHRONIZE, FALSE, game_id);
            }
            if (game) {
                if (!hidden && game_window_shown(game_id)) {
                    ShowWindow(window, SW_HIDE);
                    hidden = true;
                }
                if (hidden && !launcher.busy() && launcher.snapshot().phase == Phase::failed) {
                    ShowWindow(window, SW_SHOWNORMAL);
                    SetForegroundWindow(window);
                    hidden = false;
                }
                if (WaitForSingleObject(game, 0) == WAIT_OBJECT_0) {
                    CloseHandle(game);
                    game = nullptr;
                    launcher.game_exited(hidden);
                    if (hidden) {
                        ShowWindow(window, SW_SHOWNORMAL);
                        SetForegroundWindow(window);
                        hidden = false;
                    }
                }
            }
            if (hidden || IsIconic(window)) { Sleep(hidden ? 100 : 50); continue; }
            paint();
        }
        g_paint = nullptr;
        g_pad_feed = nullptr;
        ShowWindow(window, SW_HIDE);
        if (game) CloseHandle(game);
        launcher.cancel();
        // Closing the window with Settings still open must not lose changes.
        try { launcher.save(); } catch (...) {}
    }
    g_renderer = nullptr;
    renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(window);
    if (com) CoUninitialize();
    return result;
}

} // namespace dingosdk::launcher_gui
