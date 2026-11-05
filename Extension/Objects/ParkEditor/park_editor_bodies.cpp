#include "park_editor_bodies.h"
#include "park_editor_internal.h"

namespace dingosdk::profile_runtime {
using park_editor_detail::editor_state;

bool park_bodies_ready() noexcept {
    const auto& e = editor_state();
    return e.native_ready && e.picking_api.ready && e.physics_query && e.physics_move && e.transform_move;
}
std::uintptr_t placed_body_handle(const std::uint64_t* root) {
    auto& e = editor_state();
    alignas(16) std::array<std::uint64_t, 5> component{};
    if (!root || !e.physics_query || e.physics_query(root, component.data()) != component.data() || !component[2])
        return 0;
    return component[3];
}
void move_placed_body(const void* body, const float* rigid) { editor_state().physics_move(body, rigid); }
void set_placed_transform(const void* query, const float* pose, std::uintptr_t source) {
    editor_state().transform_move(query, pose, source);
}
std::optional<editor::BuildKitIdentity> placed_identity(std::uint64_t root) {
    return editor::buildkit_identity(editor_state().picking_api, root);
}
bool park_handle_valid(const void* handle) {
    const auto& api = editor_state().picking_api;
    return api.ready && api.body_valid && api.body_valid(handle);
}
std::uint64_t surface_hit_entity(std::uintptr_t world, std::uint32_t index, std::uintptr_t data) {
    const auto& api = editor_state().picking_api;
    alignas(16) editor::SurfaceBody body{};
    alignas(16) std::array<std::uint64_t, 2> entity{};
    if (!api.ready || !api.body_entity || !park_editor_detail::surface_body(world, index, data, body))
        return 0;
    api.body_entity(entity.data(), body.data());
    return entity[0];
}
std::optional<std::array<float, 3>> cast_placed_surface(std::uintptr_t context, const std::array<float, 3>& from,
    const std::array<float, 3>& to, editor::SurfaceOwner owner, std::uint64_t* entity) {
    auto& e = editor_state();
    if (!context || !e.surface_api.ready)
        return std::nullopt;
    const auto world = e.surface_api.world(context);
    if (!world)
        return std::nullopt;
    return editor::cast_surface(e.surface_api, world, from, to, owner, {}, entity);
}
} // namespace dingosdk::profile_runtime
