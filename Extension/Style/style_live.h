#pragma once
#include "Extension/Style/style_internal.h"
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Shared by the client and render threads. Users: style_layer.cpp, style_render.cpp.
namespace dingosdk::style_layer {
// A published snapshot never changes.
struct Snapshot {
    std::array<std::vector<style::JointDelta>, style::family_count> families;
    std::array<std::vector<style::Key>, style::flip_trick_count> tricks; // sorted by time
    std::array<std::optional<std::uint16_t>, style::editable_joints.size()> skeleton_joint{};
};
struct Live {
    struct Followed {
        std::atomic<std::uintptr_t> base{}, component{}, context{}, holder{}, trick_selection{};
        std::atomic<std::uint32_t> state{};
        std::atomic<bool> active{}, share{true};
        std::atomic<std::shared_ptr<const Snapshot>> snapshot;
    } followed;
    // The local pose bookkeeping. Guarded by `mutex`.
    struct Pose {
        std::mutex mutex;
        style::PoseTracker tracker{addr::style::skeleton_joints};
        std::vector<style::SkeletonDelta> merged;
        std::vector<style::JointDelta> timeline;
        style::TrickTracker trick;
        style::Blender blender{addr::style::skeleton_joints};
        std::uint64_t blended_at{};
    } pose;
};
Live &live();
// A rig's pose buffer in game memory: `count` bones of addr::style::bone_size bytes.
struct PoseView {
    std::uintptr_t buffer{};
    std::size_t count{};
    [[nodiscard]] std::uintptr_t scale(std::size_t joint) const noexcept { return buffer + joint * addr::style::bone_size; }
    [[nodiscard]] std::uintptr_t rotation(std::size_t joint) const noexcept { return scale(joint) + addr::style::bone_rotation; }
    [[nodiscard]] std::uintptr_t position(std::size_t joint) const noexcept { return scale(joint) + addr::style::bone_position; }
};
std::optional<PoseView> pose_of(std::uintptr_t holder) noexcept;
// A state family's rotations with a trick's keyframes at `time` on top. `timeline` is scratch space.
void merged(const Snapshot &snapshot, style::Family family, std::uint8_t trick, float time, const style::Pace &pace,
            std::vector<style::JointDelta> &timeline, std::vector<style::SkeletonDelta> &out);
// The render-pose handoff and the pose filter, which tick() installs.
void on_render(std::uintptr_t animation_interface) noexcept;
void filter_capture(std::uintptr_t component, std::vector<multiplayer::Transform> &skater) noexcept;
} // namespace dingosdk::style_layer
