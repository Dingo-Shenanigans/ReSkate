#pragma once
#include "park_editor_picking.h"
#include "park_editor_surface.h"
#include <array>
#include <cstdint>
#include <optional>

// What the park editor's native functions offer for placed objects' bodies, for placement physics
// (Extension/Objects/placement_physics.cpp, placement_adapters.cpp) without its internals. Each is
// unavailable (false, 0 or empty) until the park editor's native functions are found.
namespace dingosdk::profile_runtime {
bool park_bodies_ready() noexcept;
// The body handle's address for a placed object's root {entity, flags}, or 0.
std::uintptr_t placed_body_handle(const std::uint64_t* root);
// Moves a placed object's body (rigid: position x, y, z, scale, then rotation x, y, z, w) and sets its
// transform (pose: 12 floats), as the dropper does, telling `source`'s listeners.
void move_placed_body(const void* body, const float* rigid);
void set_placed_transform(const void* query, const float* pose, std::uintptr_t source);
std::optional<editor::BuildKitIdentity> placed_identity(std::uint64_t root);
// Whether a shape or body handle is still alive.
bool park_handle_valid(const void* handle);
// The entity a surface hit's body belongs to, or 0.
std::uint64_t surface_hit_entity(std::uintptr_t world, std::uint32_t index, std::uintptr_t data);
// The closest surface along a ray in the world of a placement context, and the entity `owner` names
// for it (0 for none: that hit is passed).
std::optional<std::array<float, 3>> cast_placed_surface(std::uintptr_t context, const std::array<float, 3>& from,
    const std::array<float, 3>& to, editor::SurfaceOwner owner, std::uint64_t* entity);
} // namespace dingosdk::profile_runtime
