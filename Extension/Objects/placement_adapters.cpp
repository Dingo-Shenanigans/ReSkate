#include "placement_adapters.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Engine/Game/Build/20260929/placement_adapters.h"
#include "Engine/Game/Build/20260929/placement_physics.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "ParkEditor/park_editor_internal.h"
#include "local_placements_runtime.h"
#include <deque>
#include <intrin.h>

namespace dingosdk::placement_adapters {
namespace {
using namespace placement_geometry;
namespace abi = addr::placement_adapters;
namespace physics = addr::placement_physics;
std::uintptr_t base{};
bool attempted{}, ready{};
std::mutex mutex;
std::map<std::uint64_t, Published> published;
std::map<std::uint64_t, std::uintptr_t> selected;
std::vector<Edit> edits;
std::map<Body, std::uint64_t> surfaces;
struct Support {
    std::uintptr_t context{};
    Body body{};
    Vec3 point{};
    std::uint64_t at{};
};
Support supporting;
std::uintptr_t surface_context{};
struct CachedEdge {
    Edge local;
    Body shape;
};
struct NativeChain {
    const Edge* edges{};
    std::uint32_t count{}, padding{};
};
struct CachedChain {
    Body body{}, shape{};
    std::vector<Edge> local, world;
};
std::map<Body, std::vector<CachedEdge>> cached_edges;
std::map<std::array<std::uint64_t, 2>, CachedChain*> cached_chains;
// Contacts borrow pointers across worker phases. Never invalidate published
// storage, even on level reset. A process-wide budget bounds retention.
std::deque<CachedChain> chain_storage;
std::size_t chain_edge_count{};
bool (*chain_original)(const std::uint64_t*, NativeChain*){};
void (*edges_original)(std::uintptr_t){};
bool (*edge_query_original)(std::uintptr_t, const float*, std::uintptr_t, int, int, int){};
void (*friction_original)(std::uintptr_t, float, float, float){};
int (*motion_original)(const Body*){};
void* (*root_original)(void*, std::uint64_t, bool){};
void (*transform_original)(const void*, const float*, std::uintptr_t){};
float* (*pose_original)(float*, const void*){};
void (*select_original)(std::uintptr_t, const void*, bool){};
void (*finish_original)(std::uintptr_t, const void*, bool, bool, bool){};
bool (*valid)(const Body*){};
float* (*body_pose)(const Body*, float*){};
float* (*point_velocity)(const Body*, float*, const float*){};
Body* (*shape_body)(const Body*, Body*){};
thread_local std::uintptr_t selecting{};
thread_local std::map<std::uint64_t, Pose>* selection_poses{};
bool read_body_pose(const Body& body, Pose& pose) {
    alignas(16) std::array<float, 8> rigid{};
    if (!valid(&body) || body_pose(&body, rigid.data()) != rigid.data())
        return false;
    std::copy_n(rigid.begin(), 4, pose.begin() + 4);
    std::copy_n(rigid.begin() + 4, 3, pose.begin() + 8);
    return finite(pose);
}
bool read_pose(std::uint64_t entity, Pose& pose) {
    alignas(16) std::array<std::uint64_t, 3> query{};
    // This read uses native functions, not the plugin host or the game mutex.
    const auto query_fn = profile_runtime::placements_runtime().query;
    return query_fn(query.data(), entity) == query.data() && pose_original(pose.data(), query.data()) == pose.data() && finite(pose);
}
bool selection_contains(std::uintptr_t controller, std::uint64_t entity) {
    std::uintptr_t buckets{}, node{};
    std::uint32_t count{}, size{};
    if (!memory::peek(controller + abi::selection_buckets, buckets) || !buckets ||
        !memory::peek(controller + abi::selection_count, count) || !count || count > 65536 ||
        !memory::peek(controller + abi::selection_size, size) || !size || size > 1024 ||
        !memory::peek(buckets + (entity % count) * 8, node))
        return false;
    for (unsigned n = 0; node && n < size; ++n) {
        std::uint64_t key{};
        if (!memory::peek(node, key))
            return false;
        if (key == entity)
            return true;
        if (!memory::peek(node + abi::selection_next, node))
            return false;
    }
    return false;
}
void reconcile(std::uintptr_t controller) {
    std::lock_guard lock(mutex);
    std::erase_if(selected, [&](const auto& e) { return e.second == controller && !selection_contains(controller, e.first); });
}
void* root_hook(void* out, std::uint64_t entity, bool checks) {
    auto* result = root_original(out, entity, checks);
    if (checks && result == out && out && !multiplayer_session_active()) {
        std::lock_guard lock(mutex);
        if (published.contains(*static_cast<std::uint64_t*>(out)))
            *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(out) + abi::root_flags) &= ~abi::root_dynamic_restriction;
    }
    return result;
}
void transform_hook(const void* query, const float* pose, std::uintptr_t source) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base;
    transform_original(query, pose, source);
    if (std::none_of(abi::transform_calls.begin(), abi::transform_calls.end(), [&](auto call) { return caller == call + 5; }))
        return;
    try {
        Pose wanted{};
        std::uint64_t entity{};
        if (!query || !pose || !memory::peek(reinterpret_cast<std::uintptr_t>(pose), wanted) || !finite(wanted) ||
            !memory::peek(reinterpret_cast<std::uintptr_t>(query), entity))
            return;
        alignas(16) std::array<std::uint64_t, 2> root{};
        if (root_original(root.data(), entity, false) == root.data() && root[0])
            entity = root[0];
        std::lock_guard lock(mutex);
        const auto at = published.find(entity);
        if (at != published.end() && edits.size() < 4096)
            edits.push_back({at->second.id, wanted, false});
    } catch (...) {
    }
}
float* pose_hook(float* out, const void* query) {
    auto* result = pose_original(out, query);
    if (!selecting || result != out || !out || !query || multiplayer_session_active())
        return result;
    try {
        std::uint64_t entity{};
        if (!memory::peek(reinterpret_cast<std::uintptr_t>(query), entity))
            return result;
        if (selection_poses) {
            const auto at = selection_poses->find(entity);
            if (at != selection_poses->end()) {
                std::copy(at->second.begin(), at->second.end(), out);
                return result;
            }
        }
        Published object;
        {
            std::lock_guard lock(mutex);
            const auto at = published.find(entity);
            if (at == published.end() || selected.contains(entity))
                return result;
            object = at->second;
        }
        Pose current{};
        if (!memory::peek(reinterpret_cast<std::uintptr_t>(out), current) || !read_body_pose(object.body, current))
            return result;
        std::copy(current.begin(), current.end(), out);
        if (selection_poses)
            (*selection_poses)[entity] = current;
    } catch (...) {
    }
    return result;
}
void select_hook(std::uintptr_t controller, const void* entities, bool quiet) {
    struct Scope {
        std::uintptr_t previous{selecting};
        std::map<std::uint64_t, Pose>* poses{selection_poses};
        ~Scope() {
            selecting = previous;
            selection_poses = poses;
        }
    } scope;
    std::map<std::uint64_t, Pose> poses;
    selection_poses = &poses;
    selecting = controller;
    select_original(controller, entities, quiet);
    try {
        {
            std::lock_guard lock(mutex);
            for (const auto& [entity, pose] : poses) {
                const auto at = published.find(entity);
                if (at != published.end() && selection_contains(controller, entity) && edits.size() < 4096) {
                    selected[entity] = controller;
                    edits.push_back({at->second.id, pose, false});
                }
            }
        }
        reconcile(controller);
    } catch (...) {
    }
}
void finish_hook(std::uintptr_t controller, const void* entities, bool keep, bool cancel, bool force) {
    std::vector<std::uint64_t> finishing;
    try {
        if (!cancel) {
            std::lock_guard lock(mutex);
            for (const auto& [entity, owner] : selected)
                if (owner == controller)
                    finishing.push_back(entity);
        }
    } catch (...) {
    }
    finish_original(controller, entities, keep, cancel, force);
    try {
        for (auto entity : finishing) {
            Pose pose{};
            if (!selection_contains(controller, entity) && read_pose(entity, pose)) {
                std::lock_guard lock(mutex);
                const auto at = published.find(entity);
                if (at != published.end() && edits.size() < 4096)
                    edits.push_back({at->second.id, pose, true});
            }
        }
        reconcile(controller);
    } catch (...) {
    }
}
int motion_hook(const Body* body) {
    const auto actual = motion_original(body);
    if (actual != 3 || !body || multiplayer_session_active())
        return actual;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base;
    int view = actual;
    for (auto call : abi::moving_calls)
        if (caller == call + 5)
            view = 2;
    for (auto call : abi::material_calls)
        if (caller == call + 5)
            view = 1;
    if (view == actual)
        return actual;
    Body handle{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(body), handle))
        return actual;
    std::lock_guard lock(mutex);
    return surface_context && surfaces.contains(handle) ? view : actual;
}
void adapt_edges(std::uintptr_t result, Vec3 center, Vec3 half) {
    if (multiplayer_session_active())
        return;
    try {
        std::uint32_t count{};
        if (!memory::peek(result, count) || count > 100)
            return;
        std::lock_guard lock(mutex);
        if (!surface_context || surfaces.empty())
            return;
        std::erase_if(cached_edges,
                      [&](const auto& e) { return !surfaces.contains(e.first) || !valid(&e.first) || motion_original(&e.first) != 3; });
        std::vector<Edge> native(count);
        if (count && !memory::peek_bytes(result + abi::edge_data, native.data(), count * sizeof(Edge)))
            return;
        for (const auto& edge : native) {
            const auto index = edge.details[1];
            if (index >= 1000000)
                continue;
            for (const auto& [body, id] : surfaces) {
                (void)id;
                Body shape{body.world, index, 0}, owner{};
                std::uintptr_t generations{};
                std::uint8_t generation{};
                if (!memory::peek(body.world + abi::shape_generations, generations) || !generations ||
                    !memory::peek(generations + index, generation))
                    continue;
                shape.generation = generation;
                if (!profile_runtime::park_editor_detail::editor_state().picking_api.body_valid(&shape) ||
                    shape_body(&shape, &owner) != &owner || owner != body)
                    continue;
                Pose pose{};
                pose[0] = pose[1] = pose[2] = 1;
                if (!read_body_pose(body, pose))
                    continue;
                const auto inverse = conjugate({pose[4], pose[5], pose[6], pose[7]});
                NativeChain chain{};
                if (chain_original(edge.identity.data(), &chain) && chain.edges && chain.count && chain.count <= 4096) {
                    std::vector<Edge> snapshot(chain.count);
                    if (!memory::peek_bytes(reinterpret_cast<std::uintptr_t>(chain.edges), snapshot.data(), chain.count * sizeof(Edge)) ||
                        !std::all_of(snapshot.begin(), snapshot.end(),
                                     [&](const auto& e) { return e.identity == edge.identity && e.details[1] == index && finite(e); }))
                        continue;
                    auto& stored = cached_chains[edge.identity];
                    if (!stored || stored->body != body || stored->shape != shape || stored->local.size() != chain.count) {
                        if (chain_edge_count + chain.count > 65536)
                            continue;
                        chain_storage.emplace_back();
                        stored = &chain_storage.back();
                        stored->body = body;
                        stored->shape = shape;
                        stored->local.resize(chain.count);
                        stored->world.resize(chain.count);
                        chain_edge_count += chain.count;
                    }
                    for (unsigned n = 0; n < chain.count; ++n)
                        stored->local[n] = transform(snapshot[n], inverse, rotate(inverse, {-pose[8], -pose[9], -pose[10]}));
                }
                const auto complete = cached_chains.find(edge.identity);
                if (complete == cached_chains.end() || !complete->second || complete->second->body != body ||
                    complete->second->shape != shape)
                    continue;
                const auto local = transform(edge, inverse, rotate(inverse, {-pose[8], -pose[9], -pose[10]}));
                auto& edges = cached_edges[body];
                const auto old = std::find_if(edges.begin(), edges.end(), [&](const auto& e) { return same_edge(e.local, edge); });
                if (old != edges.end())
                    *old = {local, shape};
                else if (edges.size() < 100)
                    edges.push_back({local, shape});
                break;
            }
        }
        for (auto& [body, edges] : cached_edges) {
            Pose pose{};
            pose[0] = pose[1] = pose[2] = 1;
            if (!read_body_pose(body, pose))
                continue;
            std::erase_if(edges, [&](const auto& e) {
                Body owner{};
                return !profile_runtime::park_editor_detail::editor_state().picking_api.body_valid(&e.shape) ||
                       shape_body(&e.shape, &owner) != &owner || owner != body;
            });
            for (const auto& edge : edges) {
                if (count >= 100)
                    break;
                const auto chain = cached_chains.find(edge.local.identity);
                if (chain == cached_chains.end() || !chain->second || chain->second->body != body || chain->second->shape != edge.shape)
                    continue;
                if (std::any_of(native.begin(), native.end(), [&](const auto& e) { return same_edge(edge.local, e); }))
                    continue;
                const auto current = transform(edge.local, {pose[4], pose[5], pose[6], pose[7]}, {pose[8], pose[9], pose[10]});
                if (!overlaps(current, center, half))
                    continue;
                std::memcpy(reinterpret_cast<void*>(result + abi::edge_data + count * sizeof(Edge)), &current, sizeof(current));
                ++count;
            }
        }
        *reinterpret_cast<std::uint32_t*>(result) = count;
    } catch (...) {
    }
}
bool chain_hook(const std::uint64_t* identity, NativeChain* out) {
    if (chain_original(identity, out))
        return true;
    if (multiplayer_session_active())
        return false;
    try {
        std::array<std::uint64_t, 2> key{};
        if (!out || !memory::peek(reinterpret_cast<std::uintptr_t>(identity), key))
            return false;
        std::lock_guard lock(mutex);
        const auto at = cached_chains.find(key);
        if (!surface_context || at == cached_chains.end() || !at->second)
            return false;
        auto& chain = *at->second;
        Body owner{};
        if (!surfaces.contains(chain.body) || !valid(&chain.body) || motion_original(&chain.body) != 3 ||
            !profile_runtime::park_editor_detail::editor_state().picking_api.body_valid(&chain.shape) ||
            shape_body(&chain.shape, &owner) != &owner || owner != chain.body)
            return false;
        Pose pose{};
        pose[0] = pose[1] = pose[2] = 1;
        if (!read_body_pose(chain.body, pose))
            return false;
        for (std::size_t n = 0; n < chain.local.size(); ++n)
            chain.world[n] = transform(chain.local[n], {pose[4], pose[5], pose[6], pose[7]}, {pose[8], pose[9], pose[10]});
        *out = {chain.world.data(), static_cast<std::uint32_t>(chain.world.size()), 0};
        return true;
    } catch (...) {
        return false;
    }
}
void edges_hook(std::uintptr_t cache) {
    edges_original(cache);
    std::uintptr_t context{};
    std::uint32_t flags{};
    Vec3 center{}, half{};
    if (!memory::peek(cache, context) || !memory::peek(context + abi::context_flags, flags) || (flags & abi::disabled_query) ||
        !memory::peek(context + abi::context_position, center) || !memory::peek(base + abi::query_half, half))
        return;
    {
        std::lock_guard lock(mutex);
        if (context != surface_context)
            return;
    }
    adapt_edges(cache + abi::edge_data, center, half);
}
bool edge_query_hook(std::uintptr_t registry, const float* bounds, std::uintptr_t out, int mode, int first, int second) {
    const auto result = edge_query_original(registry, bounds, out, mode, first, second);
    std::array<float, 8> box{};
    Vec3 local{};
    if (!out || !memory::peek(reinterpret_cast<std::uintptr_t>(bounds), box))
        return result;
    const Vec3 center{(box[0] + box[4]) * .5f, (box[1] + box[5]) * .5f, (box[2] + box[6]) * .5f};
    const Vec3 half{(box[4] - box[0]) * .5f, (box[5] - box[1]) * .5f, (box[6] - box[2]) * .5f};
    {
        std::lock_guard lock(mutex);
        if (!surface_context || !memory::peek(surface_context + abi::context_position, local) || distance2(center, local) > 100)
            return result;
    }
    adapt_edges(out, center, half);
    std::uint32_t count{};
    return memory::peek(out, count) && count > 0;
}
void friction_hook(std::uintptr_t grind, float first, float second, float third) {
    if (multiplayer_session_active()) {
        friction_original(grind, first, second, third);
        return;
    }
    Support current;
    {
        std::lock_guard lock(mutex);
        current = supporting;
    }
    std::uintptr_t context{};
    std::uint32_t physics_state{};
    Vec3 velocity{};
    alignas(16) std::array<float, 4> moving{}, point{current.point[0], current.point[1], current.point[2], 0};
    const bool relative = current.context && GetTickCount64() <= current.at + 100 && memory::peek(grind + abi::grind_context, context) &&
                          context == current.context && memory::peek(context + addr::skater_state::physics_state_offset, physics_state) &&
                          physics_state >= 400 && physics_state < 500 && memory::peek(context + abi::context_velocity, velocity) &&
                          valid(&current.body) && point_velocity(&current.body, moving.data(), point.data()) == moving.data() &&
                          std::all_of(moving.begin(), moving.begin() + 3, [](float v) { return std::isfinite(v) && std::abs(v) < 45; });
    if (!relative) {
        friction_original(grind, first, second, third);
        return;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        reinterpret_cast<float*>(context + abi::context_velocity)[axis] = velocity[axis] - moving[axis];
    friction_original(grind, first, second, third);
    std::copy(velocity.begin(), velocity.end(), reinterpret_cast<float*>(context + abi::context_velocity));
}
template <std::size_t N> bool matches(std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> bytes{};
    return memory::peek(address, bytes) && bytes == expected;
}
} // namespace
bool start(std::uintptr_t image_base) {
    if (attempted)
        return ready;
    attempted = true;
    base = image_base;
    const auto& movement = addr::park_editor::movement_contracts;
    if (!base || !matches(base + abi::select, abi::select_prefix) || !matches(base + abi::finish, abi::finish_prefix) ||
        !matches(base + abi::pose, abi::pose_prefix) || !matches(base + abi::friction, abi::friction_prefix) ||
        !matches(base + abi::point_velocity, abi::point_velocity_prefix) || !matches(base + abi::shape_body, abi::shape_body_prefix) ||
        !matches(base + abi::edges, abi::edges_prefix) || !matches(base + abi::edge_query, abi::edge_query_prefix) ||
        !matches(base + abi::chain, abi::chain_prefix) || !matches(base + physics::motion_type, physics::motion_type_prefix) ||
        !matches(base + physics::body_pose, physics::body_pose_prefix) ||
        !matches(base + addr::local_placements::object_root, addr::local_placements::object_root_prefix) ||
        !matches(base + movement[2].rva, movement[2].bytes))
        return false;
    const auto& validity = addr::remote_collision::body_valid;
    if (!matches(base + validity.rva, validity.bytes))
        return false;
    valid = reinterpret_cast<decltype(valid)>(base + validity.rva);
    body_pose = reinterpret_cast<decltype(body_pose)>(base + physics::body_pose);
    point_velocity = reinterpret_cast<decltype(point_velocity)>(base + abi::point_velocity);
    shape_body = reinterpret_cast<decltype(shape_body)>(base + abi::shape_body);
    if (!valid)
        return false;
    struct Hook {
        std::uintptr_t rva;
        void* replacement;
        void** original;
    };
    const std::array hooks{
        Hook{abi::select, reinterpret_cast<void*>(&select_hook), reinterpret_cast<void**>(&select_original)},
        Hook{abi::finish, reinterpret_cast<void*>(&finish_hook), reinterpret_cast<void**>(&finish_original)},
        Hook{abi::pose, reinterpret_cast<void*>(&pose_hook), reinterpret_cast<void**>(&pose_original)},
        Hook{abi::friction, reinterpret_cast<void*>(&friction_hook), reinterpret_cast<void**>(&friction_original)},
        Hook{abi::edges, reinterpret_cast<void*>(&edges_hook), reinterpret_cast<void**>(&edges_original)},
        Hook{abi::edge_query, reinterpret_cast<void*>(&edge_query_hook), reinterpret_cast<void**>(&edge_query_original)},
        Hook{abi::chain, reinterpret_cast<void*>(&chain_hook), reinterpret_cast<void**>(&chain_original)},
        Hook{physics::motion_type, reinterpret_cast<void*>(&motion_hook), reinterpret_cast<void**>(&motion_original)},
        Hook{addr::local_placements::object_root, reinterpret_cast<void*>(&root_hook), reinterpret_cast<void**>(&root_original)},
        Hook{movement[2].rva, reinterpret_cast<void*>(&transform_hook), reinterpret_cast<void**>(&transform_original)}};
    HookBatch batch;
    for (const auto& hook : hooks)
        if (hook_prepare(reinterpret_cast<void*>(base + hook.rva), hook.replacement, hook.original) != HookOk)
            return false;
    for (const auto& hook : hooks)
        if (hook_queue_enable(reinterpret_cast<void*>(base + hook.rva)) != HookOk)
            return false;
    ready = hook_apply_queued() == HookOk;
    return ready;
}
void publish(const std::map<std::uint64_t, Published>& roots) {
    std::lock_guard lock(mutex);
    published = roots;
    surfaces.clear();
    for (const auto& [entity, p] : roots) {
        (void)entity;
        if (p.surface)
            surfaces[p.body] = p.id;
    }
    if (!surfaces.contains(supporting.body))
        supporting = {};
    if (surfaces.empty())
        surface_context = 0;
    std::erase_if(selected, [](const auto& e) { return !published.contains(e.first); });
}
void edited(std::uint64_t entity, const Pose& pose, bool committed) {
    if (!finite(pose))
        return;
    std::lock_guard lock(mutex);
    const auto at = published.find(entity);
    if (at != published.end() && edits.size() < 4096)
        edits.push_back({at->second.id, pose, committed});
}
Inbox drain() {
    std::lock_guard lock(mutex);
    Inbox result;
    result.edits = std::exchange(edits, {});
    std::erase_if(selected, [](const auto& e) { return !selection_contains(e.second, e.first); });
    for (const auto& [entity, controller] : selected) {
        (void)controller;
        const auto at = published.find(entity);
        if (at != published.end())
            result.selected.insert(at->second.id);
    }
    return result;
}
void support(std::uintptr_t context, bool enabled, Body body, Vec3 point) {
    std::lock_guard lock(mutex);
    surface_context = enabled ? context : 0;
    supporting = {enabled && body.world ? context : 0, body, point, GetTickCount64()};
}
void reset() {
    std::lock_guard lock(mutex);
    published.clear();
    surfaces.clear();
    selected.clear();
    edits.clear();
    cached_edges.clear();
    cached_chains.clear();
    surface_context = 0;
    supporting = {};
}
} // namespace dingosdk::placement_adapters
