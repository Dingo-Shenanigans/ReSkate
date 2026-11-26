#include "Extension/Input/native_menu_observation.h"
#include "Engine/Game/Input/plugin_input_policy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>

int main() {
    using namespace dingosdk::native_menu_observation;
    assert(read(1000) == State::unknown);
    publish(State::closed, 1000);
    assert(known_closed(1000) && known_closed(1100) && !known_open(1100));
    assert(read(1101) == State::unknown && read(999) == State::unknown);
    publish(State::open, 1200);
    assert(known_open(1200) && known_open(1300) && !known_closed(1300));
    assert(read(1301) == State::unknown);
    publish(State::unknown, 1302);
    assert(read(1302) == State::unknown);
    publish(State::open, 0);
    assert(read(1302) == State::unknown);
    publish(State::closed, UINT64_MAX);
    assert(read(UINT64_MAX) == State::unknown);
    using dingosdk::input::menu_button;
    using dingosdk::input::preserve_menu_button;
    assert(preserve_menu_button(menu_button | 0x1000, 0x2000) == (menu_button | 0x2000));
    assert(preserve_menu_button(0x1000, menu_button | 0x2000) == 0x2000);
}
