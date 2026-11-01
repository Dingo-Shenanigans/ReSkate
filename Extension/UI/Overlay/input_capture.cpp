#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "held_input.h"
#include "scripted_pad.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Engine/Game/Input/playstation_report.h"
#include "Engine/Game/Input/owned_menu_input.h"
#include <hidsdi.h>
#include <atomic>
#include <cmath>
#include <format>
#include <array>
#include <cstring>
#include <string>
#include <mutex>
#include <set>
#include <span>
#include <tuple>

namespace dingosdk::overlay::detail {
// Window messages do not cover the game's independently polled input devices.
// Only the overlay's own Win32 backend and binding reader may bypass these gates.
thread_local HRAWINPUT noted_raw_input = nullptr;
std::atomic<ULONGLONG> mode_input_paused_until{};
std::atomic<std::uint16_t> mode_hidden_buttons{}, mode_held_buttons{};
std::atomic<int> mode_mouse_wheel{};
std::atomic<bool> challenge_menu_capture{};
std::atomic<bool> challenge_native_panel{};
std::atomic<bool> challenge_menu_release{};
bool suppress_challenge_menu() noexcept {
    return !overlay_input_access && (challenge_menu_capture.load(std::memory_order_relaxed) || challenge_menu_release.load(std::memory_order_relaxed));
}
bool challenge_menu_key(int key) noexcept {
    return key==VK_ESCAPE && suppress_challenge_menu();
}
bool challenge_panel_owns_input() noexcept {
    return challenge_native_panel.load(std::memory_order_relaxed);
}
// Keys kept from the game (hide_game_keys): their presses are dropped from every way it reads the
// keyboard. Releases pass, so a key the game saw go down still comes up.
std::array<std::atomic<std::uint64_t>, 4> hidden_keys{};
bool key_hidden(unsigned key) noexcept {
    return key < 256 && !overlay_input_access && (hidden_keys[key / 64].load(std::memory_order_relaxed) >> (key % 64) & 1) != 0;
}
namespace {
bool hidden_press(const RAWKEYBOARD& keyboard) noexcept { return !(keyboard.Flags & RI_KEY_BREAK) && key_hidden(keyboard.VKey); }
// The DirectInput keyboard's scan code (DIK_) of a hidden key.
bool scan_hidden(DWORD scan) noexcept {
    const auto key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    return key && key_hidden(key);
}
}
namespace {
std::atomic<unsigned> prompt_xinput{}, prompt_direct{}, prompt_hid{}, prompt_raw{};
// A read the game made while a card held its input: counted, by the way it read.
void note_prompt_read(std::atomic<unsigned>& count) {
    if (!overlay_input_access && state().prompt_input_active.load(std::memory_order_relaxed)) count.fetch_add(1, std::memory_order_relaxed);
}
}
PromptReads take_prompt_reads() noexcept {
    return {prompt_xinput.exchange(0), 0, prompt_direct.exchange(0), prompt_hid.exchange(0), prompt_raw.exchange(0)};
}
bool block_polled_input() {
    const auto error = GetLastError();
    const bool capture = !overlay_input_access && (owns_menu_cursor(state()) || state().freecam_controller_active.load(std::memory_order_relaxed) ||
                                                    state().prompt_input_active.load(std::memory_order_relaxed) ||
        game::owned_menu_countdown_capture(throwdowns::kit::match().input_lock,challenge_panel_owns_input()) ||
        GetTickCount64() < mode_input_paused_until.load(std::memory_order_relaxed) ||
        (mode_hidden_buttons.load(std::memory_order_relaxed) & mode_held_buttons.load(std::memory_order_relaxed)));
    SetLastError(error);
    return capture;
}

void note_input_record(const RAWINPUT& record) noexcept;
HeldInput& held_input() { static auto* value = new HeldInput; return *value; }

struct InputCaptureHooks {
    Hook hid_attributes, read_file, overlapped, overlapped_ex, completion, completion_ex, close_handle;
    struct HidRead { HANDLE file; void* buffer; DWORD capacity; PlayStationPad kind; };
    std::mutex hid_mutex;
    std::map<HANDLE, PlayStationPad> hid_devices;
    std::map<OVERLAPPED*, HidRead> hid_reads;
    Hook register_raw;
    Hook async_key, key, keyboard, raw_data, raw_buffer, capture, release_capture, cursor, show_cursor, physical_cursor, cursor_position;
    std::array<Hook, 5> xinput_state, xinput_extended, xinput_keystroke;
    // Hooks other programs put in front of those, hooked in turn (keep_xinput_capture_first).
    static constexpr std::size_t front_slots = 8;
    std::array<Hook, front_slots> xinput_front;
    std::mutex front_mutex;
    Hook direct_create;
    std::array<Hook, 2> device_create;
    // Mouse/keyboard and A/W devices may have distinct implementations. Slots
    // describe callable addresses, not the CreateDevice interface index.
    static constexpr std::size_t device_slots = 8;
    std::array<Hook, device_slots> device_state, device_data, device_format;
    struct Format { DWORD size{}; std::vector<DIOBJECTDATAFORMAT> objects; };
    std::mutex formats_mutex;
    std::map<void*, Format> formats;
};
InputCaptureHooks& input_hooks() { static auto* hooks = new InputCaptureHooks; return *hooks; }

BOOLEAN __stdcall captured_hid_attributes(HANDLE file, PHIDD_ATTRIBUTES attributes) {
    const auto result = original<decltype(&HidD_GetAttributes)>(input_hooks().hid_attributes)(file, attributes);
    const auto error = GetLastError();
    if (result && attributes) {
        const auto kind = playstation_pad(attributes->VendorID, attributes->ProductID);
        if (kind != PlayStationPad::none) {
            std::lock_guard lock(input_hooks().hid_mutex);
            input_hooks().hid_devices[file] = kind;
        }
    }
    SetLastError(error);
    return result;
}
std::atomic<dingosdk::overlay::GamepadFilter>& gamepad_filter() {
    static std::atomic<dingosdk::overlay::GamepadFilter> value{};
    return value;
}
void neutral_hid(const InputCaptureHooks::HidRead& read, DWORD size) {
    const bool capture=block_polled_input();
    const bool menu=suppress_challenge_menu();
    if ((!capture && !menu) || !read.buffer || size > read.capacity) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    // USB or Bluetooth decides where the buttons are in a report with id 1. It is the device's
    // report length that tells them apart, and Windows pads every read to that length, so it
    // is how many bytes were read: not how large a buffer the game happens to read into. A
    // game reading a USB DualSense into a larger buffer had its d-pad byte written as "up"
    // here, and placed a session marker whenever a menu took the input. (Nothing is asked of
    // the game's handle for this: 2.0.4 did, as the pad was first read.)
    const bool bluetooth = playstation_bluetooth(size);
    if (!parse_playstation_report(read.kind, bluetooth, bytes, size)) return;
    const bool sense_full = read.kind == PlayStationPad::dualsense &&
        (bytes[0] == 0x31 || (bytes[0] == 1 && !bluetooth));
    const unsigned offset = bytes[0] == 0x31 ? 2 : bytes[0] == 0x11 ? 3 : 1;
    auto* data = bytes + offset;
    if(!capture) {
        // Options is the normalized Start bit. Preserve D-pad, face buttons,
        // sticks and triggers, including the device's sequence counters.
        data[sense_full?8:5]&=std::uint8_t{0xdf};
    } else if (sense_full) {
        std::fill_n(data,4,std::uint8_t{128});
        data[4] = data[5] = 0; // triggers; retain report sequence at +6
        data[7] = 8; data[8] = data[9] = 0;
    } else {
        std::fill_n(data,4,std::uint8_t{128});
        data[4] = 8; data[5] = 0; data[6] &= 0xfc; // retain DS4 sequence bits
        data[7] = data[8] = 0;
    }
    refresh_playstation_bluetooth_crc(bytes, size);
    static std::atomic<bool> reported{};
    if (capture && !reported.exchange(true)) logging::printf(logging::Level::info, logging::Channel::input,
        "Native PlayStation HID report neutralized; overlay retains its independent input stream. (%s over %s, report 0x%02x of %lu bytes, "
        "the game's buffer %lu.)", read.kind == PlayStationPad::dualsense ? "DualSense" : "DualShock 4",
        bluetooth ? "Bluetooth" : "USB", bytes[0], size, read.capacity);
}
void dark_pop_hide_hat(std::uint8_t& byte) noexcept;
// Dark Pop: the hat nibble of a PlayStation report the game read, hidden the same way as the XInput D-pad.
void hide_hid_dpad(const InputCaptureHooks::HidRead& read, DWORD size) {
    if (overlay_input_access || !read.buffer || size > read.capacity || read.kind == PlayStationPad::none) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    const bool bluetooth = playstation_bluetooth(size);
    if (!parse_playstation_report(read.kind, bluetooth, bytes, size)) return;
    const bool sense_full = read.kind == PlayStationPad::dualsense && (bytes[0] == 0x31 || (bytes[0] == 1 && !bluetooth));
    const unsigned offset = bytes[0] == 0x31 ? 2 : bytes[0] == 0x11 ? 3 : 1;
    auto* data = bytes + offset;
    auto& hat = data[sense_full ? 7 : 4];
    const auto before = hat;
    dark_pop_hide_hat(hat);
    if (hat == before) return;
    refresh_playstation_bluetooth_crc(bytes, size);
}
// The gamepad filter (DingoSDKOverlaySetGamepadFilter) on the game's own native PlayStation
// reads, as filter_pad applies it to XInput: a DualShock 4 or DualSense read without Steam Input
// never goes through XInput. The report is parsed into XInput's layout, filtered, and what the
// filter changed (triggers, left stick X) is written back in the report's own layout.
inline constexpr unsigned hid_filter_slot = 3; // the filter's pad slot for native PlayStation pads
void filter_hid(const InputCaptureHooks::HidRead& read, DWORD size) noexcept {
    const auto filter = gamepad_filter().load(std::memory_order_acquire);
    if (!filter || !read.buffer || !size || size > read.capacity) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    const bool bluetooth = playstation_bluetooth(size);
    const auto parsed = parse_playstation_report(read.kind, bluetooth, bytes, size);
    if (!parsed) return;
    const dingosdk::overlay::GamepadValues before{parsed->buttons, parsed->left_trigger, parsed->right_trigger,
        parsed->left_x, parsed->left_y, parsed->right_x, parsed->right_y};
    auto pad = before;
    filter(hid_filter_slot, pad);
    if (pad.left_trigger == before.left_trigger && pad.right_trigger == before.right_trigger && pad.left_x == before.left_x) return;
    // Where parse_playstation_report found them: sticks (left X first), the shoulder byte (L2 0x04,
    // R2 0x08 digital bits) and the two analog triggers.
    std::uint8_t *sticks{}, *shoulder{}, *left{}, *right{};
    const auto id = bytes[0];
    if (read.kind == PlayStationPad::dualsense && ((id == 0x01 && !bluetooth) || id == 0x31)) {
        auto* data = bytes + (id == 0x31 ? 2 : 1);
        sticks = data; left = data + 4; right = data + 5; shoulder = data + 8;
    } else if (id == 0x01) {
        sticks = bytes + 1; shoulder = bytes + 6; left = bytes + 8; right = bytes + 9;
    } else if (read.kind == PlayStationPad::dualshock4 && id == 0x11) {
        sticks = bytes + 3; shoulder = bytes + 8; left = bytes + 10; right = bytes + 11;
    } else {
        return;
    }
    if (pad.left_trigger != before.left_trigger) {
        *left = pad.left_trigger;
        if (!pad.left_trigger) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x04u);
    }
    if (pad.right_trigger != before.right_trigger) {
        *right = pad.right_trigger;
        if (!pad.right_trigger) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x08u);
    }
    if (pad.left_x != before.left_x)
        sticks[0] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pad.left_x) / 258 + 128, 0, 255));
    refresh_playstation_bluetooth_crc(bytes, size);
}
// A finished native PlayStation read of the game's: neutral while a menu holds the input, else
// through the gamepad filter.
void handle_hid(const InputCaptureHooks::HidRead& read, DWORD size) noexcept {
    if (block_polled_input()) neutral_hid(read, size);
    else {
        filter_hid(read, size);
        int stick{};
        SHORT value{};
        if (!overlay_input_access && read.buffer && size <= read.capacity && held_stick(stick, value)) {
            auto* bytes = static_cast<std::uint8_t*>(read.buffer);
            if (set_playstation_stick(read.kind, playstation_bluetooth(size), bytes, size, stick, value)) {
                static std::atomic<bool> reported{};
                if (!reported.exchange(true)) logging::write(logging::Level::info, logging::Channel::input,
                    "Menu preview stick hold reached a native PlayStation HID read.");
            }
        }
    }
}
void complete_hid(OVERLAPPED* request, DWORD bytes, bool success, bool finished) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{};
    {
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_reads.find(request);
        if (found == h.hid_reads.end()) return;
        read = found->second;
        if (finished) h.hid_reads.erase(found);
    }
    if (success) handle_hid(read, bytes);
    if (success) hide_hid_dpad(read, bytes);
}
BOOL WINAPI captured_read_file(HANDLE file, LPVOID buffer, DWORD size, LPDWORD bytes, LPOVERLAPPED request) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{file, buffer, size, PlayStationPad::none};
    if (!overlay_input_access) {
        // Some readers inspect a temporary enumeration handle, then open a
        // second handle for reports. Identify that handle as well, by a read
        // the size of a Sony pad's reports: up to a DualShock 4's over
        // Bluetooth, which Windows pads to 547 bytes.
        bool known{};
        {
            std::lock_guard lock(h.hid_mutex);
            known = h.hid_devices.contains(file);
        }
        if (!known && size >= 10 && size <= dualshock4_bluetooth_read_length) {
            const auto error = GetLastError();
            HIDD_ATTRIBUTES attributes{sizeof(attributes)};
            const auto okay = original<decltype(&HidD_GetAttributes)>(h.hid_attributes)(file, &attributes);
            const auto kind = okay ? playstation_pad(attributes.VendorID, attributes.ProductID) : PlayStationPad::none;
            {
                std::lock_guard lock(h.hid_mutex);
                h.hid_devices[file] = kind;
            }
            SetLastError(error);
        }
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_devices.find(file);
        if (found != h.hid_devices.end() && found->second != PlayStationPad::none) {
            read.kind = found->second;
            if (request) h.hid_reads[request] = read;
        }
    }
    const auto result = original<decltype(&ReadFile)>(h.read_file)(file, buffer, size, bytes, request);
    const auto error = GetLastError();
    if (read.kind != PlayStationPad::none) {
        if (result) handle_hid(read, bytes ? *bytes : request ? static_cast<DWORD>(request->InternalHigh) : 0);
        if (!result && error != ERROR_IO_PENDING && request) complete_hid(request, 0, false, true);
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI captured_overlapped(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, BOOL wait) {
    const auto result = original<decltype(&GetOverlappedResult)>(input_hooks().overlapped)(file, request, bytes, wait);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE, result || error != ERROR_IO_INCOMPLETE);
    SetLastError(error); return result;
}
BOOL WINAPI captured_overlapped_ex(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetOverlappedResultEx)>(input_hooks().overlapped_ex)(file, request, bytes, timeout, alertable);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE,
        result || (error != ERROR_IO_INCOMPLETE && error != WAIT_TIMEOUT && error != WAIT_IO_COMPLETION));
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED* request, DWORD timeout) {
    const auto result = original<decltype(&GetQueuedCompletionStatus)>(input_hooks().completion)(port, bytes, key, request, timeout);
    const auto error = GetLastError();
    if (request && *request) complete_hid(*request, result && bytes ? *bytes : 0, result != FALSE, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion_ex(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG count, PULONG removed, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetQueuedCompletionStatusEx)>(input_hooks().completion_ex)(port, entries, count, removed, timeout, alertable);
    const auto error = GetLastError();
    if (result && entries && removed) for (ULONG i = 0; i < *removed; ++i)
        complete_hid(entries[i].lpOverlapped, entries[i].dwNumberOfBytesTransferred,
            static_cast<LONG>(entries[i].Internal) >= 0, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_close_handle(HANDLE file) {
    auto& h = input_hooks();
    {
        std::lock_guard lock(h.hid_mutex);
        if (h.hid_devices.erase(file))
            std::erase_if(h.hid_reads, [&](const auto& entry) { return entry.second.file == file; });
    }
    return original<decltype(&CloseHandle)>(h.close_handle)(file);
}

// Where the jump at an entry point goes, through any stubs on the way: the first address inside
// a module, and that module. Null when the entry point is not hooked (or cannot be read).
struct Landing { void* at{}; HMODULE module{}; };
Landing hook_landing(void* entry) {
    const auto self = GetCurrentProcess();
    const auto read = [&](const void* at, void* out, std::size_t size) {
        SIZE_T got{};
        return ReadProcessMemory(self, at, out, size, &got) && got == size;
    };
    auto* at = static_cast<std::uint8_t*>(entry);
    for (int hop = 0; hop < 8; ++hop) {
        std::uint8_t code[16]{};
        if (!read(at, code, sizeof(code))) return {};
        std::uint8_t* next{};
        if (code[0] == 0xe9) {
            std::int32_t offset; std::memcpy(&offset, code + 1, 4);
            next = at + 5 + offset;
        } else if (code[0] == 0xff && code[1] == 0x25) {
            std::int32_t offset; std::memcpy(&offset, code + 2, 4);
            if (!read(at + 6 + offset, &next, sizeof(next))) return {};
        } else if (code[0] == 0x48 && code[1] == 0xb8 && code[10] == 0xff && code[11] == 0xe0) {
            std::memcpy(&next, code + 2, 8);
        } else return {};
        at = next;
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(at), &module) && module) return {at, module};
    }
    return {};
}
HMODULE module_of(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}
std::string module_name(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text += static_cast<char>(*name);
    return text;
}
std::string xinput_entry_owners() {
    std::string text;
    const auto& h = input_hooks();
    const auto owner = [](void* entry) {
        const auto landing = hook_landing(entry);
        return landing.module ? module_name(landing.module) : std::string("not hooked");
    };
    constexpr std::array names{"xinput1_4", "xinput1_3", "xinput9_1_0", "xinput1_2", "xinput1_1"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (h.xinput_state[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " " + owner(h.xinput_state[i].target);
        if (h.xinput_extended[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " (extended) " + owner(h.xinput_extended[i].target);
    }
    return text.empty() ? "none" : text;
}

// The game's own read only: not ReSkate's (overlay_input_access) and not an inner layer of a
// read already being answered (Steam's hook going on to XInput runs inside OverlayInputAccess).
void filter_pad(bool outer, DWORD user, XINPUT_STATE* output) noexcept {
    if (!outer || !output) return;
    const auto filter = gamepad_filter().load(std::memory_order_acquire);
    if (!filter) return;
    auto& g = output->Gamepad;
    dingosdk::overlay::GamepadValues pad{g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
    filter(static_cast<unsigned>(user), pad);
    g.wButtons = pad.buttons;
    g.bLeftTrigger = pad.left_trigger;
    g.bRightTrigger = pad.right_trigger;
    g.sThumbLX = pad.left_x;
    g.sThumbLY = pad.left_y;
    g.sThumbRX = pad.right_x;
    g.sThumbRY = pad.right_y;
}
// Another program's hook on XInput that was put in front of the capture's own after it was
// installed (Steam's overlay does, for Steam Input, and answers the game with its own pad
// without going on to XInput): the capture then never sees the game's reads, and a menu that
// holds the game's input does not hold its controller. That hook's own function is hooked
// then, so the capture is in front of it again.
void game_pad_read(DWORD user, XINPUT_STATE* output) noexcept;
void note_xinput(DWORD user, const XINPUT_STATE& state) noexcept;
template<std::size_t I>
DWORD WINAPI captured_front(DWORD user, XINPUT_STATE* output) {
    const bool capture = block_polled_input();
    const bool menu=suppress_challenge_menu();
    const bool outer = overlay_input_access == 0;
    note_prompt_read(prompt_xinput);
    DWORD result;
    { // It may go on to XInput's own entry point, which is hooked as well.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(input_hooks().xinput_front[I])(user, output);
    }
    if (result == ERROR_SUCCESS && output) note_xinput(user, *output);
    // Only the game gets a scripted pad (the character room's walk): ReSkate's own reads get the real one.
    if (outer && output && scripted_pad(user, *output)) return ERROR_SUCCESS;
    if (result == ERROR_SUCCESS && output && capture) {
        output->Gamepad = {};
        output->dwPacketNumber ^= 0x80000000u;
    } else if (result == ERROR_SUCCESS && output) {
        if (outer) game_pad_read(user, output);
        if (menu) {
            output->Gamepad.wButtons=game::owned_menu_buttons(output->Gamepad.wButtons,true);
            output->dwPacketNumber ^= 0x40000000u;
        }
    }
    return result;
}
void keep_xinput_capture_first() {
    auto& h = input_hooks();
    static const HMODULE self = module_of(&keep_xinput_capture_first);
    const auto detours = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_front<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::front_slots>{});
    std::lock_guard lock(h.front_mutex);
    const auto guard = [&](void* entry) {
        if (!entry) return;
        const auto landing = hook_landing(entry);
        // Not hooked over, the capture's own, or a jump inside XInput itself.
        if (!landing.at || landing.module == self || landing.module == module_of(entry)) return;
        for (const auto& hook : h.xinput_front) if (hook.target == landing.at) return;
        for (std::size_t slot = 0; slot < h.xinput_front.size(); ++slot) {
            if (h.xinput_front[slot].target) continue;
            const bool okay = install(h.xinput_front[slot], landing.at, detours[slot]);
            logging::printf(okay ? logging::Level::info : logging::Level::warning, logging::Channel::input,
                okay ? "XInput is answered by %s ahead of ReSkate: its reads are now held with the rest of the game's input."
                     : "XInput is answered by %s ahead of ReSkate, and that could not be hooked: a controller may reach the game behind a menu.",
                module_name(landing.module).c_str());
            return;
        }
    };
    for (std::size_t i = 0; i < h.xinput_state.size(); ++i) guard(h.xinput_state[i].target), guard(h.xinput_extended[i].target);
}
SHORT WINAPI captured_async_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().async_key)(key);
    return block_polled_input() || challenge_menu_key(key) || key_hidden(static_cast<unsigned>(key)) ? 0 : value;
}
SHORT WINAPI captured_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().key)(key);
    return block_polled_input() || challenge_menu_key(key) || key_hidden(static_cast<unsigned>(key)) ? 0 : value;
}
BOOL WINAPI captured_keyboard(PBYTE keys) {
    const auto result = original<BOOL(WINAPI*)(PBYTE)>(input_hooks().keyboard)(keys);
    if (result && keys && block_polled_input()) std::memset(keys, 0, 256);
    else if(result && keys && challenge_menu_key(VK_ESCAPE))keys[VK_ESCAPE]=0;
    if (result && keys)
        for (unsigned key = 0; key < 256; ++key)
            if (key_hidden(key)) keys[key] = 0;
    return result;
}
UINT WINAPI captured_raw_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const auto result = original<UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT)>(
        input_hooks().raw_data)(input, command, data, size, header_size);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access && input != noted_raw_input &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) &&
        static_cast<RAWINPUT*>(data)->header.dwType == RIM_TYPEMOUSE)
        note_raw_mouse_packet(static_cast<RAWINPUT*>(data)->data.mouse);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
        note_input_record(*static_cast<RAWINPUT*>(data));
    if (result != UINT(-1) && data && command == RID_INPUT && result >= sizeof(RAWINPUTHEADER)) {
        auto* raw = static_cast<RAWINPUT*>(data);
        const bool mouse = raw->header.dwType == RIM_TYPEMOUSE && result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
        const bool keyboard =
            raw->header.dwType == RIM_TYPEKEYBOARD && result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
        if(keyboard && (challenge_menu_key(raw->data.keyboard.VKey) || hidden_press(raw->data.keyboard))) {
            raw->data.keyboard={};raw->data.keyboard.VKey=255;
            raw->data.keyboard.Flags=RI_KEY_BREAK;raw->data.keyboard.Message=WM_KEYUP;
        } else if (!block_polled_input()) {
            if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*raw);
        } else if ((mouse || keyboard) && held_input().release_only(*raw)) {
            // A key or button the game holds still comes up; presses stay blocked.
        } else if (mouse)
            raw->data.mouse = {};
        else if (keyboard) {
            raw->data.keyboard = {};
            raw->data.keyboard.VKey = 255; // documented discard packet; never synthesize a key press
            raw->data.keyboard.Flags = RI_KEY_BREAK;
            raw->data.keyboard.Message = WM_KEYUP;
        } else if (raw->header.dwType == RIM_TYPEHID && result >= offsetof(RAWINPUT, data) + 8)
            raw->data.hid.dwCount = 0;
    }
    return result;
}
// The latest keyboard and mouse input the game read, with the device it came from, for the
// log line when the game switches to mouse and keyboard: which input made it switch. Raw
// input injected by software (SendInput: Steam Input's desktop layout, macro tools) carries
// no device handle.
struct LastInput {
    std::atomic<ULONGLONG> key_at{}, mouse_at{}, message_at{};
    std::atomic<std::uint32_t> key{}; // virtual key | make code << 16
    std::atomic<std::int32_t> mouse_x{}, mouse_y{};
    std::atomic<std::uint32_t> mouse_buttons{};
    std::atomic<HANDLE> key_device{}, mouse_device{};
    std::atomic<std::uint32_t> message{}, message_key{}; // the game window's last input message
};
LastInput& last_input() { static auto* value = new LastInput; return *value; }
std::atomic<ULONGLONG> controls_at{};
namespace {
struct PadSeen {
    bool known{};
    std::uint32_t buttons{};
    std::array<float, 6> axes{};
};
}
void note_controls() noexcept { controls_at.store(GetTickCount64(), std::memory_order_relaxed); }
// A controller counts as worked when a button changes, or a stick or trigger moves an eighth of
// its travel from where it was last worked: one left resting off centre, or a little noise, does not.
void note_pad(std::size_t slot, std::uint32_t buttons, const std::array<float, 6>& axes) noexcept {
    static std::mutex mutex;
    static std::array<PadSeen, 6> seen{};
    if (slot >= seen.size()) return;
    bool moved{};
    {
        std::lock_guard lock(mutex);
        auto& was = seen[slot];
        moved = was.known && was.buttons != buttons;
        for (std::size_t i = 0; i < axes.size() && was.known; ++i) moved = moved || std::abs(axes[i] - was.axes[i]) > 0.125f;
        if (!was.known || moved) was = {true, buttons, axes};
    }
    if (moved) note_controls();
}
void note_xinput(DWORD user, const XINPUT_STATE& state) noexcept {
    const auto& g = state.Gamepad;
    const auto stick = [](SHORT v) { return static_cast<float>(v) / 32768.0f; };
    note_pad(user, g.wButtons, {stick(g.sThumbLX), stick(g.sThumbLY), stick(g.sThumbRX), stick(g.sThumbRY),
                                g.bLeftTrigger / 255.0f, g.bRightTrigger / 255.0f});
}
void note_input_record(const RAWINPUT& record) noexcept {
    auto& last = last_input();
    if (record.header.dwType == RIM_TYPEKEYBOARD) {
        note_controls();
        last.key.store(record.data.keyboard.VKey | (std::uint32_t{record.data.keyboard.MakeCode} << 16),
                       std::memory_order_relaxed);
        last.key_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.key_at.store(GetTickCount64(), std::memory_order_relaxed);
    } else if (record.header.dwType == RIM_TYPEMOUSE &&
               (record.data.mouse.lLastX || record.data.mouse.lLastY || record.data.mouse.usButtonFlags)) {
        last.mouse_x.store(record.data.mouse.lLastX, std::memory_order_relaxed);
        last.mouse_y.store(record.data.mouse.lLastY, std::memory_order_relaxed);
        last.mouse_buttons.store(record.data.mouse.usButtonFlags, std::memory_order_relaxed);
        last.mouse_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.mouse_at.store(GetTickCount64(), std::memory_order_relaxed);
        note_controls();
        if ((record.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) &&
            GetTickCount64() < mode_input_paused_until.load(std::memory_order_relaxed))
            mode_mouse_wheel.fetch_add(static_cast<SHORT>(record.data.mouse.usButtonData) / WHEEL_DELTA, std::memory_order_relaxed);
    }
}
std::string input_device_name(HANDLE device) {
    if (!device) return "no device (injected by software)";
    std::array<wchar_t, 256> name{};
    UINT size = static_cast<UINT>(name.size());
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == UINT(-1)) return "an unknown device";
    std::string text;
    for (const wchar_t* c = name.data(); *c && text.size() < 120; ++c) text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return text;
}
void note_input_message(UINT message, WPARAM wp) noexcept {
    auto& last = last_input();
    last.message.store(message, std::memory_order_relaxed);
    last.message_key.store(static_cast<std::uint32_t>(wp), std::memory_order_relaxed);
    last.message_at.store(GetTickCount64(), std::memory_order_relaxed);
    // (Windows sends a mouse move message without the mouse moving, as a window comes or goes under it.)
    if (message != WM_MOUSEMOVE) note_controls();
}
// What the game last read from the keyboard and mouse, within the 3 s before now.
std::string recent_input() {
    auto& last = last_input();
    const auto now = GetTickCount64();
    std::string text;
    const auto age = [&](ULONGLONG at) { return std::to_string(now - at) + " ms before"; };
    if (const auto at = last.key_at.load(); at && now - at <= 3000) {
        const auto key = last.key.load();
        text += std::format("key 0x{:02x} (scan 0x{:02x}) from {}, {}", key & 0xffff, key >> 16,
                            input_device_name(last.key_device.load()), age(at));
    }
    if (const auto at = last.mouse_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("mouse move ({}, {}) buttons 0x{:x} from {}, {}", last.mouse_x.load(), last.mouse_y.load(),
                            last.mouse_buttons.load(), input_device_name(last.mouse_device.load()), age(at));
    }
    if (const auto at = last.message_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("window message 0x{:x} ({:#x}), {}", last.message.load(), last.message_key.load(), age(at));
    }
    return text.empty() ? "no keyboard or mouse input in the 3 s before" : text;
}
UINT WINAPI captured_raw_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    // Calling the API drains queued input. While the overlay owns input the game gets only the
    // releases of keys and buttons it holds (HeldInput), so nothing pressed in the menu or chat
    // replays when it closes and nothing pressed before it stays stuck.
    const auto result = original<UINT(WINAPI*)(PRAWINPUT, PUINT, UINT)>(
        input_hooks().raw_buffer)(data, size, header_size);
    const bool capture = result != UINT(-1) && data && block_polled_input();
    note_prompt_read(prompt_raw);
    if (result != UINT(-1) && data && header_size == sizeof(RAWINPUTHEADER)) {
        // Skate drains its mouse here; read the packets on the way past.
        // NEXTRAWINPUTBLOCK: records are 8-byte aligned on x64.
        const auto align = [](std::uintptr_t at) { return (at + 7) & ~std::uintptr_t{7}; };
        auto* record = data;
        auto write = reinterpret_cast<std::uintptr_t>(data);
        UINT kept{};
        for (UINT i = 0; i < result && record; ++i) {
            const auto bytes = record->header.dwSize;
            const auto next = align(reinterpret_cast<std::uintptr_t>(record) + bytes);
            if (record->header.dwType == RIM_TYPEMOUSE) note_raw_mouse_packet(record->data.mouse);
            note_input_record(*record);
            const bool mouse = record->header.dwType == RIM_TYPEMOUSE && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
            const bool keyboard =
                record->header.dwType == RIM_TYPEKEYBOARD && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
            if(keyboard && (challenge_menu_key(record->data.keyboard.VKey) || hidden_press(record->data.keyboard))) {
                record->data.keyboard={};record->data.keyboard.VKey=255;
                record->data.keyboard.Flags=RI_KEY_BREAK;record->data.keyboard.Message=WM_KEYUP;
            }
            if (!capture) {
                if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*record);
            } else if ((mouse || keyboard) && held_input().release_only(*record)) {
                // Kept records move forward over the dropped ones, in the same layout.
                if (write != reinterpret_cast<std::uintptr_t>(record))
                    std::memmove(reinterpret_cast<void*>(write), record, bytes);
                write = align(write + bytes);
                ++kept;
            }
            record = reinterpret_cast<RAWINPUT*>(next);
        }
        if (capture) return kept;
    }
    return capture ? 0 : result;
}
// The game's latest mouse registration, and whether the overlay has replaced
// it with a cursor-moving one.
struct RawMouse {
    std::mutex mutex;
    RAWINPUTDEVICE game{};
    bool known{}, overridden{};
};
RawMouse& raw_mouse() { static auto* value = new RawMouse; return *value; }
constexpr DWORD cursor_blocking_flags = RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE | RIDEV_NOHOTKEYS;
bool is_mouse(const RAWINPUTDEVICE& device) { return device.usUsagePage == 1 && device.usUsage == 2; }

