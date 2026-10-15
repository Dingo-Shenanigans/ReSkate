#include "style_layer_editor.h"
#include "style_live.h"
#include "trick_gestures.h"
#include "trick_pad.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace dingosdk::style_layer {
namespace {
using namespace addr::style;
constexpr std::uint64_t room_watch_ms = 2000, room_clear_ms = 500;
// The editor collects a recording this long after it ends.
constexpr std::uint32_t record_grace_ms = 200;
} // namespace

void ignore_holder(std::uintptr_t holder, std::uintptr_t board) noexcept {
    live().room.ignored.store(holder, std::memory_order_release);
    live().room.ignored_board.store(board, std::memory_order_release);
}
std::optional<LocalTrick> local_trick() noexcept {
    auto &l = live();
    const auto selection = l.followed.trick_selection.load(std::memory_order_acquire);
    if (!selection) return std::nullopt;
    LocalTrick result;
    // Read now: the stored state stops while a recording keeps the game's animation.
    if (!memory::peek(l.followed.context.load(std::memory_order_acquire) + context_physics_state, result.state))
        result.state = l.followed.state.load(std::memory_order_relaxed);
    (void)memory::peek(selection + trick_selection_flip_trick, result.flip);
    return result;
}
bool room_present() noexcept { return GetTickCount64() < live().room.present_until.load(std::memory_order_relaxed); }
void keep_room_clear() noexcept { live().room.clear_until.store(GetTickCount64() + room_clear_ms, std::memory_order_relaxed); }
void release_room() noexcept { live().room.clear_until.store(0, std::memory_order_relaxed); }
void watch_room() noexcept { live().room.watch_until.store(GetTickCount64() + room_watch_ms, std::memory_order_relaxed); }
Rig rig() {
    const auto snapshot = live().followed.snapshot.load(std::memory_order_acquire);
    return snapshot ? snapshot->rig : Rig{};
}
std::optional<std::vector<std::uint8_t>> collect_recording() {
    auto &l = live();
    std::lock_guard lock(l.pose.mutex);
    return std::exchange(l.pose.recorded, std::nullopt);
}
void request_recording() {
    auto &l = live();
    std::lock_guard lock(l.pose.mutex);
    // A finished recording that the editor did not collect is for an earlier take.
    l.pose.recording.clear();
    l.pose.recorded.reset();
    l.pose.recorded_ticks.clear();
    l.record_started = trick_pad::game_ticks();
    l.record_until.store(l.record_started + style::to_ticks(recording_ms), std::memory_order_release);
}
void hand_over_recording(Live &l) {
    const auto until = l.record_until.load(std::memory_order_relaxed);
    if (!until || trick_pad::game_ticks() <= until + style::to_ticks(record_grace_ms)) return;
    l.record_until.store(0, std::memory_order_relaxed);
    std::size_t bytes{};
    {
        std::lock_guard lock(l.pose.mutex);
        bytes = l.pose.recording.size();
        l.pose.recorded = std::exchange(l.pose.recording, {});
    }
    logging::log(logging::Level::info, logging::Channel::skater, "Style: recorded {} bytes of the local skater and the boards in {} ms.",
                 bytes, style::to_ms(until - l.record_started));
}
void rotations_at(std::uint8_t trick, float time, const style::Pace &pace, std::vector<style::SkeletonDelta> &out) {
    out.clear();
    const auto snapshot = live().followed.snapshot.load(std::memory_order_acquire);
    if (!snapshot || !style::is_flip_trick(trick)) return;
    thread_local std::vector<style::JointDelta> timeline;
    timeline.clear();
    merged(*snapshot, style::Family::riding, trick, time, pace, timeline, out);
}
} // namespace dingosdk::style_layer
