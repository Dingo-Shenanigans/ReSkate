#pragma once
#include <cstddef>
#include <span>

namespace dingosdk::bmx::rider {
inline constexpr std::size_t board_pose_bones = 16;
inline constexpr std::size_t first_hidden_board_bone = 2;

// Reference0 and world-root1 are native-owned trajectory channels. The native
// animation refreshes the render descendants, but can retain the root's scale.
// Never hide either root and never retain a pose to restore after dismount.
template<class Bone>
bool apply_mounted_board_hide(std::span<Bone> bones, bool mounted) noexcept {
    if (!mounted || bones.size() != board_pose_bones) return false;
    for (std::size_t i = first_hidden_board_bone; i < bones.size(); ++i)
        for (unsigned axis = 0; axis < 3; ++axis) bones[i].scale[axis] = 0;
    return true;
}
} // namespace dingosdk::bmx::rider
