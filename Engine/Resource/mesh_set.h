#pragma once
// Frostbite skinned MeshSet resources: one level of detail's triangles, and how each vertex is
// weighted to the skeleton's bones. Only what a skinned mesh needs; the layout was worked out
// on skate.'s costume meshes, first by gBGYo's ReSkate fork (github.com/gBGYo/ReSkate).
#include "binary_io.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dingosdk::frostbite {
struct MeshLod {
    Guid chunk; // the geometry chunk: the vertex buffer, then the index buffer
    std::uint32_t vertex_bytes{}, index_bytes{};
};
// The `lod`-th level of detail (0 the finest) of a MeshSet resource. Throws for a layout this
// reader does not know.
[[nodiscard]] MeshLod read_mesh_lod(std::span<const std::byte> resource, std::size_t lod);

struct SkinnedVertex {
    std::array<float, 3> position{};       // model space
    std::array<std::uint16_t, 8> bones{};  // skeleton bones
    std::array<std::uint8_t, 8> weights{}; // summing to 255; 0 for an unused influence
};
struct SkinnedMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> triangles; // vertex indices, three a triangle
};
// That level's sections as one mesh, from its geometry chunk. Throws when they do not fit.
[[nodiscard]] SkinnedMesh read_skinned_mesh(std::span<const std::byte> resource, std::size_t lod,
                                            std::span<const std::byte> geometry);
}
