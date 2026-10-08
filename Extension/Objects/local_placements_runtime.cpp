#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "network_object_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/World/local_world_layers.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace dingosdk::profile_runtime {
// Called on the native server tick for observation and client tick for replay.

thread_local PlacementBatch* placement_batch{};

PlacementsRuntime& placements_runtime() { static auto* r = new PlacementsRuntime; return *r; }

namespace {
// Advanced whenever a placement message, create, apply or delete may be changing
// the native table. One tick's updates ask for the same table several times: a
// read from the last few milliseconds is reused unless this moved since.
std::atomic<std::uint64_t> table_epoch{1};
void placement_table_changed() noexcept { table_epoch.fetch_add(1, std::memory_order_acq_rel); }
struct TableCache {
    std::uintptr_t manager{};
    std::uint64_t epoch{};
    std::chrono::steady_clock::time_point at{};
    std::vector<PlacementId> rows;
};
// A table node: {u32 id, entity at +8, next at +0x18}.
struct TableNode {
    std::uint32_t id{}, unused{};
    std::uint64_t entity{}, unused2{};
    std::uintptr_t next{};
};
static_assert(sizeof(TableNode) == 0x20);
}

std::optional<std::vector<PlacementId>> placement_manager_ids(std::uintptr_t manager) {
    // Callers hold native_mutex.
    static TableCache cache;
    const auto epoch = table_epoch.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    if (cache.manager && cache.manager == manager && cache.epoch == epoch && now - cache.at < std::chrono::milliseconds(5))
        return cache.rows;
    // Thousands of nodes in a big park: guarded same-process copies, one per node,
    // rather than three system calls per node and a JSON object each.
    std::uintptr_t vtable{}, buckets{};
    std::uint32_t size{}, count{};
    if (!memory::peek(manager, vtable) || vtable != local_runtime().base + addr::local_placements::placement_manager_vtable ||
        !memory::peek(manager + 0x10, buckets) || !memory::peek(manager + 0x18, size) ||
        !size || size > 65536 || !memory::peek(manager + 0x1c, count) || count > 65536)
        return {};
    std::vector<PlacementId> rows;
    rows.reserve(count);
    thread_local std::vector<std::uintptr_t> nodes;
    nodes.clear();
    for (unsigned bucket = 0; bucket < size && rows.size() < count; ++bucket) {
        std::uintptr_t node{};
        if (!memory::peek(buckets + bucket * 8, node)) return {};
        while (node) {
            // At most `count` nodes: a cycle or a list mutated mid-read runs past it.
            if (rows.size() >= count) return {};
            TableNode value;
            if (!memory::peek(node, value)) return {};
            nodes.push_back(node);
            rows.push_back({value.id, value.entity});
            node = value.next;
        }
    }
    // A node reached twice means the table changed during the read.
    std::sort(nodes.begin(), nodes.end());
    if (rows.size() != count || std::adjacent_find(nodes.begin(), nodes.end()) != nodes.end()) return {};
    cache = {manager, epoch, now, rows};
    return rows;
}

void queue_placement_save() {
    auto& r = placements_runtime();
    if (r.map.empty() || r.save_failed) return;
    if (r.personal_document) {
        // Session edits/clear operations must never overwrite the guest's save.
        r.clear_saved = true;
        return;
    }
    std::lock_guard lock(r.save_mutex);
    r.pending[r.map] = r.document.maps.at(r.map);
    r.save_wake.notify_one();
}

void placement_reset_session() {
    placement_table_changed();
    reset_network_object_world();
    reset_park_editor();
    auto& r = placements_runtime();
    r.creation.reset();
    r.creates.clear();
    r.tracked.clear(); r.restore.clear(); r.late_restores.clear(); r.inflight.reset(); r.rows.clear(); r.teleport_token = 0; r.status.clear();
    r.map.clear(); r.map_context = 0; r.next_poll = 0; r.map_since = 0; r.map_generation = 0;
    r.restore_failed = false;
    r.clearing = false; r.clear_entities.clear();
}

