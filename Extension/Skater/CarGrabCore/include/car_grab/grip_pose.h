#pragma once
#include "reach_pose.h"
#include <array>

namespace car_grab::grip {
using reach::Joint;
using reach::Quaternion;
// Thumb first, then index, middle, ring and pinky. The thumb's palm is identity.
struct Finger { Joint palm; std::array<Joint,3> joints; };
struct Hand { bool left{}; std::array<Finger,5> fingers; };
struct Surface {
    Vec3 point{}, normal{}, tangent{}, velocity{};
    bool wrap_valid{};
    Vec3 top_point{}, bottom_point{}, top_normal{0,1,0}, bottom_normal{0,-1,0};
};
struct Choices {
    float extra_clearance{};
    std::array<float,5> depth{1,1,1,1,1}, pole{};
};
// Only geometric solver choices are retained, never native joint rotations or
// pointers. The owner resets this when the attachment/surface lease changes.
struct Continuity { bool have{}; Choices selected; };
struct Solution {
    bool available{}, clamped{}, palm_contact{}, grip_valid{}, finger_pose_available{}, visual_safe{};
    Quaternion upper{0,0,0,1}, lower{0,0,0,1}, wrist{0,0,0,1};
    std::array<std::array<Quaternion,3>,5> fingers{};
    Vec3 shoulder{}, palm_position{};
    float minimum_reach{}, maximum_reach{}, palm_error{}, palm_clearance{};
    Choices choices;
};
// Contact uses a minimum 12 mm palm proxy, increased only by measured thumb
// root protrusion. This is not an undocumented socket or exact skin mesh.
// Fingers use live joint lengths and a conservative 8 mm bone clearance.
// No translation, scale, body transform or physics bone is produced.
Solution solve(const reach::Arm&, const Hand&, const Surface&, float weight,
               float prediction_seconds = 0) noexcept;
Solution solve_continuous(const reach::Arm&, const Hand&, const Surface&, float weight,
                          Continuity&, float prediction_seconds = 0) noexcept;
} // namespace car_grab::grip
