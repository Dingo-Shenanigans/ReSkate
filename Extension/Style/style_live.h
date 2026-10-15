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
#include <utility>
#include <vector>

// Shared by the client and render threads. Users: style_layer.cpp, style_layer_editor.cpp, style_render.cpp, style_render_editor.cpp.
namespace dingosdk::style_layer {
// A published snapshot never changes.
struct Snapshot {
    std::array<std::vector<style::JointDelta>, style::family_count> families;
    std::array<std::vector<style::Key>, style::flip_trick_count> tricks; // sorted by time
    std::array<std::optional<std::uint16_t>, style::editable_joints.size()> skeleton_joint{};
    Rig rig;
};
struct Live {
    struct Followed {
        std::atomic<std::uintptr_t> base{}, component{}, context{}, holder{}, trick_selection{};
        std::atomic<std::uint32_t> state{};
        std::atomic<bool> active{}, share{true};
        std::atomic<std::shared_ptr<const Snapshot>> snapshot;
    } followed;
    // The local pose bookkeeping; the editor's recordings at the end. Guarded by `mutex`.
    struct Pose {
        std::mutex mutex;
        style::PoseTracker tracker{addr::style::skeleton_joints};
        std::vector<style::SkeletonDelta> merged;
        std::vector<style::JointDelta> timeline;
        style::TrickTracker trick;
        style::Blender blender{addr::style::skeleton_joints};
        std::uint64_t blended_at{};
        // The recording, the finished one until the editor collects it, and the last tick of each recorded rig.
        std::vector<std::uint8_t> recording;
        std::optional<std::vector<std::uint8_t>> recorded;
        std::vector<std::pair<std::uintptr_t, std::uint64_t>> recorded_ticks;
    } pose;
    // Keyed by a hash of the holder. Its own lock, so render calls do not wait on the pose lock.
    struct Kinds {
        struct Seen {
            std::uintptr_t holder{};
            std::uint64_t until{};
            style::RigKind kind{};
        };
        std::mutex mutex;
        std::array<Seen, 256> seen;
    } kinds;
    // In game ticks.
    std::atomic<std::uint64_t> record_until{};
    // In game ticks. Client thread only.
    std::uint64_t record_started{};
    struct Room {
        std::atomic<std::uint64_t> present_until{};
        std::atomic<std::uint64_t> clear_until{};
        std::atomic<std::uint64_t> watch_until{};
        std::atomic<std::uintptr_t> ignored{}; // the stand-in's pose holder, which the layer does not change
        std::atomic<std::uintptr_t> ignored_board{};
    } room;
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
// The editor's part of the layer (style_layer_editor.cpp, style_render_editor.cpp).
// Client thread, from tick().
void hand_over_recording(Live &l);
// Any thread.
[[nodiscard]] bool recording_now(const Live &l) noexcept;
// Once a game tick. Render thread; it takes pose.mutex.
void record(std::uintptr_t holder) noexcept;
// Any thread.
[[nodiscard]] bool watching_room(const Live &l) noexcept;
[[nodiscard]] bool clearing_room(const Live &l) noexcept;
// Hides a rig that is in the menu room, so that only the stand-in shows there. Render thread.
void clear_from_room(std::uintptr_t holder, bool skater, bool clear) noexcept;
} // namespace dingosdk::style_layer
