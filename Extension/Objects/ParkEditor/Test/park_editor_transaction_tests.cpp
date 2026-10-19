// Exercise the real transaction coordinator against an in-memory native adapter.
// No game process, native addresses, hooks or physics are used by this fixture.
#include "Extension/Objects/ParkEditor/editor_math.h"
#include "Extension/Objects/network_object_runtime.cpp"
#include "Extension/Objects/ParkEditor/park_editor_runtime.cpp"
#include "Extension/Objects/ParkEditor/park_editor_lobby.cpp"
#include "Extension/Objects/ParkEditor/park_editor_operations.cpp"
#include "Extension/Objects/ParkEditor/park_editor_preview.cpp"
#include "Extension/Objects/ParkEditor/park_editor_selection.cpp"
#include <bit>
#include <iostream>
#include <limits>

namespace dingosdk::profile_runtime {
namespace fixture {
std::map<std::uint64_t, profile::PlacedObject> world;
std::uint64_t next_entity = 100;
unsigned creates{}, moves{}, deletes{}, saves{};
unsigned body_moves{};
bool acknowledge = true, duplicate_on_move{}, invalid_native_position{};
bool table_valid = true;
constexpr std::uint32_t native_position_padding = 0xffc00000;
std::uintptr_t notification_source = 42;
std::array<std::uint64_t, 2> body_reference{};
std::map<std::uint64_t, profile::PlacedObject> bodies;
constexpr std::uintptr_t client_context = 0x1100, server_context = 0x2200;
unsigned surface_calls{};
bool surface_world_ready = true;
std::uint64_t surface_entity{};
bool surface_self_hit{};
void *context(void *out) {
    *static_cast<std::uintptr_t *>(out) = client_context;
    return out;
}
editor::NativeSurfaceApi surface_api{
    [](std::uintptr_t context) -> std::uintptr_t {
        return context == client_context && surface_world_ready ? 0x3300 : 0;
    },
    [](std::uintptr_t native_world, editor::NativeSurfaceResult *out, const editor::NativeSurfaceRay *ray,
       const char *) -> void * {
        ++surface_calls;
        static float fraction = .02f;
        fraction = .02f;
        if (surface_entity) {
            const auto *begin = reinterpret_cast<const editor::SurfaceBody *>(ray->ignored_bodies[0]);
            const auto *end = reinterpret_cast<const editor::SurfaceBody *>(ray->ignored_bodies[1]);
            surface_self_hit =
                !begin ||
                std::find(begin, end, editor::SurfaceBody{native_world, surface_entity + 1000}) == end;
            const float height = surface_self_hit ? world.at(surface_entity).position[1] + 2 : 2;
            fraction = (ray->start[1] - height) / (ray->start[1] - ray->end[1]);
        }
        static std::array<std::uintptr_t, 7> data{0, reinterpret_cast<std::uintptr_t>(&fraction)};
        out->world = native_world;
        out->data = reinterpret_cast<std::uintptr_t>(data.data());
        out->count = 1;
        return out;
    },
    nullptr, nullptr, true};
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void create(const void *wrapper, std::uint32_t mode, std::uint8_t flag, std::uint32_t source) {
    const bool remote = is_network_object_source(source);
    require(mode == 1 && flag == 0 && (remote || placements_runtime().creation.accepts(source)),
            "Create did not use the current placement scope");
    const auto *matrix = *static_cast<const float *const *>(wrapper);
    require(matrix[-1] == std::bit_cast<float>(1U), "Native array count is invalid");
    ++creates;
    if (!acknowledge)
        return;
    auto object = editor_state().transaction ? editor_state().transaction->steps.front().object
                                                   : network_objects_detail::state().creating->object;
    if (remote) {
        require(network_object_creation_owner(source) != 0,
                "Replicated spawn lost its private Steam ownership");
        // Physics/native matrix conversion can change the placement. Neither
        // a local object at the exact requested pose nor an old request owns it.
        require(!network_object_observe(next_entity, object) &&
                    !network_object_observe(next_entity, object, source - 1),
                "An untagged/local placement was claimed by the remote owner");
        object.position[1] += .25f;
    } else
        object.scale = 1; // Like the game: the create recipe carries no scale.
    const auto id = next_entity++;
    world[id] = object;
    if (editor_state().transaction) {
        park_editor_observe(id, object);
        // placement_finish_batch reconciles after every create batch.
        if (const auto live = placement_live_entities(0))
            reconcile_placement_rows(*live);
    } else
        require(network_object_observe(id, object, source), "Remote creation escaped its ownership callback");
}
void message_destroy(void *) {}
void send(std::uintptr_t, const void *) {
    require(false, "Moving an existing object used a copy/create network message");
}
void *resolve(void *out, std::uint64_t entity, bool check_grabbable) {
    static_cast<std::uint64_t *>(out)[0] = entity >= 2000 ? entity - 1000 : entity;
    // The live placed object has body type 0. The native property scan returns
    // 0x800 for this resting body, even though its entity/transform are valid.
    static_cast<std::uint64_t *>(out)[1] = check_grabbable ? 0x800 : 0;
    return out;
}
bool valid(const void *reference) {
    const auto *resolved = static_cast<const std::uint64_t *>(reference);
    return (world.contains(resolved[0]) || (resolved[0] >= 1000 && world.contains(resolved[0] - 1000))) &&
           resolved[1] == 0;
}
std::array<std::uint64_t, 16> mapping_manager{};
bool stale_replica{};
unsigned mapping_calls{};
void *buildkit_query(const void *reference, void *out) {
    const auto entity = *static_cast<const std::uint64_t *>(reference);
    const bool client = entity >= 1000;
    const auto server = client ? entity - 1000 : entity;
    static std::array<std::uint64_t, 8> component{};
    component = {};
    component[1] = server * 17 + (client && stale_replica ? 1 : 0);
    component[0x20 / 8] = server << 32; // Native drop ID at +0x24.
    auto *scope = static_cast<std::uint64_t *>(out);
    if (world.contains(server)) {
        scope[1] = 1;
        scope[2] = reinterpret_cast<std::uintptr_t>(component.data());
        scope[3] = entity;
    }
    return out;
}
editor::NativePickingApi picking_api{
    [](const void *hit, void *out) -> void * {
        const auto *query = static_cast<const std::uint64_t *>(hit);
        require(query[0] == 0x3300 && query[1] == 0, "Picking changed the native ray-hit layout");
        auto *body = static_cast<std::uint64_t *>(out);
        body[0] = query[0];
        body[1] =
            surface_entity ? (surface_self_hit ? surface_entity + 1000 : 0) : world.begin()->first + 1000;
        return out;
    },
    [](const void *) { return true; },
    [](void *out, const void *body) -> void * {
        const auto id = static_cast<const std::uint64_t *>(body)[1];
        *static_cast<std::uint64_t *>(out) = id ? id + 1000 : 0;
        return out;
    },
    []() -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(mapping_manager.data()); },
    [](std::uintptr_t, void *out, std::uint64_t id, int direction) -> void * {
        ++mapping_calls;
        *static_cast<std::uint64_t *>(out) = direction == 0 ? id + 1000 : id - 1000;
        return out;
    },
    true};
void *query(void *out, std::uint64_t entity) {
    static_cast<std::uint64_t *>(out)[0] = entity;
    return out;
}
float *pose(float *out, const void *reference) {
    const auto &object = world.at(*static_cast<const std::uint64_t *>(reference));
    const std::array<float, 12> data{object.scale,
                                     object.scale,
                                     object.scale,
                                     0,
                                     object.rotation[0],
                                     object.rotation[1],
                                     object.rotation[2],
                                     object.rotation[3],
                                     invalid_native_position ? std::numeric_limits<float>::infinity()
                                                             : object.position[0],
                                     object.position[1],
                                     object.position[2],
                                     std::bit_cast<float>(native_position_padding)};
    std::copy(data.begin(), data.end(), out);
    return out;
}
void *physics_query(const void *reference, void *out) {
    const auto entity = *static_cast<const std::uint64_t *>(reference);
    body_reference = entity >= 1000 ? editor::SurfaceBody{0x3300, entity} : editor::SurfaceBody{entity, 77};
    auto *component = static_cast<std::uint64_t *>(out);
    component[2] = 1;
    component[3] = reinterpret_cast<std::uintptr_t>(body_reference.data());
    return out;
}
void physics_move(const void *reference, const float *pose) {
    const auto entity = *static_cast<const std::uint64_t *>(reference);
    require(world.contains(entity), "Physics move lost the original entity");
    require(std::isfinite(pose[3]) && pose[3] >= .01f && pose[3] <= 100,
            "Physics move received an invalid uniform scale");
    auto object = world.at(entity);
    std::copy_n(pose, 3, object.position.begin());
    std::copy_n(pose + 4, 4, object.rotation.begin());
    object.scale = pose[3];
    bodies[entity] = object;
    ++body_moves;
}
void transform_move(const void *reference, const float *pose, std::uintptr_t source) {
    const auto entity = *static_cast<const std::uint64_t *>(reference);
    require(world.contains(entity) && source == notification_source, "Invalid native move target/source");
    require(pose[0] == pose[1] && pose[1] == pose[2] && pose[0] == bodies.at(entity).scale &&
                pose[3] == 0 &&
                 std::bit_cast<std::uint32_t>(pose[11]) == native_position_padding,
            "Move did not apply uniform scale or preserve transform padding");
    auto object = world.at(entity);
    std::copy_n(pose + 8, 3, object.position.begin());
    std::copy_n(pose + 4, 4, object.rotation.begin());
    object.scale = pose[0];
    require(bodies.at(entity) == object, "Physics body and entity transform disagree");
    ++moves;
    if (!acknowledge)
        return;
    world[duplicate_on_move ? next_entity++ : entity] = object;
}
} // namespace fixture
LocalRuntime &local_runtime() {
    static LocalRuntime state;
    return state;
}
PlacementsRuntime &placements_runtime() {
    static PlacementsRuntime state;
    return state;
}
CosmeticRuntime &cosmetic_runtime() {
    static CosmeticRuntime state;
    return state;
}
// (No profile here grants an object: nothing is owned, as with the empty inventory this stood in for.)
bool installed_item_owned(const profile::Snapshot &, std::string_view, std::uint32_t, bool) {
    return false;
}
bool placement_session_ready() {
    return true;
}
bool placement_same_pose(const profile::PlacedObject &a, const profile::PlacedObject &b) {
    return a.item == b.item && a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}
