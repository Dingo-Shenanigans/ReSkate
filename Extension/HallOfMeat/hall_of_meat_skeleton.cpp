#include "hall_of_meat_skeleton.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_skeleton.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace dingosdk::hall_of_meat {
namespace {
namespace build = addr::skater_skeleton;

void require(bool valid, const std::string& message) {
    if (!valid) throw std::runtime_error(message);
}
Vec3 normalised(const Vec3& v) {
    const float length = game::length(v);
    return length > 1e-8f ? Vec3{v[0] / length, v[1] / length, v[2] / length} : Vec3{0, 1, 0};
}
constexpr int no_body = -1;

// Each bone's body: its own joint's, else its nearest ancestor's; none above the hips.
std::vector<int> bodies_of(const frostbite::Skeleton& skeleton) {
    std::vector<int> body(skeleton.names.size(), no_body);
    for (std::size_t b = 1; b < skater_body::count; ++b) {
        const auto found = std::find(skeleton.names.begin(), skeleton.names.end(), skeleton_joints[b]);
        require(found != skeleton.names.end(), "the skeleton has no " + std::string(skeleton_joints[b]));
        body[static_cast<std::size_t>(found - skeleton.names.begin())] = static_cast<int>(b);
    }
    for (std::size_t bone = 0; bone < body.size(); ++bone) { // parents come first
        const auto parent = skeleton.parents[bone];
        if (body[bone] == no_body && parent >= 0) body[bone] = body[static_cast<std::size_t>(parent)];
    }
    return body;
}

vfs::GameBundle bundle(const vfs::GameData& data, const frostbite::TocDocument& toc, std::string_view name) {
    auto found = data.read_bundle(toc, name);
    require(found.has_value(), "the bundle " + std::string(name) + " is missing");
    return std::move(*found);
}
std::vector<std::byte> asset(const vfs::GameData& data, const vfs::GameBundle& bundle, frostbite::AssetKind kind,
    std::string_view name) {
    std::size_t index{};
    require(bundle.find(kind, name, &index) && bundle.payload(kind, index), "the asset " + std::string(name) + " is missing");
    return data.read(*bundle.payload(kind, index));
}
// A geometry chunk: in the bundle when it has it, else in its superbundle's TOC.
std::vector<std::byte> chunk(const vfs::GameData& data, const vfs::GameBundle& bundle, const frostbite::TocDocument& toc,
    const frostbite::Guid& id) {
    std::size_t index{};
    if (bundle.find_chunk(id, &index) && bundle.payload(frostbite::AssetKind::chunk, index))
        return data.read(*bundle.payload(frostbite::AssetKind::chunk, index));
    const auto found = std::find_if(toc.chunks.begin(), toc.chunks.end(),
        [&](const frostbite::TocChunk& candidate) { return candidate.guid == id && !candidate.removed; });
    require(found != toc.chunks.end(), "the mesh's geometry chunk is missing");
    return data.read({found->location, found->offset, found->size});
}

struct Loader {
    std::once_flag started;
    std::atomic<std::shared_ptr<const SkeletonMesh>> mesh;
};
Loader& loader() { static auto* value = new Loader; return *value; }
}

