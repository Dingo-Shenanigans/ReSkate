#pragma once
#include <array>
#include <cstddef>
#include <imgui.h>

// Skate 3's X-ray bone meshes (the 19 `dem_bones_hom` models), read from <game>/HallOfMeat/bones.bin
// (built from the player's own Skate 3 by tools/HallOfMeat). Each mesh is fitted to its live bone
// segment and drawn as projected triangles over the ragdoll.
namespace dingosdk::overlay {
using Vec3f = std::array<float, 3>;
struct BoneProjector {
    std::array<float, 16> camera{}; // rows: right, up, back, position
    float focal{};
    ImVec2 centre{};
    bool project(const Vec3f &p, ImVec2 &at, float &depth) const;
    Vec3f view_direction(const Vec3f &p) const; // from the camera to p, normalised
};
// Loads the meshes on first use; false when the file is missing or unreadable.
bool hom_bones_ready() noexcept;
// Draws bone `index` placed on the live segment a->b, `forward` the skater's body forward (for
// roll). `colour` is the bone's tint; `core` brightens its middle (a fracture) from 0 to 1.
void draw_hom_bone(ImDrawList *draw, const BoneProjector &projector, std::size_t index, const Vec3f &a, const Vec3f &b,
                   const Vec3f &forward, ImU32 colour, float opacity, float core) noexcept;
} // namespace dingosdk::overlay
