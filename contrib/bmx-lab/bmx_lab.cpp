// Standalone input/model preview. It never opens or modifies the game.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include "Engine/Game/BMX/bmx_input.h"
#include "Engine/Game/BMX/bmx_model.h"
#include "Engine/Game/BMX/bmx_controls.h"
#include "Engine/Game/Input/bmx_input_sample.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>

namespace {
using namespace dingosdk::bmx;
constexpr COLORREF background = RGB(15, 22, 34), card = RGB(25, 36, 52);
constexpr COLORREF white = RGB(231, 239, 248), muted = RGB(154, 174, 196);
constexpr COLORREF front_color = RGB(76, 205, 237), rear_color = RGB(255, 181, 85);
using Clock = std::chrono::steady_clock;
struct View {
    bool keyboard{}, focused{true}, armed{}, air{}, snapshot{};
    std::uint32_t device{};
    RawInput raw;
    Actions actions;
    InputMapper mapper;
    Model model;
    double speed{}, distance{}, height{}, vertical{}, bars{}, whip{}, wheels{}, crank{};
    double bar_remaining{}, whip_remaining{};
    std::deque<std::wstring> events;
    Clock::time_point last{Clock::now()};
    HMODULE xinput{};
    DWORD(WINAPI* get_state)(DWORD, XINPUT_STATE*){};
    HFONT title{}, normal{}, mono{};
} app;

std::wstring number(double value, int precision = 2) {
    std::wostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str();
}
void note(std::wstring text) {
    app.events.push_front(std::move(text));
    if (app.events.size() > 7) app.events.pop_back();
}
void disarm() {
    app.mapper.reset(); app.armed = false; app.raw = {}; app.actions = {};
    app.model.advance(0, {}, {}); // Rebase time/events; the lab pauses while inactive.
    app.bar_remaining = app.whip_remaining = app.bars = app.whip = 0;
}
void reset() {
    disarm(); app.speed = app.distance = app.height = app.vertical = 0;
    app.bars = app.whip = app.wheels = app.crank = 0; app.air = false;
    app.model.reset();
    note(L"Reset synthetic scene; release both sticks to arm.");
}
void load_xinput() {
    for (const auto* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) continue;
        const auto fn = std::bit_cast<decltype(app.get_state)>(GetProcAddress(module, "XInputGetState"));
        if (fn) { app.xinput = module; app.get_state = fn; return; }
        FreeLibrary(module);
    }
}
double key(int code) { return (GetAsyncKeyState(code) & 0x8000) ? 1.0 : 0.0; }
RawInput sample() {
    RawInput input; input.focused = app.focused;
    if (!app.focused) return input;
    if (app.keyboard) {
        input.available = true; input.device = 0x10000;
        input.left_x = key('D') - key('A'); input.left_y = key('W') - key('S');
        input.right_x = key('L') - key('J'); input.right_y = key('I') - key('K');
        input.pedal = key('E'); input.brake = key('Q');
    } else if (app.get_state) {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE pad{};
            if (app.get_state(slot, &pad) != ERROR_SUCCESS) continue;
            const auto axes = dingosdk::input::bmx_pad_axes(pad.Gamepad.sThumbLX, pad.Gamepad.sThumbLY,
                pad.Gamepad.sThumbRX, pad.Gamepad.sThumbRY, pad.Gamepad.wButtons);
            input.available = true; input.device = slot + 1;
            input.left_x = axes.left_x; input.left_y = axes.left_y;
            input.right_x = axes.right_x; input.right_y = axes.right_y;
            input.pedal = axes.pedal; input.brake = axes.brake; input.quick_hop = axes.quick_hop;
            break;
        }
    }
    return input;
}
bool neutral(const RawInput& input) {
    return std::abs(input.left_x) < .12 && std::abs(input.left_y) < .12 &&
        std::abs(input.right_x) < .12 && std::abs(input.right_y) < .12 && input.pedal < .05 && input.brake < .05 && !input.quick_hop;
}
void update(HWND window) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - app.last).count(); app.last = now;
    app.focused = GetForegroundWindow() == window;
    auto raw = sample();
    if (!raw.available || !raw.focused) {
        if (app.armed) note(raw.focused ? L"Controller disconnected; preview paused." : L"Focus lost; preview paused.");
        disarm(); app.device = raw.device; return;
    }
    if (raw.device != app.device) { disarm(); app.device = raw.device; note(L"Input device changed; release controls to arm."); }
    app.raw = raw;
    if (!app.armed) {
        if (!neutral(raw)) return;
        app.armed = true; note(L"Controls armed. Inputs affect this lab only.");
    }
    if (elapsed > .2) { disarm(); note(L"Long frame; release controls to resume without catch-up."); return; }
    app.actions = app.mapper.update(raw, InputContext{!app.air && app.height <= 0}, std::clamp(elapsed, 0.0, .05));
    const auto& a = app.actions;
    if (a.hop_pressed) note(L"Hop request: " + number(a.hop_strength));
    if (a.handlebar != TrickDirection::none) {
        note(a.handlebar == TrickDirection::left ? L"Barspin request: left" : L"Barspin request: right");
        app.bar_remaining = (a.handlebar == TrickDirection::left ? -1 : 1) * tau;
    }
    if (a.tailwhip != TrickDirection::none) {
        note(a.tailwhip == TrickDirection::left ? L"Tailwhip request: left" : L"Tailwhip request: right");
        app.whip_remaining = (a.tailwhip == TrickDirection::left ? -1 : 1) * tau;
    }
    const auto animate = [elapsed](double& angle, double& remaining) {
        const double turn = std::copysign(std::min(std::abs(remaining), elapsed * tau / .75), remaining);
        angle += turn; remaining -= turn;
        if (std::abs(remaining) < 1e-7) angle = remaining = 0;
    };
    animate(app.bars, app.bar_remaining); animate(app.whip, app.whip_remaining);
    const auto command = riding_command(a);
    const Contacts floor{{QueryState::hit, 0}, {QueryState::hit, 0}};
    // G holds an explicitly synthetic air pose so gestures can be inspected without a time limit.
    app.model.advance(app.air ? 0 : std::clamp(elapsed, 0.0, .05), command, floor);
    const auto& state = app.model.state();
    app.speed = state.speed; app.distance = state.z; app.height = state.y; app.vertical = state.vertical_speed;
    app.wheels = state.rear_wheel_phase; app.crank = state.crank_phase;
}
void fill(HDC dc, RECT rect, COLORREF color) {
    auto brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void text(HDC dc, int x, int y, std::wstring text_value, COLORREF color = white, HFONT font = nullptr) {
    auto old = SelectObject(dc, font ? font : app.normal); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, text_value.c_str(), static_cast<int>(text_value.size())); SelectObject(dc, old);
}
void line(HDC dc, double x1, double y1, double x2, double y2, COLORREF color, int width = 2) {
    auto pen = CreatePen(PS_SOLID, width, color); auto old = SelectObject(dc, pen);
    MoveToEx(dc, int(x1), int(y1), nullptr); LineTo(dc, int(x2), int(y2)); SelectObject(dc, old); DeleteObject(pen);
}
void circle(HDC dc, int x, int y, int radius, COLORREF color, int width = 2) {
    auto pen = CreatePen(PS_SOLID, width, color); auto old = SelectObject(dc, pen);
    auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
    SelectObject(dc, brush); SelectObject(dc, old); DeleteObject(pen);
}
void stick(HDC dc, int x, int y, double sx, double sy, COLORREF color, const wchar_t* label) {
    circle(dc, x, y, 56, muted, 1); line(dc, x - 60, y, x + 60, y, RGB(59, 78, 99), 1);
    line(dc, x, y - 60, x, y + 60, RGB(59, 78, 99), 1);
    line(dc, x, y, x + sx * 50, y - sy * 50, color, 3);
    circle(dc, x + int(sx * 50), y - int(sy * 50), 7, color, 3);
    text(dc, x - 85, y - 92, label, color);
    text(dc, x - 78, y + 72, L"X " + number(sx) + L"   Y " + number(sy), muted, app.mono);
}
void side_bike(HDC dc, int x, int y) {
    const double lift = std::min(app.height * 32.0, 95.0), manual = app.actions.manual - app.actions.nose_manual;
    y -= int(lift); int rear_y = y + int(manual < 0 ? manual * 26 : 0), front_y = y - int(std::max(manual, 0.0) * 26);
    circle(dc, x, rear_y, 29, rear_color, 3); circle(dc, x + 160, front_y, 29, front_color, 3);
    const int cx = x + 70, cy = y - 12;
    line(dc, x, rear_y, cx, cy, rear_color, 3); line(dc, x, rear_y, x + 43, y - 65, rear_color, 3);
    line(dc, x + 43, y - 65, cx, cy, rear_color, 3); line(dc, x + 43, y - 65, x + 126, y - 68, rear_color, 3);
    line(dc, cx, cy, x + 126, y - 68, rear_color, 3); line(dc, x + 160, front_y, x + 126, y - 68, front_color, 4);
    line(dc, x + 126, y - 68, x + 120, y - 90, front_color, 4); line(dc, x + 120, y - 90, x + 140, y - 96, front_color, 4);
    line(dc, x + 32, y - 73, x + 58, y - 73, white, 4);
    for (int wheel : {0, 1}) {
        const int wx = x + wheel * 160, wy = wheel ? front_y : rear_y;
        line(dc, wx, wy, wx + std::cos(app.wheels) * 25, wy + std::sin(app.wheels) * 25, muted, 2);
    }
    line(dc, cx, cy, cx + std::cos(app.crank) * 15, cy + std::sin(app.crank) * 15, white, 3);
    // Deliberately schematic rider: no game model or authored animation is implied.
    circle(dc, x + 80, y - 130, 10, white, 2);
    line(dc, x + 78, y - 118, x + 56, y - 77, white, 3);
    line(dc, x + 78, y - 116, x + 132, y - 95, white, 3);
    line(dc, x + 56, y - 77, x + 96, y - 43, white, 3); line(dc, x + 96, y - 43, cx, cy, white, 3);
}
void top_bike(HDC dc, int x, int y) {
    const double angle = app.whip, rear_x = x + std::sin(angle) * 105, rear_y = y + std::cos(angle) * 105;
    line(dc, x, y, rear_x, rear_y, rear_color, 6);
    line(dc, rear_x - std::sin(angle) * 17, rear_y - std::cos(angle) * 17,
        rear_x + std::sin(angle) * 17, rear_y + std::cos(angle) * 17, rear_color, 7);
    line(dc, x, y, x, y - 55, muted, 4);
    const double steer = app.actions.steer * .65 + app.bars;
    line(dc, x - std::cos(steer) * 40, y - std::sin(steer) * 40,
        x + std::cos(steer) * 40, y + std::sin(steer) * 40, front_color, 5);
    line(dc, x + std::sin(steer) * 18, y - 55 - std::cos(steer) * 18,
        x - std::sin(steer) * 18, y - 55 + std::cos(steer) * 18, front_color, 7);
    circle(dc, x, y, 6, white, 2);
}
void paint(HDC destination, RECT bounds) {
    auto dc = CreateCompatibleDC(destination); auto bitmap = CreateCompatibleBitmap(destination, bounds.right, bounds.bottom);
    auto old = SelectObject(dc, bitmap); fill(dc, bounds, background);
    text(dc, 28, 20, L"BMX CONTROLS LAB", white, app.title);
    text(dc, 28, 59, L"Synthetic flat-ground preview - not the game. No game attachment, input capture or save writes.", muted);
    fill(dc, {24, 98, 505, 338}, card); fill(dc, {521, 98, 1052, 338}, card);
    stick(dc, 135, 220, app.raw.left_x, app.raw.left_y, front_color, L"LEFT / HANDLEBARS");
    stick(dc, 375, 220, app.raw.right_x, app.raw.right_y, rear_color, L"RIGHT / REAR WHEEL");
    const auto device = app.snapshot ? L"Illustrative snapshot (synthetic input)" : app.keyboard ? L"Keyboard fallback (digital)" : L"XInput controller";
    text(dc, 543, 118, device, white);
    text(dc, 543, 151, !app.focused ? L"PAUSED: focus this window" : !app.raw.available ? L"No pad. Connect one or press F2." :
        !app.armed ? L"Release sticks and buttons to arm." : L"ARMED: lab preview only", app.armed ? front_color : rear_color);
    text(dc, 543, 189, app.air ? L"AIR gesture sandbox (held) - G returns" : app.height > 0 ? L"Context: AIR (synthetic hop)" : L"Context: GROUND - G holds air sandbox");
    text(dc, 543, 223, L"Steer " + number(app.actions.steer) + L"   preload " + number(app.actions.preload), muted, app.mono);
    text(dc, 543, 253, L"Balance intent: rear " + number(app.actions.manual) + L" / front " + number(app.actions.nose_manual), muted, app.mono);
    text(dc, 543, 283, L"Pedal " + number(app.actions.pedal) + L"   brake " + number(app.actions.brake), muted, app.mono);
    fill(dc, {24, 354, 630, 647}, card); fill(dc, {646, 354, 1052, 647}, card);
    text(dc, 45, 369, L"SCHEMATIC / TRICK & BALANCE POSE INTENTS", muted);
    line(dc, 44, 590, 607, 590, RGB(73, 100, 124), 2);
    side_bike(dc, 104, 560); top_bike(dc, 472, 470);
    text(dc, 45, 615, L"Speed " + number(app.speed) + L" m/s  H " + number(app.height) + L" m  yaw " +
        number(app.model.state().yaw * 180 / dingosdk::bmx::pi, 0) + L" deg", muted, app.mono);
    text(dc, 666, 369, L"RECENT CONTROL EVENTS", muted);
    int row = 407; for (const auto& event : app.events) { text(dc, 666, row, event, white); row += 29; }
    text(dc, 28, 666, L"Pad: A pedals, B brakes, R3 hops. Right down/up hops; partial right Y requests balance.");
    text(dc, 28, 696, L"Air: left flick/arc requests barspin; right flick/arc requests tailwhip. Poses are illustrative.");
    text(dc, 28, 726, L"F2 input source  |  G ground / air  |  R reset  |  Keyboard: WASD left, IJKL right, E pedal, Q brake", muted);
    BitBlt(destination, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
void fonts() {
    app.title = CreateFontW(-27, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    app.normal = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    app.mono = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
}
void release_fonts() { DeleteObject(app.title); DeleteObject(app.normal); DeleteObject(app.mono); }
int snapshot(const std::wstring& path) {
    constexpr LONG width = 1080, height = 765;
    BITMAPINFO info{}; auto& header = info.bmiHeader;
    header.biSize = sizeof(header); header.biWidth = width; header.biHeight = -height;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    void* pixels{}; auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) return 2;
    auto dc = CreateCompatibleDC(nullptr); auto old = SelectObject(dc, bitmap); fonts();
    app.snapshot = app.armed = true; app.raw.available = true;
    app.raw.left_x = .55; app.raw.right_y = -.40; app.raw.pedal = .65;
    app.actions.steer = .55; app.actions.manual = .25; app.actions.pedal = .65;
    note(L"Illustrative snapshot; no device sampled."); note(L"Schematic poses are not landed tricks.");
    paint(dc, {0, 0, width, height});
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(header); file.bfSize = file.bfOffBits + width * height * 4;
    auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool success = handle != INVALID_HANDLE_VALUE;
    if (success) {
        DWORD written{};
        const auto write = [&](const void* bytes, DWORD count) { return WriteFile(handle, bytes, count, &written, nullptr) && written == count; };
        success = write(&file, sizeof(file)) && write(&header, sizeof(header)) && write(pixels, width * height * 4);
        CloseHandle(handle);
    }
    release_fonts(); SelectObject(dc, old); DeleteDC(dc); DeleteObject(bitmap); return success ? 0 : 3;
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        fonts();
        load_xinput(); note(L"Connect a pad or press F2 for keyboard."); SetTimer(window, 1, 16, nullptr); return 0;
    case WM_ACTIVATEAPP:
        app.focused = wparam != 0; disarm(); app.last = Clock::now(); return 0;
    case WM_KEYDOWN:
        if (lparam & (1LL << 30)) return 0;
        if (wparam == VK_F2) { app.keyboard = !app.keyboard; disarm(); note(app.keyboard ? L"Keyboard fallback selected." : L"XInput selected."); }
        if (wparam == 'G') {
            app.air = !app.air; disarm(); auto state = app.model.state();
            state.y = app.air ? 1.8 : 0; state.vertical_speed = 0; app.model.reset(state); app.height = state.y;
            note(app.air ? L"Air pose held for gesture inspection." : L"Returned to synthetic flat ground.");
        }
        if (wparam == 'R') reset();
        return 0;
    case WM_TIMER: update(window); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); RECT bounds{}; GetClientRect(window, &bounds); paint(dc, bounds); EndPaint(window, &ps); return 0; }
    case WM_DESTROY:
        KillTimer(window, 1); release_fonts();
        if (app.xinput) FreeLibrary(app.xinput);
        PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    std::wstring args = GetCommandLineW();
    const auto end = args.starts_with(L'"') ? args.find(L'"', 1) + 1 : args.find(L' ');
    args = end < args.size() ? args.substr(end) : L"";
    const auto first = args.find_first_not_of(L" \t"); args = first == std::wstring::npos ? L"" : args.substr(first);
    if (args.starts_with(L"--snapshot ")) {
        auto path = args.substr(11); const auto start = path.find_first_not_of(L" \t");
        if (start == std::wstring::npos) return 4;
        path = path.substr(start);
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
        return path.empty() ? 4 : snapshot(path);
    }
    WNDCLASSW klass{}; klass.hInstance = instance; klass.lpfnWndProc = procedure;
    klass.lpszClassName = L"ReSkateBmxControlsLab"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&klass)) return 1;
    RECT size{0, 0, 1080, 765}; AdjustWindowRect(&size, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    auto window = CreateWindowExW(0, klass.lpszClassName, L"BMX controls lab - not the game",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
