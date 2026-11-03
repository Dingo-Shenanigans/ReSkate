// The skinned MeshSet and SkeletonAsset readers, on made-up resources.
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/skeleton_asset.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace dingosdk::frostbite;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class T> void put(std::vector<std::byte>& data, std::size_t offset, T value) {
    if (data.size() < offset + sizeof(T)) data.resize(offset + sizeof(T));
    std::memcpy(data.data() + offset, &value, sizeof(T));
}
void pointer(std::vector<std::byte>& data, std::size_t offset, std::size_t target) {
    put<std::uint64_t>(data, offset, target - 16);
}

// One level of detail, one section of three vertices on palette {5, 7}, one triangle.
struct Made {
    std::vector<std::byte> resource, geometry;
};
Made made_mesh(std::uint8_t second_weight = 55) {
    Made m;
    auto& r = m.resource;
    r.resize(1100);
    const std::array<std::uint32_t, 3> header{208, 188, 384};
    for (std::size_t i = 0; i < header.size(); ++i) put<std::uint32_t>(r, i * 4, header[i]);
    put<std::uint8_t>(r, 124, 1);
    put<std::uint16_t>(r, 180, 1);
    pointer(r, 48, 256);
    put<std::uint32_t>(r, 256, 1);        // kind
    put<std::uint32_t>(r, 256 + 8, 1);    // one section
    pointer(r, 256 + 12, 512);
    put<std::uint32_t>(r, 256 + 84, 33);  // 16-bit indices
    put<std::uint32_t>(r, 256 + 88, 6);   // index bytes
    put<std::uint32_t>(r, 256 + 92, 108); // vertex bytes: 3 x 36
    put<std::uint8_t>(r, 256 + 116, 0x42); // the chunk's guid
    pointer(r, 512 + 16, 1024);           // palette
    put<std::uint16_t>(r, 512 + 24, 2);
    put<std::uint8_t>(r, 512 + 31, 3);    // triangle list
    put<std::uint32_t>(r, 512 + 32, 1);
    put<std::uint32_t>(r, 512 + 44, 3);
    const std::size_t d = 512 + 112;
    const std::array<std::array<std::uint8_t, 4>, 5> elements{{{1, 3, 0, 0}, {2, 23, 12, 0}, {3, 23, 20, 0}, {4, 13, 28, 0}, {5, 13, 32, 0}}};
    for (std::size_t i = 0; i < elements.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j) put<std::uint8_t>(r, d + i * 4 + j, elements[i][j]);
    put<std::uint8_t>(r, d + 64, 36);
    put<std::uint8_t>(r, d + 96, 5);
    put<std::uint8_t>(r, d + 97, 1);
    put<std::uint16_t>(r, 1024, 5);
    put<std::uint16_t>(r, 1026, 7);
    auto& g = m.geometry;
    for (std::size_t v = 0; v < 3; ++v) {
        const auto at = v * 36;
        put<std::array<float, 3>>(g, at, {static_cast<float>(v), 0.0f, 0.0f});
        put<std::array<std::uint16_t, 4>>(g, at + 12, {0, 1, 0, 0});
        put<std::array<std::uint16_t, 4>>(g, at + 20, {0, 0, 0, 0});
        put<std::array<std::uint8_t, 4>>(g, at + 28, {200, second_weight, 0, 0});
        put<std::array<std::uint8_t, 4>>(g, at + 32, {0, 0, 0, 0});
    }
    put<std::array<std::uint16_t, 3>>(g, 108, {0, 1, 2});
    return m;
}

void a_skinned_lod_reads() {
    const auto m = made_mesh();
    const auto lod = read_mesh_lod(m.resource, 0);
    check(lod.vertex_bytes == 108 && lod.index_bytes == 6 && lod.chunk != Guid{}, "the level's buffers and chunk");
    const auto mesh = read_skinned_mesh(m.resource, 0, m.geometry);
    check(mesh.vertices.size() == 3 && mesh.triangles == std::vector<std::uint32_t>{0, 1, 2}, "three vertices, one triangle");
    const auto& v = mesh.vertices[2];
    check(v.position[0] == 2.0f && v.bones[0] == 5 && v.bones[1] == 7 && v.weights[0] == 200 && v.weights[1] == 55,
        "positions, and bones through the palette");
}