bool placement_session_ready() {
    auto& r = placements_runtime();
    auto& world = world_layers_runtime();
    if (!r.store || r.failed || r.save_failed || !r.manager || !world.model.ready ||
        world.model.map == WorldMap::none || !world.context) return false;
    const std::string map(world_map_key(world.model.map));
    if (r.map != map || r.map_context != world.context || r.map_generation != world.root_generation) {
        placement_reset_session();
        r.map = map; r.map_context = world.context; r.map_generation = world.root_generation; r.map_since = GetTickCount64();
        queue_placement_layout(r);
        if (r.failed) return false;
    }
    return true;
}

bool placement_same_pose(const profile::PlacedObject& a, const profile::PlacedObject& b) {
    if (a.item != b.item) return false;
    float distance{}, dot{};
    for (unsigned i = 0; i < 3; ++i) distance += (a.position[i] - b.position[i]) * (a.position[i] - b.position[i]);
    for (unsigned i = 0; i < 4; ++i) dot += a.rotation[i] * b.rotation[i];
    return distance < 0.0001f && std::abs(dot) > 0.99999f && std::abs(a.scale - b.scale) < .0001f;
}

std::optional<profile::PlacedObject> placement_from_applied(const PlacementApplied& applied) {
    // Retail mode 4 is a disposable preview; mode 1 is the confirmed object.
    // Nonzero variant IDs have not been observed, so leave those native.
    if (applied.words[16] != 1 || applied.words[1] != 0) return {};
    profile::PlacedObject object; object.id = 1;
    for (const auto& [key, item] : cosmetic_runtime().items) if (item.build_kit && item.hash == applied.words[0]) {
        if (!object.item.empty()) return {}; // An ambiguous hash cannot be a durable identity.
        object.item = key;
    }
    std::memcpy(object.position.data(), applied.words.data() + 8, sizeof(object.position));
    std::memcpy(object.rotation.data(), applied.words.data() + 12, sizeof(object.rotation));
    return profile::valid_placed_object(object) ? std::optional{object} : std::nullopt;
}

std::optional<std::set<std::uint64_t>> placement_live_entities(std::uintptr_t manager) {
    // The diagnostic enumerator validates vtable, counts, linked-list cycles,
    // and every node read. Never treat an unreadable table as an empty world.
    const auto table = placement_manager_ids(manager);
    if (!table) return {};
    std::set<std::uint64_t> result;
    for (const auto& row : *table) result.insert(row.entity);
    return result;
}

void observe_placement(std::uint64_t entity, const profile::PlacedObject& object) {
    auto& r = placements_runtime();
    const auto known = r.tracked.find(entity);
    const auto saved_id = known == r.tracked.end() ? 0 : known->second;
    auto row = std::find_if(r.rows.begin(), r.rows.end(), [&](const auto& x) {
        return x.entity == entity || (saved_id && x.saved_id == saved_id);
    });
    if (row == r.rows.end()) {
        if (r.rows.size() >= 2048) return;
        r.rows.push_back({r.next_token++, entity, saved_id, object, true});
    } else {
        row->entity = entity; row->saved_id = saved_id; row->object = object;
        row->spawned = true; row->missing_polls = 0;
    }
}

void reconcile_placement_rows(const std::set<std::uint64_t>& live) {
    auto& r = placements_runtime();
    const auto& objects = r.document.maps[r.map];
    // Saved objects by id (the first of a duplicate, as a search would find): a
    // search per row was rows x objects, up to two million steps a poll.
    std::unordered_map<std::uint64_t, const profile::PlacedObject*> saved_by_id;
    saved_by_id.reserve(objects.size());
    for (const auto& object : objects) saved_by_id.emplace(object.id, &object);
    for (auto& row : r.rows) {
        if (row.entity && live.contains(row.entity)) {
            row.spawned = true;
            row.missing_polls = 0;
        } else if (row.spawned && row.entity && row.missing_polls < 2) {
            // The native hash table is mutated while placement messages are
            // dispatched. Two isolated valid-but-incomplete snapshots must not
            // make a visible object unselectable.
            ++row.missing_polls;
        } else {
            if (row.entity) r.tracked.erase(row.entity);
            row.entity = 0;
            row.spawned = false;
            row.missing_polls = 0;
        }
        if (row.saved_id) {
            const auto saved = saved_by_id.find(row.saved_id);
            if (saved == saved_by_id.end()) row.saved_id = 0;
            else if (!row.spawned) row.object = *saved->second;
        }
    }
    std::erase_if(r.rows, [](const auto& row) { return !row.saved_id && !row.spawned; });
}

