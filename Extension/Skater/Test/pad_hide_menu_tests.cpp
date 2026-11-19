#include "Extension/Skater/pad_hide.h"
#include "Extension/Skater/launches.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Input/native_menu_observation.h"
#include <Windows.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace {
dingosdk::overlay::GamepadFilter filter{};
}
namespace dingosdk::launches {
bool available() noexcept { return true; }
bool grinding() noexcept { return true; }
}
extern "C" void DingoSDKOverlaySetGamepadFilter(dingosdk::overlay::GamepadFilter value) { filter = value; }

int main() {
    using namespace dingosdk;
    namespace menu = dingosdk::native_menu_observation;
    const overlay::GamepadValues physical{0x1010, 200, 180, 22000, -21000, 25000, -26000};
    pad_hide::hide("plugin", {true, true, true, true, false, false, 0x1010});
    assert(filter);
    const auto verify = [&](menu::State state, std::uint64_t age, bool hidden) {
        menu::publish(state, GetTickCount64() - age);
        auto pad = physical;
        filter(0, pad);
        assert((pad.buttons & 0x0010) != 0);
        assert((pad.buttons & 0x1000) == (hidden ? 0 : 0x1000));
        assert((pad.left_trigger == 0) == hidden);
    };
    verify(menu::State::closed, 0, true);
    verify(menu::State::open, 0, false);
    verify(menu::State::unknown, 0, true);
    verify(menu::State::open, 101, true);
    pad_hide::release("plugin");
    assert(!filter);
}
