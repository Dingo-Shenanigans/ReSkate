#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The skater's animation skeleton, read from the game's data so that a style names joints and does not use build-specific indices.
namespace dingosdk::style {
struct SkeletonJoint {
    std::string name;
    std::int32_t parent = -1;
};
// The joints of Animation/Dingo/AnimBase_Default_Skeleton in pose order. Throws if it is missing or does not have 395 joints.
std::vector<SkeletonJoint> read_game_skeleton(const std::filesystem::path &game_root);
} // namespace dingosdk::style
