#pragma once
#include <cstdint>

namespace dingosdk::plugins::joystick::detail {
// A caller can publish a request after the worker sampled its clock. That request is recent;
// subtracting it from the older clock value would wrap and incorrectly discard active input.
constexpr bool request_is_recent(std::uint64_t now, std::uint64_t asked_at, std::uint64_t timeout_ms) noexcept {
    return asked_at >= now || now - asked_at <= timeout_ms;
}

// Whether a controller took the reader's axis range and zero dead zone, from the two HRESULTs
// SetProperty returned. Every axis is divided by that range, so a controller that refused it
// would report wrong values. Any success counts, the way SUCCEEDED() reads it. SetProperty's
// documented successes are DI_OK and DI_PROPNOEFFECT (S_FALSE, 1): the change had no effect, as
// when the property already has that value. Refusing it would leave out a controller already set up.
constexpr bool axis_configuration_applied(std::int32_t range_result, std::int32_t dead_zone_result) noexcept {
    return range_result >= 0 && dead_zone_result >= 0;
}
}
