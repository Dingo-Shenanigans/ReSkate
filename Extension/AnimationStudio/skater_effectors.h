#pragma once
#include <array>
#include <cstdint>

// Attach points and procedural targets on the 395-joint animation rig. Their
// translations may be animated and must survive capture and serialization.
// Gameplay's additive body-edit layer excludes these subtrees: the native graph
// owns attachments and constraints. Their presence is not a reason to freeze them.
namespace dingosdk::anim_studio {
inline constexpr std::array<std::uint16_t, 11> effector_joints{
    50, 283, 376, 377, 378, 379, 390, 391, 392, 393, 394,
};
} // namespace dingosdk::anim_studio
