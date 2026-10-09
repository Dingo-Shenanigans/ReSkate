// The skinned MeshSet and SkeletonAsset readers, on made-up resources.
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/skeleton_asset.h"
#include <cstring>
#include <iostream>
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
        a_skeleton_reads();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Skinned mesh tests passed.\n";
    return 0;
}