void placement_finish_batch(std::uintptr_t manager, const PlacementBatch& incoming) {
    auto& r = placements_runtime();
    if (manager != r.manager || !placement_session_ready()) return;
    const auto live = placement_live_entities(manager);
    PlacementBatch batch{incoming.type, {}};
    for (const auto &applied : incoming.applied) {
        const auto object = placement_from_applied(applied);
        // The post-apply callback is direct evidence that this entity accepted
        // the recipe. A concurrently mutating ID table may be unreadable or omit
        // it for one snapshot; never discard the only acknowledgement for that.
        if (object && network_object_observe(applied.entity, *object, incoming.network_source)) continue;
        // Even an invalid/late acknowledgement may never become a local save.
        if (incoming.network_source) continue;
        if (object && park_editor_observe(applied.entity, *object)) continue;
        batch.applied.push_back(applied);
    }
    auto& objects = r.document.maps[r.map];
    bool changed{};
    for (const auto& applied : batch.applied) {
        auto candidate = placement_from_applied(applied);
        if (!candidate) continue;
        const auto claim_restore = [&]() -> std::optional<std::uint64_t> {
            if (r.inflight && placement_matches_applied(*r.inflight, *candidate)) {
                const auto id = r.inflight->id;
                r.inflight.reset();
                return id;
            }
            for (auto it = r.late_restores.begin(); it != r.late_restores.end();) {
                if (std::none_of(objects.begin(), objects.end(), [&](const auto& x) { return x.id == it->id; })) {
                    it = r.late_restores.erase(it);
                    continue;
                }
                if (placement_matches_applied(*it, *candidate)) {
                    const auto id = it->id;
                    r.late_restores.erase(it);
                    return id;
                }
                ++it;
            }
            return {};
        };
        // A native tool's object that got past the request filter while the
        // lobby forbids placement. Delete it; it never reaches the saved layout.
        const auto reject = [&] {
            if (!incoming.native || lobby_object_placement_allowed() || r.tracked.contains(applied.entity))
                return false;
            retire_local_network_object(applied.entity, *candidate);
            dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{
                {"event", "local_placement_blocked_object"}, {"type", incoming.type}, {"item", candidate->item}}.dump().c_str());
            return true;
        };
        if (!r.enabled || r.clearing) {
            // A request sent just before disabling may still be acknowledged.
            // Remember its live identity without changing the saved layout.
            if (const auto restored = claim_restore()) r.tracked[applied.entity] = *restored;
            else if (reject()) continue;
            observe_placement(applied.entity, *candidate);
            continue;
        }
        const auto known = r.tracked.find(applied.entity);
        if (known != r.tracked.end()) {
            const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& x) { return x.id == known->second; });
            if (object != objects.end()) {
                // Applied recipes omit scale. Preserve the last authoritative
                // value until update_placement_poses reads the native matrix.
                candidate->id = object->id;
                candidate->scale = object->scale;
                if (!placement_same_pose(*object, *candidate)) {
                    *object = *candidate; changed = true;
                }
            }
        } else if (const auto restored = claim_restore()) {
            r.tracked[applied.entity] = *restored;
            dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "local_placement_restored"}, {"map", r.map},
                {"id", *restored}, {"item", candidate->item}}.dump().c_str());
        } else if (reject()) {
            continue;
        } else if (objects.size() < 1024 && r.next_id != UINT64_MAX) {
            candidate->id = r.next_id++;
            r.tracked[applied.entity] = candidate->id;
            objects.push_back(*candidate); changed = true;
        }
        observe_placement(applied.entity, *candidate);
    }
    // Only explicit object/delete-all messages remove saved records. Destruction,
    // expiry, replication gaps and level unload must never clear the save.
    if (live && r.enabled && !r.clearing && (batch.type == 0x9a1d7771 || batch.type == 0xe6f66136 || batch.type == 0x8898bdc0 || batch.type == 0xd124eaf0)) {
        for (auto it = r.tracked.begin(); it != r.tracked.end();) {
            if (!live->contains(it->first)) {
                const auto id = it->second;
                std::erase_if(objects, [&](const auto& x) { return x.id == id; });
                it = r.tracked.erase(it); changed = true;
            } else ++it;
        }
    }
    if (live) reconcile_placement_rows(*live);
    if (changed) { park_editor_external_change(); queue_placement_save(); }
}

