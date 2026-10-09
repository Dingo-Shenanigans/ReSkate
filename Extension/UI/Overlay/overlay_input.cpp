#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Boot/exit_watch.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "playstation_input.h"
#include "cursor.h"

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {

bool console_toggle_key(UINT message, WPARAM wp, LPARAM lp) {
    const bool key_message = message == WM_KEYDOWN || message == WM_SYSKEYDOWN
        || message == WM_KEYUP || message == WM_SYSKEYUP;
    const auto console = dingosdk::launcher::overlay_keys().console;
    // The default also matches by position (scan code 0x29, left of 1), so
    // layouts that put another character there still open the console.
    if (console != dingosdk::launcher::default_console_key) return key_message && wp == console;
    const auto scan_code = (static_cast<ULONG_PTR>(lp) >> 16) & 0xffu;
    return key_message && (wp == VK_OEM_3 || scan_code == 0x29u);
}

bool console_character(UINT message) {
    return message == WM_CHAR || message == WM_SYSCHAR
        || message == WM_DEADCHAR || message == WM_SYSDEADCHAR;
}

// Reads one WM_INPUT packet without consuming it (the game or DefWindowProc
// still receives the message). True when it was a mouse packet and was noted.
bool note_raw_mouse(HRAWINPUT handle, bool overlay_owns_pointer) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    {
        OverlayInputAccess access;
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return false;
    }
    if (raw.header.dwType != RIM_TYPEMOUSE) return false;
    note_raw_mouse_packet(raw.data.mouse, overlay_owns_pointer);
    return true;
}

// Focus changes of the game window, with the window and program on the other side: the
// game (and Steam Input's controller layout) follow them, and a stray one can leave the
// controller unanswered until the player tabs out and back.
void log_focus_change(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    static std::atomic<unsigned> logged{};
    if (logged.fetch_add(1) >= 400) return;
    const auto describe = [](HWND other) {
        if (!other) return std::string("no window");
        wchar_t name[128]{};
        GetClassNameW(other, name, 128);
        DWORD process{};
        GetWindowThreadProcessId(other, &process);
        std::string text;
        for (const wchar_t* c = name; *c && text.size() < 100; ++c) text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        return text + (process == GetCurrentProcessId() ? " (this game)" : " (process " + std::to_string(process) + ")");
    };
    std::string text;
    if (message == WM_ACTIVATEAPP)
        text = wp ? "game became the active application" : "game stopped being the active application (thread " +
                                                              std::to_string(static_cast<DWORD>(lp)) + " took over)";
    else if (message == WM_ACTIVATE)
        text = LOWORD(wp) == WA_INACTIVE ? "game window deactivated for " + describe(reinterpret_cast<HWND>(lp))
                                         : "game window activated from " + describe(reinterpret_cast<HWND>(lp));
    else if (message == WM_KILLFOCUS)
        text = "keyboard focus moved to " + describe(reinterpret_cast<HWND>(wp));
    else if (message == WM_SETFOCUS)
        text = "keyboard focus returned from " + describe(reinterpret_cast<HWND>(wp));
    else
        return;
    (void)window;
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input, "Focus: %s.", text.c_str());
}