// Logs how the game registers raw input (flags change with its input mode),
// once per distinct registration.
BOOL WINAPI captured_register_raw(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    BOOL result{};
    if (devices && count && size == sizeof(RAWINPUTDEVICE) && count <= 64) {
        std::vector<RAWINPUTDEVICE> requested(devices, devices + count);
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        for (auto& device : requested) {
            if (!is_mouse(device)) continue;
            if (device.dwFlags & RIDEV_REMOVE) mouse.known = false;
            else { mouse.game = device; mouse.known = true; }
            // While the overlay owns the pointer the cursor must keep moving.
            if (mouse.overridden && !(device.dwFlags & RIDEV_REMOVE)) device.dwFlags &= ~cursor_blocking_flags;
        }
        result = register_raw(requested.data(), count, size);
        if (result)
            for (const auto& device : requested)
                if (is_mouse(device))
                    state().mouse_legacy_blocked.store(!(device.dwFlags & RIDEV_REMOVE) &&
                        (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
        // Skate re-registers the mouse whenever it switches between controller and
        // mouse/keyboard: the one trace of which input it thinks the player uses.
        static DWORD last_mode = ~0ul;
        for (const auto& device : devices ? std::span(devices, count) : std::span<const RAWINPUTDEVICE>{})
            if (is_mouse(device) && device.dwFlags != last_mode) {
                last_mode = device.dwFlags;
                // Leaving controller mode: say what input came just before.
                const auto cause = device.dwFlags & RIDEV_NOLEGACY ? std::string{} : "; last input: " + recent_input();
                dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
                    "Game input mode: mouse registered with flags 0x%lx (%s)%s%s.", device.dwFlags,
                    device.dwFlags & RIDEV_REMOVE ? "removed"
                    : (device.dwFlags & RIDEV_NOLEGACY) ? "controller: no Windows mouse messages" : "mouse and keyboard",
                    mouse.overridden ? "; the overlay owns the pointer" : "", cause.c_str());
            }
    } else {
        result = register_raw(devices, count, size);
    }
    try {
        static std::mutex mutex;
        static std::set<std::tuple<USHORT, USHORT, DWORD>> seen;
        for (UINT i = 0; devices && size == sizeof(RAWINPUTDEVICE) && i < count; ++i) {
            const auto& device = devices[i];
            {
                std::lock_guard lock(mutex);
                if (!seen.emplace(device.usUsagePage, device.usUsage, device.dwFlags).second) continue;
            }
            dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::input,
                "Game raw input registration: page=%u usage=%u flags=0x%lx window=%p result=%d",
                device.usUsagePage, device.usUsage, device.dwFlags, device.hwndTarget, result ? 1 : 0);
        }
    } catch (...) {}
    return result;
}
void apply_raw_mouse(RawMouse& mouse, bool overlay) {
    auto device = mouse.game;
    if (overlay) device.dwFlags &= ~cursor_blocking_flags;
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    OverlayInputAccess access;
    const bool applied = register_raw ? register_raw(&device, 1, sizeof(device)) != FALSE
                                      : RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    if (applied) state().mouse_legacy_blocked.store((device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
        "Raw mouse registration %s (flags=0x%lx, %s, Windows error %lu).",
        overlay ? "released for the overlay" : "returned to the game", device.dwFlags, applied ? "ok" : "failed",
        applied ? 0ul : GetLastError());
}
HWND WINAPI captured_capture(HWND window) {
    if (block_polled_input() || (overlay_input_access &&
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())) return GetCapture();
    return original<HWND(WINAPI*)(HWND)>(input_hooks().capture)(window);
}
BOOL WINAPI captured_release_capture() {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)()>(input_hooks().release_capture)();
}
HCURSOR WINAPI captured_cursor(HCURSOR cursor) {
    if (block_polled_input()) return GetCursor();
    return original<HCURSOR(WINAPI*)(HCURSOR)>(input_hooks().cursor)(cursor);
}
int WINAPI captured_show_cursor(BOOL show) {
    // ImGui draws the menu cursor. Do not let the game change the OS display
    // counter while it is open; the counter resumes unchanged on close.
    if (block_polled_input()) return show ? 0 : -1;
    return original<int(WINAPI*)(BOOL)>(input_hooks().show_cursor)(show);
}
BOOL WINAPI captured_physical_cursor(int x, int y) {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)(int, int)>(input_hooks().physical_cursor)(x, y);
}
BOOL WINAPI captured_cursor_position(LPPOINT output) {
    thread_local POINT last{};
    thread_local bool sampled = false;
    if (output && block_polled_input() && sampled) { *output = last; return TRUE; }
    const auto result = original<BOOL(WINAPI*)(LPPOINT)>(input_hooks().cursor_position)(output);
    if (result && output && !overlay_input_access) { last = *output; sampled = true; }
    return result;
}
// ---- Dark Pop: the D-pad hidden from the game in the air -----------------------------------------------------
// skate. opens the Quick Drop radial on D-pad Right (and Down, held), and in keyboard/mouse mode letting the radial
// go selects what the cursor sits on: the object browser. Dark Pop arms on a D-pad press in the air, so while the
// local skater is in the air with Dark Pop on (a lease the physics selector hook renews every call, see
// DingoSDKOverlayDarkPopAir) the game's own polls see D-pad Left, Down and Right let go, and a press still held at
// the landing stays hidden until it is really let go, so its release is never a tap. On the ground the D-pad is the
// game's. ReSkate's own readers (overlay_input_access), which Dark Pop reads through, always get the real pad.
struct DarkPopHide {
    std::atomic<std::uint64_t> until{};   // GetTickCount64 the air lease runs to; 0 = no lease
    std::atomic<bool> latched{};          // a hidden press is still held past the lease
    std::atomic<std::uint64_t> hidden{};  // game polls changed so far (for the log)
};
DarkPopHide& dark_pop_hide() { static auto* value = new DarkPopHide; return *value; }
constexpr std::uint64_t dark_pop_air_lease = 120; // ms; the selector hook runs every physics step
constexpr WORD dark_pop_xinput_dpad = 0x0004 | 0x0002 | 0x0008; // XINPUT_GAMEPAD_DPAD_LEFT | DOWN | RIGHT
// Whether the D-pad (Left, Down or Right, `down` = one of them is pressed in this poll) is hidden from this poll.
bool dark_pop_hiding(bool down) noexcept {
    auto& h = dark_pop_hide();
    if (GetTickCount64() < h.until.load(std::memory_order_relaxed)) {
        h.latched.store(down, std::memory_order_relaxed); // a press let go in the air leaves nothing to carry past the landing
        return down;
    }
    if (!h.latched.load(std::memory_order_relaxed)) return false;
    if (!down) { h.latched.store(false, std::memory_order_relaxed); return false; }
    return true;
}
// A PlayStation report's hat nibble: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left,
// 8 released. Left, Down and Right (and their diagonals) are hidden; Up stays.
void dark_pop_hide_hat(std::uint8_t& byte) noexcept {
    const unsigned hat = byte & 0x0f;
    if (!dark_pop_hiding(hat >= 1 && hat <= 7)) return;
    const unsigned shown = (hat == 1 || hat == 7) ? 0u : 8u;
    byte = static_cast<std::uint8_t>((byte & 0xf0) | shown);
    dark_pop_hide().hidden.fetch_add(1, std::memory_order_relaxed);
}
// ---- The sticks on a grind (the trainer's "Grinds feel like Skate 3" and its lean into a ramp) -----------------
// Both rewrites below only last while the trainer keeps asking for them: each `on` is good for half a second, so a
// trainer that has stopped ticking cannot leave the game's own polls rewritten.
constexpr std::uint64_t stick_lease = 500;
// How many of the game's polls were given a made-up right stick so far.
std::atomic<std::uint64_t>& grind_stick_mirrored() { static std::atomic<std::uint64_t> value{0}; return value; }
// Skate 3's turn on a grind: while on, the game's own polls see a right stick whose sideways part is the trainer's (`x`,
// from the left stick) and whose up and down is the player's own, times `y_scale`, plus `y_add`. For 150 ms after the
// player's right stick jumps (the flick of a pop) the game sees the stick as it is. `sent_*` is what the game last saw.
struct GrindStickTurn {
    std::atomic<bool> on{};
    std::atomic<std::uint64_t> until{};
    std::atomic<float> x{}, y_add{}, y_scale{1.0f};
    std::atomic<float> sent_x{}, sent_y{}, raw_y{};
    std::atomic<int> last_ry{0};
    std::atomic<std::uint64_t> flicked_at{0};
};
GrindStickTurn& grind_stick_turn() { static auto* value = new GrindStickTurn; return *value; }
bool turn_grind_stick(XINPUT_GAMEPAD& pad) noexcept {
    auto& turn = grind_stick_turn();
    const auto now = GetTickCount64();
    if (!turn.on.load(std::memory_order_relaxed) || now >= turn.until.load(std::memory_order_relaxed)) return false;
    const int ry = pad.sThumbRY;
    if (std::abs(ry - turn.last_ry.exchange(ry, std::memory_order_relaxed)) > 5000) turn.flicked_at.store(now, std::memory_order_relaxed);
    const float raw_y = static_cast<float>(ry) / 32767.0f;
    turn.raw_y.store(raw_y, std::memory_order_relaxed);
    if (now - turn.flicked_at.load(std::memory_order_relaxed) >= 150) {
        // The game flips the stick's sideways sign while the stick is held down (the nose is to point where the stick
        // does): flipped here first, so the left stick turns the board the same way in a 5-0 as in a nose grind.
        const float x = std::clamp(turn.x.load(std::memory_order_relaxed), -1.0f, 1.0f) * (raw_y < -0.2f ? -1.0f : 1.0f);
        const float y = std::clamp(raw_y * turn.y_scale.load(std::memory_order_relaxed) + turn.y_add.load(std::memory_order_relaxed), -1.0f, 1.0f);
        pad.sThumbRX = static_cast<SHORT>(x * 32767.0f);
        pad.sThumbRY = static_cast<SHORT>(y * 32767.0f);
        grind_stick_mirrored().fetch_add(1, std::memory_order_relaxed);
    }
    turn.sent_x.store(static_cast<float>(pad.sThumbRX) / 32767.0f, std::memory_order_relaxed);
    turn.sent_y.store(static_cast<float>(pad.sThumbRY) / 32767.0f, std::memory_order_relaxed);
    return true;
}
// Leaning into the ramp on a coping grind: while on, the game's own polls see the left stick held at (x, y) for as long
// as the player's own left stick is at rest (a stick the player moves is theirs). `raw_*` is the player's own stick as
// the last poll had it, for the trainer's own reads of the left stick.
struct GrindStickLean {
    std::atomic<bool> on{};
    std::atomic<std::uint64_t> until{};
    std::atomic<float> x{}, y{}, raw_x{}, raw_y{};
    std::atomic<std::uint64_t> given{0};
};
GrindStickLean& grind_stick_lean() { static auto* value = new GrindStickLean; return *value; }
void lean_grind_stick(XINPUT_GAMEPAD& pad) noexcept {
    auto& lean = grind_stick_lean();
    if (!lean.on.load(std::memory_order_relaxed) || GetTickCount64() >= lean.until.load(std::memory_order_relaxed)) return;
    lean.raw_x.store(static_cast<float>(pad.sThumbLX) / 32767.0f, std::memory_order_relaxed);
    lean.raw_y.store(static_cast<float>(pad.sThumbLY) / 32767.0f, std::memory_order_relaxed);
    constexpr int idle = 8000; // about a quarter of the stick
    if (std::abs(int{pad.sThumbLX}) > idle || std::abs(int{pad.sThumbLY}) > idle) return;
    pad.sThumbLX = static_cast<SHORT>(std::clamp(lean.x.load(std::memory_order_relaxed), -1.0f, 1.0f) * 32767.0f);
    pad.sThumbLY = static_cast<SHORT>(std::clamp(lean.y.load(std::memory_order_relaxed), -1.0f, 1.0f) * 32767.0f);
    lean.given.fetch_add(1, std::memory_order_relaxed);
}
// The game's own read of a controller (not ReSkate's and not an inner layer of one): ReSkate's gamepad filter, then
// Dark Pop's D-pad hiding in the air and the grind sticks. From captured_xinput, and from captured_front when
// another program's XInput hook (Steam's overlay) answers the game first.
void game_pad_read(DWORD user, XINPUT_STATE* output) noexcept {
    filter_pad(true, user, output);
    if (dark_pop_hiding((output->Gamepad.wButtons & dark_pop_xinput_dpad) != 0)) {
        output->Gamepad.wButtons = static_cast<WORD>(output->Gamepad.wButtons & ~dark_pop_xinput_dpad);
        dark_pop_hide().hidden.fetch_add(1, std::memory_order_relaxed);
    }
    (void)turn_grind_stick(output->Gamepad);
    lean_grind_stick(output->Gamepad);
}
template<std::size_t I, bool Extended = false>
DWORD WINAPI captured_xinput(DWORD user, XINPUT_STATE* output) {
    const auto& hook = Extended ? input_hooks().xinput_extended[I] : input_hooks().xinput_state[I];
    const bool game_poll = !overlay_input_access;
    const bool capture = block_polled_input();
    note_prompt_read(prompt_xinput);
    const auto hidden = overlay_input_access ? std::uint16_t{} : static_cast<std::uint16_t>(
        mode_hidden_buttons.load(std::memory_order_relaxed) |
        (suppress_challenge_menu()?XINPUT_GAMEPAD_START:0));
    DWORD result;
    { // XInput exports can forward through another hooked XInput export.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(hook)(user, output);
    }
    if (result == ERROR_SUCCESS && output) note_xinput(user, *output);
    // Only the game gets a scripted pad (the character room's walk): ReSkate's own reads get the real one.
    if (game_poll && output && scripted_pad(user, *output)) return ERROR_SUCCESS;
    if (result == ERROR_SUCCESS && output && capture) {
        output->Gamepad = {};
        // The game must notice a neutral state even if the physical packet did
        // not change between opening the menu and its next controller poll.
        output->dwPacketNumber ^= 0x80000000u;
    } else if (result == ERROR_SUCCESS && output) {
        if (game_poll) game_pad_read(user, output);
        if (hidden) {
            output->Gamepad.wButtons &= static_cast<WORD>(~hidden);
            output->dwPacketNumber ^= 0x40000000u;
        }
    }
    return result;
}
template<std::size_t I>
DWORD WINAPI captured_keystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE output) {
    const auto result = original<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE)>(
        input_hooks().xinput_keystroke[I])(user, reserved, output);
    if (result == ERROR_SUCCESS && output && block_polled_input()) { *output = {}; return ERROR_EMPTY; }
    if(result==ERROR_SUCCESS && output && suppress_challenge_menu() && output->VirtualKey==VK_PAD_START) {
        *output={};return ERROR_EMPTY;
    }
    // VK_PAD_DPAD_DOWN 0x5811, LEFT 0x5812, RIGHT 0x5813 (UP 0x5810 stays), while Dark Pop hides the D-pad.
    if (result == ERROR_SUCCESS && output && !overlay_input_access && output->VirtualKey >= 0x5811 && output->VirtualKey <= 0x5813 &&
        dark_pop_hiding((output->Flags & XINPUT_KEYSTROKE_KEYUP) == 0)) { *output = {}; return ERROR_EMPTY; }
    return result;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_format(void* device, const DIDATAFORMAT* format) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, const DIDATAFORMAT*)>(
        input_hooks().device_format[I])(device, format);
    if (SUCCEEDED(result) && format && format->dwSize == sizeof(DIDATAFORMAT) &&
        format->dwObjSize == sizeof(DIOBJECTDATAFORMAT) && format->dwNumObjs <= 1024 &&
        (!format->dwNumObjs || format->rgodf)) {
        InputCaptureHooks::Format copy;
        copy.size = format->dwDataSize;
        for (DWORD i = 0; i < format->dwNumObjs; ++i) copy.objects.push_back(format->rgodf[i]);
        std::lock_guard lock(input_hooks().formats_mutex);
        input_hooks().formats[device] = std::move(copy);
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_state(void* device, DWORD size, LPVOID data) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID)>(
        input_hooks().device_state[I])(device, size, data);
    note_prompt_read(prompt_direct);
    if (SUCCEEDED(result) && data && block_polled_input()) {
        std::memset(data, 0, size);
        std::lock_guard lock(input_hooks().formats_mutex);
        const auto found = input_hooks().formats.find(device);
        if (found != input_hooks().formats.end() && found->second.size == size) {
            for (const auto& object : found->second.objects) {
                if (object.dwOfs > size || size - object.dwOfs < 4) continue;
                DWORD neutral = 0;
                if (object.dwType & DIDFT_POV) neutral = 0xffffffffu;
                else if (object.dwType & DIDFT_ABSAXIS) {
                    neutral = 32767; // Standard DirectInput axis midpoint if range lookup fails.
                    DIPROPRANGE range{};
                    range.diph = {sizeof(range), sizeof(range.diph), object.dwOfs, DIPH_BYOFFSET};
                    auto** vtable = *reinterpret_cast<void***>(device);
                    using GetProperty = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPDIPROPHEADER);
                    if (SUCCEEDED(reinterpret_cast<GetProperty>(vtable[5])(device, DIPROP_RANGE, &range.diph)))
                        neutral = static_cast<DWORD>((static_cast<std::int64_t>(range.lMin) + range.lMax) / 2);
                } else continue;
                std::memcpy(static_cast<std::byte*>(data) + object.dwOfs, &neutral, 4);
            }
        } else if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
            // A zeroed DirectInput axis is full travel, not the center. Some
            // controllers use the standard joystick layout without a format
            // record we can inspect; keep those pads neutral while captured.
            auto* bytes = static_cast<std::byte*>(data);
            const DWORD center = 32767;
            const DWORD released_pov = 0xffffffffu;
            for (std::size_t offset = 0; offset < 8 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &center, sizeof(center));
            for (std::size_t offset = 8 * sizeof(DWORD); offset < 12 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &released_pov, sizeof(released_pov));
        }
    } else if (SUCCEEDED(result) && data && size == 256) {
        if (challenge_menu_key(VK_ESCAPE)) static_cast<std::uint8_t*>(data)[DIK_ESCAPE] = 0;
        for (DWORD scan = 1; scan < 256; ++scan)
            if (scan_hidden(scan)) static_cast<std::uint8_t*>(data)[scan] = 0;
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_data(void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const bool capture = block_polled_input();
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD)>(
        input_hooks().device_data[I])(device, size, data, count, capture ? flags & ~DIGDD_PEEK : flags);
    if (SUCCEEDED(result) && capture && count) *count = 0;
    else if (SUCCEEDED(result) && data && count && size == sizeof(DIDEVICEOBJECTDATA)) {
        std::lock_guard lock(input_hooks().formats_mutex);
        const auto format = input_hooks().formats.find(device);
        if (format != input_hooks().formats.end() && format->second.size == 256) {
            const bool escape = challenge_menu_key(VK_ESCAPE);
            DWORD kept{};
            for (DWORD i = 0; i < *count; ++i)
                if (!(escape && data[i].dwOfs == DIK_ESCAPE) && !((data[i].dwData & 0x80) && scan_hidden(data[i].dwOfs))) data[kept++] = data[i];
            *count = kept;
        }
    }
    return result;
}
bool install_device_capture(void* device) {
    auto** table = *reinterpret_cast<void***>(device);
    auto& hooks = input_hooks();
    const auto states = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_state<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto data = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_data<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto formats = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_format<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto add = [&](auto& family, void* target, const auto& detours) {
        for (const auto& hook : family) if (hook.target == target) return true;
        for (std::size_t slot = 0; slot < family.size(); ++slot)
            if (!family[slot].target) return install(family[slot], target, detours[slot]);
        return false;
    };
    // Keep installing independent methods if one fails. Report the exact gap.
    const bool state_ready = add(hooks.device_state, table[9], states);
    const bool data_ready = add(hooks.device_data, table[10], data);
    const bool format_ready = add(hooks.device_format, table[11], formats);
    if (!state_ready || !data_ready || !format_ready)
        logging::log(logging::Level::warning, logging::Channel::input,
            "DirectInput capture incomplete: state={}, buffered={}, format={}; mouse input may reach the game while the menu is open.",
            state_ready, data_ready, format_ready);
    std::lock_guard formats_lock(hooks.formats_mutex);
    hooks.formats.erase(device); // COM addresses can be reused after Release.
    return state_ready && data_ready && format_ready;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_create(void* owner, REFGUID guid, void** output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN)>(
        input_hooks().device_create[I])(owner, guid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        std::lock_guard lock(state().hook_mutex);
        install_device_capture(*output);
    }
    return result;
}

