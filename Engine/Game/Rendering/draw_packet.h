#pragma once
#include "Engine/Game/Abi/linear_transform.h"
#include <array>
#include <cmath>
#include <cstddef>

// A skinned mesh's draw packet as the renderer hands it out (Engine/Game/Build/20260929/
// skater_render.h): its bones' skinning matrices, packed, and how to place them in the world.
// Plain data and math; Extension/HallOfMeat/hall_of_meat_render.h reads it.
namespace dingosdk::draw_packet {
using game::LinearTransform;

// A packed bone: the skinning matrix's three columns of four floats, transposed for the shader.
using PackedBone = std::array<float, 12>;
constexpr LinearTransform unpack(const PackedBone& p) noexcept {
    return {{{p[0], p[4], p[8], 0}, {p[1], p[5], p[9], 0}, {p[2], p[6], p[10], 0}, {p[3], p[7], p[11], 1}}};
}
// Where the packed bones go: the packet's root (the world position's whole metres the packing left
// out) and, for a relative packet, its actor's transform after it.
constexpr LinearTransform placement(const std::array<float, 4>& root, bool relative, const LinearTransform& actor) noexcept {
    const LinearTransform at_root{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {root[0], root[1], root[2], 1}}};
    return relative ? game::compose(at_root, actor) : at_root;
}
// The bone's skinning matrix in the world. False for one that is not finite.
inline bool skin(const PackedBone& packed, const LinearTransform& placed, LinearTransform& out) noexcept {
    for (const float value : packed)
        if (!std::isfinite(value) || std::abs(value) > 1e7f) return false;
    out = game::compose(unpack(packed), placed);
    return true;
}
}