// Native create (705a6a69) and copy (2f51fbd9) requests from the game's own
// object tools. ReSkate's restores and network spawns call the server create
// directly and never pass through this handler.
constexpr bool placement_adds_objects(std::uint32_t type) {
    return type == 0x705a6a69 || type == 0x2f51fbd9;
}

void placement_message_hook(std::uintptr_t manager, const void* message) {
    PlacementBatch batch;
    batch.native = true;
    { PreserveError preserve; read(reinterpret_cast<std::uintptr_t>(message) + 0xc, batch.type); }
    if (placement_adds_objects(batch.type) && lobby_object_placement_allowed() && lobby_object_limit_reached()) {
        // As below: the request never reaches the server, so nothing is created.
        PreserveError preserve;
        placements_runtime().status = object_limit_notice;
        dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{
            {"event", "local_placement_over_limit"}, {"type", batch.type}, {"limit", lobby_object_limit()}}.dump().c_str());
        return;
    }
    if (placement_adds_objects(batch.type) && !lobby_object_placement_allowed()) {
        // Drop the request before the server sees it: no entity is created.
        PreserveError preserve;
        dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{
            {"event", "local_placement_blocked_request"}, {"type", batch.type}}.dump().c_str());
        return;
    }
    struct Scope {
        PlacementBatch* previous{placement_batch};
        ~Scope() { placement_batch = previous; }
    } scope;
    placement_batch = &batch;
    placement_table_changed();
    placements_runtime().message(manager, message);
    placement_table_changed();
    PreserveError preserve;
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        if (local_runtime().active.load(std::memory_order_acquire)) placement_finish_batch(manager, batch);
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_placement_capture_failed\"}"); }
}

void placement_apply_hook(const void* entity, const void* recipe) {
    if (placement_batch && placement_batch->network_source) {
        PreserveError preserve;
        std::lock_guard lock(local_runtime().native_mutex);
        if (!network_object_creation_owner(placement_batch->network_source)) return;
        // Preserve the native simulation owner. Steam ownership is tracked by
        // the private source token and entity key, including late callbacks.
    }
    placements_runtime().apply(entity, recipe);
    placement_table_changed();
    PreserveError preserve;
    try {
        if (!placement_batch || placement_batch->applied.size() >= 1024) return;
        PlacementApplied applied;
        if (read(reinterpret_cast<std::uintptr_t>(entity), applied.entity) &&
            read(reinterpret_cast<std::uintptr_t>(recipe), applied.words)) placement_batch->applied.push_back(applied);
    } catch (...) {}
}

std::uintptr_t placement_construct_hook(std::uintptr_t manager, std::uintptr_t allocator) {
    auto& r = placements_runtime();
    const auto result = r.construct(manager, allocator);
    PreserveError preserve;
    std::lock_guard lock(local_runtime().native_mutex);
    placement_reset_session(); r.manager = result;
    return result;
}

std::uintptr_t placement_destroy_hook(std::uintptr_t manager, unsigned flags) {
    auto& r = placements_runtime();
    std::lock_guard lock(local_runtime().native_mutex);
    { PreserveError preserve; if (r.manager == manager) { placement_reset_session(); r.manager = 0; } }
    return r.destroy(manager, flags);
}

bool queue_placement_create(const profile::PlacedObject& object, std::uint32_t item, std::uint32_t token) {
    auto& r = placements_runtime();
    return r.creates_enabled && r.server_create && r.creates.push(object, item, token, r.creation.source());
}

