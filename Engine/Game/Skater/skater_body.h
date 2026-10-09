#pragma once
#include "Engine/Game/Abi/linear_transform.h"
#include <array>
#include <cstddef>
#include <cstdint>

// The skater's body as the physics simulates it (Engine/Game/Build/20260929/skater_body.h):
// 24 rigid bodies, the ragdoll, and the contacts of each physics step. Plain data;
// Extension/HallOfMeat/hall_of_meat_skater.h reads it.
//
// The bodies by the physics bone id the contact processing reports. The ids follow the
// order of the game's physics bone name map (Engine/Game/Build/20260929/skater_body.h). The
// game confirms it twice: the contact code's feet-on-board test picks exactly 15, 16, 19 and
// 20, and the skeleton publisher maps each body to the animation joint of its name
// (rig+0x1ae0). Bone 0 is the board's root, never a body hit. The ragdoll has no head body of
// its own: NECK1, the upper neck, is the one the head rides on, so it is shown as the head.
namespace dingosdk::skater_body {
enum class Bone : std::uint8_t {
    board_root, neck1, neck, left_hand, left_forearm, left_arm, left_shoulder,
    right_hand, right_forearm, right_arm, right_shoulder, spine3, spine2, spine1, spine,
    left_toe, left_foot, left_leg, left_upleg, right_toe, right_foot, right_leg, right_upleg, hips,
};
inline constexpr std::size_t count = 24;
inline constexpr std::array<const char*, count> names{
    "Board", "Head", "Neck", "Left hand", "Left forearm", "Left upper arm", "Left collarbone",
    "Right hand", "Right forearm", "Right upper arm", "Right collarbone", "Upper chest", "Chest",
    "Lower back", "Spine", "Left toes", "Left foot", "Left shin", "Left thigh",
    "Right toes", "Right foot", "Right shin", "Right thigh", "Pelvis"};
constexpr std::size_t index(Bone bone) noexcept { return static_cast<std::size_t>(bone); }
// The feet touch the ground and the board all the time; their contacts do not tell
// whether a body is still tumbling.
constexpr bool foot(Bone bone) noexcept {
    return bone == Bone::left_toe || bone == Bone::left_foot || bone == Bone::right_toe || bone == Bone::right_foot;
}

using game::Vec3;

// What a body's hardest contact in one physics step hit, by the kind the game gives the other
// side. Kinds 5 and 11 are the game's, still to be named.
struct HitKinds {
    bool board{}, vehicle{}, world{}; // world: the ground, walls, rails and the rest of the static world
    bool kind_5{}, kind_11{};
};
// One body in one physics step: whether it touches something, and its hardest contact. Bodies
// only have contacts while the ragdoll simulates them (a bail): not while riding or walking.
struct BodyContact {
    bool touching{}; // a contact in this step
    float impact{};  // the hardest contact's speed along its normal, metres per second
    Vec3 slide{};    // its normal x relative velocity: the length is the speed along the surface
    HitKinds hit;
};
// The whole body in one physics step.
struct Contacts {
    std::array<BodyContact, count> bodies{};
};
}
