#include <Windows.h>
#include <Xinput.h>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
ULONGLONG fixture_now = 1000;
ULONGLONG WINAPI fixture_tick_count() noexcept { return fixture_now; }
bool overlay_owns{};
bool fixture_script{};
bool fixture_countdown{};
WORD fixture_pad_key = VK_PAD_START;
USHORT fixture_raw_key = VK_ESCAPE;
}
#define GetTickCount64 fixture_tick_count
#define DINGOSDK_INPUT_SOURCE_TEST
#include "Extension/UI/Overlay/input_capture.cpp"
#undef DINGOSDK_INPUT_SOURCE_TEST
#undef GetTickCount64

namespace dingosdk::native_input_source {
PublicationScope::PublicationScope(bool) noexcept {}
PublicationScope::~PublicationScope() {}
bool publication_held() noexcept { return false; }
bool publication_needed() noexcept { return false; }
std::uint64_t publication_revision() noexcept { return 1; }
void publication_changed() noexcept {}
Poll::~Poll() {}
void begin(bool, std::uint32_t, Poll&) noexcept {}
std::uint32_t finish_capture(Poll&, std::uint32_t, bool, std::uint32_t, const input_lease::Pad&,
    std::uint32_t returned_result, bool, std::uint32_t, const input_lease::Pad&,
    std::uint64_t, void*, PublishReport, bool) noexcept { return returned_result; }
}
namespace dingosdk::overlay::detail {
thread_local unsigned overlay_input_access{};
State& state() { static auto* value = new State; return *value; }
bool owns_menu_cursor(const State&) { return overlay_owns; }
bool interactive_visible(const State&) { return false; }
bool install(Hook&, void*, void*) { return false; }
}
namespace dingosdk::throwdowns::kit {
Match match() noexcept { Match value; value.input_lock = fixture_countdown; return value; }
}
namespace dingosdk::overlay {
// The stand-in pad of the character room's walk and reskate.room.turn: RB alone.
bool scripted_pad(DWORD, XINPUT_STATE& output, bool* sdk_written) noexcept {
    if (sdk_written) *sdk_written = fixture_script;
    if (!fixture_script) return false;
    output.Gamepad = {};
    output.Gamepad.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
    return true;
}
bool held_stick(int&, SHORT&) noexcept { return false; }
bool scripted_pad_active() noexcept { return fixture_script; }
// The same script as a native PlayStation read gets it, and its stand-in answering as a connected pad.
bool scripted_buttons(WORD& buttons) noexcept {
    if (!fixture_script) return false;
    buttons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
    return true;
}
bool stand_in_connected() noexcept { return fixture_script; }
}
namespace dingosdk::multiplayer {
bool native_throwdown_gameplay_input_allowed() noexcept { return true; }
}
namespace dingosdk::logging {
bool enabled(Level) noexcept { return false; }
void write(Level, Context, Channel, std::string_view) noexcept {}
void write(Level, Channel, std::string_view) noexcept {}
void printf(Level, Channel, const char*, ...) noexcept {}
}

