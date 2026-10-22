#pragma once
// The filmer's camera in their hand: the arm that holds it out and where the camera sits.
//
// Pure maths, no game code, so it is tested on its own. The skater's animation pose is the 395-joint
// AnimBase skeleton, each joint {scale, Hamilton quaternion, translation} relative to its parent, and
// composing from joint 0 lands in the world (joint 1 carries the skater's placement); see
// Extension/Skater/client_first_person.cpp. The arms hang off Spine3 (45): the right arm is 46
// RightShoulder, 47 RightArm, 48 RightForeArm, 49 RightHand; the left 275, 276, 277, 278
// (Extension/Multiplayer/Net/skater_bone_parents.h).
//
// The camera is held by its top handle. The model's origin is the middle of that handle and its lens
// looks down its -Z (assets/models/vx1000/prepare_camcorder.py), so the model is placed at the palm and
// turned to where the filmer aims.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dingosdk::filmer::hold {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

struct Bone {
    Vec3 scale{1, 1, 1};
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
};
struct Placed {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
    float scale = 1.0f;
};

enum class Hand : std::uint8_t { right, left };
// Joints from the pose's root to the hand: the arm's last four are shoulder, upper arm, forearm, hand.
inline constexpr std::array<std::uint16_t, 11> right_chain{0, 1, 7, 42, 43, 44, 45, 46, 47, 48, 49};
inline constexpr std::array<std::uint16_t, 11> left_chain{0, 1, 7, 42, 43, 44, 45, 275, 276, 277, 278};
inline constexpr std::uint16_t chest_joint = 45;
inline constexpr std::size_t pose_joints = 395;
inline constexpr std::span<const std::uint16_t> chain(Hand hand) noexcept { return hand == Hand::left ? left_chain : right_chain; }
// The legs, from the root: the last three are thigh, shin and foot. The hips (7) carry them and the spine.
inline constexpr std::array<std::uint16_t, 6> right_leg{0, 1, 7, 8, 9, 10};
inline constexpr std::array<std::uint16_t, 6> left_leg{0, 1, 7, 341, 342, 343};
inline constexpr std::uint16_t hips_joint = 7;
// The back and head, from the root: Spine, Spine1, Spine2, Spine3, Neck, Neck1, Head.
inline constexpr std::array<std::uint16_t, 10> head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
inline constexpr std::array<std::uint16_t, 7> hunched_joints{42, 43, 44, 45, 101, 102, 103};
// Every joint hold() may change, for whoever copies its result out (the hips move; the rest turn).
inline constexpr std::array<std::uint16_t, 9> moved_joints(Hand hand) noexcept {
    return {hips_joint, 8, 9, 10, 341, 342, 343, hand == Hand::left ? std::uint16_t{276} : std::uint16_t{47},
            hand == Hand::left ? std::uint16_t{277} : std::uint16_t{48}};
}
inline constexpr std::uint16_t hand_joint(Hand hand) noexcept { return hand == Hand::left ? 278 : 49; }

// The camera as the model has it: from the handle (the origin) to the front of the lens, and from the
// wrist joint to the middle of the palm along the hand.
inline constexpr Vec3 lens_from_handle{0.0f, -0.143f, -0.248f};
inline constexpr float palm_from_wrist = 0.075f;

// The camcorder worn as hand wear (a cosmetic, assets/models/vx1000/make_cosmetic.py): its mesh is
// weighted wholly to the hand, its handle bar through the fist at <Hand>_Attach. `camera` is the
// camera's rows (right, up, back) in the hand joint's own frame and `from_wrist` where its grip is
// from the hand joint, in that frame, both from the skeleton's bind pose.
struct Grip {
    Quat camera{0, 0, 0, 1};
    Vec3 from_wrist{};
};
inline constexpr Grip right_grip{{0.5398f, -0.5523f, 0.4688f, 0.4287f}, {0.1191f, 0.0435f, -0.0251f}};
inline constexpr Grip left_grip{{0.4688f, -0.4287f, 0.5398f, 0.5523f}, {0.1191f, 0.0435f, 0.0251f}};
inline constexpr const Grip &grip(Hand hand) noexcept { return hand == Hand::left ? left_grip : right_grip; }

// ---- Vector and quaternion helpers.
Quat multiply(const Quat &a, const Quat &b) noexcept;
Quat conjugate(const Quat &q) noexcept;
Vec3 rotate(const Quat &q, const Vec3 &v) noexcept;
Quat normalized(const Quat &q) noexcept;
// The shortest turn taking direction `from` onto direction `to`.
Quat arc(const Vec3 &from, const Vec3 &to) noexcept;
// Part way from a to b, the short way round.
Quat blend(const Quat &a, Quat b, float t) noexcept;
// From a matrix whose rows are right, up, back (the camera's and the flight model's), and back.
Quat from_rows(const std::array<float, 16> &m) noexcept;
// A turn of `yaw` about up then `pitch` about the result's right (radians): the camera's rows for a
// heading and a tilt.
Quat from_yaw_pitch(float yaw, float pitch) noexcept;
// The way a camera faces round the upright (level, unit), steady however steeply it points up or
// down: straight down, its forward has no level part left, but its up does (the top of the picture is
// the way it faces).
Vec3 level_heading(const Quat &aim) noexcept;

// Every joint along `joints` (each the parent of the next, the first hanging from nothing) composed
// into the world. `out` gets one per joint; false when a joint is missing or not a number.
bool compose(std::span<const Bone> pose, std::span<const std::uint16_t> joints, std::span<Placed> out) noexcept;

