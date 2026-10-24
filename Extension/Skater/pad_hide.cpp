#include "pad_hide.h"
#include "launches.h"
#include "Extension/UI/Overlay/overlay.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace dingosdk::pad_hide {
namespace {
struct Control {
    std::mutex mutex;
    std::map<std::string, Hidden, std::less<>> hidden;
    std::atomic<bool> lt{}, rt{}, left_x{}, right_stick{}, l3{}, r3{};
};
Control& control() {
    static auto* value = new Control; // process lifetime: the game polls its pad until exit
    return *value;
}
// Any thread the game polls its pad from.
void filter_pad(unsigned, overlay::GamepadValues& pad) noexcept {
    auto& c = control();
    if (c.lt.load(std::memory_order_acquire)) pad.left_trigger = 0;
    if (c.rt.load(std::memory_order_acquire)) pad.right_trigger = 0;
    if (c.left_x.load(std::memory_order_acquire) && launches::grinding()) pad.left_x = 0;
    if (c.right_stick.load(std::memory_order_acquire)) pad.right_x = pad.right_y = 0;
    if (c.l3.load(std::memory_order_acquire)) pad.buttons &= static_cast<std::uint16_t>(~0x0040u); // XINPUT_GAMEPAD_LEFT_THUMB
    if (c.r3.load(std::memory_order_acquire)) pad.buttons &= static_cast<std::uint16_t>(~0x0080u); // XINPUT_GAMEPAD_RIGHT_THUMB
}
void publish(Control& c) {
    Hidden all;
    for (const auto& [owner, h] : c.hidden) {
        all.left_trigger = all.left_trigger || h.left_trigger;
        all.right_trigger = all.right_trigger || h.right_trigger;
        all.left_x_grinding = all.left_x_grinding || h.left_x_grinding;
        all.right_stick = all.right_stick || h.right_stick;
        all.left_stick_click = all.left_stick_click || h.left_stick_click;
        all.right_stick_click = all.right_stick_click || h.right_stick_click;
    }
    c.lt.store(all.left_trigger, std::memory_order_release);
    c.rt.store(all.right_trigger, std::memory_order_release);
    c.left_x.store(all.left_x_grinding, std::memory_order_release);
    c.right_stick.store(all.right_stick, std::memory_order_release);
    c.l3.store(all.left_stick_click, std::memory_order_release);
    c.r3.store(all.right_stick_click, std::memory_order_release);
    DingoSDKOverlaySetGamepadFilter(c.hidden.empty() ? nullptr : &filter_pad);
}
} // namespace

bool available() noexcept { return launches::available(); }
void hide(std::string_view owner, Hidden hidden) {
    if (!available()) return;
    if (hidden.left_x_grinding) (void)launches::grinding(); // the physics steps followed from now on
    auto& c = control();
    std::lock_guard lock(c.mutex);
    if (hidden.any()) c.hidden.insert_or_assign(std::string(owner), hidden);
    else if (const auto found = c.hidden.find(owner); found != c.hidden.end()) c.hidden.erase(found);
    publish(c);
}
void release(std::string_view owner) {
    auto& c = control();
    std::lock_guard lock(c.mutex);
    if (const auto found = c.hidden.find(owner); found != c.hidden.end()) {
        c.hidden.erase(found);
        publish(c);
    }
}
}
