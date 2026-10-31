#include "mesh_set.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace dingosdk::frostbite {
namespace {
// The resource: three layout words this reader knows, the skinned flag, the levels of detail
// (a table of pointers) and their count. A pointer counts from byte 16.
constexpr std::array<std::uint32_t, 4> known_header{208, 188, 384, 0};
constexpr std::size_t skinned_flag = 124, lod_count = 180, lod_table = 48, pointer_base = 16;
// A level of detail: its kind (1), its sections, the index format (33: 16-bit indices), the
// buffer sizes and the geometry chunk.
constexpr std::size_t lod_kind = 0, section_count = 8, section_table = 12, index_format = 84, index_bytes = 88,
    vertex_bytes = 92, geometry_chunk = 116;
constexpr std::uint32_t known_lod_kind = 1, sixteen_bit_indices = 33;
// A section, 384 bytes: its bone palette, primitive type (3: a triangle list), triangles, first
// index, where its vertices start in the vertex buffer and how many, and its vertex declaration.
constexpr std::size_t section_size = 384, palette = 16, palette_count = 24, primitive = 31, triangle_count = 32,
    first_index = 36, vertex_start = 40, vertex_count = 44, declaration = 112;
constexpr std::uint8_t triangle_list = 3;
// The declaration: 16 elements of {usage, format, offset, stream}, then {stride, classification}
// per stream, then the element and stream counts. Four bone influences a vertex, or eight with
// the second pair (the coarser levels of detail leave it out).
constexpr std::size_t stream_table = 64, element_total = 96, stream_total = 97;
enum Usage : std::uint8_t { position = 1, bones_low = 2, bones_high = 3, weights_low = 4, weights_high = 5 };
constexpr std::array<std::uint8_t, 6> usage_format{0, 3, 23, 23, 13, 13}; // float3, ushort4, ushort4, ubyte4n, ubyte4n
constexpr std::array<std::uint8_t, 6> usage_size{0, 12, 8, 8, 4, 4};

void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(std::string("MeshSet: ") + message);
}
struct Reader {
    std::span<const std::byte> data;
    template <class T> T at(std::size_t offset) const {
        require(offset <= data.size() && sizeof(T) <= data.size() - offset, "it is truncated");
        T value{};
        std::memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }
    std::size_t pointer(std::size_t offset) const {
        const auto value = at<std::uint64_t>(offset);
        require(value && value <= data.size() - pointer_base, "a pointer leads outside it");
        return static_cast<std::size_t>(value) + pointer_base;
    }
};
struct Attribute {
    std::size_t base{}, stride{}, offset{};
    bool present{};
};
}

MeshLod read_mesh_lod(std::span<const std::byte> resource, std::size_t lod) {
    const Reader r{resource};
    for (std::size_t i = 0; i < known_header.size(); ++i)
        require(r.at<std::uint32_t>(i * 4) == known_header[i], "its layout is not the one this reader knows");
    require(r.at<std::uint8_t>(skinned_flag) == 1, "it is not skinned");
    require(lod < r.at<std::uint16_t>(lod_count), "it has no such level of detail");
    const auto at = r.pointer(lod_table + lod * 8);
    require(r.at<std::uint32_t>(at + lod_kind) == known_lod_kind, "the level of detail is of another kind");
    require(r.at<std::uint32_t>(at + index_format) == sixteen_bit_indices, "its indices are not 16-bit");
    MeshLod result;
    result.chunk = r.at<Guid>(at + geometry_chunk);
    result.vertex_bytes = r.at<std::uint32_t>(at + vertex_bytes);
    result.index_bytes = r.at<std::uint32_t>(at + index_bytes);
    require(result.chunk != Guid{} && result.vertex_bytes > 0 && result.index_bytes > 0, "the level of detail is empty");
    return result;
}

std::size_t mesh_lod_count(std::span<const std::byte> resource) {
    const Reader r{resource};
    for (std::size_t i = 0; i < known_header.size(); ++i)
        require(r.at<std::uint32_t>(i * 4) == known_header[i], "its layout is not the one this reader knows");
    return r.at<std::uint16_t>(lod_count);
}