void four_influences_are_enough() {
    auto m = made_mesh();
    m.resource[512 + 112 + 96] = std::byte{3}; // only position, the first bones and weights:
    const std::array<std::array<std::uint8_t, 4>, 3> elements{{{1, 3, 0, 0}, {2, 23, 12, 0}, {4, 13, 28, 0}}};
    for (std::size_t i = 0; i < elements.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j) put<std::uint8_t>(m.resource, 512 + 112 + i * 4 + j, elements[i][j]);
    const auto mesh = read_skinned_mesh(m.resource, 0, m.geometry);
    check(mesh.vertices[1].bones[1] == 7 && mesh.vertices[1].weights[1] == 55 && mesh.vertices[1].weights[4] == 0,
        "a coarse level of detail weighs four bones");
}

void a_bad_mesh_is_refused() {
    const auto broken = made_mesh(54);
    bool refused{};
    try { (void)read_skinned_mesh(broken.resource, 0, broken.geometry); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "weights that do not add up");
    refused = false;
    try { (void)read_mesh_lod(broken.resource, 1); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "a level of detail it does not have");
}

// Only declaration bytes, never a game resource or geometry payload. The 32-byte stream
// stride also distinguishes the two-section 416-byte fixture from a 384-byte interpretation.
std::vector<std::byte> made_static_metadata(std::size_t stride, std::uint32_t sections = 1) {
    std::vector<std::byte> resource(512 + stride * sections);
    const std::array<std::uint32_t, 4> header{192, 176, 384, 0};
    for (std::size_t i = 0; i < header.size(); ++i) put(resource, i * 4, header[i]);
    put<std::uint16_t>(resource, 180, 1);
    pointer(resource, 48, 256);
    put<std::uint32_t>(resource, 256, 1);
    put(resource, 264, sections);
    pointer(resource, 268, 512);
    put<std::uint32_t>(resource, 340, 33);
    put<std::uint32_t>(resource, 344, sections * 6);
    put<std::uint32_t>(resource, 348, sections * 96);
    put<std::uint8_t>(resource, 372, 0x42);
    for (std::uint32_t i = 0; i < sections; ++i) {
        const auto at = 512 + stride * i;
        put<std::uint8_t>(resource, at + 31, 3);
        put<std::uint32_t>(resource, at + 32, 1);
        put<std::uint32_t>(resource, at + 36, i * 3);
        put<std::uint32_t>(resource, at + 40, i * 96);
        put<std::uint32_t>(resource, at + 44, 3);
        put<std::uint8_t>(resource, at + 112, 1);
        put<std::uint8_t>(resource, at + 113, 3);
        put<std::uint8_t>(resource, at + 176, 32);
        put<std::uint8_t>(resource, at + 208, 1);
        put<std::uint8_t>(resource, at + 209, 1);
    }
    return resource;
}

void metadata_throws(const std::vector<std::byte>& resource, std::size_t lod, const char* message) {
    bool refused{};
    try { (void)read_mesh_lod_metadata(resource, lod); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, message);
}

void metadata_reports_resource_fields_without_geometry() {
    const auto metadata = read_mesh_lod_metadata(made_mesh().resource, 0);
    check(metadata.lod_kind_supported && metadata.complete && metadata.skinned && !metadata.ambiguous && !metadata.static_common_offsets && metadata.static_section_candidates.empty(), "known synthetic skinned metadata is complete without fallback observations");
    check(metadata.header == std::array<std::uint32_t, 4>{208, 188, 384, 0}, "all four header words preserved");
    check(metadata.lod_count == 1 && metadata.lod_index == 0 && metadata.lod_offset == 256 && metadata.lod_kind == 1,
        "LOD identity and raw kind preserved");
    check(metadata.index_format == 33 && metadata.section_count == 1 && metadata.section_table == 512,
        "index format and section table preserved");
    check(metadata.geometry.vertex_bytes == 108 && metadata.geometry.index_bytes == 6 &&
        metadata.geometry.chunk.bytes[0] == std::byte{0x42}, "buffer sizes and chunk are metadata only");
    check(metadata.candidates.size() == 1, "skinned layout has one section stride");
    const auto& candidate = metadata.candidates[0];
    check(candidate.stride == 384 && candidate.structurally_valid && candidate.declarations_supported &&
        candidate.sections.size() == 1, "known candidate contains its section");
    const auto& section = candidate.sections[0];
    check(section.resource_offset == 512 && section.primitive == 3 && section.vertex_count == 3 &&
        section.triangle_count == 1 && section.first_index == 0, "draw fields preserved");
    check(section.elements.size() == 5 && section.streams.size() == 1 && section.elements[0].usage == 1 &&
        section.elements[0].format == 3 && section.elements[0].width == 12 && section.streams[0].stride == 36 &&
        section.streams[0].classification == 0, "raw declarations and established width preserved");
    check(section.vertex_range_checked && section.index_range_checked, "known ranges checked without reading indices");
}

