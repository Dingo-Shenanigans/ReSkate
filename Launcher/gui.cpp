#include "gui.h"

#include "gui_internal.h"
#include "gui_renderer.h"

#include "Engine/Core/Log/logging.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <memory>
#include <stdexcept>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace dingosdk::launcher_gui {
namespace detail {

void panel_title(const Fonts& fonts, const char* text) {
    ImGui::PushFont(fonts.tile);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

ImVec2 begin_panel(const char* id, ImVec2 size, ImVec2 panel) {
    ImGui::SetNextWindowPos(ImVec2((size.x - panel.x) * 0.5f, (size.y - panel.y) * 0.5f));
    ImGui::SetNextWindowSize(panel);
    // Keep panels in front, but not over their own open combo or popup.
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) ImGui::SetNextWindowFocus();
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    return panel;
}

ImVec2 begin_page(const char* id, ImVec2 size) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) ImGui::SetNextWindowFocus();
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
    const bool pressed = ImGui::InvisibleButton(id, size);
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
    draw->AddRectFilled(start, end, ticked ? rgba(28, 38, 52) : rgba(31, 31, 34));
    // The hover tint is the Selectable's; the row's own colour is under it.
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.055f)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.09f)));
    const bool pressed = ImGui::Selectable(id, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, height));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ticked) draw->AddRectFilled(start, ImVec2(start.x + S(3), end.y), color::blue);
    draw->AddLine(ImVec2(start.x, end.y - S(1)), ImVec2(end.x, end.y - S(1)), rgba(255, 255, 255, 0.07f), S(1));
    return pressed;
}

bool toggle(const char* id, bool* on) {
    const ImVec2 size(S(42), S(22));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
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
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
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
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_NCHITTEST: {
        // The top strip drags the borderless window, except over its buttons.
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(window, &point);
        if (g_drag_allowed && point.y >= 0 && static_cast<float>(point.y) < S(44) &&
            static_cast<float>(point.x) < S(design_width - 100)) return HTCAPTION;
        return HTCLIENT;
    }
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

// Fonts are embedded as RCDATA (External/fonts); Segoe UI is the fallback.
ImFont* embedded_font(const wchar_t* name, float size) {
    static const ImWchar ranges[]{0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x2193, 0};
    const auto instance = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(instance, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!resource) return nullptr;
    const auto loaded = LoadResource(instance, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // the resource lives as long as the process
    config.OversampleH = size >= 48 ? 1 : 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(instance, resource)),
        S(size), &config, ranges);
}

ImFont* system_font(const wchar_t* file, float size) {
    std::array<wchar_t, MAX_PATH> windows{};
    GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    const auto path = fs::path(windows.data()) / L"Fonts" / file;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), S(size));
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
    colours[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);
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
    // The window cannot be resized, so the design size has to fit the screen:
    // scale down for a small laptop, or a high DPI that would push the window
    // past the taskbar. Fonts and the style are built from the final scale.
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
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) throw std::runtime_error("Cannot create the launcher window");
    const DWORD corners = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    const auto fonts = load_fonts();
    apply_style();
    ImGui_ImplWin32_Init(window);
    Renderer renderer;
    if (!renderer.init(window)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        throw std::runtime_error("This PC's graphics driver cannot draw the launcher's window. Update your "
            "graphics driver and try again. If that does not help, the graphics card is below what ReSkate and "
            "skate. need. The log names the card it tried to use.");
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
        bool running = true;
        HANDLE game{};
        DWORD game_id{};
        bool hidden{};
        bool drew{};
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
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            frame(launcher, fonts, window, ui, mods_panel);
            ImGui::Render();
            renderer.render();
            if (!drew) {
                drew = true;
                logging::write(logging::Level::info, logging::Channel::launcher, "Launcher drew its first frame.");
            }
        }
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