void update_placement_creates() {
    // Called only by the native server park tick, while holding native_mutex.
    // Keep the queue private: the client message serializer cannot carry IDs.
    auto& r = placements_runtime();
    if (!r.creates_enabled || r.creates.empty() || !r.server_create) return;
    const auto base = local_runtime().base;
    std::uintptr_t server{}, vtable{}, context{}, manager{}, begin{}, end{}, player{}, component{};
    std::uint32_t manager_offset{}, component_offset{}, parent_offset{};
    if (!read(base + addr::engine::game_server, server) || !server ||
        !read(server, vtable) || vtable != base + addr::engine::server_vtable ||
        !read(server + 8, context) || context != r.map_context ||
        !read(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000 ||
        !read(context + manager_offset, manager) || !manager ||
        !read(manager, vtable) || vtable != base + addr::engine::server_player_manager_vtable ||
        !read(manager + 0xc8, begin) || !read(manager + 0xd0, end) ||
        !begin || end < begin || (end - begin) % 8 || end - begin > 64 * 8) return;
    // ReSkate peers are client replicas. The hosted native server has one human
    // player; never accidentally dispatch for a bot or an ambiguous connection.
    for (auto entry = begin; entry < end; entry += 8) {
        std::uintptr_t candidate{};
        std::uint8_t bot{}, spectator{}, ready{};
        if (!read(entry, candidate) || !candidate || !read(candidate, vtable) ||
            vtable != base + addr::engine::server_player_vtable || !read(candidate + 0x44, bot) ||
            !read(candidate + 0x70, spectator)) return;
        if (bot || spectator) continue;
        if (player || !read(candidate + 0x2e3, ready) || !ready) return;
        player = candidate;
    }
    if (!player || !read(base + addr::local_placements::player_component_offset, component_offset) ||
        !read(base + addr::engine::player_parent_offset, parent_offset)) return;
    const auto delta = static_cast<std::int64_t>(component_offset) - parent_offset;
    if (delta < -0x10000 || delta > 0x10000 || player < 0x20000 ||
        player > UINTPTR_MAX - 0x10000) return;
    component = delta < 0 ? player - static_cast<std::uintptr_t>(-delta) : player + delta;
    std::uint64_t native_owner{};
    // The native render/collision activators run only for the native
    // simulation owner. ReSkate peers have no native ServerPlayer, so every
    // local replica must use this instance's player ID (legitimately zero).
    // Steam ownership remains in the network key; it still controls saves,
    // authoritative updates and removal when a peer leaves.
    if (!read(component + 0x98, native_owner)) return;
    const auto request = r.creates.take(r.creation.source());
    if (!request) return;
    const bool remote = is_network_object_source(request->token);
    if (remote) {
        if (!network_object_creation_owner(request->token)) return;
    } else if (!r.creation.accepts(request->token)) return;

    PlacementCreateRecipe recipe(*request, native_owner);
    const void* data = recipe.words.data();
    PlacementBatch batch{0x705a6a69, {}, remote ? request->token : 0};
    struct Scope {
        PlacementBatch* previous{placement_batch};
        ~Scope() { placement_batch = previous; }
    } scope;
    placement_batch = &batch;
    placement_table_changed();
    r.server_create(r.manager, &data, 0, player, 0);
    placement_table_changed();
    placement_finish_batch(r.manager, batch);
    dingosdk::logging::event(dingosdk::logging::Channel::objects,
        dingosdk::Json{{"event", "local_placement_server_create"}, {"remote", remote},
                      {"applied", batch.applied.size()}}.dump().c_str());
}

void update_placement_poses() {
    auto& r = placements_runtime();
    if (!placement_session_ready()) return;
    update_placement_creates();
    update_park_editor_moves();
    update_network_object_moves();
    if (!placement_session_ready() || r.clearing || park_editor_owns_placements() || GetTickCount64() < r.next_poll) return;
    r.next_poll = GetTickCount64() + 1000;
    const auto live = placement_live_entities(r.manager);
    if (!live) return;
    reconcile_placement_rows(*live);
    auto& objects = r.document.maps[r.map]; bool changed{};
    for (auto& row : r.rows) {
        if (!row.spawned) continue;
        alignas(16) std::array<std::uint64_t, 2> reference{};
        alignas(16) std::array<std::uint64_t, 3> query{};
        alignas(16) std::array<float, 12> transform{};
        if (r.resolve(reference.data(), row.entity, true) != reference.data() || !r.valid(reference.data()) ||
            r.query(query.data(), reference[0]) != query.data() || r.pose(transform.data(), query.data()) != transform.data()) continue;
        auto next = row.object;
        std::copy_n(transform.data() + 8, 3, next.position.data());
        std::copy_n(transform.data() + 4, 4, next.rotation.data());
        next.scale = transform[0];
        if (!profile::valid_placed_object(next)) continue;
        row.object = next;
        if (!r.enabled || !row.saved_id) continue;
        const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& x) { return x.id == row.saved_id; });
        if (object != objects.end() && !placement_same_pose(next, *object)) {
            next.id = object->id; *object = next; changed = true;
        }
    }
    if (changed) { park_editor_external_change(); queue_placement_save(); }
}

