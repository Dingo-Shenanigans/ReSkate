#pragma once
#include "movement.hpp"

namespace bmx {
// One ACTUALLY OBSERVED peg/rail contact, not a rail-search or snap request.
// A host must use its collision query to detect acquisition, separation, width
// and obstacle exits. Both tangent and normal are world-space unit directions.
struct GrindContact {
    bool available{},touching{};
    Vec3 point{},normal{0,1,0},tangent{0,0,1},surface_velocity{};
    double separation{},friction{.12};
};
struct GrindResult {
    bool valid{},active{};
    Vec3 force{},torque{};
    double speed{},normal_impulse{},friction_impulse{};
};
// Reference fixed-step unilateral contact and dissipative sliding friction. Gravity
// is considered while solving, but is NOT included in the returned force.
// Replace the host's response for this same contact with this result; never
// apply both this and another contact solver to the same peg/rail contact.
// No pose snapping, movement-path forcing, trick naming or scoring is supplied.
GrindResult solve_grind(const Body&,const GrindContact&,Vec3 gravity = {0,-9.8,0},double seconds = fixed_step) noexcept;
}
