#include "placement_physics.h"
#include <Windows.h>
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Engine/Game/Build/20260929/park_editor.h"
#include "Engine/Game/Build/20260929/placement_adapters.h"
#include "Engine/Game/Build/20260929/placement_physics.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Build/20260929/skater_movement.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/skater_movement.h"
#include "ParkEditor/park_editor_bodies.h"
#include "ParkEditor/park_editor_runtime.h"
#include "local_placements_runtime.h"
#include "placement_adapters.h"
#include "placement_motion.h"
#include "Engine/Core/Log/logging.h"
#include <atomic>
#include <format>

// Threads: the plugin API and client_tick run on the client tick and write only the client replica
// (its body and transform). The server copy is written only by server_tick, from the server park
// tick (local_park_rotation.cpp park_tick_hook): it mirrors a moving object's pose and carries out
// the restores the client side queues. Both hold native_mutex, which orders ReSkate's own state.
namespace dingosdk::placement_physics {
namespace {
using namespace profile_runtime;
namespace abi = addr::placement_physics;
namespace phys = addr::remote_collision;
using Pose = std::array<float, 12>;
struct Body {
    std::uintptr_t world{};
    std::uint32_t index{UINT32_MAX}, generation{};
    bool operator==(const Body&) const = default;
};
static_assert(sizeof(Body) == 16);
struct Lease {
    std::string owner;
    std::uint64_t token{}, map_generation{}, server{}, client{}, sampled{}, velocity_at{}, pose_at{}, configured_at{};
    Body body{};
    float inverse_mass{};
    plugins::ObjectPhysics options;
    Pose pose{};
    // The pose in the park's layout: where it was claimed, or where an edit put it. A released object
    // goes back to it, so physics never changes a saved layout.
    Pose fixed_pose{};
    bool editing{};
    std::uint64_t edit_until{}, edit_revision{};
    // The server copy's motion before this claim first mirrored it, read by server_tick on the thread
    // that owns the copy (placement_motion.h); unseen until then.
    int server_motion{placement_motion::unseen};
    // When the claim was made, and whether server_tick found it cannot mirror the server copy (it
    // cannot be resolved within a second, or the game moves it itself): client_tick then lets the
    // claim go, so the client replica never moves where the server copy does not follow.
    std::uint64_t claimed_at{};
    bool server_lost{};
};
// A write to the server copy, done by the next server_tick if its row is still the one it was for.
struct ServerRestore {
    std::uint64_t id{}, token{}, map_generation{}, server{};
    Pose pose{};
    // The motion to put the copy back in; unseen when the claim never wrote it.
    int motion{placement_motion::unseen};
};
struct State {
    std::uintptr_t base{};
    bool ready{};
    std::map<std::uint64_t, Lease> leases;
    plugins::ObjectPhysicsClaims claims;
    std::vector<ServerRestore> restores;
    // Set by a level's teardown (before_level_transition, which may run on another thread): the
    // leases are forgotten, without a native write, by the next call that holds native_mutex.
    std::atomic<bool> world_gone{};
    void (*motion)(const Body*, int){};
    int (*motion_type)(const Body*){};
    void (*linear)(const Body*, const float*){};
    void (*angular)(const Body*, const float*){};
    void (*wake)(const Body*){};
    void (*sleep)(const Body*, bool){};
    void (*mass)(const Body*, float){};
    bool (*valid)(const Body*){};
    float* (*body_pose)(const Body*, float*){};
};
State& state() {
    static auto* s = new State;
    return *s;
}
// Under native_mutex: drops what a torn-down level left. Its bodies are gone, so nothing is written.
void forget_gone_world() {
    auto& s = state();
    if (!s.world_gone.exchange(false))
        return;
    s.leases.clear();
    s.claims.clear();
    s.restores.clear();
}
void publish_roots() {
    std::map<std::uint64_t, placement_adapters::Published> roots;
    for (const auto& [id, lease] : state().leases) {
        const placement_adapters::Body body{lease.body.world, lease.body.index, lease.body.generation};
        roots[lease.server] = {id, body, lease.options.surface && !lease.editing};
        roots[lease.client] = {id, body, lease.options.surface && !lease.editing};
    }
    placement_adapters::publish(roots);
}
template <std::size_t N> bool matches(std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> bytes{};
    return memory::peek(address, bytes) && bytes == expected;
}
bool start(std::uintptr_t base) {
    auto& s = state();
    if (s.ready)
        return s.base == base;
    if (!base || !park_bodies_ready())
        return false;
    for (const auto* contract : {&phys::set_motion_type, &phys::set_linear_velocity, &phys::set_angular_velocity, &phys::wake_body,
                                 &phys::body_valid, &phys::set_body_980})
        if (!matches(base + contract->rva, contract->bytes))
            return false;
    if (!matches(base + abi::body_pose, abi::body_pose_prefix) || !matches(base + abi::sleep, abi::sleep_prefix) ||
        !matches(base + abi::motion_type, abi::motion_type_prefix) ||
        !matches(base + addr::local_placements::realm_context, addr::local_placements::realm_context_prefix))
        return false;
    s.base = base;
    s.motion = reinterpret_cast<decltype(s.motion)>(base + phys::set_motion_type.rva);
    s.motion_type = reinterpret_cast<decltype(s.motion_type)>(base + abi::motion_type);
    s.linear = reinterpret_cast<decltype(s.linear)>(base + phys::set_linear_velocity.rva);
    s.angular = reinterpret_cast<decltype(s.angular)>(base + phys::set_angular_velocity.rva);
    s.wake = reinterpret_cast<decltype(s.wake)>(base + phys::wake_body.rva);
    s.sleep = reinterpret_cast<decltype(s.sleep)>(base + abi::sleep);
    s.mass = reinterpret_cast<decltype(s.mass)>(base + phys::set_body_980.rva);
    s.valid = reinterpret_cast<decltype(s.valid)>(base + phys::body_valid.rva);
    s.body_pose = reinterpret_cast<decltype(s.body_pose)>(base + abi::body_pose);
    if (!placement_adapters::start(base))
        return false;
    s.ready = true;
    return true;
}
const PlacementRow* row_for(std::uint64_t id) {
    const auto& rows = placements_runtime().rows;
    const auto at =
        std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.spawned && row.saved_id == id && row.entity; });
    return at == rows.end() ? nullptr : &*at;
}
bool body_of(std::uint64_t entity, Body& body) {
    auto& r = placements_runtime();
    if (!entity || !r.resolve || !r.valid || !park_bodies_ready())
        return false;
    alignas(16) std::array<std::uint64_t, 2> root{};
    if (r.resolve(root.data(), entity, false) != root.data() || root[0] != entity || !r.valid(root.data()))
        return false;
    const auto handle = placed_body_handle(root.data());
    return handle && memory::peek(handle, body) && body.world && body.index < 1000000 && state().valid(&body);
}
std::map<std::uint32_t, std::uint64_t> client_objects() {
    std::map<std::uint32_t, std::uint64_t> result;
    std::uintptr_t context{}, begin{}, end{};
    std::uint32_t offset{};
    std::uint8_t ready{};
    auto& s = state();
    auto& r = placements_runtime();
    reinterpret_cast<void* (*)(void*, std::uint32_t)>(s.base + addr::local_placements::realm_context)(&context,
                                                                                                      addr::local_placements::client_realm);
    if (!context || !memory::peek(s.base + addr::local_placements::client_ready_offset, offset) || offset > 0x1000000)
        return result;
    const auto store = context + offset;
    if (!memory::peek(store + abi::store_ready, ready) || ready != 1 || !memory::peek(store + abi::records_begin, begin) ||
        !memory::peek(store + abi::records_end, end) || !begin || end < begin || (end - begin) % abi::record_size ||
        (end - begin) / abi::record_size > 65536)
        return result;
    for (auto at = begin; at < end; at += abi::record_size) {
        std::uint8_t active{};
        std::uint32_t id{};
        std::uint64_t entity{};
        alignas(16) std::array<std::uint64_t, 2> root{};
        if (memory::peek(at + abi::record_active, active) && active && memory::peek(at + abi::record_id, id) &&
            memory::peek(at + abi::record_entity, entity) && entity && r.resolve(root.data(), entity, false) == root.data() && root[0] &&
            r.valid(root.data()))
            result[id] = root[0];
    }
    return result;
}
// The row a lease (or a queued restore) was made for is still live, the same placement and map.
bool row_current(std::uint64_t id, std::uint64_t token, std::uint64_t map_generation, std::uint64_t server) {
    if (!placements_runtime().manager || placements_runtime().map.empty() || placements_runtime().map_generation != map_generation)
        return false;
    const auto* row = row_for(id);
    return row && row->token == token && row->entity == server;
}
bool current(std::uint64_t id, const Lease& lease) {
    if (!row_current(id, lease.token, lease.map_generation, lease.server))
        return false;
    const auto server = placed_identity(lease.server), client = placed_identity(lease.client);
    Body body;
    return server && client && *server == *client && body_of(lease.client, body) && body == lease.body;
}
bool inverse_mass(const Body& body, float& value) {
    std::uintptr_t data{}, buffer{};
    return memory::peek(body.world + abi::world_state, data) && memory::peek(data + abi::mass_buffer, buffer) && buffer &&
           memory::peek(buffer + std::uint64_t{body.index} * sizeof(float), value) && std::isfinite(value) && value > 0 && value < 1000 &&
           std::isfinite(1.f / value);
}
bool sample_pose(Lease& lease) {
    auto& r = placements_runtime();
    alignas(16) std::array<std::uint64_t, 3> query{};
    alignas(16) std::array<float, 8> rigid{};
    if (r.query(query.data(), lease.client) != query.data() || r.pose(lease.pose.data(), query.data()) != lease.pose.data() ||
        state().body_pose(&lease.body, rigid.data()) != rigid.data())
        return false;
    std::copy_n(rigid.begin(), 4, lease.pose.begin() + 4);
    std::copy_n(rigid.begin() + 4, 3, lease.pose.begin() + 8);
    if (!placement_geometry::finite(lease.pose))
        return false;
    lease.sampled = GetTickCount64();
    return true;
}
// Moves one copy's body and transform to `pose` as the dropper does, leaving it in motion type `mode`.
bool place(std::uint64_t entity, const Pose& pose, int mode) {
    auto& r = placements_runtime();
    auto& s = state();
    alignas(16) std::array<std::uint64_t, 3> query{};
    std::uintptr_t source{};
    Body body;
    if (!placement_geometry::finite(pose) || r.query(query.data(), entity) != query.data() || !body_of(entity, body) ||
        !memory::peek(s.base + addr::park_editor::transform_notification_source, source))
        return false;
    s.motion(&body, abi::motion_keyframed);
    alignas(16) std::array<float, 8> rigid{pose[8], pose[9], pose[10], pose[0], pose[4], pose[5], pose[6], pose[7]};
    move_placed_body(&body, rigid.data());
    set_placed_transform(query.data(), pose.data(), source);
    s.motion(&body, mode);
    return true;
}
// The server copy is a live placement entity, whether or not it carries a physics body.
bool entity_live(std::uint64_t entity) {
    auto& r = placements_runtime();
    if (!entity || !r.resolve || !r.valid || !r.query)
        return false;
    alignas(16) std::array<std::uint64_t, 2> root{};
    alignas(16) std::array<std::uint64_t, 3> query{};
    return r.resolve(root.data(), entity, false) == root.data() && root[0] == entity && r.valid(root.data()) &&
           r.query(query.data(), entity) == query.data();
}
// Moves the server copy (server_tick only): its body too when it has one, its transform always. A copy
// without a body is still moved, as Dynamic QuickDrop 0.3 moved it; `mode` is written only to a body.
bool place_copy(std::uint64_t entity, const Pose& pose, int mode) {
    auto& r = placements_runtime();
    auto& s = state();
    alignas(16) std::array<std::uint64_t, 3> query{};
    std::uintptr_t source{};
    if (!placement_geometry::finite(pose) || r.query(query.data(), entity) != query.data() ||
        !memory::peek(s.base + addr::park_editor::transform_notification_source, source))
        return false;
    Body body;
    const bool physical = body_of(entity, body);
    if (physical) {
        s.motion(&body, abi::motion_keyframed);
        alignas(16) std::array<float, 8> rigid{pose[8], pose[9], pose[10], pose[0], pose[4], pose[5], pose[6], pose[7]};
        move_placed_body(&body, rigid.data());
    }
    set_placed_transform(query.data(), pose.data(), source);
    if (physical)
        s.motion(&body, placement_motion::body_motion(mode) ? mode : abi::motion_fixed);
    return true;
}
void stop(const Body& body) {
    alignas(16) const std::array<float, 4> zero{};
    state().linear(&body, zero.data());
    state().angular(&body, zero.data());
}
// Ends a lease (client tick): the client replica goes back to its layout pose, fixed, with its own
// mass; the server copy is queued to follow (server_tick), even when the replica cannot be reached.
void restore(std::uint64_t id, const Lease& lease) {
    auto& s = state();
    if (s.restores.size() < 256)
        s.restores.push_back({id, lease.token, lease.map_generation, lease.server, lease.fixed_pose,
                              placement_motion::server_restore(lease.server_motion)});
    else if (static bool told{}; !std::exchange(told, true))
        logging::log(logging::Level::warning, logging::Channel::objects,
            "Object physics: more than 256 released objects wait for their server copy to be put back; the rest stay where they were left.");
    if (!current(id, lease))
        return; // Never write through a stale row/body generation.
    if (!place(lease.client, lease.fixed_pose, abi::motion_fixed))
        s.motion(&lease.body, abi::motion_fixed);
    stop(lease.body);
    s.mass(&lease.body, lease.inverse_mass);
}
bool suspended() {
    return !placement_adapters::world_current() || multiplayer_session_active() || !placements_runtime().manager ||
           placements_runtime().map.empty() || placements_runtime().clearing || !lobby_object_placement_allowed();
}
std::uint64_t contact_owner(std::uintptr_t world, std::uint32_t index, std::uintptr_t data) {
    alignas(16) std::array<std::uint64_t, 2> root{};
    const auto entity = surface_hit_entity(world, index, data);
    auto& r = placements_runtime();
    if (!entity || r.resolve(root.data(), entity, false) != root.data() || !root[0] || !r.valid(root.data()))
        return 0;
    for (const auto& [id, lease] : state().leases)
        if (lease.client == root[0] || lease.server == root[0])
            return id;
    return 0;
}
std::optional<plugins::ObjectContact> cast(const std::array<float, 3>& from, const std::array<float, 3>& to) {
    auto& r = placements_runtime();
    if (!r.context)
        return std::nullopt;
    std::array<std::uintptr_t, 2> context{};
    r.context(context.data());
    std::uint64_t id{};
    const auto point = cast_placed_surface(context[0], from, to, &contact_owner, &id);
    if (!point || !id)
        return std::nullopt;
    for (float value : *point)
        if (!std::isfinite(value) || std::abs(value) > 100000)
            return std::nullopt;
    return plugins::ObjectContact{id, *point};
}
// Client tick only: applies the dropper's edits and selections to the client replicas.
placement_adapters::Inbox receive_edits() {
    auto inbox = placement_adapters::drain();
    auto& s = state();
    const auto now = GetTickCount64();
    for (const auto& edit : inbox.edits) {
        const auto at = s.leases.find(edit.id);
        if (at == s.leases.end() || !current(at->first, at->second))
            continue;
        auto& lease = at->second;
        if (place(lease.client, edit.pose, abi::motion_fixed)) {
            stop(lease.body);
            lease.pose = edit.pose;
            lease.fixed_pose = edit.pose;
            lease.sampled = now;
            lease.edit_until = now + 250;
            lease.editing = true;
            if (edit.committed)
                ++lease.edit_revision;
        }
    }
    for (const auto id : inbox.selected) {
        const auto at = s.leases.find(id);
        if (at == s.leases.end() || !current(id, at->second))
            continue;
        auto& lease = at->second;
        if (!lease.editing)
            lease.fixed_pose = lease.pose;
        lease.editing = true;
        s.motion(&lease.body, abi::motion_fixed);
        stop(lease.body);
    }
    return inbox;
}
std::mutex marker_mutex;
plugins::MarkerJournal marker_journal;
bool marker_attempted{}, marker_ready{};
void (*save_original)(std::uintptr_t, const float*, bool, bool, bool){};
void (*return_original)(std::uintptr_t, const float*, std::uintptr_t, const void*){};
void enqueue(std::uintptr_t controller, bool saved) {
    std::uintptr_t handle{};
    std::uint64_t entity{};
    if (!memory::peek(controller + abi::marker_owner, handle) || !handle || !memory::peek(handle, entity) || !entity)
        return;
    std::lock_guard lock(marker_mutex);
    marker_journal.push(entity, saved);
}
void save_hook(std::uintptr_t controller, const float* transform, bool a, bool b, bool c) {
    save_original(controller, transform, a, b, c);
    try {
        enqueue(controller, true);
    } catch (...) {
    }
}
void return_hook(std::uintptr_t controller, const float* transform, std::uintptr_t label, const void* options) {
    std::uint32_t before{}, after{};
    const bool saved = reinterpret_cast<std::uintptr_t>(transform) == controller + abi::marker_transform &&
                       memory::peek(controller + abi::marker_transition, before) && before == 0;
    return_original(controller, transform, label, options);
    if (saved && memory::peek(controller + abi::marker_transition, after) && after == 1)
        try {
            enqueue(controller, false);
        } catch (...) {
        }
}
bool start_markers(std::uintptr_t base) {
    if (marker_attempted)
        return marker_ready;
    marker_attempted = true;
    if (!base || !matches(base + abi::marker_save, abi::marker_save_prefix) ||
        !matches(base + abi::marker_return, abi::marker_return_prefix))
        return false;
    auto* save = reinterpret_cast<void*>(base + abi::marker_save);
    auto* back = reinterpret_cast<void*>(base + abi::marker_return);
    HookBatch batch;
    if (hook_prepare(save, reinterpret_cast<void*>(&save_hook), reinterpret_cast<void**>(&save_original)) != HookOk ||
        hook_prepare(back, reinterpret_cast<void*>(&return_hook), reinterpret_cast<void**>(&return_original)) != HookOk ||
        hook_queue_enable(save) != HookOk || hook_queue_enable(back) != HookOk || hook_apply_queued() != HookOk)
        return false;
    marker_ready = true;
    return true;
}
} // namespace
std::string configure(std::uintptr_t base, std::string_view plugin, std::uint64_t id,
                      const std::optional<plugins::ObjectPhysics>& options) {
    std::lock_guard lock(local_runtime().native_mutex);
    forget_gone_world();
    auto& s = state();
    if (!options) {
        if (!s.claims.owns(plugin, id))
            return "this plugin does not control this object's physics";
        restore(id, s.leases.at(id));
        s.leases.erase(id);
        s.claims.erase(id);
        publish_roots();
        return {};
    }
    if (!plugins::valid_object_physics(*options))
        return "invalid mass";
    if (!start(base))
        return "unavailable here";
    if (suspended())
        return "object physics is offline-only and unavailable while editing or in "
               "a menu";
    const auto* row = row_for(id);
    if (!row)
        return "not a live local placement";
    if (s.claims.occupied(id) && !s.claims.owns(plugin, id))
        return "another plugin controls this object's physics";
    if (s.claims.owns(plugin, id)) {
        auto& lease = s.leases.at(id);
        if (!current(id, lease))
            return "object changed; release its old physics claim first";
        if (lease.options.dynamic == options->dynamic && lease.options.mass == options->mass && lease.options.surface == options->surface)
            return {};
        if (GetTickCount64() < lease.configured_at + 16)
            return "object physics rate limit reached";
        lease.options = *options;
    } else {
        const auto identity = placed_identity(row->entity);
        const auto records = client_objects();
        if (!identity)
            return "object identity is unavailable";
        const auto at = records.find(identity->drop_id);
        if (at == records.end())
            return "the object's client replica is unavailable";
        const auto client = placed_identity(at->second);
        if (!client || *client != *identity)
            return "object replica identity differs";
        Lease lease;
        lease.owner = plugin;
        lease.token = row->token;
        lease.map_generation = placements_runtime().map_generation;
        lease.server = row->entity;
        lease.client = at->second;
        lease.options = *options;
        if (!body_of(lease.client, lease.body) || !inverse_mass(lease.body, lease.inverse_mass) || !sample_pose(lease))
            return "object physics body is unavailable";
        lease.fixed_pose = lease.pose;
        // Only the client replica is checked here: it is the copy a claim drives. The server copy's
        // motion is taken by server_tick when it first mirrors it (it lets the claim go if it cannot).
        // A refusal says which motion the replica had: claims used to be refused on either copy with
        // one sentence, and which copy failed was never known.
        if (const auto motion = s.motion_type(&lease.body); !placement_motion::client_claimable(motion)) {
            if (static unsigned told{}; told < 8) {
                ++told;
                logging::log(logging::Level::info, logging::Channel::objects,
                    "Object physics: a claim of object {} was refused: its client replica's motion is {}, not fixed.", id, motion);
            }
            return "only fixed local placement bodies may be claimed";
        }
        if (auto why = s.claims.claim(plugin, id); !why.empty())
            return why;
        lease.claimed_at = GetTickCount64();
        s.leases.emplace(id, std::move(lease));
    }
    auto& lease = s.leases.at(id);
    s.mass(&lease.body, 1.f / lease.options.mass);
    s.motion(&lease.body, lease.options.dynamic ? abi::motion_dynamic : abi::motion_fixed);
    if (lease.options.dynamic) {
        s.wake(&lease.body);
        s.sleep(&lease.body, false);
    } else
        stop(lease.body);
    lease.configured_at = GetTickCount64();
    publish_roots();
    return {};
}
std::string velocity(std::string_view plugin, std::uint64_t id, const std::array<float, 3>& value) {
    std::lock_guard lock(local_runtime().native_mutex);
    forget_gone_world();
    auto& s = state();
    if (!plugins::valid_object_velocity(value))
        return "invalid velocity";
    if (!s.claims.owns(plugin, id))
        return "this plugin does not control this object's physics";
    auto& lease = s.leases.at(id);
    if (suspended() || lease.editing || !current(id, lease) || !lease.options.dynamic)
        return "object physics is unavailable";
    const auto now = GetTickCount64();
    if (lease.velocity_at && now < lease.velocity_at + 16)
        return "object velocity rate limit reached";
    alignas(16) std::array<float, 4> aligned{value[0], value[1], value[2], 0};
    s.linear(&lease.body, aligned.data());
    s.wake(&lease.body);
    s.sleep(&lease.body, false);
    lease.velocity_at = now;
    return {};
}
std::optional<plugins::ObjectMotion> sample(std::string_view plugin, std::uint64_t id) {
    std::lock_guard lock(local_runtime().native_mutex);
    forget_gone_world();
    auto& s = state();
    if (!s.claims.owns(plugin, id))
        return std::nullopt;
    auto& lease = s.leases.at(id);
    if (!current(id, lease) || !sample_pose(lease))
        return std::nullopt;
    plugins::ObjectMotion result;
    std::copy_n(lease.pose.begin() + 8, 3, result.position.begin());
    std::copy_n(lease.pose.begin() + 4, 4, result.rotation.begin());
    result.dynamic = lease.options.dynamic;
    result.generation = lease.token;
    result.authored_mass = 1.f / lease.inverse_mass;
    result.mass = lease.options.mass;
    result.editing = lease.editing;
    result.edit_revision = lease.edit_revision;
    result.surface = lease.options.surface;
    std::uintptr_t data{}, buffer{};
    std::uint32_t count{};
    std::uint16_t page{};
    if (!memory::peek(lease.body.world + abi::world_state, data) || !memory::peek(data + abi::velocity_buffer, buffer) || !buffer ||
        !memory::peek(data + abi::velocity_count, count) || !memory::peek(data + abi::velocity_page, page) || count > 1000000 ||
        lease.body.index >= count || page > 16)
        return std::nullopt;
    if (!memory::peek(buffer + abi::velocity_page_stride(count) * page + std::uint64_t{lease.body.index} * 16, result.velocity))
        return std::nullopt;
    for (float v : result.velocity)
        if (!std::isfinite(v) || std::abs(v) > 1000)
            return std::nullopt;
    return result;
}
std::string pose(std::string_view plugin, std::uint64_t id, const std::array<float, 3>& position, const std::array<float, 4>& rotation) {
    std::lock_guard lock(local_runtime().native_mutex);
    forget_gone_world();
    auto& s = state();
    for (float value : position)
        if (!std::isfinite(value) || std::abs(value) > 100000)
            return "invalid position";
    if (!plugins::valid_object_rotation(rotation))
        return "invalid rotation";
    if (!s.claims.owns(plugin, id))
        return "this plugin does not control this object's physics";
    auto& lease = s.leases.at(id);
    if (suspended() || lease.editing || !current(id, lease) || !sample_pose(lease))
        return "object physics is unavailable";
    const auto now = GetTickCount64();
    if (lease.pose_at && now < lease.pose_at + 250)
        return "object pose rate limit reached";
    auto wanted = lease.pose;
    std::copy(position.begin(), position.end(), wanted.begin() + 8);
    std::copy(rotation.begin(), rotation.end(), wanted.begin() + 4);
    // The client replica moves now; server_tick mirrors the new pose to the server copy.
    if (!place(lease.client, wanted, lease.options.dynamic ? abi::motion_dynamic : abi::motion_fixed))
        return "object pose could not be applied";
    stop(lease.body);
    if (lease.options.dynamic) {
        s.wake(&lease.body);
        s.sleep(&lease.body, false);
    }
    lease.pose = wanted;
    lease.sampled = now;
    lease.pose_at = now;
    return {};
}
std::optional<plugins::MarkerEvents> marker_events(std::uintptr_t base, std::string_view plugin) {
    std::lock_guard native_lock(local_runtime().native_mutex);
    if (!start_markers(base))
        return std::nullopt;
    NoBailSkater skater;
    const bool local = no_bail_skater(skater);
    std::lock_guard lock(marker_mutex);
    return marker_journal.poll(plugin, local ? skater.entity : 0);
}
bool claimed(std::uint64_t id) noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        forget_gone_world();
        return state().leases.contains(id);
    } catch (...) {
        return false;
    }
}
void release(std::string_view plugin) noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        forget_gone_world();
        auto& s = state();
        std::erase_if(s.leases, [&](auto& entry) {
            if (entry.second.owner != plugin)
                return false;
            restore(entry.first, entry.second);
            s.claims.erase(entry.first);
            return true;
        });
        std::lock_guard marker_lock(marker_mutex);
        marker_journal.release(plugin);
        publish_roots();
    } catch (...) {
    }
}
void client_tick() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        forget_gone_world();
        auto& s = state();
        if (!s.ready || s.leases.empty())
            return;
        const bool pause = suspended();
        const auto inbox = pause ? placement_adapters::Inbox{} : receive_edits();
        const auto now = GetTickCount64();
        for (auto at = s.leases.begin(); at != s.leases.end();) {
            if (!pause && !at->second.server_lost && current(at->first, at->second) && sample_pose(at->second)) {
                ++at;
                continue;
            }
            restore(at->first, at->second);
            s.claims.erase(at->first);
            at = s.leases.erase(at);
        }
        std::map<std::uint64_t, placement_adapters::Published> roots;
        const bool menu = multiplayer::sample_game_ui_state(s.base).in_menu || park_editor_owns_placements();
        for (auto& [id, lease] : s.leases) {
            const bool editing = menu || inbox.selected.contains(id) || now <= lease.edit_until;
            if (editing && !lease.editing)
                lease.fixed_pose = lease.pose;
            lease.editing = editing;
            const int mode = lease.options.dynamic && !editing ? abi::motion_dynamic : abi::motion_fixed;
            if (editing && !placement_geometry::same_pose(lease.pose, lease.fixed_pose))
                (void)place(lease.client, lease.fixed_pose, abi::motion_fixed);
            s.motion(&lease.body, mode);
            if (mode == abi::motion_fixed)
                stop(lease.body);
            else {
                s.wake(&lease.body);
                s.sleep(&lease.body, false);
            }
            const placement_adapters::Body body{lease.body.world, lease.body.index, lease.body.generation};
            roots[lease.server] = {id, body, lease.options.surface && !editing};
            roots[lease.client] = {id, body, lease.options.surface && !editing};
        }
        placement_adapters::publish(roots);
        // A claimed surface under the board, found by rays down through the deck, carries the board
        // while it grinds (placement_adapters.cpp friction_hook).
        NoBailSkater player;
        const auto movement = skater_movement::read(s.base);
        const bool riding = !menu && movement && !movement->on_foot && movement->focused && no_bail_skater(player);
        placement_adapters::Body support_body{};
        std::array<float, 3> point{};
        if (riding) {
            namespace adapter = addr::placement_adapters;
            std::uintptr_t board{}, physics{}, parts{}, owner{};
            std::uint32_t count{};
            std::array<float, 3> at{};
            if (memory::peek(player.core + addr::skater_movement::skater_board, board) && memory::peek(board + adapter::board_physics, physics) &&
                memory::peek(physics, owner) && owner == s.base + addr::skater_state::board_physics_vtable &&
                memory::peek(physics + adapter::board_parts, parts) && memory::peek(parts, count) && count == adapter::board_part_count &&
                memory::peek(parts + adapter::parts_owner, owner) && owner == physics &&
                memory::peek(parts + adapter::parts_position, at)) {
                for (float side : adapter::board_ray_offsets) {
                    const auto hit = cast({at[0] + side, at[1] + adapter::board_ray_above, at[2]},
                                          {at[0] + side, at[1] - adapter::board_ray_below, at[2]});
                    if (!hit)
                        continue;
                    const auto found = s.leases.find(hit->id);
                    if (found == s.leases.end() || !found->second.options.surface || found->second.editing)
                        continue;
                    const auto& body = found->second.body;
                    support_body = {body.world, body.index, body.generation};
                    point = hit->position;
                    break;
                }
            }
        }
        placement_adapters::support(riding ? player.context : 0, riding, support_body, point);
    } catch (...) {
        reset();
    }
}
std::optional<plugins::ObjectContact> contact(std::string_view plugin, const std::array<float, 3>& from, const std::array<float, 3>& to) {
    std::lock_guard lock(local_runtime().native_mutex);
    forget_gone_world();
    if (!state().ready || suspended())
        return std::nullopt;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(from[i]) || !std::isfinite(to[i]) || std::abs(from[i]) > 100000 || std::abs(to[i]) > 100000)
            return std::nullopt;
    if (placement_geometry::distance2(from, to) > 100)
        return std::nullopt;
    const auto hit = cast(from, to);
    if (!hit || !state().claims.owns(plugin, hit->id))
        return std::nullopt;
    const auto& lease = state().leases.at(hit->id);
    if (!current(hit->id, lease) || lease.editing)
        return std::nullopt;
    return hit;
}
void server_tick() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        forget_gone_world();
        auto& s = state();
        if (!s.ready)
            return;
        // Released objects: the server copy goes back to its layout pose, in the motion it had before
        // the claim mirrored it. A copy the claim never wrote is left as it is.
        for (const auto& queued : std::exchange(s.restores, {})) {
            if (!row_current(queued.id, queued.token, queued.map_generation, queued.server))
                continue;
            if (const auto again = s.leases.find(queued.id); again != s.leases.end()) {
                // Claimed again before this restore ran: the copy is still as the old claim left it.
                again->second.server_motion = placement_motion::inherit(again->second.server_motion, queued.motion);
                continue;
            }
            if (queued.motion == placement_motion::unseen)
                continue;
            Body body;
            if (!place_copy(queued.server, queued.pose, queued.motion) && placement_motion::body_motion(queued.motion) &&
                body_of(queued.server, body))
                s.motion(&body, queued.motion);
        }
        if (s.leases.empty() || suspended())
            return;
        // Moving objects: the server copy follows the client replica, keyframed. An object being edited
        // is left to the dropper, which moves the server copy itself.
        const auto now = GetTickCount64();
        for (auto& [id, lease] : s.leases) {
            if (lease.editing || now > lease.sampled + 100 || placement_adapters::pending(id) ||
                !row_current(id, lease.token, lease.map_generation, lease.server))
                continue;
            if (lease.server_lost)
                continue;
            if (lease.server_motion == placement_motion::unseen) {
                // Read once, before the first write. A copy that cannot be resolved yet is tried again
                // next tick, for a second; the claim on the client replica goes on meanwhile.
                Body server;
                const bool found = body_of(lease.server, server);
                if (found)
                    lease.server_motion = s.motion_type(&server);
                else if (now > lease.claimed_at + 1000 && entity_live(lease.server)) {
                    // Live, but still no body to reach after the second a body is given to turn up (not
                    // there yet right after a map load, say): it follows by transform (placement_motion.h).
                    lease.server_motion = placement_motion::bodiless;
                    if (static unsigned told{}; told < 8) {
                        ++told;
                        logging::log(logging::Level::info, logging::Channel::objects,
                            "Object physics: object {}'s server copy has no physics body ReSkate can reach; it follows by its transform.", id);
                    }
                }
                const bool seen = lease.server_motion != placement_motion::unseen;
                if (!seen ? now > lease.claimed_at + 1000 : !placement_motion::server_mirrorable(lease.server_motion)) {
                    lease.server_lost = true;
                    logging::log(logging::Level::warning, logging::Channel::objects,
                        "Object physics: object {} let go: {}.", id,
                        seen ? std::format("the game moves its server copy itself (motion {})", lease.server_motion)
                             : std::string("its server copy could not be found"));
                    continue;
                }
                if (!seen)
                    continue;
            }
            (void)place_copy(lease.server, lease.pose, abi::motion_keyframed);
        }
    } catch (...) {
    }
}
void before_level_transition(unsigned next) noexcept {
    // The same states plugin effects treat as a world's end and start (plugin_effects.cpp).
    if (next == 3 || next == 14 || next == 22 || next == 24 || next == 25) {
        state().world_gone.store(true);
        placement_adapters::world_gone();
        try {
            std::lock_guard lock(marker_mutex);
            marker_journal.reset();
        } catch (...) {
        }
    } else if (next == 13 || next == 21)
        placement_adapters::world_ready();
}
void reset() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        forget_gone_world();
        auto& s = state();
        for (auto& [id, lease] : s.leases)
            restore(id, lease);
        s.leases.clear();
        s.claims.clear();
        placement_adapters::reset();
        std::lock_guard marker_lock(marker_mutex);
        marker_journal.reset();
    } catch (...) {
    }
}
} // namespace dingosdk::placement_physics