bool prepare_standard_devices(HMODULE direct) {
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto create = reinterpret_cast<Create>(GetProcAddress(direct, "DirectInput8Create"));
    if (!create) return false;
    bool ready = true;
    std::array<void*, 2> create_targets{};
    std::size_t create_count{};
    for (const auto& iid : {IID_IDirectInput8A, IID_IDirectInput8W}) {
        ComPtr<IUnknown> owner;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid,
            reinterpret_cast<void**>(owner.GetAddressOf()), nullptr))) { ready = false; continue; }
        auto** table = *reinterpret_cast<void***>(owner.Get());
        create_targets[create_count++] = table[3];
        using CreateDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
        for (const auto& guid : {GUID_SysMouse, GUID_SysKeyboard}) {
            ComPtr<IUnknown> device;
            if (FAILED(reinterpret_cast<CreateDevice>(table[3])(owner.Get(), guid,
                reinterpret_cast<void**>(device.GetAddressOf()), nullptr))) { ready = false; continue; }
            // No Acquire or cooperative-level change: these temporary objects
            // identify shared implementations before the game starts workers.
            if (!install_device_capture(device.Get())) ready = false;
        }
    }
    auto& family = input_hooks().device_create;
    const std::array detours{reinterpret_cast<void*>(captured_device_create<0>),
        reinterpret_cast<void*>(captured_device_create<1>)};
    for (std::size_t i = 0; i < create_count; ++i) {
        if (std::any_of(family.begin(), family.end(), [&](const auto& hook) { return hook.target == create_targets[i]; })) continue;
        const auto slot = std::find_if(family.begin(), family.end(), [](const auto& hook) { return !hook.target; });
        if (slot == family.end() || !install(*slot, create_targets[i], detours[slot - family.begin()])) ready = false;
    }
    return ready;
}

