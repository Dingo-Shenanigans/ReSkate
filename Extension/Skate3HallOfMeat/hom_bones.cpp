#include "hom_bones.h"
#include "hom_art.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::overlay {
namespace {
namespace fs = std::filesystem;

struct Mesh {
    std::string name;
    std::vector<Vec3f> positions, normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::uint16_t> indices;
};
struct Library {
    std::once_flag once;
    bool ready{};
    std::vector<Mesh> meshes;
};
Library &library() {
    static Library value;
    return value;
}

Vec3f sub(const Vec3f &a, const Vec3f &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3f add(const Vec3f &a, const Vec3f &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3f mul(const Vec3f &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec3f &a, const Vec3f &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3f cross(const Vec3f &a, const Vec3f &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec3f &a) { return std::sqrt(dot(a, a)); }
Vec3f normalise(const Vec3f &a) {
    const float l = length(a);
    return l > 1e-6f ? mul(a, 1.0f / l) : Vec3f{0, 1, 0};
}
// An orthonormal frame: `axis` along the bone, `front` toward the body's forward, `side` = axis x front.
struct Frame {
    Vec3f axis, front, side;
};
Frame frame(const Vec3f &axis, const Vec3f &forward, const Vec3f &fallback) {
    Frame f;
    f.axis = normalise(axis);
    Vec3f front = sub(forward, mul(f.axis, dot(forward, f.axis)));
    if (length(front) < 0.2f) front = sub(fallback, mul(f.axis, dot(fallback, f.axis)));
    f.front = normalise(front);
    f.side = cross(f.axis, f.front);
    return f;
}

// Where each mesh sits in Skate 3's bind pose (A-pose, Y up, facing +Z, the skater's left on +X),
// as the same two points hom_rig.cpp publishes for the live bone.
struct Bind {
    Vec3f a, b;
};
Vec3f along(const Vec3f &from, const Vec3f &to, float metres) { return add(to, mul(normalise(sub(to, from)), metres)); }
const std::array<Bind, 19> &binds() {
    static const std::array<Bind, 19> value = [] {
        const Vec3f l_shoulder{0.165f, 1.425f, -0.03f}, l_elbow{0.355f, 1.21f, -0.005f}, l_wrist{0.51f, 1.04f, 0.105f};
        const Vec3f l_hip{0.10f, 0.95f, 0.0f}, l_knee{0.11f, 0.49f, 0.0f}, l_ankle{0.11f, 0.085f, -0.01f}, l_ball{0.11f, 0.03f, 0.11f};
        const auto mirror = [](Vec3f v) { return Vec3f{-v[0], v[1], v[2]}; };
        const auto mid = [](const Vec3f &a, const Vec3f &b) { return mul(add(a, b), 0.5f); };
        std::array<Bind, 19> b{};
        b[0] = {{0, 1.55f, 0.0f}, {0, 1.72f, 0.0f}};
        b[1] = {{0, 1.48f, -0.01f}, {0, 1.56f, -0.01f}};
        b[2] = {{0, 1.20f, -0.03f}, {0, 1.50f, -0.02f}};
        b[3] = {{0, 0.98f, -0.06f}, {0, 1.20f, -0.06f}};
        b[4] = {l_hip, mirror(l_hip)};
        b[5] = {l_shoulder, l_elbow};
        b[6] = {mirror(l_shoulder), mirror(l_elbow)};
        b[7] = {l_elbow, l_wrist};
        b[8] = {mirror(l_elbow), mirror(l_wrist)};
        b[9] = {l_wrist, along(l_elbow, l_wrist, 0.09f)};
        b[10] = {mirror(l_wrist), along(mirror(l_elbow), mirror(l_wrist), 0.09f)};
        b[11] = {l_hip, l_knee};
        b[12] = {mirror(l_hip), mirror(l_knee)};
        b[13] = {l_knee, l_ankle};
        b[14] = {mirror(l_knee), mirror(l_ankle)};
        b[15] = {l_ankle, mid(l_ankle, l_ball)};
        b[16] = {mirror(l_ankle), mid(mirror(l_ankle), mirror(l_ball))};
        b[17] = {l_ball, along(l_ankle, l_ball, 0.07f)};
        b[18] = {mirror(l_ball), along(mirror(l_ankle), mirror(l_ball), 0.07f)};
        return b;
    }();
    return value;
}

void load() {
    auto &lib = library();
    const auto root = overlay::hom_directory();
    if (root.empty()) return;
    const auto file = root / L"bones.bin";
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    const auto read = [&](void *out, std::size_t size) { return static_cast<bool>(in.read(static_cast<char *>(out), static_cast<std::streamsize>(size))); };
    char magic[4]{};
    std::uint32_t count{};
    if (!read(magic, 4) || std::memcmp(magic, "HOM2", 4) || !read(&count, 4) || count != 19) return;
    std::vector<Mesh> meshes(count);
    for (auto &m : meshes) {
        std::uint8_t name_length{};
        std::uint32_t vertices{}, indices{};
        if (!read(&name_length, 1)) return;
        m.name.resize(name_length);
        if (!read(m.name.data(), name_length) || !read(&vertices, 4) || vertices > 65535) return;
        m.positions.resize(vertices);
        m.normals.resize(vertices);
        m.uvs.resize(vertices);
        for (std::uint32_t i = 0; i < vertices; ++i)
            if (!read(m.positions[i].data(), 12) || !read(m.normals[i].data(), 12) || !read(m.uvs[i].data(), 8)) return;
        if (!read(&indices, 4) || indices > 200000 || indices % 3) return;
        m.indices.resize(indices);
        if (!read(m.indices.data(), indices * 2ULL)) return;
        for (const auto index : m.indices)
            if (index >= vertices) return;
    }
    // Skate 3 names its two calf meshes the wrong way round (Bones_Calf_Left sits on the skater's
    // right). Every left/right pair is put on its own side here: the left one on +X, as binds() has
    // it, else the bone is drawn its body's width away from its joint.
    const auto centre_x = [](const Mesh &m) {
        float sum = 0;
        for (const auto &p : m.positions) sum += p[0];
        return m.positions.empty() ? 0.0f : sum / static_cast<float>(m.positions.size());
    };
    for (std::size_t left = 5; left + 1 < meshes.size(); left += 2)
        if (centre_x(meshes[left]) < 0.0f && centre_x(meshes[left + 1]) > 0.0f) std::swap(meshes[left], meshes[left + 1]);
    lib.meshes = std::move(meshes);
    lib.ready = true;
    logging::log(logging::Level::info, logging::Channel::graphics, "Hall Of Meat: {} X-ray bone meshes loaded.", lib.meshes.size());
}

ImU32 scale_colour(ImU32 colour, float light, float alpha) {
    const auto channel = [&](int shift) {
        return static_cast<ImU32>(std::clamp(static_cast<float>((colour >> shift) & 0xff) * light, 0.0f, 255.0f));
    };
    return channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT | channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT |
           channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT | static_cast<ImU32>(std::clamp(alpha * 255.0f, 0.0f, 255.0f)) << IM_COL32_A_SHIFT;
}
} // namespace

bool BoneProjector::project(const Vec3f &p, ImVec2 &at, float &depth) const {
    const auto &m = camera;
    const float dx = p[0] - m[12], dy = p[1] - m[13], dz = p[2] - m[14];
    depth = -(dx * m[8] + dy * m[9] + dz * m[10]);
    if (depth < 0.1f) return false;
    const float side = dx * m[0] + dy * m[1] + dz * m[2], height = dx * m[4] + dy * m[5] + dz * m[6];
    at = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
    return true;
}
Vec3f BoneProjector::view_direction(const Vec3f &p) const { return normalise(Vec3f{p[0] - camera[12], p[1] - camera[13], p[2] - camera[14]}); }

bool hom_bones_ready() noexcept {
    auto &lib = library();
    try {
        std::call_once(lib.once, load);
    } catch (...) {
    }
    return lib.ready;
}

void draw_hom_bone(ImDrawList *draw, const BoneProjector &projector, std::size_t index, const Vec3f &a, const Vec3f &b, const Vec3f &forward,
                   ImU32 colour, float opacity, float core) noexcept {
    if (!hom_bones_ready() || index >= 19) return;
    const auto &mesh = library().meshes[index];
    const auto &bind = binds()[index];
    const float bind_length = length(sub(bind.b, bind.a)), live_length = length(sub(b, a));
    if (bind_length < 1e-4f || live_length < 1e-4f || live_length > 3.0f) return;
    const float scale = std::clamp(live_length / bind_length, 0.5f, 2.0f);
    // Hips and other sideways bones: their axis runs across the body, so roll comes from up.
    const Frame from = frame(sub(bind.b, bind.a), {0, 0, 1}, {0, 1, 0});
    const Frame to = frame(sub(b, a), forward, {0, 1, 0});
    try {
        const std::size_t count = mesh.positions.size();
        std::vector<ImVec2> screen(count);
        std::vector<float> depth(count), light(count);
        std::vector<bool> visible(count);
        const Vec3f centre_bind = mul(add(bind.a, bind.b), 0.5f);
        const float half = std::max(0.02f, bind_length * 0.5f);
        std::vector<float> heat(count, 0.0f);
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3f local = sub(mesh.positions[i], bind.a);
            const float x = dot(local, from.axis), y = dot(local, from.front), z = dot(local, from.side);
            const float girth = std::min(1.0f, scale);
            const Vec3f world = add(a, add(mul(to.axis, x * scale), mul(add(mul(to.front, y), mul(to.side, z)), girth)));
            const Vec3f &n0 = mesh.normals[i];
            const Vec3f normal = normalise(add(add(mul(to.axis, dot(n0, from.axis)), mul(to.front, dot(n0, from.front))), mul(to.side, dot(n0, from.side))));
            visible[i] = projector.project(world, screen[i], depth[i]);
            // Skate 3's X-ray glows: a pale hot core where the bone faces the camera, its colour at the
            // rim. `light` holds how much core (1 facing, 0 edge on).
            const float facing = std::abs(dot(normal, projector.view_direction(world)));
            light[i] = facing; // raised per bone below: warmer bones show more core
            if (core > 0.0f) {
                const float d = length(sub(mesh.positions[i], centre_bind)) / half;
                heat[i] = core * std::clamp(1.0f - d, 0.0f, 1.0f);
            }
        }
        // Far triangles first: there is no depth buffer.
        const std::size_t triangles = mesh.indices.size() / 3;
        std::vector<std::pair<float, std::uint32_t>> order;
        order.reserve(triangles);
        for (std::uint32_t t = 0; t < triangles; ++t) {
            const auto i0 = mesh.indices[t * 3], i1 = mesh.indices[t * 3 + 1], i2 = mesh.indices[t * 3 + 2];
            if (!visible[i0] || !visible[i1] || !visible[i2]) continue;
            order.emplace_back(depth[i0] + depth[i1] + depth[i2], t);
        }
        std::sort(order.begin(), order.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
        if (order.empty()) return;
        // Emissive, as Skate 3's X-ray reads in play: flat colour, no bone map detail.
        const auto white = ImGui::GetFontTexUvWhitePixel();
        // The core: the rim colour taken most of the way to a warm white (a cool white for white bones).
        const auto channel = [](ImU32 c, int shift) { return static_cast<float>((c >> shift) & 0xff); };
        const float rim_r = channel(colour, IM_COL32_R_SHIFT), rim_g = channel(colour, IM_COL32_G_SHIFT), rim_b = channel(colour, IM_COL32_B_SHIFT);
        const float warm = std::clamp((rim_r - rim_b) / 200.0f, 0.0f, 1.0f);
        const float core_r = 255.0f, core_g = 252.0f - 47.0f * warm, core_b = 255.0f - 80.0f * warm;
        const float core_power = 0.8f - 0.3f * warm;
        constexpr ImU32 hot = IM_COL32(185, 50, 27, 255);
        for (std::size_t first = 0; first < order.size(); first += 4000) {
            const std::size_t n = std::min<std::size_t>(4000, order.size() - first);
            draw->PrimReserve(static_cast<int>(n * 3), static_cast<int>(n * 3));
            for (std::size_t k = 0; k < n; ++k) {
                const auto t = order[first + k].second;
                for (int c = 0; c < 3; ++c) {
                    const auto i = mesh.indices[t * 3 + c];
                    const float f = std::pow(light[i], core_power);
                    ImU32 tint = IM_COL32(static_cast<int>(rim_r + (core_r - rim_r) * f), static_cast<int>(rim_g + (core_g - rim_g) * f),
                                          static_cast<int>(rim_b + (core_b - rim_b) * f), 255);
                    if (heat[i] > 0.0f) {
                        const float h = heat[i];
                        const auto mix = [&](int shift) {
                            const float x = static_cast<float>((colour >> shift) & 0xff), y = static_cast<float>((hot >> shift) & 0xff);
                            return static_cast<ImU32>(x + (y - x) * h) << shift;
                        };
                        const ImU32 base = tint;
                        const auto mix2 = [&](int shift) {
                            const float x = static_cast<float>((base >> shift) & 0xff), y = static_cast<float>((hot >> shift) & 0xff);
                            return static_cast<ImU32>(x + (y - x) * h) << shift;
                        };
                        tint = mix2(IM_COL32_R_SHIFT) | mix2(IM_COL32_G_SHIFT) | mix2(IM_COL32_B_SHIFT) | IM_COL32_A_MASK;
                        (void)mix;
                    }
                    const auto vertex = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
                    draw->PrimWriteVtx(screen[i], white, scale_colour(tint, 1.0f, opacity));
                    draw->PrimWriteIdx(vertex);
                }
            }
        }
    } catch (...) {
    }
}
} // namespace dingosdk::overlay
