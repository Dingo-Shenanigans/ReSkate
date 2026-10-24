#pragma once
#include <atomic>

// A plugin's free camera flown from the pad (placing things from the air): the controller drives
// the flight, the bumpers (or Q/E) its height and the triggers stay the plugin's. Any thread.
namespace dingosdk::pad_flight {
inline std::atomic<bool>& requested_flag() noexcept { static std::atomic<bool> value{}; return value; }
inline void request(bool on) noexcept { requested_flag().store(on, std::memory_order_release); }
inline bool requested() noexcept { return requested_flag().load(std::memory_order_acquire); }
}
