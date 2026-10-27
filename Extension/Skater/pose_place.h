#pragma once
#include "Extension/AnimationStudio/anim_core.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A joint set to a place and turn instead of moved on top of the animation (reskate.skater.pose's
// place): in its parent's frame, in another joint's (a chain of joints that do not hang from each
// other, such as a tail made of spare joints), in the skater's or in the world. The skeleton's parents
// never change: each frame the place asked for is turned into the joint's own value under its real
// parent. A joint's children can stay where they were in the world while it moves (keep). Pure maths
// on the 395-joint pose; skater_animation.cpp applies it with the rest of a pose.
namespace dingosdk::pose_place {
using anim_studio::Joint;
using anim_studio::Quat;
using anim_studio::Vec3;

enum class Frame : std::uint8_t { parent, joint, skater, world };
struct Place {
    std::optional<Vec3> position; // left out: where the animation has it, in the same frame
    std::optional<Quat> rotation;
    Frame frame{Frame::parent};
    std::uint16_t joint{}; // the joint it is placed relative to, with Frame::joint
};

// The order to pose joints in, as indices into `joints`: each after the posed joints it hangs from,
// and after the joint it is placed relative to and the posed joints that one hangs from, so it is
// placed against where they are this frame. `relative_to[i]` is that joint, or -1. Empty with the
// reason when no order works: a joint placed relative to itself or to a joint that hangs from it, or
// joints placed relative to each other in a circle.
std::string order(std::span<const std::uint16_t> joints, std::span<const int> relative_to, std::vector<std::size_t>& out);

// Sets `joint` of `pose` (parent-local values, as the game holds them) to `place`, `weight` 0..1 of
// the way from where it is. Its scale is left as it is.
void apply(std::span<Joint> pose, std::uint16_t joint, const Place& place, float weight) noexcept;

// A joint's children where they are in the world, taken before it moves; put back after, so they stay
// where they were (and what hangs from them with them).
struct Kept {
    std::uint16_t joint;
    Joint world;
};
void keep(std::span<const Joint> pose, std::uint16_t joint, std::vector<Kept>& out);
void put_back(std::span<Joint> pose, std::uint16_t joint, std::span<const Kept> kept) noexcept;
} // namespace dingosdk::pose_place