void release_game_buttons(HWND window, WNDPROC previous) {
    auto& s = state();
    if (!previous) return;
    for (unsigned key = 0; key < s.game_keys.size(); ++key) if (s.game_keys[key]) {
        s.game_keys[key] = false;
        const LPARAM bits = 1 | (static_cast<LPARAM>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC)) << 16) |
            static_cast<LPARAM>((1ull << 30) | (1ull << 31));
        CallWindowProcW(previous, window, WM_KEYUP, key, bits);
    }
    constexpr std::array releases{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (unsigned i = 0; i < releases.size(); ++i) if (s.game_mouse_buttons & (1u << i))
        CallWindowProcW(previous, window, releases[i], i >= 3 ? MAKEWPARAM(0, i == 3 ? XBUTTON1 : XBUTTON2) : 0, 0);
    s.game_mouse_buttons = 0;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    struct NativeInputScope {
        unsigned saved{overlay_input_access};
        NativeInputScope() { overlay_input_access = 0; }
        ~NativeInputScope() { overlay_input_access = saved; }
    } native_input;
    auto& s = state();
    WNDPROC previous{};
    {
        std::lock_guard lock(s.window_hook_mutex);
        if (const auto found = s.window_hooks.find(window); found != s.window_hooks.end()) {
            previous = found->second.previous;
            if (message == WM_NCDESTROY) found->second.installed = false;
        }
    }
    // Mouse movement and raw input arrive up to thousands of times a second, and
    // whether the overlay owns the pointer costs window-manager calls (the
    // foreground check). Nothing this procedure does with those messages changes
    // it, so it is worked out once for them; keys, focus and everything else still
    // ask at each step.
    const bool pointer_message = window == s.window.load() && (message == WM_INPUT || message == WM_MOUSEMOVE ||
        message == WM_SETCURSOR || message == WM_NCHITTEST);
    const bool pointer_owned = pointer_message && owns_menu_cursor(s);
    const auto owns_pointer = [&] { return pointer_message ? pointer_owned : owns_menu_cursor(s); };
    if (window == s.window.load()) sync_raw_mouse_registration(owns_pointer());
    if (message && message == cursor_sync_message()) {
        window_cursor_input(window, message, wp);
        return 0;
    }
    // An overlay above us may keep this forwarding link after the selected
    // window changes. Its original procedure belongs to that HWND.
    if (window != s.window.load())
        return previous ? CallWindowProcW(previous, window, message, wp, lp)
                        : DefWindowProcW(window, message, wp, lp);
    // The player closing the game: it is ended if it does not finish by itself.
    dingosdk::exit_watch::note_window_message(window, message, wp);
    struct ReleaseOnOpen {
        HWND window; WNDPROC previous; bool was_visible;
        ~ReleaseOnOpen() {
            if (!was_visible && interactive_visible(state())) release_game_buttons(window, previous);
        }
    } release_on_open{window, previous, interactive_visible(s)};
    // The raw packet noted below stays noted while the game handles this message,
    // so its own GetRawInputData read of the same packet is not counted again.
    struct NotedRawInput {
        HRAWINPUT saved{noted_raw_input};
        ~NotedRawInput() { noted_raw_input = saved; }
    } noted_raw_input_scope;
    if (message == WM_ACTIVATEAPP || message == WM_ACTIVATE || message == WM_KILLFOCUS || message == WM_SETFOCUS)
        log_focus_change(window, message, wp, lp);
    if ((message >= WM_KEYFIRST && message <= WM_KEYLAST && message != WM_CHAR) ||
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST))
        note_input_message(message, wp);
    if (!s.stop.load() && s.input_attached.load()) {
        if (message == WM_INPUT && note_raw_mouse(reinterpret_cast<HRAWINPUT>(lp), pointer_owned))
            noted_raw_input = reinterpret_cast<HRAWINPUT>(lp);
        if (message == WM_KILLFOCUS) s.raw_mouse_buttons.store(0);
        if (pointer_message) window_cursor_input(window, message, wp, pointer_owned);
        else window_cursor_input(window, message, wp);
        const bool key = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool release = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (console_character(message) && s.console_character_pending.exchange(false)) return 0;
        if (console_character(message) && s.chat_character_pending.exchange(false)) return 0;
        // T opens the chat, the key the game itself reserves for it
        // (Processor_Keyboard_Gameplay_Chat), while in a session and nothing
        // else of ours is open. Its character is swallowed like the console's.
        // "/" opens it too, already holding the "/" so the command list shows at once: the key
        // that types "/" on this keyboard layout (unshifted), or the numpad's.
        const bool slash = key && (wp == VK_DIVIDE ||
                                   (MapVirtualKeyW(static_cast<UINT>(wp), MAPVK_VK_TO_CHAR) == L'/' &&
                                    !(GetKeyState(VK_SHIFT) & 0x8000)));
        if (key && (wp == 'T' || slash) && (static_cast<ULONG_PTR>(lp) & (1ull << 30)) == 0 &&
            s.chat_available.load() && !interactive_visible(s) &&
            !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            s.chat_character_pending.store(true);
            s.chat_command_requested.store(slash);
            s.chat_visible.store(true);
            s.chat_focus_requested.store(true);
            sync_menu_cursor();
            window_cursor_input(window, message, wp);
            std::lock_guard lock(s.input_mutex);
            s.input.clear();
            return 0;
        }
        if ((s.chat_visible.load() || s.chat_escape_pending.load()) && !s.console_visible.load() &&
            !s.visible.load() && wp == VK_ESCAPE && (key || release)) {
            if (key && (static_cast<ULONG_PTR>(lp) & (1ull << 30)) == 0) {
                s.chat_escape_pending.store(true);
                s.chat_visible.store(false);
                sync_menu_cursor();
                window_cursor_input(window, message, wp);
                std::lock_guard lock(s.input_mutex);
                s.input.clear();
            } else if (release) s.chat_escape_pending.store(false);
            return 0;
        }
        if (console_toggle_key(message, wp, lp)) {
            if (key) {
                // TranslateMessage creates the character before the key message
                // reaches this procedure. Consume that separate packet as well.
                s.console_character_pending.store(true);
                if ((static_cast<ULONG_PTR>(lp) & (1ull << 30)) == 0) {
                    const bool opened = !s.console_visible.load();
                    s.console_visible.store(opened);
                    if (opened) s.console_focus_requested.store(true);
                    sync_menu_cursor();
                    window_cursor_input(window, message, wp);
                    std::lock_guard lock(s.input_mutex);
                    s.input.clear();
                }
            } else if (release) {
                s.console_character_pending.store(false);
            }
            return 0;
        }
        if ((s.console_visible.load() || s.console_escape_pending.load()) &&
            wp == VK_ESCAPE && (key || release)) {
            if (key && (static_cast<ULONG_PTR>(lp) & (1ull << 30)) == 0) {
                s.console_escape_pending.store(true);
                s.console_visible.store(false);
                sync_menu_cursor();
                window_cursor_input(window, message, wp);
                std::lock_guard lock(s.input_mutex);
                s.input.clear();
            } else if (release) s.console_escape_pending.store(false);
            return 0;
        }
        if (wp == dingosdk::launcher::overlay_keys().menu && (key || release)) {
            if (key && (static_cast<ULONG_PTR>(lp) & (1ull << 30)) == 0) {
                if (s.editor_visible.load()) s.editor_exit_requested.store(true);
                else s.visible.store(!s.visible.load());
                sync_menu_cursor();
                window_cursor_input(window, message, wp);
                std::lock_guard lock(s.input_mutex);
                s.input.clear();
            }
            return 0;
        }
        if (message == WM_NCDESTROY) s.selected_window_destroyed.store(true);
        if (message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_NCDESTROY) sync_menu_cursor();
        // ImGui's Win32 backend ignores raw input, so WM_INPUT is not queued.
        if ((interactive_visible(s) || message == WM_KILLFOCUS || message == WM_SETFOCUS) && message != WM_INPUT) {
            std::lock_guard lock(s.input_mutex);
            if (message == WM_MOUSEMOVE && !s.input.empty() && s.input.back().message == WM_MOUSEMOVE &&
                s.input.back().window == window) {
                // Moves in a row: ImGui applies them all in the same frame and keeps
                // the last, so only the newest is kept.
                s.input.back() = {window, message, wp, lp};
            } else if (s.input.size() < 512) s.input.push_back({window, message, wp, lp});
            else {
                // Mouse movement can exceed the bounded queue while a frame is
                // delayed. Drop the oldest packet without changing menu state.
                s.input.pop_front();
                s.input.push_back({window, message, wp, lp});
            }
        }
        const bool capture = owns_pointer();
        const bool freecam_capture = s.freecam_controller_active.load(std::memory_order_relaxed);
        if (freecam_capture) release_game_buttons(window, previous);
        if ((capture || freecam_capture) && is_input(message)) {
            // Foreground raw-input packets still need DefWindowProc cleanup.
            return message == WM_INPUT ? DefWindowProcW(window, message, wp, lp) : 0;
        }
        if (capture && message == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            OverlayInputAccess access;
            SetCursor(nullptr); // The software cursor is independent of the game's ShowCursor counter.
            return TRUE;
        }
        if ((key || release) && wp < s.game_keys.size()) s.game_keys[wp] = key;
        constexpr std::array<UINT, 4> down{WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN};
        constexpr std::array<UINT, 4> up{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP};
        for (unsigned i = 0; i < down.size(); ++i) {
            const auto bit = 1u << (i == 3 && GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 4 : i);
            if (message == down[i]) s.game_mouse_buttons |= bit;
            if (message == up[i]) s.game_mouse_buttons &= ~bit;
        }
        if (message == WM_KILLFOCUS) { s.game_keys.fill(false); s.game_mouse_buttons = 0; }
    }
    return previous ? CallWindowProcW(previous, window, message, wp, lp)
                    : DefWindowProcW(window, message, wp, lp);
}
}

