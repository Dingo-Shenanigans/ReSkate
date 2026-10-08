#pragma once
#include "profiler.h"
#include <Windows.h>
#include <array>

namespace dingosdk::profiler::detail {
// Threads the summary and the sampler name: the one running the client update, and the one
// presenting frames.
extern std::atomic<std::uint32_t> client_thread, present_thread;
// True while a sample runs; keeps the monitor (and so the zones) alive.
extern std::atomic<bool> sampling;
// When each frame was presented (now_ns), while a spike sample runs. Written by the present thread
// only, read by the sampler after it turns the log off.
inline constexpr std::size_t present_log_size = std::size_t{1} << 16;
extern std::atomic<bool> present_log_on;
extern std::array<std::atomic<std::uint64_t>, present_log_size> present_log;
extern std::atomic<std::uint32_t> present_log_count;
// Starts the once-a-second monitor if anything wants it.
void wake_monitor() noexcept;
// What a thread is, for reports: its description, or what its start address belongs to.
std::string thread_label(HANDLE thread, std::uint32_t id);
}