MeshLod mesh_lod_geometry(std::span<const std::byte> resource, std::size_t lod) {
    require(lod < mesh_lod_count(resource), "it has no such level of detail");
    const Reader r{resource};
    const auto at = r.pointer(lod_table + lod * 8);
    require(r.at<std::uint32_t>(at + lod_kind) == known_lod_kind, "the level of detail is of another kind");
    MeshLod result;
    result.chunk = r.at<Guid>(at + geometry_chunk);
    result.vertex_bytes = r.at<std::uint32_t>(at + vertex_bytes);
    result.index_bytes = r.at<std::uint32_t>(at + index_bytes);
    require(result.chunk != Guid{} && result.vertex_bytes > 0, "the level of detail is empty");
    return result;
}

std::vector<std::array<float, 3>> read_mesh_positions(std::span<const std::byte> resource, std::size_t lod,
                                                      std::span<const std::byte> geometry) {
    const auto layout = mesh_lod_geometry(resource, lod);
    const Reader r{resource}, g{geometry};
    require(geometry.size() >= layout.vertex_bytes, "the geometry chunk is too short");
    const auto at = r.pointer(lod_table + lod * 8);
    const auto sections = r.at<std::uint32_t>(at + section_count);
    require(sections <= 4096, "it has too many sections");
    const auto table = r.pointer(at + section_table);
    std::vector<std::array<float, 3>> result;
    for (std::uint32_t section = 0; section < sections; ++section) {
        const auto s = table + section * section_size;
        const std::size_t count = r.at<std::uint32_t>(s + vertex_count);
        if (!count) continue;
        const auto d = s + declaration;
        const auto elements = r.at<std::uint8_t>(d + element_total), streams = r.at<std::uint8_t>(d + stream_total);
        require(elements <= 16 && streams > 0 && streams <= 16, "a vertex declaration is malformed");
        // Each stream's vertices follow the one before, from where the section starts.
        std::array<std::size_t, 16> bases{}, strides{};
        std::size_t end = r.at<std::uint32_t>(s + vertex_start);
        for (std::size_t stream = 0; stream < streams; ++stream) {
            strides[stream] = r.at<std::uint8_t>(d + stream_table + stream * 2);
            require(strides[stream] > 0, "a vertex stream has no size");
            bases[stream] = end;
            end += strides[stream] * count;
        }
        require(end <= layout.vertex_bytes, "a section's vertices run past the vertex buffer");
        bool found{};
        for (std::size_t element = 0; element < elements && !found; ++element) {
            if (r.at<std::uint8_t>(d + element * 4) != position) continue;
            const auto stream = r.at<std::uint8_t>(d + element * 4 + 3);
            const std::size_t offset = r.at<std::uint8_t>(d + element * 4 + 2);
            require(r.at<std::uint8_t>(d + element * 4 + 1) == usage_format[position] && stream < streams &&
                    offset + usage_size[position] <= strides[stream], "its positions have another format");
            for (std::size_t vertex = 0; vertex < count; ++vertex)
                result.push_back(g.at<std::array<float, 3>>(bases[stream] + vertex * strides[stream] + offset));
            found = true;
        }
        require(found, "a section has no positions");
    }
    return result;
}

namespace {
// The two layouts check_mesh_lod reads: the skinned one above, and the static one world meshes
// have (192, 176, 384, 0), whose levels of detail are laid out the same. A static mesh's sections
// are 384 or 416 bytes, and nothing found yet says which: check_mesh_lod takes the size under
// which every section reads as one.
struct Layout {
    bool skinned{};
};
Layout layout_of(const Reader& r) {
    const auto first = r.at<std::uint32_t>(0), second = r.at<std::uint32_t>(4), third = r.at<std::uint32_t>(8);
    if (first == 208 && second == 188 && third == 384) return {true};
    if (first == 192 && second == 176 && third == 384) return {false};
    require(false, "its layout is not one this reader knows");
    return {};
}
// Whether every section, `stride` bytes apart, reads as one: a triangle list whose indices are in
// the index buffer and whose vertices start in the vertex buffer.
bool sections_read(const Reader& r, std::size_t table, std::uint32_t sections, std::size_t stride, std::size_t index_size,
                   std::size_t vertex_size, std::size_t index_total) {
    for (std::uint32_t section = 0; section < sections; ++section) {
        const auto s = table + section * stride;
        if (s + stride > r.data.size()) return false;
        const std::size_t triangles = r.at<std::uint32_t>(s + triangle_count);
        if (!triangles) continue;
        if (r.at<std::uint8_t>(s + primitive) != triangle_list || r.at<std::uint32_t>(s + vertex_start) >= vertex_size ||
            std::size_t{r.at<std::uint32_t>(s + first_index)} * index_size + triangles * 3 * index_size > index_total)
            return false;
    }
    return true;
}
}

