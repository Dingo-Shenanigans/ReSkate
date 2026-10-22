#pragma once
#include "Engine/Game/BMX/bmx_bail_policy.h"
#include "Engine/Game/BMX/bmx_geometry.h"
#include <array>

namespace dingosdk::bmx::collision_envelope {
using Vec=std::array<float,3>;
using Region=bail_policy::Region;
inline constexpr Vec com_from_root=geometry::com_from_root;
struct Capsule {Vec a,b;float radius;};
// Coordinates are relative to the assembled bike root. Scale only the authored
// bike proxies with the normalized mesh. Human radii are retained; their authored locations below cover the retargeted
// pelvis/spine/head center grid, each in its own named region. This is joint-center
// coverage, not human skin thickness or a native collision execution proof.
// No animated skinned collision mesh or extended trick-limb coverage is claimed.
inline constexpr float bike_scale=static_cast<float>(geometry::wheel_radius/.34);
inline constexpr Vec bike_point(Vec p) noexcept {
    for(auto& v:p)v*=bike_scale;return p;
}
// These capsules do not spin with the axles. Keep the original 30mm outer
// recess behind the logical tire, including the fixed 5mm half-axis, rather
// than letting the original .305m radius protrude through a smaller tire.
inline constexpr float wheel_backup_half_axis=.005f;
inline constexpr float wheel_backup_recess=.03f;
inline constexpr float wheel_backup_radius=static_cast<float>(geometry::wheel_radius)-
    wheel_backup_recess-wheel_backup_half_axis;
static_assert(wheel_backup_radius>0);
inline constexpr std::array<Capsule,8> capsules{{
    {{0,geometry::front_axle[1]-wheel_backup_half_axis,geometry::front_axle[2]},
     {0,geometry::front_axle[1]+wheel_backup_half_axis,geometry::front_axle[2]},wheel_backup_radius},
    {{0,geometry::rear_axle[1]-wheel_backup_half_axis,geometry::rear_axle[2]},
     {0,geometry::rear_axle[1]+wheel_backup_half_axis,geometry::rear_axle[2]},wheel_backup_radius},
    {bike_point({0,.45f,-.30f}),bike_point({0,.60f,.20f}),.12f*bike_scale},
    {geometry::right_grip.center,geometry::left_grip.center,.055f*bike_scale},
    {bike_point({0,.73f,-.31f}),bike_point({0,.73f,-.09f}),.10f*bike_scale},
    {{-.12f,.89f,-.305f},{.12f,.89f,-.305f},.16f},
    {{0,.97f,-.30f},{0,1.20f,-.15f},.18f},
    {{0,1.30f,.135f},{0,1.50f,.135f},.16f}
}};
struct Guard {Vec center;float radius;Region region;};
inline constexpr std::array<Guard,8> guards{{
    {capsules[2].a,capsules[2].radius,Region::frame},
    {capsules[2].b,capsules[2].radius,Region::frame},
    {capsules[3].a,capsules[3].radius,Region::frame},
    {capsules[3].b,capsules[3].radius,Region::frame},
    {{0,.89f,-.305f},.16f,Region::torso},
    {capsules[6].a,capsules[6].radius,Region::torso},
    {capsules[6].b,capsules[6].radius,Region::torso},
    {{0,1.385f,.135f},.16f,Region::head}
}};
}