void metadata_preserves_static_stride_ambiguity() {
    const auto metadata = read_mesh_lod_metadata(made_static_metadata(416), 0);
    check(!metadata.complete && metadata.ambiguous && !metadata.skinned && metadata.candidates.size() == 2,
        "one static section does not establish table stride");
    check(metadata.candidates[0].stride == 384 && metadata.candidates[1].stride == 416 &&
        metadata.candidates[0].structurally_valid && metadata.candidates[1].structurally_valid,
        "both plausible candidates retained");
}

void metadata_distinguishes_synthetic_static_tables() {
    for (const std::size_t stride : {std::size_t{384}, std::size_t{416}}) {
        const auto metadata = read_mesh_lod_metadata(made_static_metadata(stride, 2), 0);
        check(metadata.lod_kind_supported && metadata.complete && !metadata.ambiguous && metadata.candidates.size() == 2 && !metadata.static_common_offsets && metadata.static_section_candidates.empty(),
            "one supported synthetic table is reported completely");
        std::size_t valid{};
        for (const auto& candidate : metadata.candidates) {
            if (!candidate.structurally_valid) continue;
            ++valid;
            check(candidate.stride == stride && candidate.sections.size() == 2, "only intended synthetic stride fits");
            check(candidate.sections[1].resource_offset == 512 + stride && candidate.sections[1].vertex_start == 96 &&
                candidate.sections[1].first_index == 3, "second section retains independent buffer offsets");
        }
        check(valid == 1, "candidate validity is not first-fit preference");
    }
}

void metadata_refuses_core_bounds_and_unknown_headers() {
    const auto source = made_mesh().resource;
    for (const auto size : std::array<std::size_t, 5>{0, 15, 16, 181, 387}) {
        auto shortened = source;
        shortened.resize(size);
        metadata_throws(shortened, 0, "truncated core fields refused before span access");
    }
    auto broken = source;
    put<std::uint32_t>(broken, 12, 1);
    metadata_throws(broken, 0, "fourth header word is required");

    broken = source;
    put<std::uint64_t>(broken, 48, std::numeric_limits<std::uint64_t>::max());
    metadata_throws(broken, 0, "LOD pointer cannot wrap");
    broken = source;
    put<std::uint64_t>(broken, 48, 0);
    metadata_throws(broken, 0, "null LOD pointer refused");
    metadata_throws(source, 1, "out-of-range LOD refused");
    metadata_throws(source, std::numeric_limits<std::size_t>::max(), "large LOD index refused before multiplication");
    broken = source;
    put<std::uint32_t>(broken, 264, 4097);
    metadata_throws(broken, 0, "section count bounded before allocation");
    broken = source;
    put<std::uint64_t>(broken, 268, std::numeric_limits<std::uint64_t>::max());
    metadata_throws(broken, 0, "section pointer cannot wrap");
}