std::vector<std::string> check_mesh_lod(std::span<const std::byte> resource, std::size_t lod,
                                        std::span<const std::byte> geometry, std::uint32_t* format_out) {
    const Reader r{resource};
    const auto shape = layout_of(r);
    require(lod < r.at<std::uint16_t>(lod_count), "it has no such level of detail");
    const auto at = r.pointer(lod_table + lod * 8);
    const std::size_t vertex_size = r.at<std::uint32_t>(at + vertex_bytes), index_size_total = r.at<std::uint32_t>(at + index_bytes);
    const auto format = r.at<std::uint32_t>(at + index_format);
    if (format_out) *format_out = format;
    const std::size_t index_size = format == sixteen_bit_indices ? 2 : 4;
    std::vector<std::string> problems;
    const auto say = [&](std::string text) { if (problems.size() < 8) problems.push_back(std::move(text)); };
    if (geometry.size() < vertex_size + index_size_total)
        say("the geometry chunk holds " + std::to_string(geometry.size()) + " bytes of the " +
            std::to_string(vertex_size + index_size_total) + " its buffers need");
    const auto sections = r.at<std::uint32_t>(at + section_count);
    require(sections <= 4096, "it has too many sections");
    if (!sections) return problems;
    const auto table = r.pointer(at + section_table);
    std::size_t stride = section_size;
    if (!shape.skinned) {
        if (sections_read(r, table, sections, 384, index_size, vertex_size, index_size_total)) stride = 384;
        else if (sections_read(r, table, sections, 416, index_size, vertex_size, index_size_total)) stride = 416;
        else say("its sections read as neither 384 nor 416 bytes apart: one of them is out of its buffers");
    }
    for (std::uint32_t section = 0; section < sections; ++section) {
        const auto s = table + section * stride;
        const auto name = "section " + std::to_string(section);
        const auto type = r.at<std::uint8_t>(s + primitive);
        const std::size_t count = r.at<std::uint32_t>(s + vertex_count), triangles = r.at<std::uint32_t>(s + triangle_count);
        if (!triangles) continue;
        if (type != triangle_list) { say(name + " is primitive type " + std::to_string(type) + ", not a triangle list"); continue; }
        if (!count) { say(name + " has " + std::to_string(triangles) + " triangles and no vertices"); continue; }
        const std::size_t first = r.at<std::uint32_t>(s + vertex_start);
        if (first >= vertex_size) say(name + "'s vertices start at byte " + std::to_string(first) + " of a " + std::to_string(vertex_size) + "-byte vertex buffer");
        if (shape.skinned) {
            // (Where its vertices end: each stream's follow the one before.)
            const auto d = s + declaration;
            const auto streams = r.at<std::uint8_t>(d + stream_total);
            std::size_t end = first;
            for (std::size_t stream = 0; stream < streams && stream < 16; ++stream)
                end += std::size_t{r.at<std::uint8_t>(d + stream_table + stream * 2)} * count;
            if (end > vertex_size)
                say(name + "'s vertices end at byte " + std::to_string(end) + " of a " + std::to_string(vertex_size) + "-byte vertex buffer");
        }
        const std::size_t start = std::size_t{r.at<std::uint32_t>(s + first_index)} * index_size;
        const std::size_t length = triangles * 3 * index_size;
        if (start + length > index_size_total) {
            say(name + "'s indices end at byte " + std::to_string(start + length) + " of a " + std::to_string(index_size_total) + "-byte index buffer");
            continue;
        }
        const auto base = vertex_size + start;
        if (base + length > geometry.size()) continue;
        std::size_t outside{}, largest{};
        for (std::size_t i = 0; i < triangles * 3; ++i) {
            std::size_t index{};
            std::memcpy(&index, geometry.data() + base + i * index_size, index_size);
            if (index >= count) ++outside, largest = std::max(largest, index);
        }
        if (outside)
            say(name + ": " + std::to_string(outside) + " of " + std::to_string(triangles * 3) + " indices name a vertex outside its " +
                std::to_string(count) + " (the largest " + std::to_string(largest) + ")");
    }
    return problems;
}