struct Goal {
    Hand hand = Hand::right;
    Vec3 lens{};          // where the front of the lens should be (the camera's eye)
    Quat aim{0, 0, 0, 1}; // which way the camera points (rows right, up, back)
    float weight = 1.0f;  // 0: the arm as the animation has it, 1: holding the camera out
    // The camcorder is worn on the hand (Grip): the hand turns as well, so its lens points along `aim`.
    bool worn{};
    // How far the hips go down (metres, before `weight`), the feet kept where they are: a filmer's crouch.
    float crouch{};
    // How far the back curls over toward the camera and the head looks down at it (radians, before
    // `weight`; the back takes this, the neck and head half again).
    float hunch{};
    // The arm goes round the body: a hand target inside the torso, or an arm that would pass through
    // it, is pushed out to its surface.
    bool around_body = true;
    // The other hand holds the camera too, from the side and underneath, as a filmer cradles it.
    bool two_hands{};
    // The lens is reached for where it is (no holding it out in front and below the shoulder): the
    // supporting hand's reach.
    bool free_reach{};
    // The back, neck and head start from a still standing pose, not the game's (its idle fidgets do
    // not show through): on foot.
    bool still{};
    // The whole body turned round the upright through the hips (radians): a pivot on the spot after a
    // camera turned past the range.
    float pivot{};
    // Turning round on the spot: the feet stepping round, one then the other (`step` 0..1 how much;
    // `step_phase` where in the stride, a whole stride per 1).
    float step{}, step_phase{};
};
// How much a pose is pushing on a board (0 to 1): one foot down on the ground well below the other.
float pushing(std::span<const Bone> pose) noexcept;
// The back, neck and head as the skeleton stands (each joint's own turn, from its bind pose).
inline constexpr std::array<Quat, 7> standing_back{{{0.0f, 0.0f, 0.16592f, 0.98614f},
                                                    {0.0f, 0.0f, -0.19038f, 0.98171f},
                                                    {0.0f, 0.0f, -0.10393f, 0.99458f},
                                                    {0.0f, 0.0f, 0.22027f, 0.97544f},
                                                    {0.0f, 0.0f, 0.11357f, 0.99353f},
                                                    {0.0f, 0.0f, -0.19607f, 0.98059f},
                                                    {0.0f, 0.0f, -0.00959f, 0.99995f}}};
// Which way the body faces (level, in the world), from the line across its hips; {0, 0, 1} when the
// legs are not there.
Vec3 body_facing(std::span<const Bone> pose) noexcept;
// Where the supporting hand takes the camera, in the camera's own frame (right, up, back; the grip
// at the origin), for a right-hand grip: the left side of the body, underneath. Mirrored for a left grip.
inline constexpr Vec3 support_from_grip{-0.08f, -0.16f, -0.04f};
// The body as a column the arm keeps out of: about the hips' and chest's line, this far out at most
// (and inside the shoulders).
inline constexpr float body_radius = 0.24f;
// The lens is shown held at least this far out in front of the shoulder (along the aim), and at least
// this far below it.
inline constexpr float min_reach_out = 0.4f, below_shoulder = 0.05f;
// Aimed up, the lens comes up to this far below the shoulder.
inline constexpr float lift_below_shoulder = 0.15f;
// Pointed up at the sky the lens comes up to the eye (this far over the shoulder); pointed down, it is
// held over the stomach (this far under it).
inline constexpr float eye_above_shoulder = 0.18f, tucked_below_shoulder = 0.42f;
// Pointed straight down, the lens is held this far under the hips, almost between the feet.
inline constexpr float tucked_below_hips = 0.45f;
// Pointed up at the sky, held this far over the shoulder: above the head, its screen in front of the eyes.
inline constexpr float above_head_over_shoulder = 0.42f;
// Toward the holding hand's own side the camera comes up this much (all the way square to that side).
inline constexpr float side_lift = 0.2f;
struct Held {
    bool valid{};
    Placed palm; // where the camera's handle is, turned the way it points
    Vec3 lens{}; // where the front of its lens ended up (short of the goal when out of reach)
    bool reached{};
};
// Bends the arm in `pose` (its upper arm and forearm, in place, and the hand when the camera is worn) so the hand holds the camera with its
// lens as near the goal as the arm reaches, the elbow out and down, and says where the camera is.
// Leaves the pose alone and returns invalid when the joints are not there.
Held hold(std::span<Bone> pose, const Goal &goal) noexcept;
// Lowers the hips by `depth` (metres) and bends the knees so each foot stays where it was, turned as
// it was. False when the legs are not there (the pose is left alone).
// `twist`: the hips turned round the upright too (radians), the feet still kept.
bool crouch(std::span<Bone> pose, float depth, float twist = 0.0f) noexcept;
// Curls the back over toward `toward` (a direction in the world, level) by `angle` radians spread
// down the spine, and the neck and head on down by `look` more; first turns the upper body and the
// head round the upright by `twist` radians (most of it in the neck and head), to look that way.
bool hunch(std::span<Bone> pose, const Vec3 &toward, float angle, float look, float twist = 0.0f) noexcept;
// Where the camera is in a pose that already holds it (another player's, as they sent it): the palm
// of the hand, turned by the aim they sent.
Held held_in(std::span<const Bone> pose, Hand hand, const Quat &aim) noexcept;
} // namespace dingosdk::filmer::hold
