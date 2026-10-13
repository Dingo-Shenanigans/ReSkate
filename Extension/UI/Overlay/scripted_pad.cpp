#include "scripted_pad.h"
#include <atomic>
#include <chrono>
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
bool scripted_pad(DWORD user, XINPUT_STATE &output) noexcept {
    auto &s = state();
    if (user != 0) return false;
    std::lock_guard lock(s.mutex);
    if (!s.active) return false;
    const auto now = std::chrono::steady_clock::now();
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
