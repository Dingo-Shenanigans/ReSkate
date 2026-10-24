#include "park_editor_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/physics_bodies.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/World/world_entities.h"
#include "Extension/World/world_entities_contract.h"
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>

// reskate.world.pick: the world raycast, and the object it meets. The ray is the park editor's
// surface probe (park_editor_surface.h); the hit's body gives its entity as the editor's picking
// does (park_editor_selection.cpp), resolved to its root, whose pose the placements runtime reads.
//
// Which blueprint the root entity was made from is looked for in the entity's query record (its
// pointers, and theirs one level down): a world object blueprint (its type chain is
// ObjectBlueprint) or the root data a blueprint keeps right after itself (blueprint + 0x68 -> data):
// 0x80 on for a world object blueprint (read in a running game on 2026-10-21: all 152 World/Props
// blueprints, their data's type at game + 0x64062a0), 0x70 for the skateboard's (native_pose_layout.h). It is taken only when its name, looked up the way the
// game looks names up, gives that same asset back. Where it was found is logged once.
namespace dingosdk {
using namespace profile_runtime;
using namespace profile_runtime::park_editor_detail;
namespace {
std::uintptr_t pointer_at(std::uintptr_t at) noexcept {
    std::uintptr_t value{};
    return memory::read(at, value) ? value : 0;
}
// Any entity's root, for the ray's owner: nothing ignored, every hit counted. The body's own entity
// of the last hit is kept for the log.
// What the last hit gave, step by step, for the log when it gives no entity. A body is {world,
// index | generation << 32}; the world's store (world + 0x28) maps a body index to its owner
// (+0x1008, u32 each), an owner is an ECS entity when its bit in +0x8b0 is clear, and +0x8b8 keeps
// each owner's u64: the entity id for an ECS owner, something else for the others (the game's body
// owner functions 0x25d5fc0, 0x25d6fb0, 0x25d7040; body_entity 0x3d09a20 takes only ECS owners).
struct HitSteps {
    bool body{};
    std::uint64_t entity{}, root{};
    bool resolved{};
    std::uintptr_t world{}, store{};
    std::uint32_t index{}, owner{};
    bool ecs{};
    std::uint64_t value{};
};
HitSteps last_hit{};
std::uint64_t any_root(std::uintptr_t world, std::uint32_t index, std::uintptr_t data) {
    const auto &api = editor_state().picking_api;
    alignas(16) editor::SurfaceBody body{};
    alignas(16) std::array<std::uint64_t, 2> entity{}, root{};
    last_hit = {};
    if (!surface_body(world, index, data, body)) return 0;
    last_hit.body = true;
    last_hit.world = static_cast<std::uintptr_t>(body[0]);
    last_hit.index = static_cast<std::uint32_t>(body[1]);
    last_hit.store = pointer_at(last_hit.world + 0x28);
    if (last_hit.store) {
        std::uint32_t owner{};
        std::uint8_t bits{};
        if (memory::read(pointer_at(last_hit.store + 0x1008) + std::uintptr_t{last_hit.index} * 4, owner) &&
            memory::read(pointer_at(last_hit.store + 0x8b0) + (owner >> 3), bits)) {
            last_hit.owner = owner;
            last_hit.ecs = ((bits >> (owner & 7)) & 1) == 0;
            last_hit.value = pointer_at(pointer_at(last_hit.store + 0x8b8) + std::uintptr_t{owner} * 8);
        }
    }
    api.body_entity(entity.data(), body.data());
    if (!entity[0]) return 0;
    last_hit.entity = entity[0];
    auto &r = placements_runtime();
    // An entity the placements runtime does not resolve is still the hit's: its own id stands.
    if (r.resolve(root.data(), entity[0], false) != root.data() || !r.valid(root.data())) return entity[0];
    last_hit.root = root[0];
    last_hit.resolved = true;
    return root[0];
}
std::string asset_name(std::uintptr_t asset) {
    const auto at = pointer_at(asset + 0x18);
    if (!at) return {};
    std::string name;
    for (std::size_t i = 0; i < 256; ++i) {
        char c{};
        if (!memory::read(at + i, c) || !c) break;
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return {};
        name.push_back(c);
    }
    return name.size() < 256 ? name : std::string{};
}
// A world object blueprint whose name looks up to it in a loaded domain.
std::string confirmed_blueprint(std::uintptr_t base, std::uintptr_t asset) {
    const auto read = [](std::uintptr_t at) { return pointer_at(at); };
    if (!asset || !world_entities::detail::object_blueprint(asset, base, read)) return {};
    auto name = asset_name(asset);
    const auto find = game::native_data().find_asset;
    if (name.empty() || !find) return {};
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain)
        if (pointer_at(base + addr::engine::domain_owners + std::uintptr_t(domain) * 8) && find(domain, name.c_str()) == asset) return name;
    return {};
}
// `value` names a blueprint: the asset itself, or the root data it keeps (blueprint + 0x68 -> data,
// the blueprint 0x80 or 0x70 before it).
std::string blueprint_of(std::uintptr_t base, std::uintptr_t value) {
    const auto asset = value & ~std::uintptr_t{7};
    if (asset < 0x10000) return {};
    if (auto name = confirmed_blueprint(base, asset); !name.empty()) return name;
    for (const std::uintptr_t before : {0x80u, 0x70u}) {
        const auto header = asset - before;
        if ((pointer_at(header + 0x68) & ~std::uintptr_t{4}) == asset) return confirmed_blueprint(base, header);
    }
    return {};
}
std::atomic<bool> path_logged{};
// A world object's blueprint from a pointer of its physics owner: its object data (the world object
// data type, engine + object_data_vtable) found within two levels, the blueprint 0x80 before it.
std::atomic<unsigned> classic_logged{};
std::string classic_blueprint(std::uintptr_t base, std::uintptr_t start) {
    namespace e = addr::skater_entities;
    const auto data_type = base + e::object_data_vtable;
    const auto named = [&](std::uintptr_t data, const std::string &path) -> std::string {
        if (pointer_at(data) != data_type) return {};
        auto name = confirmed_blueprint(base, data - e::object_data_offset);
        if (!name.empty() && classic_logged.fetch_add(1) < 4)
            logging::log(logging::Level::info, logging::Channel::level, "World pick: a physics owner's object data at {} ({}).", path, name);
        return name;
    };
    if (auto name = named(start, "the owner"); !name.empty()) return name;
    for (std::uintptr_t offset = 0; offset < 0x200; offset += 8) {
        const auto value = pointer_at(start + offset);
        if (value < 0x10000 || value > 0x7ff000000000) continue;
        if (auto name = named(value, std::format("+0x{:x}", offset)); !name.empty()) return name;
        for (std::uintptr_t inner = 0; inner < 0x100; inner += 8) {
            const auto deeper = pointer_at(value + inner);
            if (deeper < 0x10000 || deeper > 0x7ff000000000) continue;
            if (auto name = named(deeper, std::format("+0x{:x} -> +0x{:x}", offset, inner)); !name.empty()) return name;
        }
    }
    return {};
}
// The pose's numbers: scale xyz, rotation xyzw, position xyz (the other two lanes are padding).
constexpr std::array<int, 10> numeric_lanes{0, 1, 2, 4, 5, 6, 7, 8, 9, 10};
std::string entity_blueprint(std::uintptr_t base, std::uint64_t root) {
    auto &r = placements_runtime();
    alignas(16) std::array<std::uint64_t, 2> reference{};
    alignas(16) std::array<std::uint64_t, 3> query{};
    if (r.resolve(reference.data(), root, false) != reference.data() || !r.valid(reference.data()) ||
        r.query(query.data(), root) != query.data())
        return {};
    const std::array<std::pair<const char *, std::uintptr_t>, 4> starts{
        {{"reference[1]", reference[1]}, {"query[0]", query[0]}, {"query[1]", query[1]}, {"query[2]", query[2]}}};
    for (const auto &[label, start] : starts) {
        if (start < 0x10000) continue;
        for (std::uintptr_t offset = 0; offset < 0x200; offset += 8) {
            const auto value = pointer_at(start + offset);
            if (value < 0x10000) continue;
            if (auto name = blueprint_of(base, value); !name.empty()) {
                if (!path_logged.exchange(true))
                    logging::log(logging::Level::info, logging::Channel::level, "World pick: a blueprint at {} + 0x{:x} ({}).", label, offset, name);
                return name;
            }
            for (std::uintptr_t inner = 0; inner < 0x100; inner += 8) {
                const auto deeper = pointer_at(value + inner);
                if (deeper < 0x10000) continue;
                if (auto name = blueprint_of(base, deeper); !name.empty()) {
                    if (!path_logged.exchange(true))
                        logging::log(logging::Level::info, logging::Channel::level, "World pick: a blueprint at {} + 0x{:x} -> + 0x{:x} ({}).", label,
                                     offset, inner, name);
                    return name;
                }
            }
        }
    }
    return {};
}
} // namespace