void metadata_preserves_unsupported_lod_facts() {
    for (const bool skinned : {false, true}) {
        for (const auto kind : std::array<std::uint32_t, 3>{0, 2, std::numeric_limits<std::uint32_t>::max()}) {
            auto resource = skinned ? made_mesh().resource : made_static_metadata(416);
            put(resource, 256, kind);
            // These later fields are deliberately invalid: an unknown kind must not interpret them.
            put<std::uint32_t>(resource, 264, std::numeric_limits<std::uint32_t>::max());
            put<std::uint64_t>(resource, 268, std::numeric_limits<std::uint64_t>::max());
            put<std::uint32_t>(resource, 340, 34);
            const auto metadata = read_mesh_lod_metadata(resource, 0);
            check(metadata.skinned == skinned && metadata.header[0] == (skinned ? 208u : 192u) &&
                metadata.header[1] == (skinned ? 188u : 176u) && metadata.header[2] == 384 && metadata.header[3] == 0,
                "unsupported kind preserves all recognized resource header facts");
            check(metadata.lod_count == 1 && metadata.lod_index == 0 && metadata.lod_offset == 256 &&
                metadata.lod_kind == kind, "unsupported raw kind and LOD identity survive refusal");
            check(!metadata.lod_kind_supported && !metadata.complete && !metadata.ambiguous &&
                metadata.candidates.empty() && metadata.diagnostics.size() == 1,
                "unsupported kind is incomplete with a diagnostic and no guessed candidates");
            check(metadata.index_format == 0 && metadata.section_count == 0 && metadata.section_table == 0 &&
                metadata.geometry.chunk == Guid{} && metadata.geometry.vertex_bytes == 0 && metadata.geometry.index_bytes == 0,
                "unsupported kind leaves interpreted fields unpopulated");

            resource.resize(260); // Exactly the selected LOD's four-byte raw kind remains.
            const auto short_metadata = read_mesh_lod_metadata(resource, 0);
            check(short_metadata.lod_kind == kind && short_metadata.lod_offset == 256 &&
                !short_metadata.lod_kind_supported && !short_metadata.complete && short_metadata.candidates.empty() && !short_metadata.static_common_offsets,
                "raw unsupported kind needs no assumed full LOD layout");
            resource.resize(259);
            metadata_throws(resource, 0, "even a raw unsupported kind requires all four bytes");
        }
    }
    auto second_lod = made_static_metadata(416);
    put<std::uint16_t>(second_lod, 180, 2);
    pointer(second_lod, 56, 400);
    put<std::uint32_t>(second_lod, 400, 2);
    second_lod.resize(404);
    const auto metadata = read_mesh_lod_metadata(second_lod, 1);
    check(metadata.lod_count == 2 && metadata.lod_index == 1 && metadata.lod_offset == 400 &&
        metadata.lod_kind == 2 && !metadata.lod_kind_supported && !metadata.complete,
        "unsupported facts describe the requested nonzero LOD rather than the first pointer");
    put<std::uint16_t>(second_lod, 180, std::numeric_limits<std::uint16_t>::max());
    metadata_throws(second_lod, 1, "unsupported kind does not bypass the full pointer-table bounds check");

    auto known_kind = made_static_metadata(416);
    known_kind.resize(260);
    metadata_throws(known_kind, 0, "known kind still requires its full previously checked LOD span");
}
void metadata_observes_static_common_offsets() {
    std::array<std::byte, 16> identity{};
    for (std::size_t i = 0; i < identity.size(); ++i) identity[i] = static_cast<std::byte>(0x80 + i);
    for (const auto kind : std::array<std::uint32_t, 3>{0, 2, std::numeric_limits<std::uint32_t>::max()}) {
        auto resource = made_static_metadata(416);
        put(resource, 256, kind);
        put<std::uint32_t>(resource, 264, std::numeric_limits<std::uint32_t>::max());
        put<std::uint64_t>(resource, 268, std::numeric_limits<std::uint64_t>::max());
        put<std::uint32_t>(resource, 340, 34);
        put<std::uint32_t>(resource, 344, std::numeric_limits<std::uint32_t>::max());
        put<std::uint32_t>(resource, 348, std::numeric_limits<std::uint32_t>::max() - 1);
        put(resource, 372, identity);
        resource.resize(388); // Exactly 132 bytes from the selected LOD; no section table exists.
        const auto metadata = read_mesh_lod_metadata(resource, 0);
        check(metadata.static_common_offsets.has_value(), "full static prefix exposes a separate raw observation");
        if (metadata.static_common_offsets) {
            const auto& raw = *metadata.static_common_offsets;
            check(raw.section_count_raw == std::numeric_limits<std::uint32_t>::max() &&
                raw.section_pointer_raw == std::numeric_limits<std::uint64_t>::max(),
                "hostile count and pointer remain unchanged words without allocation or pointer arithmetic");
            check(raw.index_format_raw == 34 && raw.index_bytes_raw == std::numeric_limits<std::uint32_t>::max() &&
                raw.vertex_bytes_raw == std::numeric_limits<std::uint32_t>::max() - 1 && raw.geometry_identity_raw == identity,
                "raw format, byte counts and identity bytes are copied without interpretation");
        }
        check(!metadata.lod_kind_supported && !metadata.complete && metadata.candidates.empty() &&
            metadata.section_count == 0 && metadata.section_table == 0 && metadata.index_format == 0 &&
            metadata.geometry.chunk == Guid{} && metadata.geometry.vertex_bytes == 0 && metadata.geometry.index_bytes == 0,
            "static observation never populates interpreted geometry or admits an unsupported kind");
        put<std::uint64_t>(resource, 268, 0);
        const auto null_pointer = read_mesh_lod_metadata(resource, 0);
        check(null_pointer.static_common_offsets && null_pointer.static_common_offsets->section_pointer_raw == 0 &&
            !null_pointer.complete && null_pointer.candidates.empty(), "zero pointer is a raw word, not dereferenced or rejected");

        auto wrong_header = resource;
        put<std::uint32_t>(wrong_header, 12, 1);
        metadata_throws(wrong_header, 0, "static observation requires all four exact header words");
        auto skinned = resource;
        put<std::uint32_t>(skinned, 0, 208);
        put<std::uint32_t>(skinned, 4, 188);
        const auto skinned_metadata = read_mesh_lod_metadata(skinned, 0);
        check(!skinned_metadata.static_common_offsets && !skinned_metadata.lod_kind_supported &&
            !skinned_metadata.complete && skinned_metadata.candidates.empty(),
            "unsupported skinned kinds do not enter the static observation path");

        resource.resize(387);
        const auto truncated = read_mesh_lod_metadata(resource, 0);
        check(!truncated.static_common_offsets && truncated.lod_kind == kind && truncated.lod_offset == 256 &&
            !truncated.lod_kind_supported && !truncated.complete && truncated.diagnostics.size() == 2,
            "131 bytes retain the raw-kind refusal and report missing observation span without partial fields");
    }

    auto later_lod = made_static_metadata(416);
    put<std::uint16_t>(later_lod, 180, 3);
    pointer(later_lod, 56, 400);
    pointer(later_lod, 64, 512);
    put<std::uint32_t>(later_lod, 512, 0);
    put<std::uint32_t>(later_lod, 520, 7);
    put<std::uint64_t>(later_lod, 524, 0x1122334455667788ull);
    put<std::uint32_t>(later_lod, 596, 34);
    put<std::uint32_t>(later_lod, 600, 13);
    put<std::uint32_t>(later_lod, 604, 29);
    put(later_lod, 628, identity);
    later_lod.resize(644);
    const auto metadata = read_mesh_lod_metadata(later_lod, 2);
    check(metadata.lod_count == 3 && metadata.lod_index == 2 && metadata.lod_offset == 512 &&
        metadata.lod_kind == 0 && metadata.static_common_offsets && !metadata.complete,
        "observation follows the selected third LOD rather than a fixed index or resource offset");
    if (metadata.static_common_offsets) {
        const auto& raw = *metadata.static_common_offsets;
        check(raw.section_count_raw == 7 && raw.section_pointer_raw == 0x1122334455667788ull &&
            raw.index_format_raw == 34 && raw.index_bytes_raw == 13 && raw.vertex_bytes_raw == 29 &&
            raw.geometry_identity_raw == identity, "all six observation offsets are relative to the selected LOD");
    }
}
std::vector<std::byte> made_static_section_observation(std::size_t stride, std::uint32_t sections = 2) {
    auto resource = made_static_metadata(stride, sections);
    const auto table_bytes = stride * sections;
    resource.resize(736 + table_bytes);
    std::memmove(resource.data() + 736, resource.data() + 512, table_bytes);
    pointer(resource, 268, 736); // Synthetic raw pointer 720 uses the existing resource base of 16.
    put<std::uint32_t>(resource, 256, 0);
    return resource;
}

