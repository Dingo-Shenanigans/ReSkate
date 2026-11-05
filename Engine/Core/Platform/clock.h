#pragma once

#include <chrono>
#include <cstdint>

// Readings of the steady clock in the units ReSkate's timers keep. They are all the same clock,
// so values in one unit can be compared with each other, but not with GetTickCount64 or a
// QueryPerformanceCounter count, which start elsewhere.
namespace dingosdk {

inline std::uint64_t steady_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::uint64_t steady_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline double steady_seconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace dingosdk
