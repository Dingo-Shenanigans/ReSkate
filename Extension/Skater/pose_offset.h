#pragma once
#include "Extension/AnimationStudio/anim_core.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A joint moved, turned and scaled on top of the pose as it is drawn (reskate.skater.pose's offset,
// rotation and scale, with no place and no kept children), rather than on the pose as the animation
// left it. The game's body physics changes the pose after the animation update and before it is drawn
// (the hips as the skater pops from standing still, among others); a joint written whole from the
// animation's pose loses that change while its children keep theirs, and the feet went into the deck
// (Wheel Size, 2026-10-29: a pose of the hips by 0 put them there on 5 of 5 tricks from standing
// still, none of 6 without it). Pure maths; skater_animation.cpp applies it when the pose is drawn.
namespace dingosdk::pose_offset {
using anim_studio::Joint;
using anim_studio::Quat;
using anim_studio::Vec3;

// The smallest scale a joint is given (skater_animation.cpp's): a joint at scale 0 has no inverse.
inline constexpr float smallest_scale = 0.001f;

// What goes on one joint: turned in its own axes, scaled, then moved in its parent's frame.
struct Delta {
    std::uint16_t joint{};
    Quat rotation{0, 0, 0, 1};
    float scale{1};
    Vec3 offset{};
};

// One offset at `weight` (0..1) added to `out`, after any already there for the same joint, in the
// order pose() gives them.
void add(std::vector<Delta>& out, std::uint16_t joint, const Quat& rotation, float scale, const Vec3& offset, float weight);

// `at` with `delta` on it, the same as skater_animation.cpp's pose_joints does to the animation's pose.
Joint apply(const Joint& at, const Delta& delta) noexcept;

// A joint as it was before a delta went on, and as it was written. A pose that comes to be drawn
// again still holding what was written (the renderer's copy taken, the game's values not put back yet)
// gets the delta on the joint it was written from, not on top of itself.
struct Written {
    Joint before, after;
};
Joint before_delta(const Joint& now, const std::optional<Written>& last) noexcept;
}
