#include "Extension/Input/input_publication.h"
#include "Extension/Input/native_input_source.h"
#include "scripted_pad.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <limits>
#include <string>

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
    std::string preview_owner;
    std::atomic<std::uint64_t> preview_epoch{1}, preview_until{};
    std::uint64_t held_epoch{};
};
State &state() { static auto *value = new State; return *value; }
constexpr std::chrono::milliseconds linger{2000}, no_ticks{250}, tick_length{17};
constexpr std::uint64_t preview_fresh_ms = 100;
bool preview_valid(const State& s, std::uint64_t epoch, std::uint64_t now) noexcept {
    const auto current = s.preview_epoch.load(std::memory_order_acquire);
    const auto until = s.preview_until.load(std::memory_order_acquire);
    return native_input_source::native_menu_observed(now) && epoch && epoch == current &&
        now && now < until && until - now <= preview_fresh_ms &&
        current == s.preview_epoch.load(std::memory_order_acquire);
}
void revoke_preview(State& s) noexcept {
    if (!s.preview_until.load() && s.preview_owner.empty() && !s.held_epoch) return;
    s.preview_until.store(0, std::memory_order_release);
    const auto epoch = s.preview_epoch.load();
    s.preview_epoch.store(epoch == std::numeric_limits<std::uint64_t>::max() ? 0 : epoch ? epoch + 1 : 0,
        std::memory_order_release);
    s.preview_owner.clear(); s.held_epoch = 0; s.held_until = {}; s.value = 0;
    native_input_source::publication_changed();
}
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
    native_input_source::PublicationScope publication;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    native_input_source::publication_changed();
    s.steps = std::move(steps);
    s.current = 0;
    s.ticks = 0;
    s.last_tick = s.done_at = std::chrono::steady_clock::now();
    s.active = !s.steps.empty();
}
void pad_tick() noexcept {
    native_input_source::PublicationScope publication;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.active) { native_input_source::publication_changed(); advance(s, std::chrono::steady_clock::now()); }
}
void preview_context(std::string_view owner, bool visible) {
    const auto now = GetTickCount64();
    native_input_source::PublicationScope publication;
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto epoch = s.preview_epoch.load();
    if (!visible || owner.empty() || !epoch || now > std::numeric_limits<std::uint64_t>::max() - preview_fresh_ms) {
        revoke_preview(s); return;
    }
    if (s.preview_owner != owner || !preview_valid(s, epoch, now)) {
        revoke_preview(s);
        if (!s.preview_epoch.load()) return;
        s.preview_owner = owner;
        native_input_source::publication_changed();
    }
    s.preview_until.store(now + preview_fresh_ms, std::memory_order_release);
}
bool preview_report_valid(std::uint64_t epoch, std::uint64_t now_ms) noexcept {
    return preview_valid(state(), epoch, now_ms);
}
bool hold_stick(std::string_view owner, int stick, float value, unsigned milliseconds) noexcept {
    native_input_source::PublicationScope publication;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto epoch = s.preview_epoch.load();
    if (owner.empty() || s.preview_owner != owner || !preview_valid(s, epoch, GetTickCount64())) return false;
    native_input_source::publication_changed();
    s.stick = stick < 0 || stick > 3 ? 2 : stick;
    s.value = static_cast<SHORT>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    s.held_until = value == 0 ? std::chrono::steady_clock::time_point{}
                              : std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    s.held_epoch = value == 0 ? 0 : epoch;
    return true;
}
bool held_stick(int& stick, SHORT& value) noexcept {
    native_input_source::PublicationScope publication;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    // Native-only readers may never poll XInput to retire a completed script.
    if (s.active && s.current + 1 == s.steps.size() && now - s.done_at > linger) { native_input_source::publication_changed(); s.active = false; }
    if (!s.held_epoch) return false;
    if (!preview_valid(s, s.held_epoch, GetTickCount64())) {
        revoke_preview(s); return false;
    }
    if (s.active || now >= s.held_until) return false;
    stick = s.stick;
    value = s.value;
    return true;
}
bool scripted_pad(DWORD user, XINPUT_STATE &output, input::PadAuthorship* authorship, bool physical_connected) noexcept {
    if (authorship) *authorship = {};
    native_input_source::PublicationScope publication;
    auto &s = state();
    if (user != 0) return false;
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!s.active) {
        if (!s.held_epoch) return false;
        if (!preview_valid(s, s.held_epoch, GetTickCount64())) { revoke_preview(s); return false; }
        if (!physical_connected || now >= s.held_until) return false;
        SHORT* axes[] = {&output.Gamepad.sThumbLX, &output.Gamepad.sThumbLY, &output.Gamepad.sThumbRX, &output.Gamepad.sThumbRY};
        *axes[s.stick] = s.value;
        output.dwPacketNumber = ++s.packet;
        if (authorship) *authorship = {input::PadSource::preview_hold, s.held_epoch,
            static_cast<std::uint8_t>(s.stick), s.value};
        return true;
    }
    // With no game ticks, time alone moves the script on.
    while (s.current + 1 < s.steps.size() && now - s.last_tick > no_ticks) advance(s, s.last_tick + tick_length);
    const bool done = s.current + 1 == s.steps.size();
    if (done && now - s.done_at > linger) {
        native_input_source::publication_changed();
        s.active = false;
        return false;
    }
    output.Gamepad = done ? XINPUT_GAMEPAD{} : s.steps[s.current].pad;
    output.dwPacketNumber = ++s.packet;
    if (authorship) *authorship = {input::PadSource::sdk_navigation, 0};
    return true;
}
bool scripted_pad_active() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.active || (std::chrono::steady_clock::now() < s.held_until &&
        preview_valid(s, s.held_epoch, GetTickCount64()));
}
} // namespace dingosdk::overlay
