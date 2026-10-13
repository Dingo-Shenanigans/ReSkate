#include "style_live.h"
#include "style_recording.h"
#include "trick_gestures.h"
#include "trick_pad.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace dingosdk::style_layer {
namespace {
using style::Quat;
constexpr std::size_t max_recording_bytes = 96u * 1024 * 1024, reserved_recording_bytes = 8u * 1024 * 1024;
constexpr auto &room_centre = addr::style::menu_room, &room_reach = addr::style::menu_room_reach;
// A rig in the menu room is hidden: a board moves this far down, and a rig below this height was moved. A hidden skater has this scale.
constexpr float hidden_drop_m = 2000.0f, moved_below_m = -1000.0f, hidden_skater_scale = 0.001f;
constexpr float hidden_scale_below = 0.01f;
constexpr std::uint64_t room_present_ms = 500;
// The fourth lane keeps its value.
bool write_scale(std::uintptr_t bone, float scale) noexcept { return memory::poke(bone, std::array<float, 3>{scale, scale, scale}); }
} // namespace

void clear_from_room(std::uintptr_t holder, bool skater, bool clear) noexcept {
    auto &l = live();
    const auto pose = pose_of(holder);
    if (!pose || style::rig_kind(pose->count) == style::RigKind::other) return;
    const std::size_t board_joints = std::min<std::size_t>(pose->count, style::max_board_joints);
    // A skater has its world position in joint 1. Each joint of a board has its own.
    const std::size_t first = skater ? style::skater_world : style::rig_deck;
    const std::size_t end = skater ? style::skater_world + 1 : board_joints;
    for (std::size_t joint = first; joint < end; ++joint) {
        const auto at = pose->position(joint);
        std::array<float, 3> position{};
        if (!memory::peek(at, position) || !std::isfinite(position[0] + position[1] + position[2])) continue;
        // An earlier frame moved it, and the game did not write the pose again.
        const bool moved = position[1] < moved_below_m;
        if (std::abs(position[0] - room_centre[0]) > room_reach[0] || std::abs(position[2] - room_centre[2]) > room_reach[2] ||
            std::abs(position[1] + (moved ? hidden_drop_m : 0.0f) - room_centre[1]) > room_reach[1]) {
            continue;
        }
        if (skater) l.room.present_until.store(GetTickCount64() + room_present_ms, std::memory_order_relaxed);
        // The game does not restore the scale, so the layer restores it when the skater is no longer hidden.
        if (skater && !clear) {
            float scale{};
            if (memory::peek(pose->scale(style::skater_world), scale) && scale < hidden_scale_below)
                (void)write_scale(pose->scale(style::skater_world), 1.0f);
        }
        if (!skater && !clear) {
            if (moved) {
                position[1] += hidden_drop_m;
                (void)memory::poke(at, position);
            }
            Quat scale{};
            if (joint == style::rig_deck && memory::peek(pose->scale(style::rig_deck), scale) && scale[0] == 0.0f && scale[1] == 0.0f)
                for (std::size_t each = 0; each < board_joints; ++each) (void)write_scale(pose->scale(each), 1.0f);
        }
        if (moved || !clear) continue;
        if (skater) {
            // A tiny scale hides the skater; a move or scale 0 can move the game's camera or hide the room.
            (void)write_scale(pose->scale(style::skater_world), hidden_skater_scale);
            continue;
        }
        position[1] -= hidden_drop_m;
        (void)memory::poke(at, position);
        // The game does not draw a board from joint 1, so each joint also gets scale 0.
        for (std::size_t each = 0; each < board_joints; ++each) (void)write_scale(pose->scale(each), 0.0f);
    }
}
// The time is the tick's, for any frame rate.
void record(std::uintptr_t holder) noexcept {
    auto &l = live();
    const auto pose = pose_of(holder);
    // Other rigs would fill the recording.
    if (!pose || style::rig_kind(pose->count) == style::RigKind::other) return;
    try {
        std::lock_guard lock(l.pose.mutex);
        if (l.pose.recording.size() > max_recording_bytes) return;
        if (l.pose.recording.empty()) l.pose.recording.reserve(reserved_recording_bytes);
        const auto tick = trick_pad::game_ticks();
        auto seen = std::ranges::find(l.pose.recorded_ticks, holder, &std::pair<std::uintptr_t, std::uint64_t>::first);
        if (seen == l.pose.recorded_ticks.end()) seen = l.pose.recorded_ticks.insert(seen, {holder, ~0ull});
        if (std::exchange(seen->second, tick) == tick) return;
        const auto at = l.pose.recording.size();
        const auto bones = style::append_rig(l.pose.recording, style::to_ms(tick), holder, pose->count);
        if (!memory::peek_bytes(pose->buffer, bones.data(), bones.size())) l.pose.recording.resize(at);
    } catch (...) {}
}
bool recording_now(const Live &l) noexcept { return trick_pad::game_ticks() < l.record_until.load(std::memory_order_relaxed); }
bool watching_room(const Live &l) noexcept { return GetTickCount64() < l.room.watch_until.load(std::memory_order_relaxed); }
bool clearing_room(const Live &l) noexcept { return GetTickCount64() < l.room.clear_until.load(std::memory_order_relaxed); }
} // namespace dingosdk::style_layer