namespace {
using namespace dingosdk;
namespace capture = dingosdk::overlay::detail;
namespace menu = dingosdk::native_menu_observation;

HRESULT STDMETHODCALLTYPE fake_state(void*, DWORD size, LPVOID data) {
    std::memset(data, 0, size);
    if (size == 256) {
        auto* keys = static_cast<std::uint8_t*>(data);
        keys[DIK_R] = keys[DIK_ESCAPE] = 0x80;
    } else if (size == sizeof(DIJOYSTATE)) {
        auto* pad = static_cast<DIJOYSTATE*>(data);
        pad->lX = 1234;
        pad->rgbButtons[0] = pad->rgbButtons[7] = 0x80;
    }
    return DI_OK;
}
DWORD WINAPI fake_keystroke(DWORD, DWORD, PXINPUT_KEYSTROKE output) {
    *output = {};
    output->VirtualKey = fixture_pad_key;
    output->Flags = XINPUT_KEYSTROKE_KEYDOWN;
    return ERROR_SUCCESS;
}
DWORD WINAPI fake_xinput(DWORD, XINPUT_STATE* output) {
    *output = {};
    output->dwPacketNumber = 7;
    output->Gamepad.wButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A;
    return ERROR_SUCCESS;
}
SHORT WINAPI fake_key(int) { return static_cast<SHORT>(0x8000); }
BOOL WINAPI fake_keyboard(PBYTE keys) {
    std::memset(keys, 0, 256);
    keys[VK_ESCAPE] = keys['R'] = 0x80;
    return TRUE;
}
UINT WINAPI fake_raw_data(HRAWINPUT, UINT, LPVOID data, PUINT size, UINT) {
    if (!data) return 0;
    auto* packet = static_cast<RAWINPUT*>(data);
    *packet = {};
    packet->header.dwType = RIM_TYPEKEYBOARD;
    packet->header.dwSize = sizeof(RAWINPUT);
    packet->data.keyboard.VKey = fixture_raw_key;
    packet->data.keyboard.Message = WM_KEYDOWN;
    *size = sizeof(RAWINPUT);
    return sizeof(RAWINPUT);
}
void observe(menu::State state) { menu::publish(state, fixture_now); }
void keyboard_lane(void* device) {
    std::array<std::uint8_t, 256> keys{};
    capture::captured_device_state<0>(device, 256, keys.data());
    const bool open = menu::known_open(fixture_now);
    assert((keys[DIK_R] != 0) == open);
    assert(keys[DIK_ESCAPE] == 0x80);
    assert(capture::key_hidden('R') == !open);
    assert(!capture::key_hidden(VK_ESCAPE));
    assert((capture::gameplay_hidden_buttons() != 0) == !open);
}
void state_cases(void* device) {
    overlay::hide_game_buttons(0xffff);
    overlay::hold_game_buttons(0xffff);
    assert((capture::mode_hidden_buttons.load() & input::menu_button) == 0);
    assert((capture::mode_held_buttons.load() & input::menu_button) == 0);
    const std::array<std::uint8_t, 2> hidden{'R', VK_ESCAPE};
    overlay::hide_game_keys(hidden);
    observe(menu::State::closed); keyboard_lane(device);
    observe(menu::State::open); keyboard_lane(device);
    observe(menu::State::unknown); keyboard_lane(device);
    observe(menu::State::closed); fixture_now += 101; keyboard_lane(device);
    overlay::hide_game_keys({});
    overlay::hide_game_buttons(0);
    overlay::hold_game_buttons(0);
    capture::input_hooks().formats[device].keyboard = false;
    overlay::hide_game_keys(hidden);
    observe(menu::State::closed);
    std::array<std::uint8_t, 256> untyped{};
    capture::captured_device_state<0>(device, 256, untyped.data());
    // A 256-byte state whose format was not seen to be a keyboard's is read as before: hidden keys go.
    assert(untyped[DIK_R] == 0 && untyped[DIK_ESCAPE] == 0x80);
    overlay::hide_game_keys({});
    capture::input_hooks().formats[device].keyboard = true;
}
void directinput_capture(void* device) {
    std::array<std::uint8_t, 256> keys{};
    observe(menu::State::closed);
    overlay::pause_game_input(true);
    capture::captured_device_state<0>(device, 256, keys.data());
    assert(keys[DIK_R] == 0 && keys[DIK_ESCAPE] == 0x80);
    observe(menu::State::open);
    capture::captured_device_state<0>(device, 256, keys.data());
    assert(keys[DIK_R] == 0x80 && keys[DIK_ESCAPE] == 0x80);
    observe(menu::State::unknown);
    capture::captured_device_state<0>(device, 256, keys.data());
    assert(keys[DIK_R] == 0 && keys[DIK_ESCAPE] == 0x80);
    // The free camera and a pause empty a pad's whole state, as before: which of its buttons is
    // Start is not known here, so none is let through.
    capture::state().freecam_controller_active.store(true);
    DIJOYSTATE pad{};
    capture::captured_device_state<0>(device, sizeof(pad), &pad);
    assert(pad.rgbButtons[0] == 0 && pad.rgbButtons[7] == 0 && pad.lX == 32767);
    capture::state().freecam_controller_active.store(false);
    // So is a 256-byte state that was not seen to be a keyboard's, its Escape included.
    capture::input_hooks().formats[device].keyboard = false;
    capture::captured_device_state<0>(device, 256, keys.data());
    assert(keys[DIK_R] == 0 && keys[DIK_ESCAPE] == 0);
    capture::input_hooks().formats[device].keyboard = true;
    // The challenge panel keeps Escape for itself, also while a plugin has paused the input.
    overlay::capture_challenge_menu_input(true, false, false);
    capture::captured_device_state<0>(device, 256, keys.data());
    assert(keys[DIK_R] == 0 && keys[DIK_ESCAPE] == 0);
    assert(capture::captured_async_key(VK_ESCAPE) == 0);
    XINPUT_STATE xbox{};
    assert(capture::captured_xinput<0>(0, &xbox) == ERROR_SUCCESS && xbox.Gamepad.wButtons == 0);
    overlay::capture_challenge_menu_input(false, false, false);
    assert(capture::captured_xinput<0>(0, &xbox) == ERROR_SUCCESS && xbox.Gamepad.wButtons == XINPUT_GAMEPAD_START);
    overlay::pause_game_input(false);
}
void polled_lanes() {
    observe(menu::State::closed);
    overlay::hide_game_buttons(0xffff);
    const std::array<std::uint8_t, 2> hidden{'R', VK_ESCAPE};
    overlay::hide_game_keys(hidden);
    fixture_pad_key = VK_PAD_START;
    XINPUT_KEYSTROKE stroke{};
    assert(capture::captured_keystroke<0>(0, 0, &stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_START);
    fixture_pad_key = VK_PAD_A;
    assert(capture::captured_keystroke<0>(0, 0, &stroke) == ERROR_EMPTY);
    assert(capture::captured_async_key(VK_ESCAPE) != 0 && capture::captured_key(VK_ESCAPE) != 0);
    assert(capture::captured_async_key('R') == 0 && capture::captured_key('R') == 0);
    std::array<BYTE, 256> keys{};
    assert(capture::captured_keyboard(keys.data()) && keys[VK_ESCAPE] == 0x80 && keys['R'] == 0);
    observe(menu::State::open);
    assert(capture::captured_keystroke<0>(0, 0, &stroke) == ERROR_SUCCESS && stroke.VirtualKey == VK_PAD_A);
    assert(capture::captured_async_key('R') != 0);
    assert(capture::captured_keyboard(keys.data()) && keys['R'] == 0x80);
    observe(menu::State::unknown);
    assert(capture::captured_keystroke<0>(0, 0, &stroke) == ERROR_EMPTY);
    assert(capture::captured_async_key('R') == 0);
    observe(menu::State::closed); fixture_now += 101;
    assert(capture::captured_keystroke<0>(0, 0, &stroke) == ERROR_EMPTY);
    assert(capture::captured_async_key('R') == 0);
    overlay::hide_game_buttons(0);
    overlay::hide_game_keys({});
}
void xinput_lanes() {
    const auto copies = capture::passive_copy_calls.load();
    XINPUT_STATE idle_xbox{}, idle_front{};
    assert(capture::captured_xinput<0>(0, &idle_xbox) == ERROR_SUCCESS);
    assert(capture::captured_front<0>(0, &idle_front) == ERROR_SUCCESS);
    assert(capture::passive_copy_calls.load() == copies);
    overlay::hide_game_buttons(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A);
    const auto verify = [&](menu::State state, std::uint64_t age, bool hide_a) {
        menu::publish(state, fixture_now - age);
        XINPUT_STATE xbox{}, steam{};
        assert(capture::captured_xinput<0>(0, &xbox) == ERROR_SUCCESS);
        assert(capture::captured_front<0>(0, &steam) == ERROR_SUCCESS);
        for (const auto& packet : {xbox, steam}) {
            assert((packet.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0);
            assert(((packet.Gamepad.wButtons & XINPUT_GAMEPAD_A) == 0) == hide_a);
        }
    };
    verify(menu::State::closed, 0, true);
    verify(menu::State::open, 0, false);
    verify(menu::State::unknown, 0, true);
    verify(menu::State::closed, 101, true);
    overlay::hide_game_buttons(0);
    observe(menu::State::closed);
    overlay::pause_game_input(true);
    XINPUT_STATE xbox{}, steam{};
    assert(capture::captured_xinput<0>(0, &xbox) == ERROR_SUCCESS);
    assert(capture::captured_front<0>(0, &steam) == ERROR_SUCCESS);
    assert(xbox.Gamepad.wButtons == XINPUT_GAMEPAD_START && steam.Gamepad.wButtons == XINPUT_GAMEPAD_START);
    overlay::pause_game_input(false);
    // The character room's walk and reskate.room.turn drive the game's menu with a stand-in pad: the
    // game gets it with the menu open as with it closed or unknown.
    fixture_script = true;
    for (const auto state : {menu::State::open, menu::State::closed, menu::State::unknown}) {
        observe(state);
        XINPUT_STATE walk{}, front{};
        assert(capture::captured_xinput<0>(0, &walk) == ERROR_SUCCESS && walk.Gamepad.wButtons == XINPUT_GAMEPAD_RIGHT_SHOULDER);
        assert(capture::captured_front<0>(0, &front) == ERROR_SUCCESS && front.Gamepad.wButtons == XINPUT_GAMEPAD_RIGHT_SHOULDER);
    }
    fixture_script = false;
}
void countdown_without_tick() {
    fixture_countdown = true;
    assert(capture::trusted_polled_capture());
    fixture_countdown = false;
    XINPUT_STATE output{};
    assert(capture::captured_xinput<0>(0, &output) == ERROR_SUCCESS);
    assert(output.Gamepad.wButtons == (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A));
    assert(!capture::trusted_polled_capture());
}
void raw_lane() {
    const std::array<std::uint8_t, 2> hidden{'R', VK_ESCAPE};
    overlay::hide_game_keys(hidden);
    RAWINPUT packet{};
    UINT size = sizeof(packet);
    fixture_raw_key = VK_ESCAPE;
    observe(menu::State::closed);
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == sizeof(packet));
    assert(packet.data.keyboard.VKey == VK_ESCAPE);
    fixture_raw_key = 'R';
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == UINT(-1));
    observe(menu::State::open);
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == sizeof(packet));
    assert(packet.data.keyboard.VKey == 'R');
    observe(menu::State::unknown);
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == UINT(-1));
    observe(menu::State::open); fixture_now += 101;
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == UINT(-1));
    fixture_raw_key = VK_ESCAPE;
    observe(menu::State::closed);
    overlay::pause_game_input(true);
    assert(capture::captured_raw_data(nullptr, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) == sizeof(packet));
    assert(packet.data.keyboard.VKey == VK_ESCAPE);
    overlay::pause_game_input(false);
    overlay::hide_game_keys({});
}
void hid_options() {
    fixture_now += 1;
    observe(menu::State::closed);
    overlay::pause_game_input(true);
    std::array<std::uint8_t, 64> report{1, 128, 128, 128, 128, 8, 0x20, 0, 0, 0};
    capture::InputCaptureHooks::HidRead read{nullptr, report.data(), static_cast<DWORD>(report.size()), PlayStationPad::dualshock4};
    capture::neutral_hid(read, static_cast<DWORD>(report.size()));
    const auto parsed = parse_playstation_report(PlayStationPad::dualshock4, false, report.data(), report.size());
    assert(parsed && parsed->buttons == input::menu_button);
    overlay::pause_game_input(false);
    overlay::hide_game_buttons(input::menu_button | XINPUT_GAMEPAD_A);
    for (const auto state : {menu::State::closed, menu::State::open, menu::State::unknown, menu::State::closed}) {
        auto buttons = report;
        buttons[5] = 0x28; // Cross and released D-pad.
        observe(state);
        capture::hide_hid_buttons({nullptr, buttons.data(), static_cast<DWORD>(buttons.size()), PlayStationPad::dualshock4},
            static_cast<DWORD>(buttons.size()));
        const auto read_back = parse_playstation_report(PlayStationPad::dualshock4, false, buttons.data(), buttons.size());
        assert(read_back && (read_back->buttons & input::menu_button));
        assert(((read_back->buttons & XINPUT_GAMEPAD_A) != 0) == (state == menu::State::open));
    }
    observe(menu::State::closed); fixture_now += 101;
    auto stale = report;
    stale[5] = 0x28;
    capture::hide_hid_buttons({nullptr, stale.data(), static_cast<DWORD>(stale.size()), PlayStationPad::dualshock4},
        static_cast<DWORD>(stale.size()));
    const auto stale_pad = parse_playstation_report(PlayStationPad::dualshock4, false, stale.data(), stale.size());
    assert(stale_pad && (stale_pad->buttons & XINPUT_GAMEPAD_A) == 0 && (stale_pad->buttons & input::menu_button));
    overlay::hide_game_buttons(0);
}
}
int main() {
    auto* device = reinterpret_cast<void*>(std::uintptr_t{0x1234});
    capture::input_hooks().device_state[0].original = reinterpret_cast<void*>(&fake_state);
    capture::input_hooks().xinput_keystroke[0].original = reinterpret_cast<void*>(&fake_keystroke);
    capture::input_hooks().xinput_state[0].original = reinterpret_cast<void*>(&fake_xinput);
    capture::input_hooks().xinput_front[0].original = reinterpret_cast<void*>(&fake_xinput);
    capture::input_hooks().async_key.original = reinterpret_cast<void*>(&fake_key);
    capture::input_hooks().key.original = reinterpret_cast<void*>(&fake_key);
    capture::input_hooks().keyboard.original = reinterpret_cast<void*>(&fake_keyboard);
    capture::input_hooks().raw_data.original = reinterpret_cast<void*>(&fake_raw_data);
    capture::input_hooks().formats[device].size = 256;
    capture::input_hooks().formats[device].keyboard = true;
    state_cases(device);
    directinput_capture(device);
    polled_lanes();
    xinput_lanes();
    countdown_without_tick();
    raw_lane();
    capture::input_hooks().formats.erase(device);
    hid_options();
}
