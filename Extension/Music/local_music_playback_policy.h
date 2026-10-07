#pragma once

#include <cstdint>
#include <array>

namespace dingosdk::profile_runtime::music_playback {
inline constexpr std::uint32_t native_default_mode = 0;
inline constexpr std::uint32_t native_sequential_mode = 1;
inline constexpr std::uint32_t cursor_sync_sentinel = 0x7fffffffu;

struct CursorPlan {
    std::uint32_t mode{};
    bool set_mode{};
    bool set_cursor{};
    std::uint32_t cursor{};
};

constexpr bool find_member_position(const std::uint8_t* members, std::uint32_t member_count,
                                    const std::array<std::uint8_t, 16>& guid,
                                    std::uint32_t& position) noexcept {
    if (!members) return false;
    for (std::uint32_t i = 0; i < member_count; ++i) {
        bool equal = true;
        for (std::uint32_t byte = 0; byte < guid.size(); ++byte)
            equal &= members[i * guid.size() + byte] == guid[byte];
        if (equal) { position = i; return true; }
    }
    return false;
}

// Plan a native transition without touching the game's data. The current position
// is in the mode-1 ordered-index vector after rebuilding it as identity order.
constexpr CursorPlan plan_cursor(bool shuffle, std::uint32_t current_mode,
                                 std::uint32_t current_cursor, bool new_group,
                                 bool explicit_request_pending, bool current_track_found,
                                 std::uint32_t current_order_position,
                                 std::uint32_t order_count) noexcept {
    const auto desired_mode = shuffle ? native_default_mode : native_sequential_mode;
    CursorPlan plan{desired_mode, current_mode != desired_mode, false, current_cursor};
    if (shuffle) return plan;

    if (explicit_request_pending) {
        plan.set_cursor = current_cursor != cursor_sync_sentinel || plan.set_mode || new_group;
        plan.cursor = cursor_sync_sentinel;
    } else if (new_group) {
        plan.set_cursor = true;
        plan.cursor = 0;
    } else if (plan.set_mode) {
        plan.set_cursor = true;
        plan.cursor = current_track_found && order_count
            ? (current_order_position + 1 == order_count ? 0 : current_order_position + 1)
            : 0;
    } else if (current_cursor == cursor_sync_sentinel) {
        // No queued manual song can synchronize the sentinel, so a fresh playlist
        // starts at its first authored entry.
        plan.set_cursor = true;
        plan.cursor = 0;
    }
    return plan;
}
} // namespace dingosdk::profile_runtime::music_playback