MeshLod any_mesh_lod_geometry(std::span<const std::byte> resource, std::size_t lod, std::size_t* lods) {
    const Reader r{resource};
    (void)layout_of(r);
    const std::size_t total = r.at<std::uint16_t>(lod_count);
    if (lods) *lods = total;
    require(lod < total, "it has no such level of detail");
    const auto at = r.pointer(lod_table + lod * 8);
    MeshLod result;
    result.chunk = r.at<Guid>(at + geometry_chunk);
    result.vertex_bytes = r.at<std::uint32_t>(at + vertex_bytes);
    result.index_bytes = r.at<std::uint32_t>(at + index_bytes);
    return result;
}

namespace {
constexpr std::size_t metadata_section_limit = 4096, metadata_diagnostic_limit = 64;

bool metadata_span(const Reader& r, std::size_t at, std::size_t size) {
    return at <= r.data.size() && size <= r.data.size() - at;
}
std::size_t metadata_pointer(const Reader& r, std::size_t slot, std::size_t required) {
    const auto relative = r.at<std::uint64_t>(slot);
    require(r.data.size() >= pointer_base && relative != 0 && relative <= r.data.size() - pointer_base,
            "a metadata pointer leads outside the resource");
    const auto at = static_cast<std::size_t>(relative) + pointer_base;
    require(metadata_span(r, at, required), "a metadata pointer target is truncated");
    return at;
}
constexpr std::array<std::size_t, 2> observation_strides{section_size, 416};
constexpr std::size_t observation_section_limit = 64, observation_span_limit = 32768;
constexpr std::size_t observation_slot_limit = 16, observation_diagnostic_limit = 16;

void observation_problem(MeshSectionCandidateObservation& candidate, std::string text) {
    candidate.observation_complete = false;
    ++candidate.diagnostic_count;
    if (candidate.diagnostics.size() < observation_diagnostic_limit) candidate.diagnostics.push_back(std::move(text));
}
MeshSectionCandidateObservation observe_section_candidate(const Reader& r,
    const MeshLodCommonOffsetObservation& common, std::size_t stride) {
    MeshSectionCandidateObservation result;
    result.stride = stride;
    const auto count = common.section_count_raw;
    if (count == 0) {
        observation_problem(result, "raw section count is zero; no section table was observed");
        return result;
    }
    if (count > observation_section_limit) {
        observation_problem(result, "raw section count exceeds observation limit 64");
        return result;
    }
    // Charge both possible tables before allocating sections, even when one later fails bounds.
    if (count > observation_span_limit / (observation_strides[0] + observation_strides[1])) {
        observation_problem(result, "combined raw candidate table spans exceed observation limit 32768");
        return result;
    }
    const auto relative = common.section_pointer_raw;
    if (r.data.size() < pointer_base || relative == 0 || relative > r.data.size() - pointer_base) {
        observation_problem(result, "raw section pointer cannot be resolved within the resource");
        return result;
    }
    result.table_offset = static_cast<std::size_t>(relative) + pointer_base;
    result.table_pointer_resolved = true;
    if (count > (r.data.size() - result.table_offset) / stride) {
        observation_problem(result, "raw candidate table span is outside the resource");
        return result;
    }
    result.span_bytes = static_cast<std::size_t>(count) * stride;
    result.table_span_valid = true;
    result.observation_complete = true;
    result.sections.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto at = result.table_offset + index * stride;
        MeshSectionObservation section;
        section.resource_offset = at;
        section.primitive_raw = r.at<std::uint8_t>(at + primitive);
        section.triangle_count_raw = r.at<std::uint32_t>(at + triangle_count);
        section.first_index_raw = r.at<std::uint32_t>(at + first_index);
        section.vertex_start_raw = r.at<std::uint32_t>(at + vertex_start);
        section.vertex_count_raw = r.at<std::uint32_t>(at + vertex_count);
        const auto d = at + declaration;
        section.element_count_raw = r.at<std::uint8_t>(d + element_total);
        section.stream_count_raw = r.at<std::uint8_t>(d + stream_total);
        if (section.element_count_raw > observation_slot_limit || section.stream_count_raw > observation_slot_limit) {
            observation_problem(result, "section " + std::to_string(index) +
                ": raw declaration count exceeds 16; both declaration arrays were skipped");
            result.sections.push_back(std::move(section));
            continue;
        }
        section.elements.reserve(section.element_count_raw);
        for (std::size_t element = 0; element < section.element_count_raw; ++element) {
            const auto e = d + element * 4;
            section.elements.push_back({r.at<std::uint8_t>(e), r.at<std::uint8_t>(e + 1),
                r.at<std::uint8_t>(e + 2), r.at<std::uint8_t>(e + 3)});
        }
        section.streams.reserve(section.stream_count_raw);
        for (std::size_t stream = 0; stream < section.stream_count_raw; ++stream) {
            const auto s = d + stream_table + stream * 2;
            section.streams.push_back({r.at<std::uint8_t>(s), r.at<std::uint8_t>(s + 1)});
        }
        section.declarations_observed = true;
        result.sections.push_back(std::move(section));
    }
    return result;
}
void metadata_problem(MeshSectionLayoutMetadata& candidate, std::string text, bool structural) {
    if (structural) candidate.structurally_valid = false;
    candidate.declarations_supported = false;
    ++candidate.diagnostic_count;
    if (candidate.diagnostics.size() < metadata_diagnostic_limit) candidate.diagnostics.push_back(std::move(text));
}
std::uint8_t metadata_width(std::uint8_t format) {
    // These widths match the position/skinning reader; other declarations remain unknown.
    switch (format) {
    case 3: return 12;
    case 23: return 8;
    case 13: return 4;
    default: return 0;
    }
}
MeshSectionLayoutMetadata metadata_candidate(const Reader& r, const MeshLodMetadata& lod, std::size_t stride) {
    MeshSectionLayoutMetadata result;
    result.stride = stride;
    if (lod.section_table > r.data.size() || lod.section_count > (r.data.size() - lod.section_table) / stride) {
        metadata_problem(result, "section table span is outside the resource", true);
        return result;
    }
    result.sections.reserve(lod.section_count);
    for (std::size_t index = 0; index < lod.section_count; ++index) {
        const auto at = lod.section_table + index * stride;
        MeshSectionMetadata section;
        section.resource_offset = at;
        section.primitive = r.at<std::uint8_t>(at + primitive);
        section.triangle_count = r.at<std::uint32_t>(at + triangle_count);
        section.first_index = r.at<std::uint32_t>(at + first_index);
        section.vertex_start = r.at<std::uint32_t>(at + vertex_start);
        section.vertex_count = r.at<std::uint32_t>(at + vertex_count);
        const auto d = at + declaration;
        section.element_count = r.at<std::uint8_t>(d + element_total);
        section.stream_count = r.at<std::uint8_t>(d + stream_total);
        const auto label = "section " + std::to_string(index) + ": ";
        const auto problem = [&](const char* message, bool structural) { metadata_problem(result, label + message, structural); };
        if (section.element_count > 16 || section.stream_count > 16) {
            problem("declaration count exceeds its fixed table", true);
            result.sections.push_back(std::move(section));
            continue;
        }
        const bool has_vertices = section.vertex_count != 0;
        const bool has_triangles = section.triangle_count != 0;
        if (has_triangles && !has_vertices) problem("triangles have no vertices", true);
        if (has_triangles && section.primitive != triangle_list) problem("primitive interpretation is unsupported", false);
        if (has_vertices && (section.element_count == 0 || section.stream_count == 0))
            problem("vertices have no complete declaration", true);
        bool per_vertex = true;
        for (std::size_t stream = 0; stream < section.stream_count; ++stream) {
            const auto s = d + stream_table + stream * 2;
            const MeshStreamMetadata value{r.at<std::uint8_t>(s), r.at<std::uint8_t>(s + 1)};
            section.streams.push_back(value);
            if (value.classification != 0) {
                per_vertex = false;
                problem("stream classification interpretation is unsupported", false);
            } else if (value.stride == 0) {
                problem("per-vertex stream has zero stride", true);
            }
        }
        std::array<bool, 6> seen{};
        for (std::size_t element = 0; element < section.element_count; ++element) {
            const auto e = d + element * 4;
            MeshElementMetadata value;
            value.usage = r.at<std::uint8_t>(e);
            value.format = r.at<std::uint8_t>(e + 1);
            value.offset = r.at<std::uint8_t>(e + 2);
            value.stream = r.at<std::uint8_t>(e + 3);
            value.width = metadata_width(value.format);
            value.semantic_supported = value.usage >= position && value.usage <= weights_high &&
                                       value.format == usage_format[value.usage];
            if (!value.semantic_supported) problem("element usage/format interpretation is unsupported", false);
            if (value.usage >= position && value.usage <= weights_high) {
                if (seen[value.usage]) problem("known element usage is duplicated", true);
                seen[value.usage] = true;
            }
            if (value.stream >= section.stream_count) {
                problem("element refers to an absent stream", true);
            } else {
                const auto& stream = section.streams[value.stream];
                // An unknown classification cannot rule out a candidate through an assumed stride interpretation.
                if (stream.classification == 0 && (value.offset >= stream.stride ||
                    (value.width != 0 && value.width > stream.stride - value.offset)))
                    problem("element span is outside its stream stride", true);
            }
            section.elements.push_back(value);
        }
        if (has_vertices && !seen[position]) problem("vertex declaration has no position element", true);
        if (has_vertices && per_vertex && section.stream_count != 0) {
            std::size_t end = section.vertex_start;
            bool fits = end <= lod.geometry.vertex_bytes;
            for (const auto& stream : section.streams) {
                if (!fits) break;
                if (stream.stride > (lod.geometry.vertex_bytes - end) / section.vertex_count) fits = false;
                else end += std::size_t{stream.stride} * section.vertex_count;
            }
            section.vertex_range_checked = true;
            if (!fits) problem("declared streams exceed the vertex buffer", true);
        }
        if (has_triangles && section.primitive == triangle_list) {
            if (lod.index_format != sixteen_bit_indices) {
                problem("index width interpretation is unsupported", false);
            } else {
                section.index_range_checked = true;
                const std::size_t total = lod.geometry.index_bytes;
                const bool start_fits = section.first_index <= total / 2;
                if (!start_fits || section.triangle_count > (total - std::size_t{section.first_index} * 2) / 6)
                    problem("triangle range exceeds the index buffer", true);
            }
        }
        result.sections.push_back(std::move(section));
    }
    return result;
}
}

