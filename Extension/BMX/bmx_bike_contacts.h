#pragma once
#include "Engine/Game/BMX/bmx_geometry.h"

namespace dingosdk::bmx::contacts {
using Vec = geometry::Vec;
// Assembled bike coordinates: +X rider left, +Y up, +Z forward.
// Supplied-model geometry and rider targets share the canonical fitted anchors.
inline constexpr Vec steering_pivot=geometry::steering_pivot;
inline constexpr Vec steering_axis=geometry::steering_axis;
inline constexpr float grip_radius=geometry::left_grip.radius;
inline constexpr Vec grip_center(bool left) noexcept {
    return geometry::grip(left).center;
}
// Outward along the cylinder. Mirrored hands have opposite outward axes.
inline constexpr Vec grip_axis(bool left) noexcept {
    return geometry::grip(left).outward;
}
} // namespace dingosdk::bmx::contacts
