#pragma once
#include "Extension/Skater/arm_reach.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

// Something held out in the hands and aimed, the way a person holds a camera or a phone they point:
// level, out in front at the chest; aimed down, lower and closer, down to the stomach; aimed up, up to
// the eye and over the head. The elbows keep a bend and the arms go round the body; the other hand can
// hold it too, from the side and underneath. The body can crouch over it (the feet kept where they
// are), curl its back over it and look down at it, turn on the spot after it and step round as it does.
// Pure maths on the 395-joint AnimBase pose (arm_reach.h), made from the Filmer's arm holding its
// camera (PR 343, filmer_hold.cpp); skater_animation.cpp applies it as the pose is evaluated, drawn and sent.
namespace dingosdk::carry {
using anim_studio::Joint;
using anim_studio::Quat;
using anim_studio::Vec3;
using arm_reach::Hand;
using arm_reach::Placed;

// The legs from the root: the last three are thigh, shin and foot. The hips (7) carry them and the spine.
inline constexpr std::array<std::uint16_t, 6> right_leg{0, 1, 7, 8, 9, 10};
inline constexpr std::array<std::uint16_t, 6> left_leg{0, 1, 7, 341, 342, 343};
inline constexpr std::uint16_t hips_joint = 7;
// The back and head from the root: Spine, Spine1, Spine2, Spine3, Neck, Neck1, Head.
inline constexpr std::array<std::uint16_t, 10> head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
inline constexpr std::array<std::uint16_t, 7> back_joints{42, 43, 44, 45, 101, 102, 103};
// The back, neck and head as the skeleton stands (each joint's own turn, from its bind pose).
inline constexpr std::array<Quat, 7> standing_back{{{0.0f, 0.0f, 0.16592f, 0.98614f},
                                                    {0.0f, 0.0f, -0.19038f, 0.98171f},
                                                    {0.0f, 0.0f, -0.10393f, 0.99458f},
                                                    {0.0f, 0.0f, 0.22027f, 0.97544f},
                                                    {0.0f, 0.0f, 0.11357f, 0.99353f},
                                                    {0.0f, 0.0f, -0.19607f, 0.98059f},
                                                    {0.0f, 0.0f, -0.00959f, 0.99995f}}};
// Every joint carry() may write, lowest first: the hips and legs, the back and head, both arms.
inline constexpr std::array<std::uint16_t, 22> written{7, 8, 9, 10, 42, 43, 44, 45, 46, 47, 48, 49, 101, 102, 103, 275, 276, 277, 278, 341, 342, 343};

// Worn on the hand (a cosmetic weighted wholly to the hand): the item's rows (right, up, back) in the
// hand joint's own frame, and where its grip is from the hand joint, in that frame, both from the
// skeleton's bind pose. The hand turns so the item points along the aim.
struct Grip {
    Quat item{0, 0, 0, 1};
    Vec3 from_wrist{};
};
struct Goal {
    Hand hand = Hand::right;
    Vec3 front{};          // where the item's front (a lens, a screen) should be, in the world
    Quat aim{0, 0, 0, 1};  // the item's rotation in the world: its -Z the way it points, +Y its top
    Vec3 front_from_grip{}; // its front from where the hand grips it, in its own frame
    std::optional<Grip> worn;
    float weight = 1.0f;   // 0: the body as the animation has it, 1: carrying
    bool both{};           // the other hand holds it too, from the side and underneath
    float crouch{};        // how far the hips go down (metres), the feet kept where they are
    float hunch{};         // how far the back curls over it (radians), the head looking down at it
    bool still{};          // the back, neck and head from a still standing pose (no idle fidgets)
    float turn{};          // the whole body turned round the upright through the hips (radians)
    float step{}, step_phase{}; // turning on the spot: how much the feet step (0..1), where in the stride (one per 1)
};
struct Held {
    bool valid{};
    Placed item;      // where the item's grip is, turned the way it points
    Vec3 front{};     // where its front ended up (short of the goal when out of reach)
    bool reached{};
};
// Bends the pose (in place) to carry the item as the goal says. Leaves the pose alone and returns
// invalid when the joints are not there, or the arm's bones are not an arm's length.
Held carry(std::span<Joint> pose, const Goal& goal) noexcept;
// How much a pose is pushing on a board (0 to 1): one foot down on the ground well below the other.
float pushing(std::span<const Joint> pose) noexcept;
// Which way the body faces (level, in the world), from the line across its hips; {0, 0, 1} when the
// legs are not there.
Vec3 body_facing(std::span<const Joint> pose) noexcept;
// Lowers the hips by `depth` (metres) and turns them round the upright by `twist` (radians), bending
// the knees so each foot stays where it was, turned as it was. False when the legs are not there.
bool crouch(std::span<Joint> pose, float depth, float twist = 0.0f) noexcept;
// Curls the back over toward `toward` (a direction in the world, level) by `angle` radians spread
// down the spine, and the neck and head on down by `look` more; first turns the upper body and the
// head round the upright by `twist` radians (most of it in the neck and head).
bool hunch(std::span<Joint> pose, const Vec3& toward, float angle, float look, float twist = 0.0f) noexcept;
}