MeshLodMetadata read_mesh_lod_metadata(std::span<const std::byte> resource, std::size_t lod) {
    const Reader r{resource};
    MeshLodMetadata result;
    result.header = r.at<std::array<std::uint32_t, 4>>(0);
    constexpr std::array<std::uint32_t, 4> static_header{192, 176, 384, 0};
    require(result.header == known_header || result.header == static_header, "metadata layout header is unsupported");
    result.skinned = result.header == known_header;
    result.lod_count = r.at<std::uint16_t>(lod_count);
    result.lod_index = lod;
    require(lod < result.lod_count, "metadata has no such level of detail");
    require(lod_table <= resource.size() && result.lod_count <= (resource.size() - lod_table) / 8,
            "metadata LOD pointer table is truncated");
    result.lod_offset = metadata_pointer(r, lod_table + lod * 8, sizeof(std::uint32_t));
    const auto at = result.lod_offset;
    result.lod_kind = r.at<std::uint32_t>(at + lod_kind);
    if (result.lod_kind != known_lod_kind) {
        result.diagnostics.push_back("LOD kind is unsupported; no LOD layout interpretation was performed");
        if (result.header == static_header) {
            if (metadata_span(r, at, geometry_chunk + 16)) {
                // Raw offset observations do not establish a layout or authorize geometry decoding.
                MeshLodCommonOffsetObservation observed;
                observed.section_count_raw = r.at<std::uint32_t>(at + section_count);
                observed.section_pointer_raw = r.at<std::uint64_t>(at + section_table);
                observed.index_format_raw = r.at<std::uint32_t>(at + index_format);
                observed.index_bytes_raw = r.at<std::uint32_t>(at + index_bytes);
                observed.vertex_bytes_raw = r.at<std::uint32_t>(at + vertex_bytes);
                observed.geometry_identity_raw = r.at<std::array<std::byte, 16>>(at + geometry_chunk);
                result.static_common_offsets = observed;
                for (const auto stride : observation_strides)
                    result.static_section_candidates.push_back(observe_section_candidate(r, observed, stride));
            } else {
                result.diagnostics.push_back("static common-offset observation unavailable: full 132-byte span is missing");
            }
        }
        return result;
    }
    require(metadata_span(r, at, geometry_chunk + sizeof(Guid)), "metadata LOD fields are truncated");
    result.lod_kind_supported = true;
    result.index_format = r.at<std::uint32_t>(at + index_format);
    result.geometry.index_bytes = r.at<std::uint32_t>(at + index_bytes);
    result.geometry.vertex_bytes = r.at<std::uint32_t>(at + vertex_bytes);
    result.geometry.chunk = r.at<Guid>(at + geometry_chunk);
    result.section_count = r.at<std::uint32_t>(at + section_count);
    require(result.section_count <= metadata_section_limit, "metadata has too many sections");
    if (result.skinned && r.at<std::uint8_t>(skinned_flag) != 1) result.diagnostics.push_back("skinned header flag is inconsistent");
    if (result.index_format != sixteen_bit_indices) result.diagnostics.push_back("index width interpretation is unsupported");
    if (result.geometry.chunk == Guid{}) result.diagnostics.push_back("geometry chunk identity is zero");
    if (result.geometry.vertex_bytes == 0 || result.geometry.index_bytes == 0) result.diagnostics.push_back("geometry buffer is empty");
    if (result.section_count == 0) {
        if (r.at<std::uint64_t>(at + section_table) != 0) result.section_table = metadata_pointer(r, at + section_table, 0);
        result.diagnostics.push_back("no sections to interpret");
        return result;
    }
    result.section_table = metadata_pointer(r, at + section_table, 0);
    result.candidates.push_back(metadata_candidate(r, result, section_size));
    if (!result.skinned) result.candidates.push_back(metadata_candidate(r, result, 416));
    std::size_t plausible{};
    bool supported{};
    for (const auto& candidate : result.candidates) {
        if (!candidate.structurally_valid) continue;
        ++plausible;
        supported = candidate.declarations_supported;
    }
    result.ambiguous = plausible > 1;
    result.complete = plausible == 1 && supported && result.diagnostics.empty();
    return result;
}
SkinnedMesh read_skinned_mesh(std::span<const std::byte> resource, std::size_t lod, std::span<const std::byte> geometry) {
    const auto layout = read_mesh_lod(resource, lod);
    const Reader r{resource}, g{geometry};
    require(geometry.size() >= std::size_t{layout.vertex_bytes} + layout.index_bytes, "the geometry chunk is too short");
    const auto at = r.pointer(lod_table + lod * 8);
    const auto sections = r.at<std::uint32_t>(at + section_count);
    const auto table = r.pointer(at + section_table);
    SkinnedMesh result;
    for (std::uint32_t section = 0; section < sections; ++section) {
        const auto s = table + section * section_size;
        require(r.at<std::uint8_t>(s + primitive) == triangle_list, "a section is not a triangle list");
        const auto count = r.at<std::uint32_t>(s + vertex_count), triangles = r.at<std::uint32_t>(s + triangle_count);
        const auto bones = r.pointer(s + palette);
        const auto bone_count = r.at<std::uint16_t>(s + palette_count);
        const auto d = s + declaration;
        const auto elements = r.at<std::uint8_t>(d + element_total), streams = r.at<std::uint8_t>(d + stream_total);
        require(elements <= 16 && streams > 0 && streams <= 16, "a vertex declaration is malformed");
        std::array<std::size_t, 16> bases{}, strides{};
        std::size_t end = r.at<std::uint32_t>(s + vertex_start);
        for (std::size_t stream = 0; stream < streams; ++stream) {
            strides[stream] = r.at<std::uint8_t>(d + stream_table + stream * 2);
            require(strides[stream] > 0 && r.at<std::uint8_t>(d + stream_table + stream * 2 + 1) == 0,
                "a vertex stream is not per vertex");
            bases[stream] = end;
            end += strides[stream] * count;
        }
        require(end <= layout.vertex_bytes, "a section's vertices run past the vertex buffer");
        std::array<Attribute, 6> attributes{};
        for (std::size_t element = 0; element < elements; ++element) {
            const auto usage = r.at<std::uint8_t>(d + element * 4);
            if (usage < position || usage > weights_high) continue;
            const auto stream = r.at<std::uint8_t>(d + element * 4 + 3);
            const std::size_t offset = r.at<std::uint8_t>(d + element * 4 + 2);
            require(r.at<std::uint8_t>(d + element * 4 + 1) == usage_format[usage] && stream < streams &&
                    offset + usage_size[usage] <= strides[stream],
                "a skinning attribute has another format");
            attributes[usage] = {bases[stream], strides[stream], offset, true};
        }
        for (const auto usage : {position, bones_low, weights_low}) require(attributes[usage].present, "a section is not skinned");
        require(attributes[bones_high].present == attributes[weights_high].present, "a section's second influences are half there");
        const std::size_t influences = attributes[bones_high].present ? 8 : 4;
        const auto address = [&](std::uint8_t usage, std::size_t vertex) {
            const auto& a = attributes[usage];
            return a.base + vertex * a.stride + a.offset;
        };
        const auto first = static_cast<std::uint32_t>(result.vertices.size());
        for (std::size_t vertex = 0; vertex < count; ++vertex) {
            SkinnedVertex out;
            out.position = g.at<std::array<float, 3>>(address(position, vertex));
            for (const float coordinate : out.position) require(std::isfinite(coordinate), "a vertex is not finite");
            unsigned total{};
            for (std::size_t influence = 0; influence < influences; ++influence) {
                const auto high = influence >= 4;
                const auto weight = g.at<std::uint8_t>(address(high ? weights_high : weights_low, vertex) + influence % 4);
                if (!weight) continue; // an unused slot's bone need not be set
                const auto slot = g.at<std::uint16_t>(address(high ? bones_high : bones_low, vertex) + influence % 4 * 2);
                require(slot < bone_count, "a vertex names a bone outside its palette");
                out.bones[influence] = r.at<std::uint16_t>(bones + slot * 2u);
                out.weights[influence] = weight;
                total += weight;
            }
            require(total == 255, "a vertex's weights do not add up");
            result.vertices.push_back(out);
        }
        const std::size_t start = std::size_t{r.at<std::uint32_t>(s + first_index)} * 2;
        require(start + std::size_t{triangles} * 6 <= layout.index_bytes, "a section's indices run past the index buffer");
        for (std::size_t i = 0; i < std::size_t{triangles} * 3; ++i) {
            const auto index = g.at<std::uint16_t>(layout.vertex_bytes + start + i * 2);
            require(index < count, "a triangle names a vertex outside its section");
            result.triangles.push_back(first + index);
        }
    }
    return result;
}
}
