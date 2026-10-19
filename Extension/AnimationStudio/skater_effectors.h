#pragma once
#include <array>
#include <cstdint>

// The skater's attach and effector joints (the game's animbase skeleton, 395 joints; names read 2026-10-19): a
// prop's attach point, the wrists' and ankles' effectors and the deck's ones. Their places are the rig's, not the
// animation's: a recording copied them as if they were moved by the animation, and they jumped (up to a metre
// across a trick). Animation Studio holds them at their rest place and keeps only their turns (anim_core.h:
// hold_effector_positions).
namespace dingosdk::anim_studio {
inline constexpr std::array<std::uint16_t, 11> effector_joints{
    50, 283, 376, 377, 378, 379, 390, 391, 392, 393, 394,
};
} // namespace dingosdk::anim_studio