HRESULT WINAPI captured_direct_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
        input_hooks().direct_create)(instance, version, iid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        auto** table = *reinterpret_cast<void***>(*output);
        std::lock_guard lock(state().hook_mutex);
        auto& hooks = input_hooks();
        // A/W implementations sometimes share their callable address.
        for (const auto& hook : hooks.device_create) if (hook.target == table[3]) return result;
        if (!hooks.device_create[0].target)
            install(hooks.device_create[0], table[3], reinterpret_cast<void*>(captured_device_create<0>));
        else install(hooks.device_create[1], table[3], reinterpret_cast<void*>(captured_device_create<1>));
    }
    return result;
}

namespace {
// `owns_pointer` is asked only when the packet moves a cursor Windows is not moving.
template<class OwnsPointer> void note_packet(const RAWMOUSE& mouse, OwnsPointer&& owns_pointer) {
    auto& s = state();
    const bool relative = !(mouse.usFlags & MOUSE_MOVE_ABSOLUTE);
    if (relative) {
        s.raw_mouse_x.fetch_add(mouse.lLastX);
        s.raw_mouse_y.fetch_add(mouse.lLastY);
        s.look_mouse_x.fetch_add(mouse.lLastX);
        s.look_mouse_y.fetch_add(mouse.lLastY);
    }
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL) s.look_wheel_raw.fetch_add(static_cast<SHORT>(mouse.usButtonData));
    auto buttons = s.raw_mouse_buttons.load();
    const auto flags = mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) buttons |= 1u;
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) buttons &= ~1u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) buttons |= 2u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) buttons &= ~2u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) buttons |= 4u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) buttons &= ~4u;
    s.raw_mouse_buttons.store(buttons);
    s.raw_mouse_time.store(GetTickCount64());
    // Windows is not moving the cursor (RIDEV_NOLEGACY): move it for the
    // overlay so its pointer and the editor's hover keep working.
    if (relative && (mouse.lLastX || mouse.lLastY) && s.mouse_legacy_blocked.load() && owns_pointer()) {
        OverlayInputAccess access;
        POINT cursor{};
        if (GetCursorPos(&cursor)) SetCursorPos(cursor.x + mouse.lLastX, cursor.y + mouse.lLastY);
    }
}
} // namespace

