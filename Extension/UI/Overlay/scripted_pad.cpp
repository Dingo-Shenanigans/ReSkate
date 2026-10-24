#include "scripted_pad.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dingosdk::overlay {
namespace {
struct State {
    std::mutex mutex;
    std::vector<PadStep> steps;
    std::size_t current{};
    std::uint32_t ticks{};
    std::chrono::steady_clock::time_point last_tick, done_at;
    bool active{};
    DWORD packet{};
    // A stick held (hold_stick).
    int stick{};
    SHORT value{};
    std::chrono::steady_clock::time_point held_until{};
};
State &state() { static auto *value = new State; return *value; }
constexpr std::chrono::milliseconds linger{2000}, no_ticks{250}, tick_length{17};
// One game tick of the script. The caller holds s.mutex.
void advance(State &s, std::chrono::steady_clock::time_point now) {
    ++s.ticks;
    s.last_tick = now;
    while (s.current + 1 < s.steps.size() && s.steps[s.current + 1].tick <= s.ticks) {
        ++s.current;
        if (s.current + 1 == s.steps.size()) s.done_at = now;
    }
}
} // namespace

void play_pad(std::vector<PadStep> steps) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.steps = std::move(steps);
    s.current = 0;
    s.ticks = 0;
    s.last_tick = s.done_at = std::chrono::steady_clock::now();
    s.active = !s.steps.empty();
}
void pad_tick() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.active) advance(s, std::chrono::steady_clock::now());
}
void hold_stick(int stick, float value, unsigned milliseconds) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.stick = stick < 0 || stick > 3 ? 2 : stick;
    s.value = static_cast<SHORT>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    s.held_until = value == 0 ? std::chrono::steady_clock::time_point{}
                              : std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}
bool scripted_pad(DWORD user, XINPUT_STATE &output) noexcept {
    auto &s = state();
    if (user != 0) return false;
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!s.active) {
        if (now >= s.held_until) return false;
        output.Gamepad = XINPUT_GAMEPAD{};
        SHORT* axes[] = {&output.Gamepad.sThumbLX, &output.Gamepad.sThumbLY, &output.Gamepad.sThumbRX, &output.Gamepad.sThumbRY};
        *axes[s.stick] = s.value;
        output.dwPacketNumber = ++s.packet;
        return true;
    }
    // With no game ticks, time alone moves the script on.
    while (s.current + 1 < s.steps.size() && now - s.last_tick > no_ticks) advance(s, s.last_tick + tick_length);
    const bool done = s.current + 1 == s.steps.size();
    if (done && now - s.done_at > linger) {
        s.active = false;
        return false;
    }
    output.Gamepad = done ? XINPUT_GAMEPAD{} : s.steps[s.current].pad;
    output.dwPacketNumber = ++s.packet;
    return true;
}
} // namespace dingosdk::overlay