namespace {
struct ControllerSample {
    XINPUT_GAMEPAD pad{};
    unsigned device{};
    dingosdk::ControllerStyle style{};
};
using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
GetState xinput_get_state() {
    // Optional OS API, resolved outside DllMain. Keep the module for process
    // lifetime; a missing controller/runtime must not prevent keyboard flight.
    static const auto get_state = []() -> GetState {
        for (const auto name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            const auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) continue;
            const auto entry = GetProcAddress(module, "XInputGetState");
            if (entry) {
                GetState fn{};
                static_assert(sizeof(fn) == sizeof(entry));
                std::memcpy(&fn, &entry, sizeof(fn));
                return fn;
            }
            FreeLibrary(module);
        }
        return nullptr;
    }();
    return get_state;
}
// The XInput slots with a pad in them, shared by every reader. Polling an empty
// slot is slow (it looks for a device), so readers poll only these, and a
// background thread retries the empty ones once a second while pads are read:
// the client tick, which reads bound combos, never waits for that.
struct XInputSlots {
    std::atomic<unsigned> connected{};
    std::atomic<ULONGLONG> next_probe{};
    std::atomic<HANDLE> wake{}; // set when a probe is due; the probe thread waits on it
    std::mutex start;           // starting the thread (tried again at the next probe if it fails)
};
XInputSlots& xinput_slots() { static auto* value = new XInputSlots; return *value; } // process lifetime
DWORD WINAPI probe_xinput_slots(void* parameter) noexcept {
    auto& slots = *static_cast<XInputSlots*>(parameter);
    const auto get_state = xinput_get_state();
    for (;;) {
        WaitForSingleObject(slots.wake.load(), INFINITE);
        OverlayInputAccess access;
        for (DWORD slot = 0; get_state && slot < XUSER_MAX_COUNT; ++slot) {
            const unsigned bit = 1u << slot;
            if (slots.connected.load() & bit) continue;
            XINPUT_STATE sample{};
            if (get_state(slot, &sample) == ERROR_SUCCESS) slots.connected.fetch_or(bit);
        }
    }
}
// Wakes the probe thread (starting it the first time) at most once a second.
void request_xinput_probe(XInputSlots& slots) {
    const auto now = GetTickCount64();
    auto due = slots.next_probe.load();
    if (now < due || !slots.next_probe.compare_exchange_strong(due, now + 1000)) return;
    if (!slots.wake.load()) {
        std::lock_guard lock(slots.start);
        if (!slots.wake.load()) {
            if (const auto wake = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
                slots.wake.store(wake);
                if (const auto thread = CreateThread(nullptr, 0, probe_xinput_slots, &slots, 0, nullptr))
                    CloseHandle(thread);
                else {
                    slots.wake.store(nullptr);
                    CloseHandle(wake);
                }
            }
        }
    }
    if (const auto wake = slots.wake.load()) SetEvent(wake);
}
ControllerSample read_controller_sample() {
    OverlayInputAccess access; // Binding capture needs the real controller while the menu is open.
    const auto get_state = xinput_get_state();
    // Virtual pads (DS4Windows, Steam, ViGEm leftovers) can take any slot, and
    // an idle one may sit ahead of the pad in use, so merge every connected
    // slot. Empty slots are retried in the background (xinput_slots above).
    auto& slots = xinput_slots();
    if (get_state) request_xinput_probe(slots);
    const auto connected_slots = slots.connected.load();
    unsigned connected = 0;
    ControllerSample result;
    for (DWORD slot = 0; get_state && slot < XUSER_MAX_COUNT; ++slot) {
        const unsigned bit = 1u << slot;
        if (!(connected_slots & bit)) continue;
        XINPUT_STATE sample{};
        if (get_state(slot, &sample) != ERROR_SUCCESS) { slots.connected.fetch_and(~bit); continue; }
        connected |= bit;
        const auto& pad = sample.Gamepad;
        result.pad.wButtons |= pad.wButtons;
        result.pad.bLeftTrigger = std::max(result.pad.bLeftTrigger, pad.bLeftTrigger);
        result.pad.bRightTrigger = std::max(result.pad.bRightTrigger, pad.bRightTrigger);
        const auto magnitude = [](SHORT x, SHORT y) { return std::abs(int(x)) + std::abs(int(y)); };
        if (magnitude(pad.sThumbLX, pad.sThumbLY) > magnitude(result.pad.sThumbLX, result.pad.sThumbLY)) {
            result.pad.sThumbLX = pad.sThumbLX; result.pad.sThumbLY = pad.sThumbLY;
        }
        if (magnitude(pad.sThumbRX, pad.sThumbRY) > magnitude(result.pad.sThumbRX, result.pad.sThumbRY)) {
            result.pad.sThumbRX = pad.sThumbRX; result.pad.sThumbRY = pad.sThumbRY;
        }
    }
    // A PlayStation pad the tool left visible still names the buttons, even
    // while its virtual XInput twin supplies them.
    const auto playstation = read_playstation_pads();
    result.style = playstation.style;
    if (connected) {
        // XInput wins whenever a pad is there: reading both would double a
        // DS4Windows pad and mix its remapped buttons with the physical ones.
        result.device = connected;
        return result;
    }
    if (!playstation.available) return {};
    const auto& pad = playstation.pad;
    result.pad.wButtons = pad.buttons;
    result.pad.bLeftTrigger = pad.left_trigger; result.pad.bRightTrigger = pad.right_trigger;
    result.pad.sThumbLX = pad.left_x; result.pad.sThumbLY = pad.left_y;
    result.pad.sThumbRX = pad.right_x; result.pad.sThumbRY = pad.right_y;
    result.device = 0x100u + (playstation.generation & 0xffffffu);
    return result;
}
dingosdk::overlay::FlightInput read_player_flight_controller() {
    const auto sample = read_controller_sample();
    if (!sample.device) return {};
    const auto& pad = sample.pad;
    auto input = dingosdk::controller_flight_input(pad.sThumbLX, pad.sThumbLY, pad.bLeftTrigger, pad.bRightTrigger,
        (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
    const float rx = static_cast<float>(pad.sThumbRX), ry = static_cast<float>(pad.sThumbRY);
    const float magnitude = std::sqrt(rx * rx + ry * ry);
    constexpr float deadzone = 8689.0f;
    if (magnitude > deadzone) {
        const float scale = std::clamp((magnitude - deadzone) / (32767.0f - deadzone), 0.0f, 1.0f) / magnitude;
        input.look_x = rx * scale * 25.0f;
        input.look_y = ry * scale * -25.0f;
    }
    return input;
}
}

extern "C" void DingoSDKOverlaySetFreecamInputCapture(bool active) {
    const bool previous = state().freecam_controller_active.exchange(active, std::memory_order_relaxed);
    if (previous != active)
        dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
            "Freecam controller input capture %s.", active ? "enabled" : "disabled");
}

extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput* output, bool allow_menu) {
    if (!output) return;
    struct PreserveError { DWORD value = GetLastError(); ~PreserveError() { SetLastError(value); } } preserve_error;
    *output = {};
    const auto& s = state();
    const HWND window = s.window.load();
    if (!window || s.stop.load() || s.failed.load() || (!allow_menu && interactive_visible(s)) ||
        !game_window_foreground(window)) return;
    // Read through the overlay's bypass: freecam capture hides keys from the game.
    {
        OverlayInputAccess access;
        for (unsigned key = 0; key < 256; ++key) {
            if (dingosdk::bindable_keyboard_key(key) && (GetAsyncKeyState(key) & 0x8000)) {
                output->keys[key / 64] |= std::uint64_t{1} << (key % 64);
            }
        }
    }
    const auto sample = read_controller_sample();
    output->style = sample.style;
    if (!sample.device) return;
    const auto& pad = sample.pad;
    output->available = true;
    output->device = sample.device;
    output->buttons = pad.wButtons & dingosdk::controller_button_mask;
    if (pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) output->buttons |= 0x10000;
    if (pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) output->buttons |= 0x20000;
}

// Raw mouse packets arrived recently enough to trust the tracked buttons.
static bool raw_mouse_live(const State& s) {
    const auto time = s.raw_mouse_time.load();
    return time && GetTickCount64() < time + 10000;
}