void note_raw_mouse_packet(const RAWMOUSE& mouse) {
    note_packet(mouse, [] { return owns_menu_cursor(state()); });
}

void note_raw_mouse_packet(const RAWMOUSE& mouse, bool overlay_owns_pointer) {
    note_packet(mouse, [overlay_owns_pointer] { return overlay_owns_pointer; });
}

void sync_raw_mouse_registration() {
    sync_raw_mouse_registration(owns_menu_cursor(state()));
}

void sync_raw_mouse_registration(bool overlay_owns_pointer) {
    try {
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        const bool overlay = overlay_owns_pointer;
        if (overlay == mouse.overridden) return;
        mouse.overridden = overlay;
        // Only a registration that actually blocks the cursor needs replacing.
        if (!mouse.known || !(mouse.game.dwFlags & cursor_blocking_flags)) return;
        apply_raw_mouse(mouse, overlay);
    } catch (...) {}
}

bool install_input_capture() {
    auto& h = input_hooks();
    const auto hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    if (!hid || !install(h.hid_attributes, reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetAttributes")),
            reinterpret_cast<void*>(captured_hid_attributes))) return false;
    const auto kernel_hook = [&](Hook& hook, const char* name, void* detour) {
        return install(hook, reinterpret_cast<void*>(GetProcAddress(kernel, name)), detour);
    };
    if (!kernel_hook(h.read_file, "ReadFile", reinterpret_cast<void*>(captured_read_file)) ||
        !kernel_hook(h.overlapped, "GetOverlappedResult", reinterpret_cast<void*>(captured_overlapped)) ||
        !kernel_hook(h.overlapped_ex, "GetOverlappedResultEx", reinterpret_cast<void*>(captured_overlapped_ex)) ||
        !kernel_hook(h.completion, "GetQueuedCompletionStatus", reinterpret_cast<void*>(captured_completion)) ||
        !kernel_hook(h.completion_ex, "GetQueuedCompletionStatusEx", reinterpret_cast<void*>(captured_completion_ex)) ||
        !kernel_hook(h.close_handle, "CloseHandle", reinterpret_cast<void*>(captured_close_handle))) return false;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto add = [&](Hook& hook, const char* name, void* detour) {
        const bool okay = install(hook, reinterpret_cast<void*>(GetProcAddress(user32, name)), detour);
        if (!okay) dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::input, name);
        return okay;
    };
    add(h.register_raw, "RegisterRawInputDevices", reinterpret_cast<void*>(captured_register_raw));
    if (!add(h.async_key, "GetAsyncKeyState", reinterpret_cast<void*>(captured_async_key)) ||
        !add(h.key, "GetKeyState", reinterpret_cast<void*>(captured_key)) ||
        !add(h.keyboard, "GetKeyboardState", reinterpret_cast<void*>(captured_keyboard)) ||
        !add(h.raw_data, "GetRawInputData", reinterpret_cast<void*>(captured_raw_data)) ||
        !add(h.raw_buffer, "GetRawInputBuffer", reinterpret_cast<void*>(captured_raw_buffer)) ||
        !add(h.capture, "SetCapture", reinterpret_cast<void*>(captured_capture)) ||
        !add(h.release_capture, "ReleaseCapture", reinterpret_cast<void*>(captured_release_capture)) ||
        !add(h.cursor, "SetCursor", reinterpret_cast<void*>(captured_cursor)) ||
        !add(h.cursor_position, "GetCursorPos", reinterpret_cast<void*>(captured_cursor_position)) ||
        !add(h.show_cursor, "ShowCursor", reinterpret_cast<void*>(captured_show_cursor))) return false;
    // Some Windows builds alias both cursor-position exports to one function,
    // which is already intercepted by the overlay's SetCursorPos hook.
    const auto physical = GetProcAddress(user32, "SetPhysicalCursorPos");
    if (physical && physical != GetProcAddress(user32, "SetCursorPos") &&
        !add(h.physical_cursor, "SetPhysicalCursorPos", reinterpret_cast<void*>(captured_physical_cursor))) return false;
    const std::array<void*, 5> states{reinterpret_cast<void*>(captured_xinput<0>), reinterpret_cast<void*>(captured_xinput<1>),
        reinterpret_cast<void*>(captured_xinput<2>), reinterpret_cast<void*>(captured_xinput<3>), reinterpret_cast<void*>(captured_xinput<4>)};
    const std::array<void*, 5> extended{reinterpret_cast<void*>(captured_xinput<0, true>), reinterpret_cast<void*>(captured_xinput<1, true>),
        reinterpret_cast<void*>(captured_xinput<2, true>), reinterpret_cast<void*>(captured_xinput<3, true>), reinterpret_cast<void*>(captured_xinput<4, true>)};
    const std::array<void*, 5> strokes{reinterpret_cast<void*>(captured_keystroke<0>), reinterpret_cast<void*>(captured_keystroke<1>),
        reinterpret_cast<void*>(captured_keystroke<2>), reinterpret_cast<void*>(captured_keystroke<3>), reinterpret_cast<void*>(captured_keystroke<4>)};
    const std::array names{L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"};
    std::vector<void*> seen;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto module = LoadLibraryExW(names[i], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) continue;
        const auto hook_export = [&](Hook& hook, const char* name, void* detour) {
            const auto target = reinterpret_cast<void*>(GetProcAddress(module, name));
            if (!target || std::find(seen.begin(), seen.end(), target) != seen.end()) return true;
            if (!install(hook, target, detour)) { dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::input, "XInput input hook failed: module=%zu target=%p", i, target); return false; }
            seen.push_back(target); return true;
        };
        if (!hook_export(h.xinput_state[i], "XInputGetState", states[i]) ||
            !hook_export(h.xinput_extended[i], MAKEINTRESOURCEA(100), extended[i]) ||
            !hook_export(h.xinput_keystroke[i], "XInputGetKeystroke", strokes[i])) return false;
    }
    const auto direct = LoadLibraryExW(L"dinput8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (direct) {
        if (prepare_standard_devices(direct))
            logging::write(logging::Level::info, logging::Channel::input, "DirectInput mouse/keyboard capture prepared before game startup.");
        else logging::write(logging::Level::warning, logging::Channel::input,
            "DirectInput startup preparation incomplete; device creation will retry the missing hooks.");
        if (!install(h.direct_create, reinterpret_cast<void*>(GetProcAddress(direct, "DirectInput8Create")),
            reinterpret_cast<void*>(captured_direct_create))) return false;
    }
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::input, "Menu input capture installed: keyboard, raw input, XInput, DirectInput, PlayStation HID and cursor ownership.");
    return true;
}

}