void metadata_observes_both_static_section_layouts() {
    auto resource = made_static_section_observation(416);
    put<std::uint8_t>(resource, 736 + 31, 255);
    for (const std::size_t offset : {32u, 36u, 40u, 44u})
        put<std::uint32_t>(resource, 736 + offset, std::numeric_limits<std::uint32_t>::max());
    put<std::uint8_t>(resource, 736 + 112, 250);
    put<std::uint8_t>(resource, 736 + 113, 251);
    put<std::uint8_t>(resource, 736 + 114, 252);
    put<std::uint8_t>(resource, 736 + 115, 253);
    put<std::uint8_t>(resource, 736 + 176, 0);
    put<std::uint8_t>(resource, 736 + 177, 255);
    const auto metadata = read_mesh_lod_metadata(resource, 0);
    check(metadata.static_section_candidates.size() == 2 && !metadata.complete && !metadata.lod_kind_supported &&
        metadata.candidates.empty() && metadata.section_table == 0 && metadata.geometry.chunk == Guid{},
        "two raw candidates never select or populate an interpreted layout");
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& candidate = metadata.static_section_candidates[i];
        const auto stride = i == 0 ? 384u : 416u;
        check(candidate.stride == stride && candidate.table_pointer_resolved && candidate.table_offset == 736 &&
            candidate.table_span_valid && candidate.span_bytes == stride * 2 && candidate.sections.size() == 2,
            "both full candidate spans are retained with the checked pointer base");
        const auto& section = candidate.sections[0];
        check(section.resource_offset == 736 && section.primitive_raw == 255 &&
            section.triangle_count_raw == std::numeric_limits<std::uint32_t>::max() &&
            section.first_index_raw == std::numeric_limits<std::uint32_t>::max() &&
            section.vertex_start_raw == std::numeric_limits<std::uint32_t>::max() &&
            section.vertex_count_raw == std::numeric_limits<std::uint32_t>::max(), "draw words remain raw even when nonsensical");
        check(section.declarations_observed && section.elements.size() == 1 && section.streams.size() == 1 &&
            section.elements[0].usage_raw == 250 && section.elements[0].format_raw == 251 &&
            section.elements[0].offset_raw == 252 && section.elements[0].stream_raw == 253 &&
            section.streams[0].stride_raw == 0 && section.streams[0].classification_raw == 255,
            "unknown declaration codes and zero strides are observed without semantic checks");
        check(candidate.sections[1].resource_offset == 736 + stride, "each candidate uses its own stride for later records");
    }
    check(!metadata.static_section_candidates[0].observation_complete && metadata.static_section_candidates[1].observation_complete,
        "a malformed competing declaration does not remove either full-span candidate");

    auto short_layout = made_static_section_observation(384);
    const auto one_span = read_mesh_lod_metadata(short_layout, 0);
    check(one_span.static_section_candidates.size() == 2 && one_span.static_section_candidates[0].table_span_valid &&
        !one_span.static_section_candidates[1].table_span_valid && one_span.static_section_candidates[1].sections.empty() &&
        !one_span.complete, "exact 384-byte table retains the unavailable 416-byte candidate");
    short_layout.resize(short_layout.size() - 1);
    const auto no_span = read_mesh_lod_metadata(short_layout, 0);
    check(no_span.static_section_candidates.size() == 2 && !no_span.static_section_candidates[0].table_span_valid &&
        !no_span.static_section_candidates[1].table_span_valid && no_span.static_common_offsets,
        "truncated candidate tables preserve common facts and both failed candidates");
}

