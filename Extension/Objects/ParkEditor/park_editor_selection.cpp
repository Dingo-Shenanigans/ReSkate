#include "park_editor_internal.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/buildkit_limits.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Objects/local_placements_runtime.h"

namespace dingosdk::profile_runtime::park_editor_detail {
bool surface_body(std::uintptr_t world, std::uint32_t index, std::uintptr_t data, editor::SurfaceBody &body) {
    const auto &api = editor_state().picking_api;
    if (!api.ready)
        return false;
    alignas(16) const std::array<std::uint64_t, 3> hit{world, index, data};
    api.hit_body(hit.data(), body.data());
    return body[0] == world && api.body_valid(body.data());
}
std::uint64_t surface_owner(std::uintptr_t world, std::uint32_t index, std::uintptr_t data) {
    auto &e = editor_state();
    const auto &api = e.picking_api;
    alignas(16) editor::SurfaceBody body{};
    alignas(16) std::array<std::uint64_t, 2> entity{}, root{};
    if (!surface_body(world, index, data, body))
        return 0;
    api.body_entity(entity.data(), body.data());
    if (!entity[0])
        return 0;
    auto &r = placements_runtime();
    r.resolve(root.data(), entity[0], false);
    if (!r.valid(root.data()))
        return 0;
    if (std::any_of(r.rows.begin(), r.rows.end(), [&](const auto &row) {
            return row.spawned && row.entity == root[0];
        }))
        return root[0];
    // Persisted Build Kit replicas do not populate the general entity mapping
    // tables in a local session. Their native drop ID and replicated instance
    // key match the authoritative object even though their ECS IDs differ.
    if (const auto client = editor::buildkit_identity(api, root[0])) {
        if (const auto it = e.native_drops.find(client->drop_id); it != e.native_drops.end()) {
            alignas(16) std::array<std::uint64_t, 2> server{};
            r.resolve(server.data(), it->second, false);
            if (server[0] == it->second && r.valid(server.data())) {
                const auto identity = editor::buildkit_identity(api, server[0]);
                if (identity && *identity == *client) {
                    e.client_roots[server[0]] = root[0];
                    return server[0];
                }
            }
        }
        return 0; // A recycled drop ID must not bind a stale visible replica.
    }
    return editor::mapped_entity(api, root[0], 1);
}
std::uint64_t client_root(std::uint64_t server) {
    auto &e = editor_state();
    auto &r = placements_runtime();
    if (const auto it = e.client_roots.find(server); it != e.client_roots.end()) {
        alignas(16) std::array<std::uint64_t, 2> root{};
        r.resolve(root.data(), it->second, false);
        if (root[0] == it->second && r.valid(root.data())) {
            const auto client = editor::buildkit_identity(e.picking_api, root[0]);
            const auto authoritative = editor::buildkit_identity(e.picking_api, server);
            if (client && authoritative && *client == *authoritative)
                return root[0];
        }
        e.client_roots.erase(it);
    }
    return editor::mapped_entity(e.picking_api, server, 0);
}
void update_highlights() {
    auto &e = editor_state();
    auto &r = placements_runtime();
    if (!e.highlight_api.ready || !e.picking_api.ready)
        return;
    std::set<std::uint64_t> desired;
    if (GetTickCount64() < e.selection_time + 2000) {
        std::vector<std::uint64_t> servers;
        for (const auto &row : r.rows)
            if (row.spawned &&
                std::find(e.selection.begin(), e.selection.end(), row.saved_id) != e.selection.end())
                servers.push_back(row.entity);
        if (e.transaction && e.transaction->preview && e.transaction->preview->placing &&
            !e.transaction->preview->cancelled && !e.transaction->steps.empty() &&
            e.transaction->steps.front().entity)
            servers.push_back(e.transaction->steps.front().entity);
        for (const auto server : servers) {
            const auto client = client_root(server);
            if (client)
                desired.insert(client);
        }
    }
    // Resolve each entity afresh. Never retain a component pointer across ticks.
    for (auto it = e.highlights.begin(); it != e.highlights.end();) {
        if (desired.contains(it->first)) {
            ++it;
            continue;
        }
        std::array<std::uint64_t, 2> root{};
        r.resolve(root.data(), it->first, false);
        if (root[0] == it->first && r.valid(root.data()))
            editor::highlight_value(e.highlight_api, root.data(), it->second.original, it->second.applied);
        it = e.highlights.erase(it);
    }
    if (desired.empty())
        return;
    std::uintptr_t manager{};
    const auto base = local_runtime().base;
    if (!memory::read(base + addr::engine::settings_manager, manager) || !manager)
        return;
    const auto settings = e.highlight_api.settings(
        manager, reinterpret_cast<const void *>(base + addr::buildkit_limits::object_persistence_settings_type));
    std::int32_t color{};
    if (!settings || !memory::read(settings + 0x88, color))
        return;
    const editor::HighlightValue applied{color, 1}; // BuildKitColorSetOutline.
    for (const auto client : desired) {
        if (e.highlights.contains(client))
            continue;
        std::array<std::uint64_t, 2> root{};
        r.resolve(root.data(), client, false);
        if (root[0] != client || !r.valid(root.data()))
            continue;
        if (const auto original = editor::highlight_value(e.highlight_api, root.data(), applied))
            e.highlights.emplace(client, EditorRuntime::HighlightLease{*original, applied});
    }
}
} // namespace dingosdk::profile_runtime::park_editor_detail
namespace dingosdk {
bool local_surface_hit_body(std::uintptr_t world, std::uint32_t index, std::uintptr_t data, std::array<std::uint64_t, 2> &body) {
    return profile_runtime::park_editor_detail::surface_body(world, index, data, body);
}
} // namespace dingosdk