std::uintptr_t placement_client_channel() {
    auto& r = placements_runtime();
    std::array<std::uintptr_t, 2> context{};
    r.context(context.data());
    std::uint32_t offset{}, connection_offset{};
    std::uint8_t ready{};
    std::uintptr_t connection{}, channel{};
    const auto base = local_runtime().base;
    if (!context[0] || !read(base + addr::local_placements::client_ready_offset, offset) || offset > 0x1000000 ||
        !read(context[0] + offset + 0x100, ready) || ready != 1 ||
        !read(base + addr::engine::context_client_connection_offset, connection_offset) || connection_offset > 0x1000000 ||
        !read(context[0] + connection_offset, connection) || !connection ||
        !read(connection + 0x50, channel) || !channel) return 0;
    return channel;
}

void send_placement_delete(std::uintptr_t channel, const std::vector<std::uint32_t>& ids) {
    auto& r = placements_runtime();
    // Native delete-list message, containing only validated IDs of tracked
    // objects. The original API also borrows a stack message while sending.
    std::vector<std::uint32_t> payload(ids.size() + 2);
    payload[0] = payload[1] = static_cast<std::uint32_t>(ids.size());
    std::copy(ids.begin(), ids.end(), payload.begin() + 2);
    alignas(16) std::array<std::byte, 0x50> message{};
    r.delete_ctor(message.data());
    struct Destroy { decltype(r.message_destroy) call; void* message; ~Destroy() { call(message); } } destroy{r.message_destroy, message.data()};
    const void* data = payload.data() + 2;
    std::memcpy(message.data() + 0x48, &data, sizeof(data));
    placement_table_changed();
    r.send(channel, message.data());
}

void update_placement_clear() {
    auto& r = placements_runtime();
    if (!r.clear_saved || r.clear_map != r.map) return;
    const auto table = placement_manager_ids(r.manager);
    if (!table) return;
    std::vector<std::uint32_t> ids;
    for (const auto& row : *table)
        if (r.clear_entities.contains(row.entity)) ids.push_back(row.id);
    if (ids.empty() || (r.clear_sent && GetTickCount64() > r.sent_at + 10000)) {
        for (auto entity : r.clear_entities) r.tracked.erase(entity);
        std::set<std::uint64_t> live;
        for (const auto& row : *table) live.insert(row.entity);
        std::erase_if(r.rows, [&](const auto& row) {
            return r.clear_entities.contains(row.entity) && !live.contains(row.entity);
        });
        reconcile_placement_rows(live);
        r.clearing = false;
        r.status = ids.empty() ? "Selected objects deleted." : "Save updated. Reload the level to remove remaining objects.";
        return;
    }
    if (r.clear_sent) return;
    const auto channel = placement_client_channel();
    if (!channel) return;
    r.clear_sent = true; r.sent_at = GetTickCount64();
    send_placement_delete(channel, ids);
    r.status = "Save updated. Removing selected objects...";
}

