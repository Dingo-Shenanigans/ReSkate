#include "trick_pad.h"
#include "style_internal.h"
#include "trick_gestures.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/module_path.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace dingosdk::trick_pad {
namespace {
std::atomic<std::uint64_t> ticks{};
// Runs on the read's own thread.
std::vector<style::TrickGesture> read_gestures() {
    auto list = style::read_trick_gestures(executable_folder());
    logging::log(logging::Level::info, logging::Channel::skater, "Trick pad: read {} flip trick gestures from the game's data.",
                 list.size());
    return list;
}
style::ReadOnce<std::vector<style::TrickGesture>> &gestures() {
    static auto *value = new style::ReadOnce<std::vector<style::TrickGesture>>;
    value->start("the game's flip trick gestures", read_gestures);
    return *value;
}
// `list` keeps the gesture alive.
const style::TrickGesture *gesture_of(std::uint8_t trick, std::shared_ptr<const std::vector<style::TrickGesture>> &list,
                                      std::string &error) {
    auto &read = gestures();
    list = read.value();
    if (!list) {
        error = read.failed() ? "the gestures could not be read"
                              : "the game's gestures are still being read; try again in a few seconds";
        return nullptr;
    }
    const auto found = std::ranges::find(*list, trick, &style::TrickGesture::trick);
    if (found != list->end()) return &*found;
    error = "the game's data has no gesture for that trick";
    return nullptr;
}
} // namespace

bool gestures_ready() {
    const auto &read = gestures();
    return read.value() || read.failed();
}
bool play_trick(std::uint8_t trick, std::string &error) {
    std::shared_ptr<const std::vector<style::TrickGesture>> list;
    const auto *found = gesture_of(trick, list, error);
    if (!found) return false;
    overlay::play_pad(style::flick(*found, flick_ms));
    return true;
}
void press(std::uint16_t buttons, std::uint32_t hold_ms, std::uint32_t end_ms) {
    XINPUT_GAMEPAD pressed{}, neutral{};
    pressed.wButtons = buttons;
    overlay::play_pad({{0, pressed}, {style::to_ticks(hold_ms), neutral}, {style::to_ticks(end_ms), neutral}});
}
void mount() { press(XINPUT_GAMEPAD_Y, tap_ms, tap_end_ms); }
std::uint64_t game_ticks() noexcept { return ticks.load(std::memory_order_acquire); }
void tick() noexcept {
    ticks.fetch_add(1, std::memory_order_acq_rel);
    overlay::pad_tick();
}
} // namespace dingosdk::trick_pad