void metadata_bounds_static_section_observations() {
    const auto source = made_static_section_observation(416);
    for (const auto raw_pointer : std::array<std::uint64_t, 3>{0, std::numeric_limits<std::uint64_t>::max(), source.size() - 15}) {
        auto resource = source;
        put(resource, 268, raw_pointer);
        const auto metadata = read_mesh_lod_metadata(resource, 0);
        check(metadata.static_section_candidates.size() == 2 && metadata.static_common_offsets->section_pointer_raw == raw_pointer,
            "invalid pointer word remains visible beside both refused candidate records");
        for (const auto& candidate : metadata.static_section_candidates)
            check(!candidate.table_pointer_resolved && !candidate.table_span_valid && candidate.sections.empty() &&
                !candidate.observation_complete && candidate.diagnostic_count != 0, "pointer rejected before addition or allocation");
    }
    auto at_end = source;
    put<std::uint64_t>(at_end, 268, source.size() - 16);
    const auto end_metadata = read_mesh_lod_metadata(at_end, 0);
    check(end_metadata.static_section_candidates.size() == 2, "end pointer retains both raw candidate records");
    for (const auto& candidate : end_metadata.static_section_candidates)
        check(candidate.table_pointer_resolved && candidate.table_offset == source.size() && !candidate.table_span_valid &&
            candidate.sections.empty(), "valid pointer to resource end cannot admit a nonempty table");
    for (const std::uint32_t count : {0u, 65u, std::numeric_limits<std::uint32_t>::max()}) {
        auto resource = source;
        put(resource, 264, count);
        const auto metadata = read_mesh_lod_metadata(resource, 0);
        check(metadata.static_section_candidates.size() == 2 && !metadata.complete, "raw count refusals retain both candidates");
        for (const auto& candidate : metadata.static_section_candidates)
            check(candidate.sections.empty() && !candidate.observation_complete && candidate.diagnostic_count != 0,
                "zero or excessive raw count does not allocate sections");
    }
    const auto within_budget = read_mesh_lod_metadata(made_static_section_observation(416, 40), 0);
    const auto over_budget = read_mesh_lod_metadata(made_static_section_observation(416, 41), 0);
    for (std::size_t i = 0; i < 2; ++i) {
        check(within_budget.static_section_candidates[i].table_span_valid && within_budget.static_section_candidates[i].sections.size() == 40,
            "32000 aggregate candidate bytes remain inside the observation budget");
        check(over_budget.static_section_candidates[i].sections.empty() && !over_budget.static_section_candidates[i].table_span_valid &&
            over_budget.static_section_candidates[i].diagnostic_count != 0, "32800 aggregate candidate bytes are refused despite fitting the resource");
    }
}

