#include "placement_physics.h"
#include "local_placements_runtime.h"
#include "ParkEditor/park_editor_internal.h"
#include "Engine/Game/Build/20260929/placement_physics.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Skater/no_bail.h"
#include <deque>

namespace dingosdk::placement_physics {
namespace {
using namespace profile_runtime;
namespace pe = profile_runtime::park_editor_detail;
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
};
struct State {
    std::uintptr_t base{};
    bool ready{};
    std::map<std::uint64_t, Lease> leases;
    plugins::ObjectPhysicsClaims claims;
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
State& state() { static auto* s = new State; return *s; }
template<std::size_t N> bool matches(std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> bytes{};
    return memory::peek(address, bytes) && bytes == expected;
}
bool start(std::uintptr_t base) {
    auto& s = state();
    if (s.ready) return s.base == base;
    if (!base || !pe::editor_state().native_ready || !pe::editor_state().picking_api.ready) return false;
    for (const auto* contract : {&phys::set_motion_type, &phys::set_linear_velocity, &phys::set_angular_velocity,
                                &phys::wake_body, &phys::body_valid, &phys::set_body_980})
        if (!matches(base + contract->rva, contract->bytes)) return false;
    if (!matches(base + abi::body_pose, abi::body_pose_prefix) || !matches(base + abi::sleep, abi::sleep_prefix) ||
        !matches(base + abi::motion_type, abi::motion_type_prefix) ||
        !matches(base + addr::local_placements::realm_context, addr::local_placements::realm_context_prefix)) return false;
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
    s.ready = true;
    return true;
}
const PlacementRow* row_for(std::uint64_t id) {
    const auto& rows = placements_runtime().rows;
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.spawned && row.saved_id == id && row.entity; });
    return at == rows.end() ? nullptr : &*at;
}
bool body_of(std::uint64_t entity, Body& body) {
    auto& r = placements_runtime();
    auto& e = pe::editor_state();
    if (!entity || !r.resolve || !r.valid || !e.physics_query) return false;
    alignas(16) std::array<std::uint64_t, 2> root{};
    alignas(16) std::array<std::uint64_t, 5> component{};
    return r.resolve(root.data(), entity, false) == root.data() && root[0] == entity && r.valid(root.data()) &&
        e.physics_query(root.data(), component.data()) == component.data() && component[2] && component[3] &&
        memory::peek(component[3], body) && body.world && body.index < 1000000 && state().valid(&body);
}
std::map<std::uint32_t, std::uint64_t> client_objects() {
    std::map<std::uint32_t, std::uint64_t> result;
    std::uintptr_t context{}, begin{}, end{};
    std::uint32_t offset{}; std::uint8_t ready{};
    auto& s = state(); auto& r = placements_runtime();
    reinterpret_cast<void* (*)(void*, std::uint32_t)>(s.base + addr::local_placements::realm_context)(&context, addr::local_placements::client_realm);
    if (!context || !memory::peek(s.base + addr::local_placements::client_ready_offset, offset) || offset > 0x1000000) return result;
    const auto store = context + offset;
    if (!memory::peek(store + abi::store_ready, ready) || ready != 1 || !memory::peek(store + abi::records_begin, begin) ||
        !memory::peek(store + abi::records_end, end) || !begin || end < begin || (end - begin) % abi::record_size ||
        (end - begin) / abi::record_size > 65536) return result;
    for (auto at = begin; at < end; at += abi::record_size) {
        std::uint8_t active{}; std::uint32_t id{}; std::uint64_t entity{};
        alignas(16) std::array<std::uint64_t, 2> root{};
        if (memory::peek(at + abi::record_active, active) && active && memory::peek(at + abi::record_id, id) &&
            memory::peek(at + abi::record_entity, entity) && entity && r.resolve(root.data(), entity, false) == root.data() &&
            root[0] && r.valid(root.data())) result[id] = root[0];
    }
    return result;
}
bool current(std::uint64_t id, const Lease& lease) {
    if (!placements_runtime().manager || placements_runtime().map.empty() || placements_runtime().map_generation != lease.map_generation) return false;
    const auto* row = row_for(id);
    if (!row || row->token != lease.token || row->entity != lease.server) return false;
    const auto& api = pe::editor_state().picking_api;
    const auto server = editor::buildkit_identity(api, lease.server), client = editor::buildkit_identity(api, lease.client);
    Body body;
    return server && client && *server == *client && body_of(lease.client, body) && body == lease.body;
}
bool inverse_mass(const Body& body, float& value) {
    std::uintptr_t data{}, buffer{};
    return memory::peek(body.world + abi::world_state, data) && memory::peek(data + abi::mass_buffer, buffer) && buffer &&
        memory::peek(buffer + std::uint64_t{body.index} * sizeof(float), value) && std::isfinite(value) && value > 0 && value < 1000;
}
bool finite(const Pose& pose) {
    for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(pose[i]) || std::abs(pose[i]) > 100000) return false;
    float norm{};
    for (unsigned i = 4; i < 8; ++i) norm += pose[i] * pose[i];
    return norm > .95f && norm < 1.05f && pose[0] > .001f;
}
bool sample_pose(Lease& lease) {
    auto& r = placements_runtime();
    alignas(16) std::array<std::uint64_t, 3> query{};
    alignas(16) std::array<float, 8> rigid{};
    if (r.query(query.data(), lease.client) != query.data() || r.pose(lease.pose.data(), query.data()) != lease.pose.data() ||
        state().body_pose(&lease.body, rigid.data()) != rigid.data()) return false;
    std::copy_n(rigid.begin(), 4, lease.pose.begin() + 4);
    std::copy_n(rigid.begin() + 4, 3, lease.pose.begin() + 8);
    if (!finite(lease.pose)) return false;
    lease.sampled = GetTickCount64();
    return true;
}
bool place(std::uint64_t entity, const Pose& pose, int mode) {
    auto& r = placements_runtime(); auto& e = pe::editor_state(); auto& s = state();
    alignas(16) std::array<std::uint64_t, 3> query{};
    std::uintptr_t source{};
    Body body;
    if (!finite(pose) || r.query(query.data(), entity) != query.data() || !body_of(entity, body) ||
        !memory::peek(s.base + addr::park_editor::transform_notification_source, source)) return false;
    s.motion(&body, 2);
    alignas(16) std::array<float, 8> rigid{pose[8], pose[9], pose[10], pose[0], pose[4], pose[5], pose[6], pose[7]};
    e.physics_move(&body, rigid.data()); e.transform_move(query.data(), pose.data(), source);
    s.motion(&body, mode);
    return true;
}
void stop(const Body& body) {
    alignas(16) const std::array<float, 4> zero{};
    state().linear(&body, zero.data()); state().angular(&body, zero.data());
}
void restore(std::uint64_t id, Lease lease) {
    auto& s = state();
    if (!current(id, lease)) return; // Never write through a stale row/body generation.
    if (sample_pose(lease)) (void)place(lease.server, lease.pose, 1);
    stop(lease.body); s.mass(&lease.body, lease.inverse_mass); s.motion(&lease.body, 1);
}
bool suspended() {
    return multiplayer_session_active() || !placements_runtime().manager || placements_runtime().map.empty() ||
        placements_runtime().clearing || !lobby_object_placement_allowed() || park_editor_owns_placements() ||
        multiplayer::sample_game_ui_state(state().base).in_menu;
}
std::mutex marker_mutex;
plugins::MarkerJournal marker_journal;
bool marker_attempted{}, marker_ready{};
void (*save_original)(std::uintptr_t, const float*, bool, bool, bool){};
void (*return_original)(std::uintptr_t, const float*, std::uintptr_t, const void*){};
void enqueue(std::uintptr_t controller, bool saved) {
    std::uintptr_t handle{}; std::uint64_t entity{};
    if (!memory::peek(controller + abi::marker_owner, handle) || !handle || !memory::peek(handle, entity) || !entity) return;
    std::lock_guard lock(marker_mutex);
    marker_journal.push(entity, saved);
}
void save_hook(std::uintptr_t controller, const float* transform, bool a, bool b, bool c) {
    save_original(controller, transform, a, b, c);
    try { enqueue(controller, true); } catch (...) {}
}
void return_hook(std::uintptr_t controller, const float* transform, std::uintptr_t label, const void* options) {
    std::uint32_t before{}, after{};
    const bool saved = reinterpret_cast<std::uintptr_t>(transform) == controller + abi::marker_transform &&
        memory::peek(controller + abi::marker_transition, before) && before == 0;
    return_original(controller, transform, label, options);
    if (saved && memory::peek(controller + abi::marker_transition, after) && after == 1)
        try { enqueue(controller, false); } catch (...) {}
}
bool start_markers(std::uintptr_t base) {
    if (marker_attempted) return marker_ready;
    marker_attempted = true;
    if (!base || !matches(base + abi::marker_save, abi::marker_save_prefix) || !matches(base + abi::marker_return, abi::marker_return_prefix)) return false;
    auto* save = reinterpret_cast<void*>(base + abi::marker_save);
    auto* back = reinterpret_cast<void*>(base + abi::marker_return);
    HookBatch batch;
    if (hook_prepare(save, reinterpret_cast<void*>(&save_hook), reinterpret_cast<void**>(&save_original)) != HookOk ||
        hook_prepare(back, reinterpret_cast<void*>(&return_hook), reinterpret_cast<void**>(&return_original)) != HookOk ||
        hook_queue_enable(save) != HookOk || hook_queue_enable(back) != HookOk || hook_apply_queued() != HookOk) return false;
    marker_ready = true;
    return true;
}
}
std::string configure(std::uintptr_t base, std::string_view plugin, std::uint64_t id, const std::optional<plugins::ObjectPhysics>& options) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& s = state();
    if (!options) {
        if (!s.claims.owns(plugin, id)) return "this plugin does not control this object's physics";
        restore(id, s.leases.at(id)); s.leases.erase(id); s.claims.erase(id); return {};
    }
    if (!plugins::valid_object_physics(*options)) return "invalid mass";
    if (!start(base)) return "unavailable here";
    if (suspended()) return "object physics is offline-only and unavailable while editing or in a menu";
    const auto* row = row_for(id);
    if (!row) return "not a live local placement";
    if (s.claims.occupied(id) && !s.claims.owns(plugin, id)) return "another plugin controls this object's physics";
    if (s.claims.owns(plugin, id)) {
        auto& lease = s.leases.at(id);
        if (!current(id, lease)) return "object changed; release its old physics claim first";
        if (lease.options.dynamic == options->dynamic && lease.options.mass == options->mass) return {};
        if (GetTickCount64() < lease.configured_at + 16) return "object physics rate limit reached";
        lease.options = *options;
    } else {
        const auto identity = editor::buildkit_identity(pe::editor_state().picking_api, row->entity);
        const auto records = client_objects();
        if (!identity) return "object identity is unavailable";
        const auto at = records.find(identity->drop_id);
        if (at == records.end()) return "the object's client replica is unavailable";
        const auto client = editor::buildkit_identity(pe::editor_state().picking_api, at->second);
        if (!client || *client != *identity) return "object replica identity differs";
        Lease lease;
        lease.owner = plugin; lease.token = row->token; lease.map_generation = placements_runtime().map_generation;
        lease.server = row->entity; lease.client = at->second; lease.options = *options;
        if (!body_of(lease.client, lease.body) || !inverse_mass(lease.body, lease.inverse_mass) || !sample_pose(lease)) return "object physics body is unavailable";
        Body authoritative;
        if (s.motion_type(&lease.body) != 1 || !body_of(lease.server, authoritative) || s.motion_type(&authoritative) != 1)
            return "only fixed local placement bodies may be claimed";
        if (auto why = s.claims.claim(plugin, id); !why.empty()) return why;
        s.leases.emplace(id, std::move(lease));
    }
    auto& lease = s.leases.at(id);
    s.mass(&lease.body, 1.f / lease.options.mass);
    s.motion(&lease.body, lease.options.dynamic ? 3 : 1);
    if (lease.options.dynamic) { s.wake(&lease.body); s.sleep(&lease.body, false); }
    else stop(lease.body);
    lease.configured_at = GetTickCount64();
    return {};
}
std::string velocity(std::string_view plugin, std::uint64_t id, const std::array<float, 3>& value) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& s = state();
    if (!plugins::valid_object_velocity(value)) return "invalid velocity";
    if (!s.claims.owns(plugin, id)) return "this plugin does not control this object's physics";
    auto& lease = s.leases.at(id);
    if (suspended() || !current(id, lease) || !lease.options.dynamic) return "object physics is unavailable";
    const auto now = GetTickCount64();
    if (lease.velocity_at && now < lease.velocity_at + 16) return "object velocity rate limit reached";
    alignas(16) std::array<float, 4> aligned{value[0], value[1], value[2], 0};
    s.linear(&lease.body, aligned.data()); s.wake(&lease.body); s.sleep(&lease.body, false);
    lease.velocity_at = now;
    return {};
}
std::optional<plugins::ObjectMotion> sample(std::string_view plugin, std::uint64_t id) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& s = state();
    if (!s.claims.owns(plugin, id)) return std::nullopt;
    auto& lease = s.leases.at(id);
    if (!current(id, lease) || !sample_pose(lease)) return std::nullopt;
    plugins::ObjectMotion result;
    std::copy_n(lease.pose.begin() + 8, 3, result.position.begin());
    std::copy_n(lease.pose.begin() + 4, 4, result.rotation.begin());
    result.dynamic = lease.options.dynamic;
    result.generation = lease.token;
    std::uintptr_t data{}, buffer{}; std::uint32_t count{}; std::uint16_t page{};
    if (!memory::peek(lease.body.world + abi::world_state, data) || !memory::peek(data + abi::velocity_buffer, buffer) || !buffer ||
        !memory::peek(data + abi::velocity_count, count) || !memory::peek(data + abi::velocity_page, page) ||
        count > 1000000 || lease.body.index >= count || page > 16) return std::nullopt;
    const auto stride = ((std::uint64_t{count} * 16 + 60) & ~std::uint64_t{63});
    if (!memory::peek(buffer + stride * page + std::uint64_t{lease.body.index} * 16, result.velocity)) return std::nullopt;
    for (float v : result.velocity) if (!std::isfinite(v) || std::abs(v) > 1000) return std::nullopt;
    return result;
}
std::string pose(std::string_view plugin, std::uint64_t id, const std::array<float, 3>& position, const std::array<float, 4>& rotation) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& s = state();
    for (float value : position) if (!std::isfinite(value) || std::abs(value) > 100000) return "invalid position";
    if (!plugins::valid_object_rotation(rotation)) return "invalid rotation";
    if (!s.claims.owns(plugin, id)) return "this plugin does not control this object's physics";
    auto& lease = s.leases.at(id);
    if (suspended() || !current(id, lease) || !sample_pose(lease)) return "object physics is unavailable";
    const auto now = GetTickCount64();
    if (lease.pose_at && now < lease.pose_at + 250) return "object pose rate limit reached";
    auto wanted = lease.pose;
    std::copy(position.begin(), position.end(), wanted.begin() + 8);
    std::copy(rotation.begin(), rotation.end(), wanted.begin() + 4);
    if (!place(lease.client, wanted, lease.options.dynamic ? 3 : 1) || !place(lease.server, wanted, 2)) return "object pose could not be applied";
    stop(lease.body);
    if (lease.options.dynamic) { s.wake(&lease.body); s.sleep(&lease.body, false); }
    lease.pose = wanted; lease.sampled = now; lease.pose_at = now;
    return {};
}
std::optional<plugins::MarkerEvents> marker_events(std::uintptr_t base, std::string_view plugin) {
    std::lock_guard native_lock(local_runtime().native_mutex);
    if (!start_markers(base)) return std::nullopt;
    NoBailSkater skater;
    const bool local = no_bail_skater(skater);
    std::lock_guard lock(marker_mutex);
    return marker_journal.poll(plugin, local ? skater.entity : 0);
}
void release(std::string_view plugin) noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto& s = state();
        std::erase_if(s.leases, [&](auto& entry) {
            if (entry.second.owner != plugin) return false;
            restore(entry.first, entry.second); s.claims.erase(entry.first); return true;
        });
        std::lock_guard marker_lock(marker_mutex); marker_journal.release(plugin);
    } catch (...) {}
}
void client_tick() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto& s = state();
        if (!s.ready || s.leases.empty()) return;
        const bool pause = suspended();
        for (auto at = s.leases.begin(); at != s.leases.end();) {
            if (!pause && current(at->first, at->second) && sample_pose(at->second)) { ++at; continue; }
            restore(at->first, at->second); s.claims.erase(at->first); at = s.leases.erase(at);
        }
    } catch (...) { reset(); }
}
void server_tick() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto& s = state();
        if (!s.ready || s.leases.empty() || suspended()) return;
        const auto now = GetTickCount64();
        for (const auto& [id, lease] : s.leases)
            if (now <= lease.sampled + 100 && current(id, lease)) (void)place(lease.server, lease.pose, 2);
    } catch (...) {}
}
void reset() noexcept {
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        auto& s = state();
        for (auto& [id, lease] : s.leases) restore(id, lease);
        s.leases.clear(); s.claims.clear();
        std::lock_guard marker_lock(marker_mutex);
        marker_journal.reset();
    } catch (...) {}
}
}
