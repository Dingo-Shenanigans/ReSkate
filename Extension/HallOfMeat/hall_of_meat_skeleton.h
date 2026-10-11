#pragma once
#include "Engine/Game/Skater/skater_body.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/skeleton_asset.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The skeleton Hall of Meat draws: skate.'s own skeleton mesh, read once from the installed game's
// data (Engine/Game/Build/20260929/skater_skeleton.h) and posed by the renderer's own skinning
// matrices (hall_of_meat_render.h), so it lies where the game draws the skater. Each vertex knows the
// ragdoll's body (skater_body.h) it moves with, for whatever that body went through.
namespace dingosdk::hall_of_meat {
using game::LinearTransform;
using game::Vec3;

// The render bone each body poses, by name; the game's skeleton publisher maps them the same way
// (rig+0x1ae0: 102 Neck1, 101 Neck, 278 LeftHand, ... 7 Hips). The board's root has none.
inline constexpr std::array<std::string_view, skater_body::count> skeleton_joints{
    "", "Neck1", "Neck", "LeftHand", "LeftForeArm", "LeftArm", "LeftShoulder",
    "RightHand", "RightForeArm", "RightArm", "RightShoulder", "Spine3", "Spine2", "Spine1", "Spine",
    "LeftToeBase", "LeftFoot", "LeftLeg", "LeftUpLeg", "RightToeBase", "RightFoot", "RightLeg", "RightUpLeg", "Hips"};
inline constexpr std::size_t max_influences = 4; // bones per vertex, the heaviest

struct SkeletonVertex {
    Vec3 position{}, normal{}; // model space
    std::array<std::uint16_t, max_influences> bones{};
    std::array<float, max_influences> weights{}; // summing to 1, the unused ones 0
};
struct SkeletonMesh {
    std::size_t bone_count{}; // the render skeleton's: a pose has a skinning matrix for each
    std::vector<SkeletonVertex> vertices;
    std::vector<std::uint8_t> parts;      // each vertex's body: its heaviest bone's own or nearest ancestor's joint
    std::vector<std::uint32_t> triangles; // vertex indices, three a triangle
};
// Binds a mesh skinned on `skeleton`. Throws when the skeleton lacks a body's joint, or a vertex
// rides on a bone no body moves (the root, the board) or does not fit it.
[[nodiscard]] SkeletonMesh bind_skeleton(const frostbite::Skeleton& skeleton, const frostbite::SkinnedMesh& mesh);

struct PosedSkeleton {
    std::vector<Vec3> positions, normals; // world space, one per vertex
};
// The mesh skinned by `skin`, each render bone's skinning matrix (a vertex from model space into the
// world). False, leaving `posed` as it was, when it lacks some.
bool pose_skeleton(const SkeletonMesh& mesh, std::span<const LinearTransform> skin, PosedSkeleton& posed);

// The game's folder (the one with Skate.exe), whose data the skeleton and the overlay's images come from.
std::filesystem::path game_folder();
// From the game folder; throws when it cannot be read.
[[nodiscard]] SkeletonMesh read_skeleton_mesh(const std::filesystem::path& game_root);
// Starts reading it in the background, once.
void prepare_skeleton() noexcept;
// Any thread: the mesh once read; nullptr until then, and for good when it could not be (logged).
std::shared_ptr<const SkeletonMesh> skeleton_mesh() noexcept;
}
