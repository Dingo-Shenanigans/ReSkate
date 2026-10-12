// What the render thread runs: the local skater's style.
#include "style_live.h"
#include "style_layer.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace dingosdk::style_layer {
namespace {
using namespace addr::style;
using style::Family;
using style::Quat;
constexpr float blend_seconds = 0.08f; // smooths sudden changes, such as the catch or a slam

std::optional<Family> family_of(std::uint32_t state) noexcept {
    if (state >= riding_states_begin && state < riding_states_end) return Family::riding;
    if (state >= grind_states_begin && state < grind_states_end) return Family::grind;
    if (on_foot(state)) return Family::offboard;
    return std::nullopt;
}
bool write_pose(style::PoseTracker &tracker, std::uintptr_t holder, const std::vector<style::SkeletonDelta> &deltas) {
    if (deltas.empty() && tracker.adjusted().empty()) return false;
    const auto pose = pose_of(holder);
    if (!pose || pose->count != skeleton_joints) return false;
    const auto read = [&](std::uint16_t joint) -> std::optional<Quat> {
        Quat value{};
        if (!memory::peek(pose->rotation(joint), value)) return std::nullopt;
        return value;
    };
    tracker.begin();
    for (const auto &delta : deltas) {
        const auto now = read(delta.joint);
        if (!now) throw std::runtime_error("pose unreadable");
        if (!style::finite(*now)) continue; // the pose is not loaded yet
        const auto next = tracker.adjust(delta.joint, *now, delta.rotation);
        if (!memory::poke(pose->rotation(delta.joint), next)) throw std::runtime_error("pose unwritable");
    }
    tracker.end(read, [&](std::uint16_t joint, const Quat &base) noexcept { (void)memory::poke(pose->rotation(joint), base); });
    return true;
}
void apply(std::uintptr_t holder) {
    auto &l = live();
    const auto snapshot = l.followed.snapshot.load(std::memory_order_acquire);
    std::optional<Family> family;
    std::uint32_t state{};
    const bool known = memory::peek(l.followed.context.load(std::memory_order_acquire) + context_physics_state, state);
    if (known) {
        l.followed.state.store(state, std::memory_order_relaxed);
        family = family_of(state);
    }
    std::int32_t flip{-1};
    const auto selection = l.followed.trick_selection.load(std::memory_order_acquire);
    const bool flip_known = selection && memory::peek(selection + trick_selection_flip_trick, flip);
    std::lock_guard lock(l.pose.mutex);
    const auto micro = multiplayer::now_us();
    const auto moment = l.pose.trick.step(flip_known ? flip : -1, grounded(state),
                                          known && family.has_value(), family == Family::riding, micro / 1000);
    l.pose.merged.clear();
    if (family && snapshot && l.followed.active.load(std::memory_order_acquire))
        merged(*snapshot, *family, moment.trick, moment.time, l.pose.trick.pace(moment.trick), l.pose.timeline, l.pose.merged);
    const auto elapsed =
        l.pose.blended_at ? static_cast<float>(std::min<std::uint64_t>(micro - l.pose.blended_at, 100000)) / 1000000.0f : 0.0f;
    l.pose.blended_at = micro;
    const auto &written = l.pose.blender.step(l.pose.merged, style::ease_amount(elapsed, blend_seconds));
    (void)write_pose(l.pose.tracker, holder, written);
}
} // namespace

Live &live() { static auto *value = new Live; return *value; }
std::optional<PoseView> pose_of(std::uintptr_t holder) noexcept {
    try {
        const auto layout = multiplayer::read_native_pose_layout(memory::peek_bytes, live().followed.base.load(std::memory_order_acquire),
                                                                 holder, style::max_rig_joints);
        if (!layout.buffer) return std::nullopt;
        return PoseView{layout.buffer, layout.count};
    } catch (...) {
        return std::nullopt;
    }
}
void merged(const Snapshot &snapshot, style::Family family, std::uint8_t trick, float time, const style::Pace &pace,
            std::vector<style::JointDelta> &timeline, std::vector<style::SkeletonDelta> &out) {
    out.clear();
    const auto add = [&](const style::JointDelta &delta) {
        const auto joint = snapshot.skeleton_joint[delta.joint];
        if (!joint) return;
        const auto same = std::ranges::find(out, *joint, &style::SkeletonDelta::joint);
        if (same == out.end()) out.push_back({*joint, delta.rotation});
        else same->rotation = delta.rotation;
    };
    for (const auto &delta : snapshot.families[static_cast<std::size_t>(family)]) add(delta);
    if (!style::is_flip_trick(trick)) return;
    style::evaluate(snapshot.tricks[trick], time, timeline, pace);
    for (const auto &delta : timeline) add(delta);
}
// Client thread. With sharing off, other players get the game's own rotations.
void filter_capture(std::uintptr_t component, std::vector<multiplayer::Transform> &skater) noexcept {
    auto &l = live();
    if (l.followed.share.load(std::memory_order_acquire) || component != l.followed.component.load(std::memory_order_acquire)) return;
    std::lock_guard lock(l.pose.mutex);
    for (const auto joint : l.pose.tracker.adjusted())
        if (joint < skater.size())
            if (const auto *base = l.pose.tracker.base(joint, skater[joint].rotation)) skater[joint].rotation = *base;
}
// The render pose handoff, on the engine thread after the client tick. The layer writes here because physics rewrites joints earlier.
void on_render(std::uintptr_t animation_interface) noexcept {
    struct PreserveError { DWORD value = GetLastError(); ~PreserveError() { SetLastError(value); } } preserve_error;
    auto &l = live();
    const auto holder = l.followed.holder.load(std::memory_order_acquire);
    if (!holder || animation_interface - addr::style::holder_animation_interface != holder) return;
    try {
        apply(holder);
    } catch (...) {}
}
} // namespace dingosdk::style_layer
