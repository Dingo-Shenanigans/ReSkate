#pragma once
#include <cstdint>

namespace dingosdk::native_menu_observation {
enum class State : std::uint8_t { unknown, closed, open };
inline constexpr std::uint64_t maximum_age_ms = 100;

// The state and time are one publication, so a reader cannot combine different ticks.
void publish(State state, std::uint64_t now_ms) noexcept;
State read(std::uint64_t now_ms) noexcept;
bool known_open(std::uint64_t now_ms) noexcept;
bool known_closed(std::uint64_t now_ms) noexcept;
}