extern "C" void DingoSDKOverlayReadFlightInput(dingosdk::overlay::FlightInput* output, bool flight_active, bool player_flight) {
    if (!output) return;
    struct PreserveError { DWORD value = GetLastError(); ~PreserveError() { SetLastError(value); } } preserve_error;
    *output = {};
    // Only this client thread owns the mouse baseline; ImGui is never accessed.
    thread_local POINT previous{};
    thread_local bool looking = false;
    const auto& s = state();
    const HWND window = s.window.load();
    const bool editor_flight = s.editor_visible.load() && s.editor_flight.load() && !s.console_visible.load() &&
        GetTickCount64() < s.editor_input_time.load() + 150;
    if (!flight_active || !window || s.stop.load() || s.failed.load() || (interactive_visible(s) && !editor_flight) ||
        !game_window_foreground(window)) {
        looking = false;
        return;
    }
    OverlayInputAccess editor_access;
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Physical right button: the logical one unless the buttons are swapped.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const bool raw = raw_mouse_live(s);
    const bool raw_look_button = raw && (s.raw_mouse_buttons.load() & (swapped ? 1u : 2u)) != 0;
    const bool look_button = held(VK_RBUTTON) || raw_look_button;
    if (s.editor_visible.load() && !look_button) { looking = false; return; }
    output->active = true;
    output->right = static_cast<float>(held('D')) - static_cast<float>(held('A'));
    output->up = static_cast<float>(held('E')) - static_cast<float>(held('Q'));
    output->forward = static_cast<float>(held('W')) - static_cast<float>(held('S'));
    output->boost = held(VK_SHIFT);
    if (player_flight) {
        const auto controller = read_player_flight_controller();
        output->right = std::clamp(output->right + controller.right, -1.0f, 1.0f);
        output->forward = std::clamp(output->forward + controller.forward, -1.0f, 1.0f);
        output->up = std::clamp(output->up + controller.up, -1.0f, 1.0f);
        output->boost = output->boost || controller.boost;
        output->look_x = controller.look_x;
        output->look_y = controller.look_y;
        looking = false;
        return; // Native camera owns look input; do not sample or recenter the cursor.
    }
    POINT current{};
    const long raw_x = state().raw_mouse_x.exchange(0), raw_y = state().raw_mouse_y.exchange(0);
    if (look_button && GetCursorPos(&current)) {
        if (looking) {
            // Raw deltas when the game window receives raw input: they keep
            // working whatever Skate does with the Windows cursor.
            output->look_x = static_cast<float>(raw ? raw_x : current.x - previous.x);
            output->look_y = static_cast<float>(raw ? raw_y : current.y - previous.y);
        }
        previous = current;
        looking = true;
        RECT bounds{};
        if (GetClientRect(window, &bounds)) {
            POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
            if (ClientToScreen(window, &center) && SetCursorPos(center.x, center.y)) previous = center;
        }
    } else looking = false;
}
