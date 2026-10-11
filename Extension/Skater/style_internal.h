#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// The part of the style layer that only the style editor uses.
namespace dingosdk::style_layer {
// The rotations that the current style gives a flip trick at a timeline time.
void rotations_at(std::uint8_t trick, float time, const style::Pace &pace, std::vector<style::JointDelta> &out);
// The pace of the clip on the stand-in. A new keyframe starts as the pose shown at that pace.
void note_editor_pace(const style::Pace &pace);
// The layer does not change these poses: the stand-in and its board. 0 is none.
void ignore_holder(std::uintptr_t holder, std::uintptr_t board = 0) noexcept;
// True if the menu's skater was in its room in the last 500 ms. Always false unless watch_stage() was called in the last 2 s.
[[nodiscard]] bool stage_present() noexcept;
void watch_stage() noexcept;
// Adds one frame of a clip for the restyle of Skatepedia's skater. False until the skeleton is read.
bool add_demo(std::uint8_t trick, float time, const std::vector<multiplayer::Transform> &skater);
void clear_demos();
// Hides the menu's skater and board for the next 500 ms.
void keep_stage_clear() noexcept;
// The joints that the editor uses to read a recorded skeleton.
struct Rig {
    std::uint16_t trajectory{}, deck{}, left_foot{}, right_foot{};
    std::shared_ptr<const std::vector<std::int32_t>> parents;
};
[[nodiscard]] Rig rig();
// The completed recording from request_recording(). Empty before completion and after collection.
[[nodiscard]] std::vector<std::uint8_t> collect_recording();
// The editor camera's first framing: a little above level, 3.6 m back, aimed 0.1 m above the skater's root.
inline constexpr float editor_pitch = 0.05f, editor_distance = 3.6f, editor_height = 0.1f;
// The editor camera's yaw for a skater whose root has the rotation `q`: the skater's facing, turned by the editor's angle.
[[nodiscard]] inline float editor_yaw(const style::Quat &q) noexcept {
    const float along = std::atan2(1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[2] - q[3] * q[1]));
    return std::remainder(along + addr::style::editor_camera_turn, 6.2831853f);
}
// Writes a camera (rows right, up, backward, position) that looks at `target` raised by `height`, from `distance` away at `yaw`
// and `pitch` (radians). The camera keeps the handedness it had. False when the view has no right axis.
inline bool orbit_camera(std::array<float, 16> &matrix, float yaw, float pitch, float distance, const std::array<float, 3> &target,
                         float height) noexcept {
    const std::array<float, 3> back{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    std::array<float, 3> right{back[2], 0, -back[0]};
    const float length = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (length < 1e-4f) return false;
    for (auto &axis : right) axis /= length;
    const std::array<float, 3> up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                                  back[0] * right[1] - back[1] * right[0]};
    const std::array<float, 3> had{matrix[1] * matrix[6] - matrix[2] * matrix[5], matrix[2] * matrix[4] - matrix[0] * matrix[6],
                                   matrix[0] * matrix[5] - matrix[1] * matrix[4]};
    if (had[0] * matrix[8] + had[1] * matrix[9] + had[2] * matrix[10] < 0)
        for (auto &axis : right) axis = -axis;
    for (std::size_t i = 0; i < 3; ++i) {
        matrix[i] = right[i];
        matrix[4 + i] = up[i];
        matrix[8 + i] = back[i];
        matrix[12 + i] = target[i] + (i == 1 ? height : 0.0f) + back[i] * distance;
    }
    return true;
}
} // namespace dingosdk::style_layer