SkeletonMesh bind_skeleton(const frostbite::Skeleton& skeleton, const frostbite::SkinnedMesh& mesh) {
    const auto body_of = bodies_of(skeleton);
    SkeletonMesh result;
    result.bone_count = skeleton.names.size();
    for (const auto& source : mesh.vertices) {
        // The heaviest few bones, scaled back to 1.
        std::array<std::size_t, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return source.weights[a] > source.weights[b]; });
        SkeletonVertex vertex;
        vertex.position = source.position;
        float kept{};
        for (std::size_t slot = 0; slot < max_influences && source.weights[order[slot]]; ++slot) {
            const auto bone = source.bones[order[slot]];
            require(bone < body_of.size(), "a vertex names a bone the skeleton lacks");
            require(body_of[bone] != no_body, "a vertex rides on a bone no body moves");
            vertex.bones[slot] = bone;
            vertex.weights[slot] = source.weights[order[slot]] / 255.0f;
            kept += vertex.weights[slot];
        }
        require(kept > 0, "a vertex has no weight");
        for (auto& weight : vertex.weights) weight /= kept;
        result.parts.push_back(static_cast<std::uint8_t>(body_of[vertex.bones[0]]));
        result.vertices.push_back(vertex);
    }
    require(mesh.triangles.size() % 3 == 0, "a triangle is incomplete");
    for (const auto index : mesh.triangles) require(index < result.vertices.size(), "a triangle names a missing vertex");
    result.triangles = mesh.triangles;
    // Area-weighted smooth normals: the costume's own are packed in a form not worth decoding
    // for an untextured x-ray.
    for (std::size_t i = 0; i < result.triangles.size(); i += 3) {
        auto& a = result.vertices[result.triangles[i]];
        auto& b = result.vertices[result.triangles[i + 1]];
        auto& c = result.vertices[result.triangles[i + 2]];
        const Vec3 u{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const Vec3 v{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        const Vec3 n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (auto* vertex : {&a, &b, &c})
            for (std::size_t axis = 0; axis < 3; ++axis) vertex->normal[axis] += n[axis];
    }
    for (auto& vertex : result.vertices) vertex.normal = normalised(vertex.normal);
    return result;
}

bool pose_skeleton(const SkeletonMesh& mesh, std::span<const LinearTransform> skin, PosedSkeleton& posed) {
    if (skin.size() < mesh.bone_count) return false;
    posed.positions.resize(mesh.vertices.size());
    posed.normals.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& vertex = mesh.vertices[i];
        Vec3 position{}, normal{};
        for (std::size_t slot = 0; slot < max_influences && vertex.weights[slot] > 0; ++slot) {
            const auto& transform = skin[vertex.bones[slot]];
            const auto p = game::place(vertex.position, transform), n = game::turn(vertex.normal, transform);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                position[axis] += p[axis] * vertex.weights[slot];
                normal[axis] += n[axis] * vertex.weights[slot];
            }
        }
        posed.positions[i] = position;
        posed.normals[i] = normalised(normal);
    }
    return true;
}

std::filesystem::path game_folder() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(length && length < path.size(), "the game's folder is unknown");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

SkeletonMesh read_skeleton_mesh(const std::filesystem::path& game_root) {
    const vfs::GameData data(game_root);
    const auto skeleton_toc = data.read_toc(build::skeleton_toc);
    const auto skeleton_document =
        frostbite::ebx::read_document(asset(data, bundle(data, skeleton_toc, build::skeleton_bundle), frostbite::AssetKind::ebx,
            build::skeleton_asset));
    const auto* root = skeleton_document.root();
    require(root && root->object, "the render skeleton is empty");
    const auto skeleton = frostbite::read_skeleton(*root->object);
    const auto mesh_toc = data.read_toc(build::mesh_toc);
    const auto mesh_bundle = bundle(data, mesh_toc, build::mesh_bundle);
    const auto resource = asset(data, mesh_bundle, frostbite::AssetKind::resource, build::mesh_asset);
    const auto lod = frostbite::read_mesh_lod(resource, build::mesh_lod);
    const auto geometry = chunk(data, mesh_bundle, mesh_toc, lod.chunk);
    return bind_skeleton(skeleton, frostbite::read_skinned_mesh(resource, build::mesh_lod, geometry));
}

void prepare_skeleton() noexcept {
    try {
        std::call_once(loader().started, [] {
            std::thread([] {
                const auto started = GetTickCount64();
                try {
                    auto read = std::make_shared<const SkeletonMesh>(read_skeleton_mesh(game_folder()));
                    logging::log(logging::Level::info, logging::Channel::skater,
                        "Hall of Meat: read the skater's skeleton ({} vertices, {} triangles) in {} ms.", read->vertices.size(),
                        read->triangles.size() / 3, GetTickCount64() - started);
                    loader().mesh.store(std::move(read));
                } catch (const std::exception& failure) {
                    logging::log(logging::Level::warning, logging::Channel::skater, "Hall of Meat shows no skeleton: {}.", failure.what());
                }
            }).detach();
        });
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::skater, "Hall of Meat shows no skeleton: its reading could not start.");
    }
}

std::shared_ptr<const SkeletonMesh> skeleton_mesh() noexcept { return loader().mesh.load(); }
}