void metadata_bounds_static_declaration_observations() {
    auto resource = made_static_section_observation(416);
    put<std::uint8_t>(resource, 736 + 208, 16);
    put<std::uint8_t>(resource, 736 + 209, 16);
    for (std::size_t i = 0; i < 16; ++i) {
        put<std::uint8_t>(resource, 736 + 112 + 4 * i, static_cast<std::uint8_t>(100 + i));
        put<std::uint8_t>(resource, 736 + 113 + 4 * i, static_cast<std::uint8_t>(200 + i));
        put<std::uint8_t>(resource, 736 + 114 + 4 * i, static_cast<std::uint8_t>(240 + i));
        put<std::uint8_t>(resource, 736 + 115 + 4 * i, static_cast<std::uint8_t>(255 - i));
        put<std::uint8_t>(resource, 736 + 176 + 2 * i, static_cast<std::uint8_t>(i));
        put<std::uint8_t>(resource, 736 + 177 + 2 * i, static_cast<std::uint8_t>(255 - i));
    }
    const auto full = read_mesh_lod_metadata(resource, 0);
    const auto& section = full.static_section_candidates[1].sections[0];
    check(section.declarations_observed && section.elements.size() == 16 && section.streams.size() == 16 &&
        section.elements[15].usage_raw == 115 && section.elements[15].format_raw == 215 &&
        section.elements[15].offset_raw == 255 && section.elements[15].stream_raw == 240 &&
        section.streams[15].stride_raw == 15 && section.streams[15].classification_raw == 240,
        "all sixteen raw declaration slots are bounded and observable");
    for (const std::size_t offset : {208u, 209u}) {
        auto invalid = resource;
        put<std::uint8_t>(invalid, 736 + offset, 17);
        const auto metadata = read_mesh_lod_metadata(invalid, 0);
        const auto& candidate = metadata.static_section_candidates[1];
        const auto& first = candidate.sections[0];
        check(candidate.sections.size() == 2 && !first.declarations_observed && first.elements.empty() && first.streams.empty() &&
            first.vertex_count_raw == 3 && first.element_count_raw == (offset == 208 ? 17 : 16) &&
            first.stream_count_raw == (offset == 209 ? 17 : 16) &&
            candidate.sections[1].declarations_observed && !candidate.observation_complete,
            "either excessive declaration count skips arrays but preserves scalars and later sections");
    }
    put<std::uint8_t>(resource, 736 + 208, 0);
    put<std::uint8_t>(resource, 736 + 209, 0);
    const auto empty = read_mesh_lod_metadata(resource, 0);
    check(empty.static_section_candidates[1].sections[0].declarations_observed &&
        empty.static_section_candidates[1].sections[0].elements.empty() && empty.static_section_candidates[1].sections[0].streams.empty(),
        "zero declaration counts are raw observations without inferred position requirements");
    auto many_bad = made_static_section_observation(416, 40);
    for (std::size_t i = 0; i < 40; ++i) put<std::uint8_t>(many_bad, 736 + 416 * i + 208, 17);
    put<std::uint32_t>(many_bad, 736 + 416 * 39 + 44, 1234);
    const auto capped = read_mesh_lod_metadata(many_bad, 0);
    const auto& candidate = capped.static_section_candidates[1];
    check(candidate.sections.size() == 40 && candidate.diagnostic_count == 40 && candidate.diagnostics.size() == 16 &&
        candidate.sections.back().vertex_count_raw == 1234 && !candidate.observation_complete && !capped.complete,
        "diagnostic retention is capped without dropping total count or later raw section records");
}
void metadata_returns_candidate_bounds_failures() {
    auto short_table = made_static_metadata(384);
    short_table.resize(short_table.size() - 1);
    const auto metadata = read_mesh_lod_metadata(short_table, 0);
    check(!metadata.complete && metadata.candidates.size() == 2 &&
        !metadata.candidates[0].structurally_valid && !metadata.candidates[1].structurally_valid,
        "truncated candidates remain errors, not empty success");
    struct Mutation { std::size_t offset; std::uint8_t value; };
    for (const auto change : std::array<Mutation, 6>{{{720, 17}, {721, 17}, {627, 1}, {626, 25}, {688, 0}, {721, 0}}}) {
        auto bad = made_static_metadata(384);
        put(bad, change.offset, change.value);
        const auto result = read_mesh_lod_metadata(bad, 0);
        check(!result.complete && !result.candidates[0].structurally_valid,
            "counts, stream references, attribute ends and zero stride checked");
    }
    for (const auto offset : std::array<std::size_t, 5>{344, 348, 544, 548, 552}) {
        auto bad = made_static_metadata(384);
        put<std::uint32_t>(bad, offset, offset == 344 ? 5u : offset == 348 ? 95u : std::numeric_limits<std::uint32_t>::max());
        const auto result = read_mesh_lod_metadata(bad, 0);
        check(!result.complete && !result.candidates[0].structurally_valid, "declared buffer range arithmetic checked");
    }
}

