#pragma once
#include <cstdint>

namespace dingosdk::plugins::joystick::detail {
// A caller can publish a request after the worker sampled its clock. That request is recent;
// subtracting it from the older clock value would wrap and incorrectly discard active input.
constexpr bool request_is_recent(std::uint64_t now, std::uint64_t asked_at, std::uint64_t timeout_ms) noexcept {
    return asked_at >= now || now - asked_at <= timeout_ms;
}
}
