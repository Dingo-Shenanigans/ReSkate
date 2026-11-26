#pragma once
#include <atomic>
#include <cstdint>

namespace dingosdk {
// The game's window and the graphics card it draws with, as the overlay found them setting up (Extension/UI/Overlay):
// for features that size the window or read the card's memory (Extension/Rendering/picture_shape.h). Any thread.
namespace game_window_detail {
inline std::atomic<void*> window{};
inline std::atomic<std::uint64_t> adapter{}; // its LUID: high << 32 | low (0: not known)
} // namespace game_window_detail
inline void publish_game_window(void* window, std::uint32_t adapter_low, std::int32_t adapter_high) noexcept {
    game_window_detail::adapter.store(static_cast<std::uint64_t>(static_cast<std::uint32_t>(adapter_high)) << 32 | adapter_low,
                                      std::memory_order_relaxed);
    game_window_detail::window.store(window, std::memory_order_release);
}
inline void* game_window() noexcept { return game_window_detail::window.load(std::memory_order_acquire); }
inline std::uint64_t game_adapter() noexcept { return game_window_detail::adapter.load(std::memory_order_relaxed); }
} // namespace dingosdk
