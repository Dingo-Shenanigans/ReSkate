#pragma once
#include "Extension/AnimationStudio/anim_core.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

// An arm that reaches: the hand joint of the local skater's pose taken to a point in the world, and
// turned to a rotation, by bending the upper arm and forearm (the elbow out from the body and a little
// down and back) and turning the hand. Pure maths on the 395-joint AnimBase pose, each joint relative
// to its parent and joint 0 composing into the world (Extension/Skater/client_first_person.cpp); made
// from the Filmer's arm holding its camera (neekgate/ReSkate, filmer-zee, filmer_hold.cpp).
// skater_animation.cpp applies it as the pose is evaluated, drawn and sent.
namespace dingosdk::arm_reach {
using anim_studio::Joint;
using anim_studio::Quat;
using anim_studio::Vec3;

enum class Hand : std::uint8_t { right, left };
// From the pose's root to the hand: chest (45), shoulder, upper arm, forearm, hand.
inline constexpr std::array<std::uint16_t, 11> right_chain{0, 1, 7, 42, 43, 44, 45, 46, 47, 48, 49};
inline constexpr std::array<std::uint16_t, 11> left_chain{0, 1, 7, 42, 43, 44, 45, 275, 276, 277, 278};
inline constexpr std::span<const std::uint16_t> chain(Hand hand) noexcept { return hand == Hand::left ? left_chain : right_chain; }
// The joints a reach writes: the upper arm to the hand, one after another in the pose.
inline constexpr std::uint16_t first_written(Hand hand) noexcept { return hand == Hand::left ? 276 : 47; }
inline constexpr std::uint16_t last_written(Hand hand) noexcept { return hand == Hand::left ? 278 : 49; }

struct Placed {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
    float scale = 1.0f;
};
struct Goal {
    Hand hand = Hand::right;
    Vec3 at{};                   // where the hand joint should be, in the world
    std::optional<Quat> rotation; // the hand joint's rotation in the world; none: as the forearm leaves it
    float weight = 1.0f;          // 0: the arm as the animation has it, 1: reaching
};
struct Reached {
    bool valid{};
    Placed hand; // where the hand joint is, after the reach
    bool reached{}; // the point was within the arm's reach
};

// Every joint along `joints` (each the parent of the next) composed into the world; false when one is
// missing or not a number.
bool compose(std::span<const Joint> pose, std::span<const std::uint16_t> joints, std::span<Placed> out) noexcept;
// Bends the arm in `pose` (in place) toward the goal. Leaves the pose alone and returns invalid when the
// joints are not there, or the arm's bones are not an arm's length.
Reached reach(std::span<Joint> pose, const Goal& goal) noexcept;
// Where the hand joint is in the pose as it is.
Reached hand_of(std::span<const Joint> pose, Hand hand) noexcept;
}
