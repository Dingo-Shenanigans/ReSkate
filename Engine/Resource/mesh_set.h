#pragma once
// Frostbite skinned MeshSet resources: one level of detail's triangles, and how each vertex is
// weighted to the skeleton's bones. Only what a skinned mesh needs; the layout was worked out
// on skate.'s costume meshes, first by gBGYo's ReSkate fork (github.com/gBGYo/ReSkate).
#include "binary_io.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::frostbite {
struct MeshLod {
    Guid chunk; // the geometry chunk: the vertex buffer, then the index buffer
    std::uint32_t vertex_bytes{}, index_bytes{};
};
// Resource-only observations. Unknown codes remain numeric; no geometry chunk is read.
struct MeshElementMetadata {
    std::uint8_t usage{}, format{}, offset{}, stream{}, width{}; // width 0 means unknown
    bool semantic_supported{};
};
struct MeshStreamMetadata { std::uint8_t stride{}, classification{}; };
struct MeshSectionMetadata {
    std::size_t resource_offset{};
    std::uint8_t primitive{}, element_count{}, stream_count{};
    std::uint32_t triangle_count{}, first_index{}, vertex_start{}, vertex_count{};
    std::vector<MeshElementMetadata> elements;
    std::vector<MeshStreamMetadata> streams;
    bool vertex_range_checked{}, index_range_checked{};
};
struct MeshSectionLayoutMetadata {
    std::size_t stride{};
    bool structurally_valid{true}, declarations_supported{true};
    std::vector<MeshSectionMetadata> sections;
    std::size_t diagnostic_count{}; // diagnostics holds at most 64 messages
    std::vector<std::string> diagnostics;
};
// Raw values at offsets shared by known readers, not a layout interpretation or a chunk GUID.
struct MeshLodCommonOffsetObservation {
    std::uint32_t section_count_raw{};
    std::uint64_t section_pointer_raw{};
    std::uint32_t index_format_raw{}, index_bytes_raw{}, vertex_bytes_raw{};
    std::array<std::byte, 16> geometry_identity_raw{};
};
// These bytes use existing reader offsets without qualifying a static section layout.
struct MeshElementObservation {
    std::uint8_t usage_raw{}, format_raw{}, offset_raw{}, stream_raw{};
};
struct MeshStreamObservation { std::uint8_t stride_raw{}, classification_raw{}; };
struct MeshSectionObservation {
    std::size_t resource_offset{};
    std::uint8_t primitive_raw{}, element_count_raw{}, stream_count_raw{};
    std::uint32_t triangle_count_raw{}, first_index_raw{}, vertex_start_raw{}, vertex_count_raw{};
    std::vector<MeshElementObservation> elements;
    std::vector<MeshStreamObservation> streams;
    bool declarations_observed{};
};
struct MeshSectionCandidateObservation {
    std::size_t stride{}, table_offset{}, span_bytes{};
    bool table_pointer_resolved{}, table_span_valid{}, observation_complete{};
    std::vector<MeshSectionObservation> sections;
    std::size_t diagnostic_count{}; // diagnostics holds at most 16 messages
    std::vector<std::string> diagnostics;
};
struct MeshLodMetadata {
    std::array<std::uint32_t, 4> header{};
    bool skinned{}, lod_kind_supported{};
    std::size_t lod_count{}, lod_index{}, lod_offset{}, section_table{};
    std::uint32_t lod_kind{}, index_format{}, section_count{};
    MeshLod geometry;
    std::optional<MeshLodCommonOffsetObservation> static_common_offsets;
    std::vector<MeshSectionCandidateObservation> static_section_candidates;
    std::vector<MeshSectionLayoutMetadata> candidates;
    std::vector<std::string> diagnostics;
    bool ambiguous{}, complete{};
};
// Throws for unknown headers or unreadable core resource/LOD fields. An unsupported LOD kind
// returns complete false with a diagnostic. When lod_kind_supported is false, only header,
// skinned, lod_count, lod_index, lod_offset, lod_kind and diagnostics are available. For the
// exact static header, static_common_offsets may additionally hold six raw observations when
// their full 132-byte span is present. Then static_section_candidates always holds both 384/416
// stride attempts, including failures; neither is selected or interpreted. A resolved pointer
// makes table_offset available, and a valid full table span makes span_bytes available.
// observation_complete only means all bounded raw section/declaration bytes were observed.
// Counts are capped at 64, combined table spans at 32768 bytes, and each declaration at 16 slots.
// No geometry identity is interpreted. Other fields remain defaults, not observations. Kind 1
// still requires its full LOD span and leaves both static observation fields empty.
// Parseable candidate errors and unknown declarations are returned explicitly. Static section
// strides are never chosen by first fit: both plausible candidates make complete false.
// complete means this reader reported one structurally sound, understood declaration layout;
// it never qualifies index contents, native encoding, materials, dependencies or rendering.
[[nodiscard]] MeshLodMetadata read_mesh_lod_metadata(std::span<const std::byte> resource, std::size_t lod);
// The `lod`-th level of detail (0 the finest) of a MeshSet resource. Throws for a layout this
// reader does not know.
[[nodiscard]] MeshLod read_mesh_lod(std::span<const std::byte> resource, std::size_t lod);

// What a geometry comparison needs, of any MeshSet of the known layout, skinned or not and
// whatever its index size: how many levels of detail it has, a level's geometry chunk, and
// where that level's vertices are. Nothing of the skinning is read, so a mesh the skinned
// reader refuses for its bones still gives its positions. Throws for another layout, and for
// positions that are not three floats.
[[nodiscard]] std::size_t mesh_lod_count(std::span<const std::byte> resource);
[[nodiscard]] MeshLod mesh_lod_geometry(std::span<const std::byte> resource, std::size_t lod);
[[nodiscard]] std::vector<std::array<float, 3>> read_mesh_positions(std::span<const std::byte> resource, std::size_t lod,
                                                                    std::span<const std::byte> geometry);

// A level of detail's geometry chunk and buffer sizes, of either layout check_mesh_lod reads (the
// skinned one, and world meshes' static one); `lods` gets how many levels there are.
[[nodiscard]] MeshLod any_mesh_lod_geometry(std::span<const std::byte> resource, std::size_t lod, std::size_t* lods = nullptr);
// What is wrong with a level of detail's geometry as the GPU would read it, a skinned or a static
// MeshSet: a section whose vertices or indices run past their buffer, or a triangle that
// names a vertex outside its section. Empty when it is sound. `index_format` gets the level's
// index format (33: 16-bit; anything else is read as 32-bit). Throws for another layout.
[[nodiscard]] std::vector<std::string> check_mesh_lod(std::span<const std::byte> resource, std::size_t lod,
                                                      std::span<const std::byte> geometry, std::uint32_t* index_format = nullptr);
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
