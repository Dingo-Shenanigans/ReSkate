// Plugin throwdown modes without the game: the registry's checks, and the pure helpers the native
// setup page and flag cursor use.
#include "Extension/Throwdowns/custom_modes.h"
#include "Extension/Throwdowns/native_flag_cursor_height.h"
#include "Extension/UI/NativeMenu/native_menu_presenters.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dingosdk::multiplayer;
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }

custom_throwdowns::Mode duel() {
    custom_throwdowns::Mode mode;
    mode.plugin = "demo";
    mode.id = "duel";
    mode.name = "Duel";
    mode.description = "Beat the last total.";
    mode.players = 9; // more than max: clamped
    return mode;
}

void modes_are_checked_and_listed() {
    const auto before = custom_throwdowns::revision();
    check(custom_throwdowns::set_mode(duel()).empty(), "the mode registers");
    auto bad = duel();
    bad.id = "a/b";
    check(!custom_throwdowns::set_mode(bad).empty(), "an id with / is refused");
    bad = duel();
    bad.replaces = "bowling";
    check(!custom_throwdowns::set_mode(bad).empty(), "replaces names a stock throwdown");
    bad = duel();
    bad.card = "bowling";
    check(!custom_throwdowns::set_mode(bad).empty(), "a card is a stock one");
    bad = duel();
    bad.cursor_height = -1;
    check(!custom_throwdowns::set_mode(bad).empty(), "a cursor height is not negative");
    bad.cursor_height = 200000;
    check(!custom_throwdowns::set_mode(bad).empty(), "a cursor height is at most 100000 m");
    const auto mode = custom_throwdowns::find("demo/duel");
    check(mode && mode->players == 6 && mode->card == "spot" && mode->cursor_height == 0, "players are clamped, the card is Spot Battle's");
    // An update replaces it, and its flag may stand on anything now.
    auto again = duel();
    again.description = "New words.";
    again.cursor_height = 99999;
    check(custom_throwdowns::set_mode(again).empty() && custom_throwdowns::revision() != before, "an update");
    check(custom_throwdowns::find("demo/duel")->cursor_height == 99999 && custom_throwdowns::modes().size() == 1, "one mode, updated");
    custom_throwdowns::remove_plugin("demo");
    check(custom_throwdowns::modes().empty() && !custom_throwdowns::find("demo/duel"), "a plugin's modes go with it");
}

struct Presenter { std::uintptr_t blueprint{}; struct { std::uintptr_t record{}; std::uint64_t handle{}; } data; };
void setup_page_helpers() {
    using menu_data::native_slider_choice, menu_data::native_slider_position;
    check(native_slider_choice(0.f, 10, 120, 10) == 10 && native_slider_choice(1.f, 10, 120, 10) == 120, "slider ends");
    check(native_slider_choice(native_slider_position(70, 10, 120), 10, 120, 10) == 70, "a choice survives the round trip");
    check(native_slider_choice(.52f, 1, 7, 1) == 4, "rounds to the nearest step");
    check(native_slider_choice(std::nanf(""), 1, 7, 1) == 1, "a broken value is the minimum");
    // Two 32-byte rows with a presenter at 8: the same presenters need no publish; a new one keeps the rest.
    std::vector<std::byte> live(64), saved(64);
    const Presenter a{1, {0, 10}}, b{2, {0, 20}};
    std::memcpy(live.data() + 8, &a, sizeof a);
    std::memcpy(saved.data() + 8, &a, sizeof a);
    live[0] = std::byte{7}; // native focus state
    check(!menu_data::remount_presenters<Presenter>(live, saved, 32, 8), "same presenters: nothing to publish");
    std::memcpy(saved.data() + 40, &b, sizeof b);
    const auto next = menu_data::remount_presenters<Presenter>(live, saved, 32, 8);
    check(next && (*next)[0] == std::byte{7}, "a new presenter keeps the live focus bytes");
    check(menu_data::remount_presenters<Presenter>(live, std::span(saved).first(32), 32, 8)->size() == 32, "another count takes the saved rows");
}
// The flag cursor's ceiling: raised to the registration's height, restored only when still ours.
void cursor_height_lease() {
    NativeFlagCursorHeight lease;
    check(!lease.apply(5.f, 0.f), "no height asked: nothing changes");
    check(lease.apply(5.f, 99999.f) == 99999.f, "raised to the asked height");
    check(!lease.apply(99999.f, 99999.f), "already raised");
    check(lease.apply(99999.f, 300.f) == 300.f, "another height replaces ours");
    check(lease.restore(300.f) == 5.f, "the native value comes back");
    check(!lease.apply(400.f, 300.f), "a native ceiling above the asked one stays");
    check(lease.apply(5.f, 50.f) == 50.f && !lease.restore(7.f), "a native change since is not undone");
}
} // namespace

int main() {
    try {
        modes_are_checked_and_listed();
        setup_page_helpers();
        cursor_height_lease();
        std::cout << "custom throwdown checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