namespace dingosdk::overlay {
// A plugin's overlay dialog is up: it owns the cursor and the pad. (The old name stays until
// every caller uses this one.)
void set_plugin_dialog_active(bool active) noexcept { detail::state().plugin_dialog_visible.store(active); }
bool interface_open() noexcept { return detail::interactive_visible(detail::state()); }
void hide_game_buttons(std::uint16_t buttons) noexcept { detail::mode_hidden_buttons.store(buttons, std::memory_order_relaxed); }
void capture_challenge_menu_input(bool active,bool native_panel,bool opening_key_down) noexcept {
    detail::challenge_menu_release.store(game::owned_menu_release_latch(
        detail::challenge_menu_release.load(std::memory_order_relaxed),active,opening_key_down),std::memory_order_relaxed);
    detail::challenge_menu_capture.store(active,std::memory_order_relaxed);
    detail::challenge_native_panel.store(native_panel,std::memory_order_relaxed);
}
void hide_game_keys(std::span<const std::uint8_t> keys) noexcept {
    std::array<std::uint64_t, 4> bits{};
    for (const auto key : keys) bits[key / 64] |= std::uint64_t{1} << (key % 64);
    for (std::size_t i = 0; i < bits.size(); ++i) detail::hidden_keys[i].store(bits[i], std::memory_order_relaxed);
}
void hold_game_buttons(std::uint16_t held) noexcept { detail::mode_held_buttons.store(held, std::memory_order_relaxed); }
void pause_game_input(bool paused) noexcept {
    detail::mode_input_paused_until.store(paused ? GetTickCount64() + 500 : 0, std::memory_order_relaxed);
}
bool key_down(int virtual_key) noexcept { detail::OverlayInputAccess access; return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }
int take_mouse_wheel() noexcept { return detail::mode_mouse_wheel.exchange(0, std::memory_order_relaxed); }
}

