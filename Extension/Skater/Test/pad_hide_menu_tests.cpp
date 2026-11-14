#include "Engine/Game/Input/plugin_input_policy.h"
#include "Extension/Skater/pad_hide.h"
#include "Extension/Skater/launches.h"
#include "Extension/Input/native_input_source.h"
#include "Extension/UI/Overlay/overlay.h"
#include <cstdio>
#include <cstdlib>

namespace {
dingosdk::overlay::GamepadFilter filter{};
bool menu_permit{};
unsigned checks{};
void check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
bool same(const dingosdk::overlay::GamepadValues& a, const dingosdk::overlay::GamepadValues& b) {
    return a.buttons == b.buttons && a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
        a.left_x == b.left_x && a.left_y == b.left_y && a.right_x == b.right_x && a.right_y == b.right_y;
}
}
namespace dingosdk::native_input_source {
bool native_menu_closed(std::uint64_t) noexcept { return menu_permit; }
}
namespace dingosdk::launches {
bool available() noexcept { return true; }
bool grinding() noexcept { return true; }
}
extern "C" void DingoSDKOverlaySetGamepadFilter(dingosdk::overlay::GamepadFilter value) { filter = value; }

int main() {
    namespace hide = dingosdk::pad_hide;
    check(dingosdk::input::menu_escape(true, false, 27) && dingosdk::input::menu_escape(false, true, 27),
        "keyboard Escape presses and releases bypass gameplay camera capture");
    check(!dingosdk::input::menu_escape(false, false, 27) && !dingosdk::input::menu_escape(true, false, 26),
        "a mouse or raw-input payload equal to Escape is not a keyboard escape");
    const dingosdk::overlay::GamepadValues physical{0x10d1, 200, 180, 22000, -21000, 25000, -26000};
    hide::hide("hostile", {true, true, true, true, true, true});
    check(filter != nullptr, "real hide implementation registers its native filter");
    menu_permit = true;
    auto gameplay = physical; filter(0, gameplay);
    check(!same(gameplay, physical) && !gameplay.right_x && !gameplay.right_y,
        "known-closed gameplay can still hide the requested pad controls");
    check((gameplay.buttons & 0x0010) != 0, "hiding gameplay controls retains physical Start");
    menu_permit = false;
    for (unsigned i = 0; i != 10; ++i) {
        hide::hide("hostile", {true, true, true, true, true, true});
        auto menu = physical; filter(i % 4, menu);
        check(same(menu, physical), "a repeated hide cannot consume physical menu controls without a fresh permit");
    }
    hide::release("hostile");
    check(filter == nullptr, "owner release removes its real native filter");
    std::printf("PASS %u actual pad-hide filter checks; native menu observations are controlled\n", checks);
    return 0;
}