// Which plugin object (world_entities.h) a hit's entity ids belong to: each object's native entity
// is searched (0x400 bytes, and 0x200 of every pointer in them) for the hit's raw or root id. Logged,
// to learn where a world object entity keeps its ECS id (for a plugin object made not solid).
std::atomic<unsigned> ids_logged{};
void log_plugin_object_ids(std::uint64_t raw, std::uint64_t root) {
    if (ids_logged.load() >= 6) return;
    const auto entities = world_entities::live_entities();
    for (const auto entity : entities) {
        for (std::uintptr_t offset = 0; offset < 0x400; offset += 8) {
            const auto value = pointer_at(entity + offset);
            if (value && (value == raw || value == root)) {
                ++ids_logged;
                logging::log(logging::Level::info, logging::Channel::level, "World pick: plugin object {:#x} keeps the hit's {} id {:#x} at +0x{:x}.",
                             entity, value == root ? "root" : "body", value, offset);
                return;
            }
            if (value < 0x10000 || value > 0x7ff000000000) continue;
            for (std::uintptr_t inner = 0; inner < 0x200; inner += 8) {
                const auto deeper = pointer_at(value + inner);
                if (deeper && (deeper == raw || deeper == root)) {
                    ++ids_logged;
                    logging::log(logging::Level::info, logging::Channel::level,
                                 "World pick: plugin object {:#x} keeps the hit's {} id {:#x} at +0x{:x} -> +0x{:x}.", entity,
                                 deeper == root ? "root" : "body", deeper, offset, inner);
                    return;
                }
            }
        }
    }
    ++ids_logged;
    logging::log(logging::Level::info, logging::Channel::level, "World pick: the hit's ids {:#x} (body) and {:#x} (root) are in none of {} plugin objects.", raw,
                 root, entities.size());
}
std::optional<WorldPick> local_world_pick(const std::array<float, 3> &from, const std::array<float, 3> &to) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    auto &e = editor_state();
    if (!local_runtime().active || !r.context || !e.surface_api.ready || !e.picking_api.ready) return std::nullopt;
    for (const auto v : {from[0], from[1], from[2], to[0], to[1], to[2]})
        if (!std::isfinite(v) || std::abs(v) > 1e5f) return std::nullopt;
    std::array<std::uintptr_t, 2> context{};
    r.context(context.data());
    if (!context[0]) return std::nullopt;
    const auto world = e.surface_api.world(context[0]);
    if (!world) return std::nullopt;
    std::uint64_t root{};
    const auto hit = editor::cast_surface(e.surface_api, world, from, to, &any_root, {}, &root);
    if (!hit) return std::nullopt;
    WorldPick pick;
    pick.at = *hit;
    pick.entity = root;
    static std::atomic<unsigned> steps_logged{};
    if (steps_logged.fetch_add(1) < 8)
        logging::log(logging::Level::info, logging::Channel::level,
                     "World pick: hit at ({:.2f}, {:.2f}, {:.2f}); body {}, entity {:#x}, root {:#x}{}; world {:#x} store {:#x} body {} owner {} {} {:#x}.",
                     pick.at[0], pick.at[1], pick.at[2], last_hit.body ? "read" : "not read", last_hit.entity, last_hit.root,
                     last_hit.resolved ? "" : " (not resolved)", last_hit.world, last_hit.store, last_hit.index, last_hit.owner,
                     last_hit.ecs ? "ecs" : "other", last_hit.value);
    // An owner that is not an ECS entity: the object data it leads to names the blueprint.
    if (!last_hit.ecs && last_hit.value > 0x10000 && pick.blueprint.empty()) {
        try {
            pick.blueprint = classic_blueprint(local_runtime().base, static_cast<std::uintptr_t>(last_hit.value));
        } catch (...) {
        }
    }
    if (root) {
        try {
            log_plugin_object_ids(last_hit.entity, root);
        } catch (...) {
        }
        alignas(16) std::array<std::uint64_t, 2> reference{};
        alignas(16) std::array<std::uint64_t, 3> query{};
        alignas(16) std::array<float, 12> pose{};
        if (r.resolve(reference.data(), root, false) == reference.data() && r.valid(reference.data()) &&
            r.query(query.data(), root) == query.data() && r.pose(pose.data(), query.data()) == pose.data() &&
            std::all_of(numeric_lanes.begin(), numeric_lanes.end(), [&](int index) { return std::isfinite(pose[index]); }))
            pick.pose = pose;
        try {
            pick.blueprint = entity_blueprint(local_runtime().base, root);
        } catch (...) {
        }
    }
    return pick;
}
}