std::uintptr_t placement_client_channel() {
    return 1;
}
void queue_placement_save() {
    if (placements_runtime().personal_document) return;
    ++fixture::saves;
}
void update_buildkit_limits() {}
std::optional<std::set<std::uint64_t>> placement_live_entities(std::uintptr_t) {
    if (!fixture::table_valid) return {};
    std::set<std::uint64_t> ids;
    for (const auto &[id, obj] : fixture::world) {
        (void)obj;
        ids.insert(id);
    }
    return ids;
}
std::optional<std::vector<PlacementId>> placement_manager_ids(std::uintptr_t) {
    if (!fixture::table_valid) return {};
    std::vector<PlacementId> objects;
    for (const auto &[id, obj] : fixture::world) {
        (void)obj;
        objects.push_back({static_cast<std::uint32_t>(id), id});
    }
    return objects;
}
void reconcile_placement_rows(const std::set<std::uint64_t> &live) {
    // As the real one: a row whose id is neither saved nor in the editor's pending edit is unlinked.
    auto &r = placements_runtime();
    std::erase_if(r.rows, [&](const auto &row) { return !live.contains(row.entity); });
    const auto pending = park_editor_pending_layout();
    for (auto &row : r.rows) {
        const auto has = [&](const profile::ObjectLayout &layout) {
            return std::any_of(layout.begin(), layout.end(), [&](const auto &object) { return object.id == row.saved_id; });
        };
        if (row.saved_id && !has(r.document.maps[r.map]) && !(pending && has(*pending)))
            row.saved_id = 0;
    }
}
void observe_placement(std::uint64_t entity, const profile::PlacedObject &object) {
    auto &r = placements_runtime();
    std::erase_if(r.rows, [&](const auto &row) { return row.entity == entity; });
    r.rows.push_back({entity, entity, object.id, object, true});
}
void send_placement_delete(std::uintptr_t, const std::vector<std::uint32_t> &ids) {
    ++fixture::deletes;
    if (fixture::acknowledge)
        for (auto id : ids)
            fixture::world.erase(id);
}
bool begin_placement_delete(const std::set<std::uint64_t> &tokens) {
    auto &r = placements_runtime();
    std::erase_if(r.document.maps[r.map], [&](const auto &object) {
        return std::any_of(r.rows.begin(), r.rows.end(),
                           [&](const auto &row) { return tokens.contains(row.token) && row.saved_id == object.id; });
    });
    return true;
}
PlacementRequest placement_request(const profile::PlacedObject &object, std::uint32_t item) {
    PlacementRequest request;
    request.item = item;
    request.transform = {
        object.scale, 0, 0, 0, 0, object.scale, 0, 0, 0, 0, object.scale, 0,
        object.position[0], object.position[1], object.position[2], 1};
    return request;
}
bool queue_placement_create(const profile::PlacedObject& object, std::uint32_t item, std::uint32_t token) {
    auto request = placement_request(object, item);
    const void* data = request.transform.data();
    fixture::create(&data, 1, 0, token);
    return true;
}
void update_placement_restore() {
    update_park_editor();
}
} // namespace dingosdk::profile_runtime
int main() {
    try {
        using namespace dingosdk;
        using namespace profile_runtime;
        using fixture::require;
        // Private IDs with every low-three-bit pattern must leave the native
        // source at 3, including IDs that previously decoded as invalid 5..7.
        PlacementCreateQueue queue;
        const profile::PlacedObject queued_object{1, "own_bk_ramp", {1, 2, 3}, {0, 0, 0, 1}};
        for (const auto prefix : {0xd1000000U, 0xd2000000U}) {
            for (unsigned id = 1; id <= 16; ++id) {
                require(queue.push(queued_object, 42, prefix + id, 5), "Create was not queued");
                require(!queue.push(queued_object, 42, prefix + id + 1, 5), "Pending create was overwritten");
                const auto pending = queue.take(5);
                require(pending && pending->token == prefix + id && queue.empty(), "Private create ID was lost");
                PlacementCreateRecipe recipe(*pending, 1001);
                require(recipe.words[2] == 0x7f,
                        "Scripted object did not use the native free-roam placement context");
                require(recipe.words[0] == 42 && recipe.words[4] == 1001 && recipe.words[16] == 1 &&
                            recipe.words[17] == 3 && recipe.header[3] == 1,
                        "Private ID escaped into the native object recipe");
                require(std::none_of(recipe.words.begin(), recipe.words.end(),
                                     [&](auto word) { return word == pending->token; }),
                        "Native payload contains a private tracking token");
                // Offline player ID zero is valid even for a remote request.
                // A Steam ID here leaves the client replica invisible/inert.
                const PlacementCreateRecipe offline_recipe(*pending, 0);
                require(offline_recipe.words[4] == 0 && offline_recipe.words[5] == 0,
                        "Remote request replaced the native offline simulation owner");
            }
        }
        require(queue.push(queued_object, 42, 0xd2000001, 5) && !queue.take(6),
                "An old-world create was dispatched after a transition");
        require(queue.push(queued_object, 42, 0xd1000001, 6), "Queue did not recover after cancellation");
        queue.clear();
        require(!queue.take(6), "Cancelled lobby create reached the server");
        auto &runtime = local_runtime();
        runtime.active = true;
        runtime.base = reinterpret_cast<std::uintptr_t>(&fixture::notification_source) -
                       addr::park_editor::transform_notification_source;
        auto &r = placements_runtime();
        r.map = "bam";
        r.manager = 1;
        r.next_id = 1;
        r.send = fixture::send;
        r.message_destroy = fixture::message_destroy;
        r.resolve = fixture::resolve;
        r.valid = fixture::valid;
        r.query = fixture::query;
        r.pose = fixture::pose;
        r.context = fixture::context;
        r.map_context = fixture::server_context;
        auto &e = editor_state();
        e.surface_api = fixture::surface_api;
        e.native_ready = true;
        e.physics_query = fixture::physics_query;
        e.physics_move = fixture::physics_move;
        e.transform_move = fixture::transform_move;
        e.next_catalog = UINT64_MAX;
        e.assets = std::make_shared<const std::vector<EditorAsset>>(
            std::vector<EditorAsset>{{"own_bk_ramp", "Ramp", "Ramps"}});
        cosmetic_runtime().items["own_bk_ramp"] = {42, {}, "Ramps", 0, true};
        const EditorSurfaceRequest probe{e.generation, 1, {4, 12, 6}, {0, -1, 0}, 0};
        require(queue_local_park_surface(probe), "Surface sample was not queued");
        update_park_editor_moves();
        require(e.probe && !e.surface.id && fixture::surface_calls == 0,
                "Server update consumed a client collision query");
        update_park_editor();
        require(!e.probe && e.surface.id == probe.id && e.surface.available && e.surface.hit &&
                    e.surface.position == editor::Vec3{4, 2, 6} && fixture::surface_calls == 1 &&
                    !e.transaction && fixture::creates == 0 && fixture::saves == 0,
                "Idle client update did not resolve collision through the client TLS context");
        fixture::surface_world_ready = false;
        queue_local_park_surface({e.generation, 2, probe.origin, probe.direction, 0});
        update_park_editor();
        require(e.surface.id == 2 && !e.surface.available && !e.failed && fixture::surface_calls == 1,
                "Unavailable collision world queried physics or stopped park editing");
        fixture::surface_world_ready = true;
        queue_local_park_surface({e.generation, 3, probe.origin, probe.direction, 0});
        update_park_editor();
        require(e.surface.id == 3 && e.surface.hit, "Client collision did not recover after world readiness");
        require(!queue_local_park_surface({e.generation - 1, 4, probe.origin, probe.direction, 0}),
                "Stale level submitted a collision query");
        const auto run = [&](std::string_view operation,
                             const profile::PlacedObject &object = profile::PlacedObject{}) {
            return edit_local_park(operation, r.map, e.generation, e.revision, {}, object);
        };
        const auto finish = [&] {
            for (unsigned i = 0; i < 20 && e.transaction; ++i) {
                update_park_editor_moves();
                update_park_editor();
            }
            if (e.transaction)
                std::cerr << "unfinished transaction: status=" << e.status
                          << " steps=" << e.transaction->steps.size()
                          << " sent=" << e.transaction->sent
                          << " first_ack="
                          << (!e.transaction->steps.empty() && e.transaction->steps.front().acknowledged)
                          << " first_dirty="
                          << (!e.transaction->steps.empty() && e.transaction->steps.front().live_dirty) << '\n';
            if (e.failed)
                std::cerr << "failed transaction: status=" << e.status << '\n';
            require(!e.transaction && !e.failed, "Transaction failed to finish");
        };
        profile::PlacedObject ramp{0, "own_bk_ramp", {1, 2, 3}, {0, 0, 0, 1}};
        require(!run("place", ramp).starts_with("error"), "Place request rejected");
        update_park_editor();
        require(fixture::creates == 1 && fixture::saves == 0 && r.document.maps[r.map].empty(),
                "Unacknowledged create changed saved layout");
        finish();
        require(r.document.maps[r.map].size() == 1 && fixture::saves == 1 && e.undo.size() == 1,
                "Create was not committed once");
        const auto first = r.document.maps[r.map][0];
        const auto first_entity = fixture::world.begin()->first;
        e.picking_api = fixture::picking_api;
        require(!editor::mapped_entity(e.picking_api, first_entity, 0),
                "Unready native mapping table was called");
        fixture::mapping_manager[0x48 / 8] = 1;
        fixture::mapping_manager[0x50 / 8] = 0x100000001;
        fixture::mapping_manager[0x70 / 8] = 1;
        fixture::mapping_manager[0x78 / 8] = 0x100000001;
        require(editor::mapped_entity(e.picking_api, first_entity, 0) == first_entity + 1000,
                "Highlight mapping did not resolve server-to-client");
        // Local Build Kit tables retain one allocated bucket with zero entries.
        fixture::mapping_manager[0x50 / 8] = 1;
        fixture::mapping_manager[0x78 / 8] = 1;
        e.picking_api.buildkit_query = fixture::buildkit_query;
        const auto mapping_calls = fixture::mapping_calls;
        require(queue_local_park_surface({e.generation, 10, probe.origin, probe.direction, 0, true}),
                "Collision selection was not queued");
        update_park_editor();
        require(e.surface.object == first.id && e.surface.entity == first_entity,
                "Collision body/root mapping failed to select the persistent object");
        fixture::table_valid = false;
        require(queue_local_park_surface({e.generation, 9, probe.origin, probe.direction, 0, true}),
                "Transient-table collision selection was not queued");
        update_park_editor();
        require(e.surface.object == first.id && e.surface.entity == first_entity,
                "One failed native-table refresh discarded the valid picking cache");
        fixture::table_valid = true;
        require(client_root(first_entity) == first_entity + 1000 && fixture::mapping_calls == mapping_calls,
                "Local collision identity still depended on the empty general entity mapping");
        fixture::stale_replica = true;
        require(!client_root(first_entity) && e.client_roots.empty(),
                "A recycled native drop ID retained a stale client root");
        queue_local_park_surface({e.generation, 11, probe.origin, probe.direction, 0, true});
        update_park_editor();
        require(!e.surface.object && !e.surface.entity && e.client_roots.empty(),
                "A different replicated instance selected the object sharing its old drop ID");
        fixture::stale_replica = false;
        queue_local_park_surface({e.generation, 12, probe.origin, probe.direction, 0, true});
        update_park_editor();
        require(e.surface.object == first.id && client_root(first_entity) == first_entity + 1000,
                "The current replica did not become selectable after a stale hit");
        static std::array<std::uint8_t, 24> highlight_component{};
        e.highlight_api = {[](const void *, editor::NativeHighlightScope *scope) -> void * {
                               scope->type = 1;
                               scope->component =
                                   reinterpret_cast<std::uintptr_t>(highlight_component.data());
                               return scope;
                           },
                           [](editor::NativeHighlightScope *) {}, nullptr, true};
        const editor::HighlightValue outline{7, 1};
        const auto install_highlight = [&] {
            editor::highlight_value(e.highlight_api, nullptr, outline);
            e.highlights[first_entity + 1000] = {{0, 0}, outline};
            e.selection = {first.id};
            e.selection_time = GetTickCount64();
        };
        install_highlight();
        e.selection_time -= 2100;
        update_highlights();
        require(e.highlights.empty() &&
                    editor::highlight_value(e.highlight_api, nullptr) == editor::HighlightValue{0, 0},
                "Expired selection lease left a native highlight behind");
        install_highlight();
        require(!queue_local_park_selection({e.generation - 1, {}}),
                "Stale generation changed the current highlights");
        require(queue_local_park_selection({e.generation, {}}), "Deselection did not queue");
        update_highlights();
        require(e.highlights.empty() &&
                    editor::highlight_value(e.highlight_api, nullptr) == editor::HighlightValue{0, 0},
                "Deselection failed to restore a client's original color override");
        e.highlight_api = {};
        e.picking_api = {};
        ramp = first;
        ramp.position = {8, 9, 10};
        ramp.rotation = editor::rotation({0, 90, 0});
        ramp.scale = 2.5f;
        run("move", ramp);
        update_park_editor();
        require(fixture::moves == 0, "Client update mutated authoritative object transform");
        fixture::table_valid = false;
        update_park_editor_moves();
        update_park_editor();
        require(e.transaction && !e.failed && fixture::moves == 0,
                "One unreadable native object-table sample stopped the editor update");
        fixture::table_valid = true;
        update_park_editor_moves();
        const auto unrelated_entity = fixture::next_entity++;
        fixture::world[unrelated_entity] = {999, "own_bk_ramp", {90, 4, 90}, {0, 0, 0, 1}};
        finish();
        fixture::world.erase(unrelated_entity);
        require(fixture::moves == 1 && r.document.maps[r.map][0].position == ramp.position &&
                    r.document.maps[r.map][0].scale == ramp.scale,
                "Move/scale did not commit while an unrelated object changed");
        require(fixture::world.size() == 1 && fixture::world.contains(first_entity) &&
                    fixture::world.at(first_entity).rotation == ramp.rotation &&
                    fixture::world.at(first_entity).scale == ramp.scale && fixture::creates == 1 &&
                    fixture::body_moves == 1,
                "Move/rotate/scale duplicated the object, changed identity or left physics behind");
        run("undo");
        finish();
        require(r.document.maps[r.map][0] == first && e.redo.size() == 1,
                "Undo did not restore original transform");
        run("redo");
        finish();
        require(r.document.maps[r.map][0].position == ramp.position &&
                    r.document.maps[r.map][0].scale == ramp.scale,
                "Redo did not restore edited transform");
        const auto original_revision = e.revision;
        require(edit_local_park("delete", r.map, e.generation - 1, original_revision, {}, ramp)
                    .starts_with("error"),
                "Stale world selection accepted");
        require(edit_local_park("delete", r.map, e.generation, original_revision - 1, {}, ramp)
                    .starts_with("error"),
                "Stale revision accepted");
        require(fixture::deletes == 0, "Rejected selection sent a delete");
        run("delete", ramp);
        finish();
        require(r.document.maps[r.map].empty() && fixture::world.empty(),
                "Delete did not remove live and saved object");
        run("undo");
        finish();
        require(r.document.maps[r.map].size() == 1 && fixture::world.size() == 1,
                "Undo delete did not respawn object");
        const auto before = r.document.maps[r.map];
        const auto deletes = fixture::deletes;
        auto invalid = before;
        invalid[0].item = "own_bk_missing";
        bool rejected = false;
        try {
            begin(invalid, 0);
        } catch (...) {
            rejected = true;
        }
        require(rejected && fixture::deletes == deletes && !e.transaction,
                "Invalid replacement deleted existing objects before validation");
        // Live transforms remain outside the saved layout/history until release.
        const auto live_saved = fixture::saves;
        const auto live_history = e.undo.size();
        const auto live_entity = fixture::world.begin()->first;
        EditorPreviewRequest live;
        live.action = EditorPreviewAction::begin_move;
        live.generation = e.generation;
        live.revision = e.revision;
        live.token = 1;
        live.sequence = 1;
        live.object = before[0].id;
        live.position = before[0].position;
        live.rotation = before[0].rotation;
        require(queue_local_park_preview(live), "Live move did not start");
        const auto sample = [&](EditorPreviewAction action) {
            live.action = action;
            ++live.sequence;
            require(queue_local_park_preview(live), "Live sample was rejected");
        };
        const auto check_drag_ground = [&](std::uint64_t entity) {
            e.picking_api = fixture::picking_api;
            e.picking_api.buildkit_query = fixture::buildkit_query;
            e.client_roots.clear();
            fixture::surface_entity = entity;
            const auto saved = fixture::saves;
            for (unsigned i = 0; i < 4; ++i) {
                queue_local_park_surface({e.generation, 20 + i, {4, 30, 6}, {0, -1, 0}, 1});
                update_park_editor();
                require(e.surface.hit && e.surface.position == editor::Vec3{4, 2, 6} && !e.surface.entity,
                        "Live move or placement snapped onto its own client collision");
                require(client_root(entity) == entity + 1000 && fixture::saves == saved,
                        "Preview self exclusion lost replica identity or saved an intermediate pose");
            }
            fixture::surface_entity = 0;
            e.picking_api = {};
        };
        check_drag_ground(live_entity);
        live.position[0] += 4;
        sample(EditorPreviewAction::update);
        update_park_editor_moves();
        require(fixture::world.at(live_entity).position == live.position && fixture::world.size() == 1 &&
                    r.document.maps[r.map] == before && fixture::saves == live_saved &&
                    e.undo.size() == live_history,
                "Held drag failed to move the original body or saved an intermediate pose");
        require(!queue_local_park_preview(live), "Repeated preview sequence accepted");
        auto stale = live;
        stale.sequence++;
        stale.generation--;
        require(!queue_local_park_preview(stale), "Stale generation updated a live drag");
        live.position[0] += 7;
        sample(EditorPreviewAction::commit);
        finish();
        require(fixture::world.at(live_entity).position == live.position &&
                    fixture::saves == live_saved + 1 && e.undo.size() == live_history + 1,
                "Release did not commit the final position once");
        run("undo");
        finish();
        require(r.document.maps[r.map] == before, "A drag needed more than one undo");
        const auto redo_count = e.redo.size();
        const auto cancel_saved = fixture::saves;
        live.token++;
        live.revision = e.revision;
        sample(EditorPreviewAction::begin_move);
        update_park_editor_moves();
        sample(EditorPreviewAction::cancel);
        finish();
        require(fixture::world.at(live_entity) == before[0] && fixture::saves == cancel_saved &&
                    e.redo.size() == redo_count,
                "Escape did not restore the body or preserved neither save nor redo");
        live.token++;
        live.revision = e.revision;
        sample(EditorPreviewAction::begin_move);
        update_park_editor_moves();
        e.transaction->preview->last_sample = GetTickCount64() - 2100;
        finish();
        require(fixture::world.at(live_entity) == before[0] && fixture::saves == cancel_saved,
                "Abandoned preview lease did not restore the starting pose");
        // Start a second object while dragging; coalesce motion while create is awaiting its ACK.
        live.token++;
        live.revision = e.revision;
        live.item = "own_bk_ramp";
        live.object = 0;
        sample(EditorPreviewAction::begin_place);
        const auto create_count = fixture::creates;
        update_park_editor();
        require(fixture::creates == create_count + 1 && fixture::world.size() == 2 &&
                    r.document.maps[r.map] == before,
                "Dragged asset did not spawn a temporary native object");
        // Simulate the collision replica arriving before the delayed create callback.
        const auto preview_entity = fixture::world.rbegin()->first;
        e.transaction->steps.front().entity = 0;
        e.transaction->steps.front().acknowledged = false;
        const auto queries_before_ack = fixture::surface_calls;
        queue_local_park_surface({e.generation, 30, {4, 30, 6}, {0, -1, 0}, 0});
        update_park_editor();
        require(e.probe && fixture::surface_calls == queries_before_ack && e.transaction,
                "Preview collision was queried before its native identity was acknowledged");
        park_editor_observe(preview_entity, fixture::world.at(preview_entity));
        e.picking_api = fixture::picking_api;
        e.picking_api.buildkit_query = fixture::buildkit_query;
        fixture::surface_entity = preview_entity;
        update_park_editor();
        require(!e.probe && e.surface.hit && e.surface.position == editor::Vec3{4, 2, 6},
                "Deferred preview probe did not resume on real ground after creation was acknowledged");
        fixture::surface_entity = 0;
        e.picking_api = {};
        live.position[2] += 12;
        sample(EditorPreviewAction::update);
        update_park_editor();
        update_park_editor_moves();
        require(fixture::world.rbegin()->second.position == live.position &&
                    fixture::creates == create_count + 1,
                "Pending create lost its ACK pose, failed to follow the cursor or created duplicates");
        check_drag_ground(fixture::world.rbegin()->first);
        sample(EditorPreviewAction::commit);
        finish();
        require(r.document.maps[r.map].size() == 2 && fixture::saves == cancel_saved + 1,
                "Asset drop did not save exactly once");
        const auto group_before = r.document.maps[r.map];
        live.token++;
        live.revision = e.revision;
        live.object = group_before[0].id;
        live.transforms.clear();
        for (const auto &object : group_before)
            live.transforms.push_back({object.id, editor::add(object.position, {2, 3, 4}), object.rotation});
        auto invalid_group = live;
        invalid_group.action = EditorPreviewAction::begin_move;
        invalid_group.transforms[1].id = UINT64_MAX;
        require(!queue_local_park_preview(invalid_group) && !e.transaction,
                "Invalid group partially began a native edit");
        sample(EditorPreviewAction::begin_move);
        update_park_editor_moves();
        for (const auto &[id, object] : fixture::world) {
            (void)id;
            require(object.position == editor::add(find_object(group_before, object.id)->position, {2, 3, 4}),
                    "Group drag failed to move every original entity");
        }
        require(r.document.maps[r.map] == group_before, "Group preview saved before release");
        sample(EditorPreviewAction::commit);
        finish();
        run("undo");
        finish();
        require(r.document.maps[r.map] == group_before, "Group move did not undo together");
        live.token++;
        live.revision = e.revision;
        sample(EditorPreviewAction::begin_move);
        update_park_editor_moves();
        sample(EditorPreviewAction::cancel);
        finish();
        require(r.document.maps[r.map] == group_before, "Group cancel altered the document");
        for (const auto &[id, object] : fixture::world) {
            (void)id;
            require(object == *find_object(group_before, object.id), "Group cancel left a moved body behind");
        }
        live.transforms.clear();
        live.object = 0;
        for (const int cancel_stage : {0, 1, 2}) {
            live.token++;
            live.revision = e.revision;
            const auto saves_before_cancel = fixture::saves;
            sample(EditorPreviewAction::begin_place);
            if (cancel_stage > 0)
                update_park_editor();
            if (cancel_stage > 1) {
                update_park_editor();
                update_park_editor_moves();
            }
            sample(EditorPreviewAction::cancel);
            finish();
            require(fixture::world.size() == 2 && fixture::saves == saves_before_cancel &&
                        r.document.maps[r.map] == group_before,
                    "Cancelling a dragged asset leaked an object or saved its temporary placement");
        }
        // Revoking editor access cancels both queued and already spawned drags.
        for (const int stage : {0, 1, 2}) {
            ++live.token;
            live.revision = e.revision;
            sample(EditorPreviewAction::begin_place);
            for (int i = 0; i < stage; ++i) update_park_editor();
            const auto saved = fixture::saves;
            set_lobby_object_placement_allowed(false);
            require(!lobby_object_placement_allowed() && !idle() && !local_park_editor().available,
                    "Disabled editor remained available");
            require(!queue_local_park_surface({e.generation, 500, {4, 30, 6}, {0, -1, 0}, 1}),
                    "Disabled editor queued a new surface query");
            require(!queue_local_park_selection({e.generation, {group_before[0].id}}),
                    "Disabled editor accepted selection");
            finish();
            require(fixture::world.size() == 2 && r.document.maps[r.map] == group_before && fixture::saves == saved,
                    "Revoking editor access leaked or saved a placement preview");
            auto blocked = live;
            blocked.action = EditorPreviewAction::begin_place;
            blocked.revision = e.revision;
            blocked.token++;
            require(!queue_local_park_preview(blocked), "Disabled editor allowed a new placement");
            set_lobby_object_placement_allowed(true);
            require(idle(), "Editor did not recover after host enabled it");
        }
        ++live.token;
        live.revision = e.revision;
        live.object = group_before[0].id;
        live.position = editor::add(group_before[0].position, {3, 2, 4});
        live.rotation = group_before[0].rotation;
        const auto before_revoked_move = fixture::world;
        const auto move_saves = fixture::saves;
        sample(EditorPreviewAction::begin_move);
        update_park_editor_moves();
        require(fixture::world != before_revoked_move, "Revoked move fixture never moved the native object");
        set_lobby_object_placement_allowed(false);
        finish();
        require(fixture::world == before_revoked_move && r.document.maps[r.map] == group_before &&
                    fixture::saves == move_saves, "Disabling the editor committed an active move");
        set_lobby_object_placement_allowed(true);
        const auto paste_saves = fixture::saves;
        const auto paste_undo = e.undo.size();
        const auto paste_world = fixture::world;
        EditorPasteRequest paste{e.generation, e.revision};
        for (const auto &object : group_before)
            paste.objects.push_back(
                {object.id, object.item, editor::add(object.position, {1, 0, 1}), object.rotation, true});
        paste.objects[1].scale = 2.5f; // A scaled copy, as a saved set or Ctrl+V of a resized object
        set_lobby_object_placement_allowed(false);
        require(!queue_local_park_paste(paste) && !e.transaction,
                "Disabled editor accepted a clipboard paste");
        set_lobby_object_placement_allowed(true);
        auto invalid_paste = paste;
        invalid_paste.objects[1].item = "own_bk_missing";
        const auto paste_next_id = r.next_id;
        require(!queue_local_park_paste(invalid_paste) && !e.transaction && r.next_id == paste_next_id,
                "Invalid pasted group partially began a transaction or consumed object IDs");
        invalid_paste = paste;
        invalid_paste.generation--;
        require(!queue_local_park_paste(invalid_paste), "Stale clipboard command crossed levels");
        invalid_paste = paste;
        invalid_paste.objects[0].position[1] = std::numeric_limits<float>::infinity();
        require(!queue_local_park_paste(invalid_paste), "Nonfinite pasted pose reached native placement");
        require(queue_local_park_paste(paste) && r.document.maps[r.map] == group_before,
                "Paste rejected a valid group or saved before native creation");
        require(!queue_local_park_paste(paste), "Busy paste accepted a duplicate command");
        finish();
        require(fixture::world.size() == paste_world.size() + 2 && fixture::saves == paste_saves + 1 &&
                    e.undo.size() == paste_undo + 1,
                "Group paste failed to create fresh objects with one save and undo step");
        for (const auto &[id, object] : paste_world)
            require(fixture::world.at(id) == object, "Paste moved or replaced an original object");
        for (const auto &copy : paste.objects)
            require(std::any_of(r.document.maps[r.map].begin(), r.document.maps[r.map].end(),
                                [&](const auto &object) {
                                    return object.id >= paste_next_id && object.item == copy.item &&
                                           object.position == copy.position &&
                                           object.rotation == copy.rotation && object.scale == copy.scale;
                                }),
                    "Paste lost a copied object's pose or reused its persistent ID");
        for (const auto &object : r.document.maps[r.map])
            require(std::any_of(r.rows.begin(), r.rows.end(),
                                [&](const auto &row) { return row.saved_id == object.id && row.spawned; }),
                    "A pasted object lost its world link while the rest of the paste was created");
        for (const auto &copy : paste.objects)
            require(std::any_of(fixture::world.begin(), fixture::world.end(),
                                [&](const auto &entry) {
                                    return !paste_world.contains(entry.first) && entry.second.item == copy.item &&
                                           entry.second.scale == copy.scale;
                                }),
                    "A pasted object spawned in the world without its scale");
        {
            // The pasted group moves and turns together right away (the gizmo drag on a placed set).
            const auto pasted_layout = r.document.maps[r.map];
            const editor::Quat turn{0, .7071068f, 0, .7071068f};
            live.token++;
            live.revision = e.revision;
            live.transforms.clear();
            for (const auto &object : pasted_layout)
                if (object.id >= paste_next_id) {
                    live.object = object.id;
                    live.transforms.push_back({object.id, editor::add(object.position, {3, 0, -2}),
                                               editor::multiplied(turn, object.rotation), object.scale});
                }
            require(live.transforms.size() == 2, "The pasted group is not two objects");
            sample(EditorPreviewAction::begin_move);
            update_park_editor_moves();
            sample(EditorPreviewAction::commit);
            finish();
            for (const auto &moved : live.transforms) {
                const auto *saved = find_object(r.document.maps[r.map], moved.id);
                require(saved && saved->position == moved.position && saved->rotation == moved.rotation &&
                            saved->scale == moved.scale,
                        "Moving the pasted group did not save every object's new pose");
                require(std::any_of(fixture::world.begin(), fixture::world.end(),
                                    [&](const auto &entry) {
                                        return entry.second.id == moved.id && entry.second.position == moved.position &&
                                               entry.second.rotation == moved.rotation;
                                    }),
                        "Moving the pasted group left an object behind in the world");
            }
            run("undo");
            finish();
            require(r.document.maps[r.map] == pasted_layout, "Undo did not put the moved group back");
            live.transforms.clear();
            live.object = 0;
        }
        {
            // A session with object scaling off: the scaled copy is made at 1x in the world without
            // waiting on a resize the server would not take, and the layout keeps its scale.
            set_lobby_object_scaling_allowed(false);
            const auto world_before = fixture::world;
            EditorPasteRequest scaled{e.generation, e.revision};
            scaled.objects.push_back(paste.objects[1]);
            scaled.objects[0].position[0] += 10;
            require(queue_local_park_paste(scaled), "A paste with object scaling off was refused");
            finish();
            require(!e.transaction, "A scaled paste with object scaling off did not finish");
            require(std::any_of(fixture::world.begin(), fixture::world.end(),
                                [&](const auto &entry) {
                                    return !world_before.contains(entry.first) && entry.second.item == scaled.objects[0].item &&
                                           entry.second.scale == 1.0f;
                                }),
                    "With object scaling off a pasted object was resized in the world");
            require(std::any_of(r.document.maps[r.map].begin(), r.document.maps[r.map].end(),
                                [&](const auto &object) {
                                    return object.position == scaled.objects[0].position && object.scale == scaled.objects[0].scale;
                                }),
                    "With object scaling off a pasted object lost its scale in the layout");
            run("undo");
            finish();
            set_lobby_object_scaling_allowed(true);
        }
        {
            // Deleting a multi-selection: every object leaves the world in one edit and one undo
            // step, and undo brings them all back at their scale.
            std::vector<std::uint64_t> pasted_ids;
            for (const auto &object : r.document.maps[r.map])
                if (object.id >= paste_next_id)
                    pasted_ids.push_back(object.id);
            const auto pasted_layout = r.document.maps[r.map];
            const auto pasted_live = fixture::world;
            const auto delete_undo = e.undo.size();
            EditorDeleteRequest erase{e.generation, e.revision, pasted_ids};
            auto stale_erase = erase;
            stale_erase.objects.push_back(999999);
            require(pasted_ids.size() == 2 && !queue_local_park_delete(stale_erase) && !e.transaction,
                    "Deleting a selection with a missing object began an edit");
            stale_erase = erase;
            --stale_erase.revision;
            require(!queue_local_park_delete(stale_erase), "A stale_erase selection was deleted");
            require(queue_local_park_delete(erase) && !queue_local_park_delete(erase),
                    "Multi-delete was refused, or accepted twice while busy");
            finish();
            require(fixture::world == paste_world && r.document.maps[r.map] == group_before &&
                        e.undo.size() == delete_undo + 1,
                    "Multi-delete left objects in the world or took more than one undo step");
            run("undo");
            finish();
            require(r.document.maps[r.map] == pasted_layout && fixture::world.size() == pasted_live.size() &&
                        std::any_of(fixture::world.begin(), fixture::world.end(),
                                    [](const auto &entry) { return entry.second.scale == 2.5f; }),
                    "Undoing a multi-delete did not bring every object back at its scale");
        }
        run("undo");
        finish();
        require(fixture::world == paste_world && r.document.maps[r.map] == group_before,
                "One undo did not remove the complete pasted group");
        run("delete", group_before[1]);
        finish();
        require(r.document.maps[r.map] == before, "Live tests did not restore the original layout");
        fixture::invalid_native_position = true;
        ramp = before[0];
        ramp.position[0] += 2;
        const auto previous_moves = fixture::moves, previous_body_moves = fixture::body_moves;
        const auto previous_saves = fixture::saves;
        run("move", ramp);
        update_park_editor_moves();
        require(e.failed && !e.transaction && fixture::moves == previous_moves &&
                    fixture::body_moves == previous_body_moves && fixture::saves == previous_saves &&
                    r.document.maps[r.map] == before &&
                    e.status.starts_with("Selected object's native transform is unavailable."),
                "Invalid XYZ reached native movement, changed the saved layout or lost its specific error");
        reset_park_editor();
        fixture::invalid_native_position = false;
        fixture::acknowledge = false;
        ramp = before[0];
        ramp.position[0] += 2;
        run("move", ramp);
        update_park_editor_moves();
        const auto sent = fixture::moves, saved = fixture::saves;
        e.transaction->sent_at = GetTickCount64() - 11000;
        update_park_editor();
        update_park_editor();
        require(e.failed && fixture::moves == sent && fixture::saves == saved &&
                    r.document.maps[r.map] == before,
                "Timeout retried or overwrote the last committed layout");
        require(!park_editor_owns_placements(),
                "A failed editor permanently captured the regular placement updater");
        require(run("delete", ramp).starts_with("error"), "Uncertain edit allowed another mutation");
        const auto generation = e.generation;
        reset_park_editor();
        require(e.generation != generation && !e.failed && e.undo.empty() && e.redo.empty() &&
                    e.native_drops.empty() && e.client_roots.empty(),
                "World reset retained old transaction history");
        fixture::acknowledge = true;
        fixture::duplicate_on_move = true;
        ramp = r.document.maps[r.map][0];
        ramp.position[0] += 3;
        const auto committed = r.document.maps[r.map];
        run("move", ramp);
        update_park_editor_moves();
        update_park_editor_moves();
        require(e.failed && !e.transaction && r.document.maps[r.map] == committed,
                "An unexpected duplicate was accepted as a successful move");
        reset_park_editor();
        fixture::duplicate_on_move = false;
        const auto local_document = r.document;
        const auto local_world = fixture::world;
        const auto local_saves = fixture::saves, remote_creates = fixture::creates;
        std::vector<NetworkObjectOwner> owners{{1001, 1, {{1, "own_bk_ramp", {30, 2, 40}, {0, 0, 0, 1}, 1.75f}}},
                                               {1002, 1, {{1, "own_bk_ramp", {35, 2, 40}, {0, 0, 0, 1}}}}};
        const auto tick_remote = [&] {
            network_objects_detail::state().next_poll = 0;
            update_network_objects();
            update_network_object_moves();
        };
        set_remote_network_objects(r.map, owners);
        require(network_object_extra_budget() == 2,
                "Remote copies did not reserve additional native capacity");
        for (unsigned i = 0; i < 4; ++i)
            tick_remote();
        auto &remote = network_objects_detail::state();
        require(remote.live.size() == 2 && fixture::world.size() == local_world.size() + 2 &&
                    fixture::creates == remote_creates + 2,
                "Remote owners with matching IDs collided or duplicated");
        const auto remote_entity = remote.live.at({1001, 1, 1}).entity;
        owners[0].objects[0].position[0] = 44;
        owners[0].objects[0].rotation = {0, 1, 0, 0};
        owners[0].objects[0].scale = 2.25f;
        set_remote_network_objects(r.map, owners);
        tick_remote();
        require(remote.live.at({1001, 1, 1}).entity == remote_entity &&
                    fixture::world.at(remote_entity).position[0] == 44 &&
                    fixture::world.at(remote_entity).rotation[1] == 1 &&
                    fixture::world.at(remote_entity).scale == 2.25f &&
                    fixture::creates == remote_creates + 2,
                "Remote movement recreated an object or lost rotation/scale");
        require(network_object_observe(remote_entity, fixture::world.at(remote_entity)),
                "Remote movement was not excluded from autosave observation");
        const auto exported = capture_local_network_objects();
        require(exported && exported->objects.size() == r.document.maps[r.map].size() &&
                    r.document == local_document && fixture::saves == local_saves,
                "Remote objects leaked into local capture or saved settings");
        owners.erase(owners.begin());
        remove_remote_network_objects(1001, 1);
        tick_remote();
        tick_remote();
        require(!fixture::world.contains(remote_entity) && remote.live.size() == 1,
                "A departing owner's objects were not removed");
        clear_remote_network_objects();
        tick_remote();
        tick_remote();
        require(network_object_extra_budget() == 0, "Disconnect did not restore the local object budget");
        require(remote.live.empty() && fixture::world == local_world && r.document == local_document &&
                    fixture::saves == local_saves,
                "Disconnect changed local objects or left remote copies behind");
        set_remote_network_objects(r.map, owners);
        fixture::acknowledge = false;
        tick_remote();
        require(network_objects_inflight() && !idle(),
                "A pending remote spawn allowed a conflicting local transaction");
        clear_remote_network_objects();
        const auto late_object = remote.creating->object;
        const auto late_source = remote.creating->source;
        const auto late_entity = fixture::next_entity++;
        fixture::world[late_entity] = late_object;
        require(network_object_creation_owner(late_source) == 1002 &&
                    network_object_observe(late_entity, late_object, late_source),
                "Late remote creation lost its owner after disconnect");
        fixture::acknowledge = true;
        tick_remote();
        tick_remote();
        require(!fixture::world.contains(late_entity) && fixture::saves == local_saves,
                "Late remote spawn survived disconnect or was saved");
        reset_network_object_world();
        require(!network_object_creation_owner(late_source),
                "World teardown accepted a stale remote creation tag");
        require(remote.live.empty() && remote.desired.empty() && !remote.creating,
                "World reset retained remote entity handles");
        set_remote_network_objects(r.map, owners);
        fixture::acknowledge = false;
        tick_remote();
        require(remote.creating.has_value(), "Timeout fixture did not begin its network create");
        const auto timed_out_source = remote.creating->source;
        remote.creating->sent = GetTickCount64() - 11000;
        tick_remote();
        require(!network_objects_inflight() && idle() &&
                    network_object_creation_owner(timed_out_source) == 1002,
                "Timed-out network creation blocked local editing or lost late-callback ownership");
        fixture::acknowledge = true;
        reset_network_object_world();
        require(!network_object_creation_owner(timed_out_source),
                "World reset retained timed-out network ownership");
        owners[0].objects[0].item = "own_bk_uninstalled";
        const auto creates_before_missing = fixture::creates;
        set_remote_network_objects(r.map, owners);
        tick_remote();
        require(fixture::creates == creates_before_missing && !remote.creating &&
                    remote.status.find("not installed") != std::string::npos,
                "An uninstalled peer asset reached native creation or lost its status");
        reset_network_object_world();

        // Joining an already loaded map must hide personal saves, preserve the
        // host's copies, and discard guest edits on every leave/rejoin cycle.
        reset_park_editor();
        fixture::world.clear();
        r.rows.clear(); r.tracked.clear(); r.restore.clear(); r.inflight.reset();
        r.document = {};
        profile::PlacedObject personal{1, "own_bk_ramp", {101, 2, 103}, {0, 0, 0, 1}};
        r.document.maps[r.map] = {personal};
        r.document.maps["other-map"] = {personal};
        const auto personal_document = r.document;
        const auto personal_saves = fixture::saves;
        const auto restore_personal_fixture = [&] {
            const auto entity = fixture::next_entity++;
            fixture::world[entity] = personal;
            r.tracked[entity] = personal.id;
            observe_placement(entity, personal);
            r.restore.clear();
            return entity;
        };
        for (unsigned visit = 0; visit < 3; ++visit) {
            const auto personal_entity = restore_personal_fixture();
            const auto previous_scope = r.creation.source();
            r.inflight = personal; // A personal restore was already queued.
            set_lobby_object_guest(true);
            require(r.personal_document == personal_document && r.document.maps[r.map].empty() &&
                        r.rows.empty() && r.restore.empty() && !r.inflight &&
                        !r.creation.accepts(previous_scope) && network_objects_inflight(),
                    "Join retained personal saves/queued restores or lost deletion ownership");
            const auto joined = capture_local_network_objects();
            require(joined && joined->objects.empty(), "Guest broadcast its personal saved layout");
            require(network_object_observe(personal_entity, personal),
                    "Personal cleanup acknowledgement could reenter guest autosave");
            set_lobby_object_guest(true); // Repeated policy updates must be harmless.
            require(r.personal_document == personal_document, "Repeated join lost the personal save");
            tick_remote(); tick_remote();
            require(fixture::world.empty() && !network_objects_inflight(),
                    "Guest personal copies survived entry into the host's world");

            owners = {{1001, visit + 1ULL, {{1, "own_bk_ramp", {30, 2, 40}, {0, 0, 0, 1}}}}};
            set_remote_network_objects(r.map, owners);
            tick_remote(); tick_remote();
            require(fixture::world.size() == 1 && capture_local_network_objects()->objects.empty(),
                    "Host saved objects were missing or rebroadcast as guest-owned objects");
            require(!run("place", ramp).starts_with("error"), "Guest could not place a temporary object");
            finish();
            require(r.document.maps[r.map].size() == 1 && capture_local_network_objects()->objects.size() == 1 &&
                        fixture::saves == personal_saves && r.personal_document == personal_document,
                    "Guest edits failed to sync or overwrote personal saves");

            // Leave while another create has produced an entity but has not
            // committed. It is absent from the layout and must still be removed.
            require(!run("place", ramp).starts_with("error"), "Pending guest create was rejected");
            update_park_editor();
            require(e.transaction && e.transaction->steps.front().entity,
                    "Leave fixture did not have an uncommitted native entity");
            const auto guest_scope = r.creation.source();
            clear_remote_network_objects();
            set_lobby_object_guest(false);
            require(!r.personal_document && r.document == personal_document && !e.transaction &&
                        !r.creation.accepts(guest_scope) && r.restore == std::vector<std::uint64_t>{1},
                    "Leave retained session objects/history or failed to restore the personal layout");
            tick_remote(); tick_remote();
            require(fixture::world.empty() && remote.live.empty() && !network_objects_inflight() &&
                        fixture::saves == personal_saves,
                    "Pending personal restores blocked cleanup or a session object followed the guest");
            set_lobby_object_guest(false);
            require(r.restore.size() == 1, "Repeated leave duplicated personal restore requests");
        }
        std::cout
            << "Park editor native-adapter transaction, history, stale-selection and timeout tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