bool begin_placement_delete(const std::set<std::uint64_t>& tokens) {
    auto& r = placements_runtime();
    if (r.clearing || r.inflight || park_editor_owns_placements() || tokens.empty()) return false;
    std::set<std::uint64_t> saved_ids;
    r.clear_map = r.map; r.clear_entities.clear();
    for (const auto& row : r.rows) if (tokens.contains(row.token)) {
        if (row.saved_id) saved_ids.insert(row.saved_id);
        if (row.entity) r.clear_entities.insert(row.entity);
        if (row.token == r.teleport_token) r.teleport_token = 0;
    }
    auto& layout = r.document.maps[r.map];
    std::erase_if(layout, [&](const auto& x) { return saved_ids.contains(x.id); });
    std::erase_if(r.restore, [&](auto id) { return saved_ids.contains(id); });
    std::erase_if(r.late_restores, [&](const auto& object) { return saved_ids.contains(object.id); });
    r.clearing = true; r.clear_sent = false; r.clear_saved = false;
    { std::lock_guard pending(r.save_mutex); r.clear_wait_map = r.map; r.clear_wait_layout = layout; }
    r.status = "Deleting selected objects...";
    park_editor_external_change();
    queue_placement_save();
    return true;
}

std::array<float, 16> placement_teleport_transform(const profile::PlacedObject& object) {
    // Keep the skater upright, beside and above the object's origin. The native
    // teleport manager handles streaming, ground checks and physics reset.
    return {1,0,0,0, 0,1,0,0, 0,0,1,0,
        object.position[0] + 3, object.position[1] + 2, object.position[2], 1};
}

std::uint32_t send_placement_teleport(std::uintptr_t manager, const profile::PlacedObject& object) {
    auto& r = placements_runtime();
    alignas(16) const auto transform = placement_teleport_transform(object);
    alignas(16) std::array<std::uint64_t, 2> transition{};
    r.transition_ctor(transition.data());
    struct Destroy { decltype(r.transition_destroy) call; void* value; ~Destroy() { call(value); } } destroy{r.transition_destroy, transition.data()};
    return r.teleport(manager, transform.data(), 1, transition.data());
}

void update_position_teleport() {
    auto& r = placements_runtime();
    if (!r.position_teleport) return;
    if (GetTickCount64() > r.position_teleport_at + 5000) {
        r.position_teleport.reset();
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::objects,
                                 "Teleport dropped: the skater was busy for 5 s (a bail, a menu or an activity).");
        return;
    }
    std::uintptr_t manager{}; std::uint32_t state{};
    if (!r.teleport || !r.transition_ctor || !r.transition_destroy || !placement_client_channel() ||
        !read(local_runtime().base + addr::local_placements::teleport_manager, manager) || !manager ||
        !read(manager, state) || state != 0) return;
    const auto at = *r.position_teleport;
    r.position_teleport.reset();
    // Upright at the spot. The native teleport manager handles streaming, ground checks and physics reset.
    alignas(16) const std::array<float, 16> transform{1,0,0,0, 0,1,0,0, 0,0,1,0, at[0], at[1], at[2], 1};
    alignas(16) std::array<std::uint64_t, 2> transition{};
    r.transition_ctor(transition.data());
    struct Destroy { decltype(r.transition_destroy) call; void* value; ~Destroy() { call(value); } } destroy{r.transition_destroy, transition.data()};
    const auto request = r.teleport(manager, transform.data(), 1, transition.data());
    dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "position_teleport_requested"},
        {"x", at[0]}, {"y", at[1]}, {"z", at[2]}, {"request", request}}.dump().c_str());
}

void update_placement_teleport() {
    auto& r = placements_runtime();
    if (!r.teleport_token) return;
    const auto row = std::find_if(r.rows.begin(), r.rows.end(), [&](const auto& x) { return x.token == r.teleport_token; });
    if (row == r.rows.end() || !profile::valid_placed_object(row->object)) { r.teleport_token = 0; return; }
    if (GetTickCount64() > r.teleport_queued_at + 5000) {
        r.teleport_token = 0; r.status = "Teleport unavailable. Finish the current activity and try again."; return;
    }
    std::uintptr_t manager{}; std::uint32_t state{};
    if (!placement_client_channel() || !read(local_runtime().base + addr::local_placements::teleport_manager, manager) || !manager ||
        !read(manager, state) || state != 0) return;
    const auto object = row->object;
    const auto token = r.teleport_token; r.teleport_token = 0;
    const auto request = send_placement_teleport(manager, object);
    r.status = "Teleport requested. Close the menu to resume skating.";
    dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "local_placement_teleport_requested"}, {"map", r.map}, {"row", token}, {"request", request}}.dump().c_str());
}

