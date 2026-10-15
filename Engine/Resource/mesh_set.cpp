#include "mesh_set.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

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
