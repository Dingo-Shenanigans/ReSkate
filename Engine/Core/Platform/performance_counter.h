#pragma once

// Windows' performance counter. Windows only: nothing the Linux server builds may include this.
#ifndef _WIN32
#error "performance_counter.h is Windows only"
#endif

#include <Windows.h>
#include <cstdint>

namespace dingosdk {

// The raw QueryPerformanceCounter count, not a steady_clock reading: its users keep counts and
// turn them into time with QueryPerformanceFrequency themselves.
inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

} // namespace dingosdk