void update_placement_restore() {
    auto& r = placements_runtime();
    if (!placement_session_ready()) return;
    update_park_editor();
    // Teleport is independent of the placement mutation channel. Service it
    // before background editor/network work can return early.
    update_placement_teleport();
    if (park_editor_owns_placements()) return;
    if (r.clearing) { update_placement_clear(); return; }
    update_network_objects();
    if (network_objects_inflight()) return;
    if (!r.enabled || r.restore_failed || GetTickCount64() < r.map_since + 3000) return;
    if (r.inflight) {
        if (GetTickCount64() > r.sent_at + 10000) {
            if (r.late_restores.size() < 1024) r.late_restores.push_back(*r.inflight);
            r.inflight.reset();
            r.restore_failed = false;
            r.status = "One saved object did not acknowledge its restore. It was skipped without blocking editing.";
            dingosdk::logging::event(dingosdk::logging::Channel::objects,
                "{\"event\":\"local_placement_restore_unacknowledged\",\"retry\":false,\"editing_released\":true}");
        }
        return;
    }
    if (r.restore.empty() || cosmetic_runtime().items.empty() || !placement_client_channel()) return;
    const auto id = r.restore.front();
    const auto& objects = r.document.maps[r.map];
    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& x) { return x.id == id; });
    if (object == objects.end()) { r.restore.erase(r.restore.begin()); return; }
    const auto item = cosmetic_runtime().items.find(object->item);
    if (item == cosmetic_runtime().items.end() || !item->second.build_kit) {
        dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "local_placement_restore_missing_item"}, {"item", object->item}}.dump().c_str());
        r.restore.erase(r.restore.begin()); return; // retain unavailable entries in the JSON
    }
    if (!r.creation.source()) { r.restore_failed = true; r.status = "Object request IDs exhausted; restart the game."; return; }
    if (!queue_placement_create(*object, item->second.hash, r.creation.source())) return;
    r.inflight = *object; r.sent_at = GetTickCount64(); r.restore.erase(r.restore.begin());
    dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "local_placement_restore_sent"}, {"map", r.map}, {"id", id}}.dump().c_str());
}

void initialize_placement_store(const std::filesystem::path& profile_path) {
    auto& r = placements_runtime();
    try {
        r.store = std::make_unique<profile::PlacementStore>(profile_path.parent_path() / L"objects.sqlite3");
        r.document = r.store->snapshot();
        r.enabled = local_runtime().store->bool_option("ReSkate.ObjectPersistence").value_or(true);
        r.writer = std::jthread([&r](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::map<std::string, profile::ObjectLayout, std::less<>> pending;
                {
                    std::unique_lock lock(r.save_mutex);
                    r.save_wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return !r.pending.empty() || stop.stop_requested(); });
                    pending.swap(r.pending);
                }
                if (r.save_failed) continue;
                try {
                    for (const auto& [map, objects] : pending) {
                        r.store->replace_map(map, objects);
                        { std::lock_guard lock(r.save_mutex);
                            if (r.clear_wait_map == map && objects == r.clear_wait_layout) r.clear_saved = true;
                        }
                        dingosdk::logging::event(dingosdk::logging::Channel::objects, dingosdk::Json{{"event", "local_placements_saved"}, {"map", map}, {"objects", objects.size()}}.dump().c_str());
                    }
                } catch (...) {
                    r.save_failed = true;
                    dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_placements_save_failed\",\"previous_save_retained\":true}");
                }
            }
        });
        dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_placements_initialized\",\"file\":\"objects.sqlite3\"}");
    } catch (...) { r.failed = true; dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_placements_unavailable\"}"); }
}
}
