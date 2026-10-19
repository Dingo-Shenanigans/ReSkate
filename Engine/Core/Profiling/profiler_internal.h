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
// While a sample of the present thread runs: when each frame was presented (now_ns), so its report
// can give the slowest frames as the client one does. One writer, the present thread.
constexpr std::size_t present_mark_capacity = 1 << 14;
extern std::atomic<bool> marking_presents;
extern std::atomic<std::uint32_t> present_mark_count;
extern std::array<std::atomic<std::uint64_t>, present_mark_capacity> present_marks;
// Starts the once-a-second monitor if anything wants it.
void wake_monitor() noexcept;
// What a thread is, for reports: its description, or what its start address belongs to.
std::string thread_label(HANDLE thread, std::uint32_t id);
}
