#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Multiplayer/Net/protocol.h"
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
// True if Skatepedia's skater was on its stage in the last 500 ms. Always false unless watch_stage() was called in the last 2 s.
[[nodiscard]] bool stage_present() noexcept;
void watch_stage() noexcept;
// Adds one frame of a learned clip for the restyle of Skatepedia's skater. False until the skeleton is read.
bool add_demo(std::uint8_t trick, float time, const std::vector<multiplayer::Transform> &skater);
void clear_demos();
// Hides Skatepedia's skater and board for the next 500 ms.
void keep_stage_clear() noexcept;
// The joints that the editor uses to read a recorded skeleton.
struct Rig {
    std::uint16_t trajectory{}, deck{}, left_foot{}, right_foot{};
    std::shared_ptr<const std::vector<std::int32_t>> parents;
};
[[nodiscard]] Rig rig();
// The completed recording from request_learn(). Empty before completion and after collection.
[[nodiscard]] std::vector<std::uint8_t> collect_learned();
} // namespace dingosdk::style_layer
