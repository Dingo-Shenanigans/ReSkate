#pragma once
#include <string_view>

// Pad controls kept from the game, as plugins ask (reskate.input.hide). A hidden trigger reads to
// the game as let go; the left stick's left/right, hidden "while grinding", reads as centred only
// while the skater grinds (launches.h), so it reaches the game again at the pop; the right stick
// reads centred and the stick clicks up, whatever the skater does. ReSkate's own reads, and so the
// plugins' reskate.input.gamepad, still see them all.
namespace dingosdk::pad_hide {
struct Hidden {
    bool left_trigger{}, right_trigger{}, left_x_grinding{};
    bool right_stick{}, left_stick_click{}, right_stick_click{};
    bool any() const noexcept { return left_trigger || right_trigger || left_x_grinding || right_stick || left_stick_click || right_stick_click; }
};
// Requires launches::available().
bool available() noexcept;
void hide(std::string_view owner, Hidden hidden);
// Whatever the plugin hid.
void release(std::string_view owner);
}
