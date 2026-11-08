#include "traffic_vehicles.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/mission_progression.h"
#include "Engine/Game/Build/20260929/traffic_vehicles.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <vector>

namespace dingosdk::traffic_vehicles {
namespace {
namespace tv = addr::traffic_vehicles;
namespace mission = addr::mission_progression;
using Update = std::uint64_t (*)(void*, std::uint64_t*, std::uint64_t*, void*, void*, std::uint32_t);
using Construct = void* (*)(void*, std::uint64_t);
using Query = std::uint64_t (*)(const void*, const void*, void*, std::uint32_t);
struct Vehicle {
    std::uint64_t owner{}, handle{}, picked{}, current{}, hint{}, root{}, agent{}, candidate{}, started{}, last_root{};
    unsigned climbs{};
    // The nodes between the picked shape and the root, each with its local transform as found. The
    // traffic systems lean the body on one of them, and against a root they no longer move they lean it
    // to about 20° and keep it there (seen in game: the drawn bus and its collision rolled while
    // the root read back level), so a held vehicle keeps them as they were when it was taken.
    std::array<std::uint64_t, 4> chain{};
    std::array<Pose, 4> chain_pose{};
    unsigned chain_count{};
    State state{State::finding};
    Pose pose{};
    std::string reason;
};
std::mutex guard;
std::vector<Vehicle> vehicles;
std::uint64_t next_handle{};
std::uintptr_t base{};
Update original{};
bool attempted{}, ready{};
// Whether any vehicle is finding or held. The hierarchy pass runs thousands of times a frame on
// several threads, so with nothing taken it returns before touching the lock.
std::atomic<bool> active{};
void refresh() noexcept {
    active.store(std::any_of(vehicles.begin(), vehicles.end(), [](const auto& v) {
        return v.owner && (v.state == State::finding || v.state == State::held);
    }), std::memory_order_release);
}

void* component(std::uint64_t id, std::uintptr_t type) noexcept {
    std::array<std::uintptr_t, 3> context{};
    const auto construct = reinterpret_cast<Construct>(base + mission::query_context_constructor);
    if (construct(context.data(), id) != context.data() || context[0] != id || !context[1] || !context[2]) return nullptr;
    std::array<std::uintptr_t, 5> result{base + tv::query_result_vtable, 0, 0, id, base + type};
    reinterpret_cast<Query>(base + mission::component_query)(context.data(), reinterpret_cast<void*>(base + type), result.data(), 0);
    return result[1] && result[2] ? reinterpret_cast<void*>(result[2]) : nullptr;
}
bool position(std::uint64_t id, std::uintptr_t type, Vec3& out) noexcept {
    const auto* ptr = static_cast<const float*>(component(id, type));
    return ptr && memory::peek(reinterpret_cast<std::uintptr_t>(ptr + 8), out) &&
        std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); });
}
bool root_pose(std::uint64_t id, Pose& out) noexcept {
    const auto* ptr = static_cast<const float*>(component(id, tv::local_transform_type));
    if (!ptr || !memory::peek(reinterpret_cast<std::uintptr_t>(ptr + 4), out.rotation) ||
        !memory::peek(reinterpret_cast<std::uintptr_t>(ptr + 8), out.position)) return false;
    return normalize(out.rotation) && std::all_of(out.position.begin(), out.position.end(), [](float x) { return std::isfinite(x); });
}
bool write_pose(std::uint64_t id, const Pose& pose) noexcept {
    auto* ptr = static_cast<float*>(component(id, tv::local_transform_type));
    if (!ptr) return false;
    __try {
        // The traffic systems replace these lanes before every hierarchy pass. Leave scale and
        // the unknown lanes alone; the hierarchy pass consumes the seven changed floats.
        for (int i = 0; i < 4; ++i) ptr[4 + i] = pose.rotation[i];
        for (int i = 0; i < 3; ++i) ptr[8 + i] = pose.position[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A held vehicle is not timed out: its hierarchy pass also stops while the game is paused. The game
// removing it is seen by its root's local transform being gone (read checks that).
void expire(Vehicle& v, std::uint64_t now) {
    if (v.state == State::finding && now - v.started > 2000) { v.state = State::lost; v.reason = "no agent"; }
}
std::uint64_t hierarchy(void* system, std::uint64_t* context, std::uint64_t* parent, void* a, void* b, std::uint32_t c) {
    if (!active.load(std::memory_order_acquire)) return original(system, context, parent, a, b, c);
    std::uint64_t id{}, parent_id{};
    if (context) memory::peek(reinterpret_cast<std::uintptr_t>(context), id);
    if (parent) memory::peek(reinterpret_cast<std::uintptr_t>(parent), parent_id);
    if (id) {
        const auto now = GetTickCount64();
        std::lock_guard lock(guard);
        for (auto& v : vehicles) {
            if (!v.owner || v.state == State::released || v.state == State::lost) continue;
            expire(v, now);
            if (v.state == State::lost) continue;
            if (v.state == State::finding && id == v.current) {
                if (parent_id && parent_id != id && v.climbs < 4) {
                    Vec3 child{}, above{};
                    if (position(id, tv::world_transform_type, child) &&
                        position(parent_id, tv::world_transform_type, above) &&
                        parent_candidate(id, parent_id, child, above, v.climbs)) {
                        if (v.chain_count < v.chain.size() && root_pose(id, v.chain_pose[v.chain_count]))
                            v.chain[v.chain_count++] = id;
                        v.current = parent_id;
                        ++v.climbs;
                    } else { v.state = State::lost; v.reason = "not a vehicle"; }
                } else if (!parent_id) {
                    v.root = id;
                    for (const auto& other : vehicles) {
                        if (&other != &v && other.root == id &&
                            (other.state == State::finding || other.state == State::held)) {
                            v.state = State::lost; v.reason = "already taken"; break;
                        }
                    }
                    if (v.state == State::lost) continue;
                    Vec3 world{};
                    if (!root_pose(id, v.pose) || !position(id, tv::world_transform_type, world) ||
                        !within(v.pose.position, world, 0.5f)) { v.state = State::lost; v.reason = "layout differs"; }
                }
            }
            if (v.state == State::finding && v.root && !parent_id && id != v.root &&
                (id >> 32) != (v.root >> 32)) {
                Vec3 candidate{};
                if (position(id, tv::local_transform_type, candidate) &&
                    agent_candidate(v.root, id, v.pose.position, candidate)) {
                    if (v.candidate && v.candidate != id) { v.state = State::lost; v.reason = "ambiguous agent"; }
                    else v.candidate = id;
                }
            }
            if (v.state == State::finding && v.root && id == v.root && v.candidate) {
                v.agent = v.candidate;
                v.state = State::held;
                v.last_root = now;
            }
            if (v.state == State::held && (id == v.root || id == v.agent)) {
                if (!write_pose(id, v.pose)) { v.state = State::lost; v.reason = "vehicle removed"; }
                if (id == v.root && v.state == State::held) v.last_root = now;
            }
            if (v.state == State::held) {
                for (unsigned i = 0; i < v.chain_count; ++i)
                    if (id == v.chain[i]) write_pose(id, v.chain_pose[i]);
            }
        }
        refresh();
    }
    return original(system, context, parent, a, b, c);
}
template<std::size_t N> bool matches(std::uintptr_t at, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> bytes{};
    return memory::peek(at, bytes) && bytes == expected;
}
// The component query as the game has it, or with the mission override's hook in front of it: a
// jump over its first pushes and the rest of the prefix unchanged.
bool query_matches(std::uintptr_t at) noexcept {
    const auto& expected = mission::component_query_prefix;
    std::remove_cvref_t<decltype(expected)> bytes{};
    if (!memory::peek(at, bytes)) return false;
    if (bytes == expected) return true;
    return bytes[0] == tv::detours_jump &&
        std::equal(bytes.begin() + tv::component_query_hooked_bytes, bytes.end(), expected.begin() + tv::component_query_hooked_bytes);
}
// Installed once, on the first take; a refusal is logged with its step and every later take answers
// "unavailable".
bool refused(const char* step, HookStatus status = HookOk) noexcept {
    logging::log(logging::Level::warning, logging::Channel::runtime, "Traffic vehicles: unavailable ({}{}{}).", step, status == HookOk ? "" : ": ",
                 status == HookOk ? "" : hook_status_string(status));
    return false;
}
bool install(std::uintptr_t image_base) noexcept {
    if (attempted) return ready;
    attempted = true;
    if (!image_base) return refused("no image base");
    if (!matches(image_base + tv::hierarchy_update, tv::hierarchy_update_prefix)) return refused("the hierarchy update's bytes differ");
    if (!matches(image_base + mission::query_context_constructor, mission::query_context_constructor_prefix) ||
        !query_matches(image_base + mission::component_query)) return refused("the component query's bytes differ");
    base = image_base;
    if (const auto status = hook_initialize(); status != HookOk && status != HookAlreadyInitialized) return refused("hook initialize", status);
    auto* target = reinterpret_cast<void*>(base + tv::hierarchy_update);
    {
        // Installed while the game runs: in a batch of its own, which attaches it as it ends.
        HookBatch batch;
        if (const auto status = hook_prepare(target, reinterpret_cast<void*>(&hierarchy), reinterpret_cast<void**>(&original)); status != HookOk || !original)
            return refused("hook prepare", status);
        if (const auto status = hook_enable(target); status != HookOk) return refused("hook enable", status);
    }
    ready = true;
    logging::log(logging::Level::info, logging::Channel::runtime, "Traffic vehicles: the hierarchy update hook is in.");
    return true;
}
Vehicle* find(std::uint64_t owner, std::uint64_t handle) noexcept {
    for (auto& v : vehicles) if (v.owner == owner && v.handle == handle) return &v;
    return nullptr;
}
}
std::string take(std::uintptr_t image_base, std::uint64_t owner, std::uint64_t entity,
                 std::uint64_t root_hint, std::uint64_t& handle) {
    handle = 0;
    // Any ECS owner may be tried: one that is not a traffic vehicle never climbs to a root with an
    // agent beside it, so it ends "not a vehicle" or "no agent".
    if (!entity) return "not a vehicle";
    {
        std::lock_guard lock(guard);
        if (!install(image_base)) return "unavailable";
    }
    // The picker's resolved root is a useful early layout check. An unresolved pick still
    // gets the same check when hierarchy updates reveal its actual root. (Read without the lock:
    // the game's job threads wait on it inside the hierarchy pass, so this thread never calls into
    // the game while it holds it.)
    if (root_hint && root_hint != entity && (root_hint >> 32) == (entity >> 32)) {
        Pose local{};
        Vec3 world{};
        if (!root_pose(root_hint, local) || !position(root_hint, tv::world_transform_type, world) ||
            !within(local.position, world, 0.5f)) return "layout differs";
    }
    std::lock_guard lock(guard);
    unsigned held{};
    for (const auto& v : vehicles) {
        if (v.owner == owner && (v.state == State::finding || v.state == State::held)) ++held;
        if ((v.picked == entity || (root_hint && (v.root == root_hint || v.hint == root_hint))) &&
            (v.state == State::finding || v.state == State::held)) return "already taken";
    }
    if (held >= 4) return "too many vehicles";
    Vehicle* slot{};
    for (auto& v : vehicles) if (!v.owner || v.state == State::lost || v.state == State::released) { slot = &v; break; }
    if (!slot) slot = &vehicles.emplace_back();
    *slot = {};
    slot->owner = owner; slot->handle = ++next_handle; slot->picked = entity; slot->current = entity;
    slot->hint = root_hint; slot->started = GetTickCount64();
    handle = slot->handle;
    refresh();
    return {};
}
// A held vehicle whose root the game has removed (despawned, the level gone) is lost: its root's
// local transform is gone. Asked without the lock (see take), then marked under it.
bool still_there(std::uint64_t owner, std::uint64_t handle) {
    std::uint64_t root{};
    {
        std::lock_guard lock(guard);
        const auto* v = find(owner, handle);
        if (!v || v->state != State::held) return true;
        root = v->root;
    }
    if (component(root, tv::local_transform_type)) return true;
    std::lock_guard lock(guard);
    if (auto* v = find(owner, handle); v && v->state == State::held && v->root == root) {
        v->state = State::lost;
        v->reason = "vehicle removed";
        refresh();
    }
    return false;
}
Sample read(std::uint64_t owner, std::uint64_t handle) {
    (void)still_there(owner, handle);
    std::lock_guard lock(guard);
    auto* v = find(owner, handle);
    if (!v) return {State::lost, {}, "unknown vehicle"};
    expire(*v, GetTickCount64());
    refresh();
    return {v->state, v->pose, v->reason};
}
std::string set_pose(std::uint64_t owner, std::uint64_t handle, const Pose& pose) {
    Pose checked = pose;
    if (!normalize(checked.rotation) || !std::all_of(checked.position.begin(), checked.position.end(),
        [](float x) { return std::isfinite(x) && std::abs(x) <= 1e5f; })) return "invalid pose";
    // A plugin that only ever sets the pose still learns that its vehicle has gone, and stops
    // holding the hierarchy pass to the lock for it.
    if (!still_there(owner, handle)) return "vehicle removed";
    std::lock_guard lock(guard);
    auto* v = find(owner, handle);
    if (!v || v->state != State::held) return "vehicle is not held";
    v->pose = checked;
    return {};
}
bool release(std::uint64_t owner, std::uint64_t handle) {
    std::lock_guard lock(guard);
    auto* v = find(owner, handle);
    if (!v) return false;
    v->state = State::released;
    refresh();
    return true;
}
void release_all(std::uint64_t owner) {
    std::lock_guard lock(guard);
    for (auto& v : vehicles) if (v.owner == owner) v.state = State::released;
    refresh();
}
}