void metadata_reports_unknown_declarations_without_guessing() {
    auto unknown = made_static_metadata(416);
    put<std::uint8_t>(unknown, 720, 2);
    put<std::uint8_t>(unknown, 628, 200);
    put<std::uint8_t>(unknown, 629, 201);
    put<std::uint8_t>(unknown, 630, 16);
    const auto metadata = read_mesh_lod_metadata(unknown, 0);
    check(!metadata.complete && metadata.ambiguous, "unsupported semantics cannot choose a static stride");
    for (const auto& candidate : metadata.candidates) {
        check(candidate.structurally_valid && !candidate.declarations_supported, "unknown declaration stays plausible");
        const auto& element = candidate.sections[0].elements[1];
        check(element.usage == 200 && element.format == 201 && element.offset == 16 && element.width == 0 &&
            !element.semantic_supported, "unknown codes retain no invented width or semantics");
    }
    unknown = made_static_metadata(384);
    put<std::uint32_t>(unknown, 340, 34);
    const auto indices = read_mesh_lod_metadata(unknown, 0);
    check(!indices.complete && indices.index_format == 34 && !indices.candidates[0].sections[0].index_range_checked,
        "unknown index code is not guessed as uint32");
    unknown = made_static_metadata(384);
    put<std::uint8_t>(unknown, 688, 0);
    put<std::uint8_t>(unknown, 689, 1);
    put<std::uint8_t>(unknown, 626, 255);
    const auto classified = read_mesh_lod_metadata(unknown, 0);
    check(!classified.complete && classified.candidates[0].structurally_valid &&
        !classified.candidates[0].declarations_supported && classified.candidates[0].sections[0].streams[0].classification == 1 &&
        !classified.candidates[0].sections[0].vertex_range_checked,
        "unknown classification cannot reject a candidate through assumed per-vertex stride bounds");
}

void metadata_bounds_diagnostic_storage() {
    auto resource = made_static_metadata(384, 80);
    for (std::size_t i = 0; i < 80; ++i) put<std::uint8_t>(resource, 512 + i * 384 + 176, 0);
    const auto metadata = read_mesh_lod_metadata(resource, 0);
    check(!metadata.complete && !metadata.candidates[0].structurally_valid, "many errors cannot become success");
    const auto& candidate = metadata.candidates[0];
    check(candidate.diagnostics.size() <= 64 && candidate.diagnostic_count > candidate.diagnostics.size(),
        "bounded diagnostics retain total error count");
}

ebx::Value object(std::vector<std::pair<std::string, ebx::Value>> fields) {
    auto made = std::make_shared<ebx::Object>();
    for (auto& [name, value] : fields) made->fields.push_back({0, std::move(name), std::move(value)});
    return {made};
}

void a_skeleton_reads() {
    const auto asset = object({
        {"BoneNames", {ebx::Value::Array{{std::string("Root")}, {std::string("Hand")}}}},
        {"Hierarchy", {ebx::Value::Array{{std::int64_t{-1}}, {std::int64_t{0}}}}},
    });
    const auto skeleton = read_skeleton(*std::get<std::shared_ptr<ebx::Object>>(asset.data));
    check(skeleton.names == std::vector<std::string>{"Root", "Hand"} && skeleton.parents == std::vector<int>{-1, 0}, "names and parents");
    const auto backwards = object({
        {"BoneNames", {ebx::Value::Array{{std::string("A")}, {std::string("B")}}}},
        {"Hierarchy", {ebx::Value::Array{{std::int64_t{1}}, {std::int64_t{-1}}}}},
    });
    bool refused{};
    try { (void)read_skeleton(*std::get<std::shared_ptr<ebx::Object>>(backwards.data)); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "a parent after its child");
}
}

int main() {
    try {
        a_skinned_lod_reads();
        four_influences_are_enough();
        a_bad_mesh_is_refused();
        metadata_reports_resource_fields_without_geometry();
        metadata_preserves_static_stride_ambiguity();
        metadata_distinguishes_synthetic_static_tables();
        metadata_refuses_core_bounds_and_unknown_headers();
        metadata_preserves_unsupported_lod_facts();
        metadata_observes_static_common_offsets();
        metadata_observes_both_static_section_layouts();
        metadata_bounds_static_section_observations();
        metadata_bounds_static_declaration_observations();
        metadata_returns_candidate_bounds_failures();
        metadata_reports_unknown_declarations_without_guessing();
        metadata_bounds_diagnostic_storage();
        a_skeleton_reads();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Skinned mesh tests passed.\n";
    return 0;
}