// Dark Pop: the local skater is in the air with the D-pad hide on; renewed every physics step (see DarkPopHide).
extern "C" void DingoSDKOverlayDarkPopAir() noexcept {
    dingosdk::overlay::detail::dark_pop_hide().until.store(GetTickCount64() + dingosdk::overlay::detail::dark_pop_air_lease, std::memory_order_relaxed);
}
// Game polls (XInput states, keystrokes, PlayStation reports) whose D-pad Dark Pop hid so far.
extern "C" std::uint64_t DingoSDKOverlayDarkPopHidden() noexcept {
    return dingosdk::overlay::detail::dark_pop_hide().hidden.load(std::memory_order_relaxed);
}
// Skate 3's turn on a grind: the right stick the game's own polls see (see GrindStickTurn). Asked for again every tick.
extern "C" void DingoSDKOverlaySetGrindStickTurn(bool on, float x, float y_add, float y_scale) noexcept {
    auto& turn = dingosdk::overlay::detail::grind_stick_turn();
    turn.x.store(x, std::memory_order_relaxed);
    turn.y_add.store(y_add, std::memory_order_relaxed);
    turn.y_scale.store(y_scale, std::memory_order_relaxed);
    turn.until.store(GetTickCount64() + dingosdk::overlay::detail::stick_lease, std::memory_order_relaxed);
    turn.on.store(on, std::memory_order_relaxed);
}
// What the game's last poll saw of the right stick (x, y) and the player's own right stick up / down.
extern "C" void DingoSDKOverlayGrindStickSent(float* values) noexcept {
    auto& turn = dingosdk::overlay::detail::grind_stick_turn();
    values[0] = turn.sent_x.load(std::memory_order_relaxed);
    values[1] = turn.sent_y.load(std::memory_order_relaxed);
    values[2] = turn.raw_y.load(std::memory_order_relaxed);
}
// Leaning into the ramp on a coping grind: the left stick the game's own polls see while the player's is at rest.
extern "C" void DingoSDKOverlaySetGrindStickLean(bool on, float x, float y) noexcept {
    auto& lean = dingosdk::overlay::detail::grind_stick_lean();
    lean.x.store(x == x ? x : 0.0f, std::memory_order_relaxed);
    lean.y.store(y == y ? y : 0.0f, std::memory_order_relaxed);
    lean.until.store(GetTickCount64() + dingosdk::overlay::detail::stick_lease, std::memory_order_relaxed);
    lean.on.store(on, std::memory_order_relaxed);
}
// The player's own left stick as the game's last poll had it, while the lean is on (false: it is off, read the controller).
extern "C" bool DingoSDKOverlayGrindStickLeanRaw(float* values) noexcept {
    auto& lean = dingosdk::overlay::detail::grind_stick_lean();
    if (!lean.on.load(std::memory_order_relaxed) || GetTickCount64() >= lean.until.load(std::memory_order_relaxed)) return false;
    values[0] = lean.raw_x.load(std::memory_order_relaxed);
    values[1] = lean.raw_y.load(std::memory_order_relaxed);
    return true;
}
// How many of the game's polls were given the lean so far.
extern "C" std::uint64_t DingoSDKOverlayGrindStickLeaned() noexcept {
    return dingosdk::overlay::detail::grind_stick_lean().given.load(std::memory_order_relaxed);
}
// How many of the game's polls were given a made-up right stick so far.
extern "C" std::uint64_t DingoSDKOverlayGrindStickMirrored() noexcept {
    return dingosdk::overlay::detail::grind_stick_mirrored().load(std::memory_order_relaxed);
}
extern "C" void DingoSDKOverlaySetGamepadFilter(dingosdk::overlay::GamepadFilter filter) {
    dingosdk::overlay::detail::gamepad_filter().store(filter, std::memory_order_release);
}
