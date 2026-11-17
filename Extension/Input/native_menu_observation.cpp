#include "native_menu_observation.h"
#include <atomic>

namespace dingosdk::native_menu_observation {
namespace {
constexpr std::uint64_t time_mask = (std::uint64_t{1} << 62) - 1;
std::atomic<std::uint64_t> observation{};
}
void publish(State state, std::uint64_t now_ms) noexcept {
    const auto value = state == State::unknown || !now_ms || now_ms > time_mask ? 0 :
        (static_cast<std::uint64_t>(state) << 62) | now_ms;
    observation.store(value, std::memory_order_release);
}
State read(std::uint64_t now_ms) noexcept {
    const auto value = observation.load(std::memory_order_acquire);
    const auto at = value & time_mask;
    if (!at || now_ms < at || now_ms - at > maximum_age_ms) return State::unknown;
    return static_cast<State>(value >> 62);
}
bool known_open(std::uint64_t now_ms) noexcept { return read(now_ms) == State::open; }
bool known_closed(std::uint64_t now_ms) noexcept { return read(now_ms) == State::closed; }
}
