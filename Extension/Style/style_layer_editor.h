#pragma once
#include "Extension/Style/style_internal.h"
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace dingosdk::style_layer {
inline constexpr std::uint32_t recording_ms = 4000;
// Meanwhile the local skater shows the game's own animation, without the style.
void request_recording();
// Once per recording. Empty when no rig was recorded.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> collect_recording();
struct LocalTrick {
    std::uint32_t state{};
    std::int32_t flip{-1}; // the game's FlipTrickType, -1 for none
};
[[nodiscard]] std::optional<LocalTrick> local_trick() noexcept;
// Plain riding only, not the state after a landing.
[[nodiscard]] constexpr bool riding_still(std::uint32_t state) noexcept { return state == addr::style::riding_ground_state; }
// The layer does not change these poses: the stand-in and its board. 0 is none.
void ignore_holder(std::uintptr_t holder, std::uintptr_t board = 0) noexcept;
// True if the menu's skater was in its room in the last 500 ms. Always false unless watch_room() was called in the last 2 s.
[[nodiscard]] bool room_present() noexcept;
void watch_room() noexcept;
// Hides the menu's skater and board for the next 500 ms.
void keep_room_clear() noexcept;
// Ends keep_room_clear now, so the menu's skater and board come back while the room still shows.
void release_room() noexcept;
void rotations_at(std::uint8_t trick, float time, const style::Pace &pace, std::vector<style::SkeletonDelta> &out);
[[nodiscard]] Rig rig();
} // namespace dingosdk::style_layer