// ---- the physics world, for world_entities' objects that are not solid or stand on the floor ------
namespace dingosdk {
namespace {
namespace pb = addr::physics_bodies;
std::uintptr_t physics_world() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto &r = placements_runtime();
        auto &e = editor_state();
        if (!local_runtime().active || !r.context || !e.surface_api.ready) return 0;
        std::array<std::uintptr_t, 2> context{};
        r.context(context.data());
        return context[0] ? e.surface_api.world(context[0]) : 0;
    } catch (...) {
        return 0;
    }
}
bool physics_body_valid(const world_entities::Body &b) noexcept {
    const auto &api = editor_state().picking_api;
    if (!api.ready) return false;
    alignas(16) editor::SurfaceBody body{b[0], b[1]};
    return api.body_valid(body.data());
}
std::uint32_t physics_body_owner(const world_entities::Body &b) noexcept {
    const auto store = pointer_at(static_cast<std::uintptr_t>(b[0]) + pb::store_offset);
    std::uint32_t owner{UINT32_MAX};
    if (store) memory::read(pointer_at(store + pb::body_owner_offset) + std::uintptr_t{static_cast<std::uint32_t>(b[1])} * 4, owner);
    return owner;
}
bool physics_body_ghost(const world_entities::Body &b, bool ghost) noexcept {
    const auto base = local_runtime().base;
    static const bool known = [&] {
        const auto same = [&](std::uintptr_t rva, const std::array<unsigned char, 24> &prefix) {
            std::array<unsigned char, 24> bytes{};
            return memory::read_bytes(base + rva, bytes.data(), bytes.size()) && bytes == prefix;
        };
        return same(pb::ghost_body, pb::ghost_body_prefix) && same(pb::solid_body, pb::solid_body_prefix) &&
               same(pb::main_ghost, pb::main_ghost_prefix) && same(pb::main_solid, pb::main_solid_prefix);
    }();
    static std::atomic<unsigned> logged{};
    if (!known || !physics_body_valid(b)) {
        if (logged.fetch_add(1) < 4)
            logging::log(logging::Level::info, logging::Channel::runtime, "World objects: body {} not {}: {}.", static_cast<std::uint32_t>(b[1]),
                         ghost ? "ghosted" : "made solid", known ? "the world says it is not valid" : "the physics functions differ");
        return false;
    }
    // Both halves of the game's own ghosting, on every body of the object.
    alignas(16) std::array<std::uint64_t, 2> body{b[0], b[1]};
    reinterpret_cast<void (*)(std::uint64_t *)>(base + (ghost ? pb::main_ghost : pb::main_solid))(body.data());
    reinterpret_cast<void (*)(std::uint64_t *)>(base + (ghost ? pb::ghost_body : pb::solid_body))(body.data());
    if (logged.fetch_add(1) < 8)
        logging::log(logging::Level::info, logging::Channel::runtime, "World objects: body {} {}.", static_cast<std::uint32_t>(b[1]),
                     ghost ? "ghosted" : "made solid");
    return true;
}
std::optional<float> physics_floor(const std::array<float, 3> &from, float depth, const std::vector<world_entities::Body> &ignored) noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto &e = editor_state();
        const auto world = physics_world();
        if (!world || !e.surface_api.ready) return std::nullopt;
        std::vector<editor::SurfaceBody> bodies;
        for (const auto &body : ignored) bodies.push_back({body[0], body[1]});
        const auto hit = editor::cast_surface(e.surface_api, world, {from[0], from[1] + 0.25f, from[2]}, {from[0], from[1] - depth, from[2]}, nullptr, {},
                                              nullptr, bodies);
        if (!hit) return std::nullopt;
        return (*hit)[1];
    } catch (...) {
        return std::nullopt;
    }
}
// The body a ray meets first: the surface cast's owner is asked for the nearer hit each time, so the
// last body it is asked about is the closest.
thread_local editor::SurfaceBody closest_body{};
std::uint64_t remember_body(std::uintptr_t world, std::uint32_t index, std::uintptr_t data) {
    editor::SurfaceBody body{};
    if (surface_body(world, index, data, body)) closest_body = body;
    return 0;
}
std::optional<world_entities::Body> physics_hit(const std::array<float, 3> &from, const std::array<float, 3> &to) noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto &e = editor_state();
        const auto world = physics_world();
        if (!world || !e.surface_api.ready || !e.picking_api.ready) return std::nullopt;
        closest_body = {};
        if (!editor::cast_surface(e.surface_api, world, from, to, &remember_body) || !closest_body[0]) return std::nullopt;
        return world_entities::Body{closest_body[0], closest_body[1]};
    } catch (...) {
        return std::nullopt;
    }
}
// Every body of the body's owner: the store's body-to-owner array (+0x1008, u32 each) is a vector
// whose end is at +0x1010; empty when that does not read as one, or the owner has more than `most`.
std::vector<world_entities::Body> physics_owned(const world_entities::Body &b, std::size_t most) noexcept {
    try {
        const auto world = static_cast<std::uintptr_t>(b[0]);
        const auto store = pointer_at(world + pb::store_offset);
        const auto begin = pointer_at(store + pb::body_owner_offset), end = pointer_at(store + pb::body_owner_offset + 8);
        static std::atomic<bool> logged{};
        if (!logged.exchange(true))
            logging::log(logging::Level::info, logging::Channel::runtime, "World objects: the body owners at {:#x}..{:#x} ({} bodies), then {:#x}.", begin,
                         end, end > begin ? (end - begin) / 4 : 0, pointer_at(store + pb::body_owner_offset + 16));
        if (!begin || end <= begin || (end - begin) / 4 > (1u << 24)) return {};
        const auto count = static_cast<std::uint32_t>((end - begin) / 4);
        const auto index = static_cast<std::uint32_t>(b[1]);
        if (index >= count) return {};
        std::uint32_t owner{};
        if (!memory::read(begin + std::uintptr_t{index} * 4, owner) || owner == UINT32_MAX) return {};
        // Read in pieces: the array is only partly backed (unused bodies are UINT32_MAX, and whole
        // pages of them may not be there to read).
        constexpr std::uint32_t piece = 0x4000;
        std::vector<std::uint32_t> owners(piece);
        std::vector<world_entities::Body> bodies;
        for (std::uint32_t first = 0; first < count; first += piece) {
            const auto n = std::min(piece, count - first);
            if (!memory::read_bytes(begin + std::uintptr_t{first} * 4, owners.data(), std::size_t{n} * 4)) continue;
            for (std::uint32_t j = 0; j < n; ++j) {
                const auto i = first + j;
                if (owners[j] != owner) continue;
                if (bodies.size() >= most) return {};
                // Its generation as the store keeps it (+0xff8, a byte per body), as body_valid checks.
                std::uint8_t generation{};
                if (!memory::read(pointer_at(store + 0xff8) + i, generation)) continue;
                const world_entities::Body body{b[0], std::uint64_t{i} | (std::uint64_t{generation} << 32)};
                if (physics_body_valid(body)) bodies.push_back(body);
            }
        }
        static std::atomic<unsigned> owner_logged{};
        if (owner_logged.fetch_add(1) < 8)
            logging::log(logging::Level::info, logging::Channel::runtime, "World objects: body {} belongs to owner {}, which has {} bod{}.", index, owner,
                         bodies.size(), bodies.size() == 1 ? "y" : "ies");
        return bodies;
    } catch (...) {
        return {};
    }
}
// Test only (objbody): the store's per-body bit setters next to the ghost ones (each sets, or
// with "off" clears, one bit per body and some tell the world), to learn which one takes a body
// out of the skater's collision.
bool physics_test(std::string_view name, const world_entities::Body &b) noexcept {
    static constexpr std::pair<std::string_view, std::uintptr_t> setters[]{
        {"a70", 0x25d38a0}, {"a70off", 0x25d3250}, {"f88", 0x25d3930}, {"4c0", 0x25d33d0}, {"4c0off", 0x25d2ec0},
        {"310", 0x25d3560},  {"310off", 0x25d3090}, {"830", 0x25d3590}, {"830off", 0x25d30c0}, {"8d0", 0x25d36a0},
        {"8d8", 0x25d3900},  {"aa0", 0x25d3670},    {"aa0off", 0x25d31d0}, {"1598", 0x25d3400}, {"40", 0x25d3330},
        {"40off", 0x25d2e60}, {"1680", 0x25d3530}};
    const auto found = std::find_if(std::begin(setters), std::end(setters), [&](const auto &s) { return s.first == name; });
    if (found == std::end(setters) || !physics_body_valid(b)) return false;
    alignas(16) std::array<std::uint64_t, 2> body{b[0], b[1]};
    reinterpret_cast<void (*)(std::uint64_t *)>(local_runtime().base + found->second)(body.data());
    return true;
}
} // namespace
void register_world_physics() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        world_entities::set_physics(
            {&physics_world, &physics_body_valid, &physics_body_owner, &physics_body_ghost, &physics_floor, &physics_hit, &physics_owned,
             &physics_test});
    });
}
}
