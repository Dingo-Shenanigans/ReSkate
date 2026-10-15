#pragma once
#include "car_grab/controller.h"
#include <array>
#include <cstdint>

namespace dingosdk::car_grab_native {
// The SDK picking path returns the hit collision handle. Keep its complete
// world/index/generation, never an index alone. Native code owns the storage.
struct PhysicsBodyToken {
    std::uintptr_t world{};
    std::uint32_t index{}, generation{};
    bool operator==(const PhysicsBodyToken&) const = default;
};
static_assert(sizeof(PhysicsBodyToken) == 16);
enum class SurfaceProbeStage {
    none, api_unavailable, invalid_input, no_world, no_owned_rear,
    neighborhood, footprint, ready_face, ready_lip
};
enum class SurfaceOwnerStage {
    none, invalid_selected, invalid_body, missing_owner, invalid_raw,
    direct, mapped, parent_mapped, query_unavailable, no_parent,
    invalid_parent, cycle, depth_limit, changed, unmapped,
    traffic_peer, parent_traffic_peer, parent_direct
};
enum class SurfaceMappingStatus { none, api_unavailable, table_unavailable, lookup_miss, mapped };
enum class SurfacePeerStatus { none, api_unavailable, not_found, invalid_result, mismatch, paired, changed };
// Copied probe evidence only. Owner values come from the existing ownership
// checks; diagnostics never introduce extra native queries or relax ownership.
struct SurfaceProbeReport {
    SurfaceProbeStage stage{SurfaceProbeStage::none};
    std::uint32_t ray_calls{}, closest_hits{}, owner_rejects{};
    std::uint64_t raw_owner{}, mapped_owner{};
    bool raw_valid{};
    SurfaceOwnerStage owner_stage{SurfaceOwnerStage::none};
    SurfaceMappingStatus mapping_status{SurfaceMappingStatus::none};
    std::uint32_t ancestor_depth{};
    std::uint64_t ancestor_owner{}, ancestor_mapped{};
    SurfacePeerStatus peer_status{SurfacePeerStatus::none};
    std::uint64_t peer_owner{}, peer_selected{};
    std::uint64_t peer_ancestor_owner{};
    std::uint32_t peer_ancestor_depth{};
    bool self_available{}, self_changed{};
    std::uint64_t self_entity{};
    std::uint32_t self_excluded{};
};
struct NativeGripSurface {
    bool valid{};
    car_grab::Vec3 point{}, normal{}, tangent{};
    car_grab::Vec3 localpoint{}, localnormal{}, localtangent{};
    PhysicsBodyToken body;
    // A wrap is enabled only by measured top AND underside intersections of a
    // thin lip. A flat rear collision plane does not establish these fields.
    bool wrap_valid{};
    car_grab::Vec3 top_point{}, bottom_point{}, top_normal{}, bottom_normal{};
    car_grab::Vec3 localtop_point{}, localbottom_point{}, localtop_normal{}, localbottom_normal{};
};

// Initialization only checks the pinned SDK byte contracts; it calls no game API.
void surface_start(std::uintptr_t base) noexcept;
// Native physics queries are confined to the already guarded park tick phase.
// world_forward comes from this fresh vehicle's verified native pose axis,
// with observed motion as the fallback; it is never a guessed bumper point.
// The client context owns the collision world; selected remains the exact
// authoritative server traffic handle. Raw owners must equal/map to it, carry
// the current reciprocal native traffic peer, or have a revalidated parent
// that establishes either exact relationship to that same full handle.
// rider_position is the verified local transform, not a guessed bumper point.
// Acquisition rays start at that approach position at bounded search heights;
// collision between it and the rear contact remains a strict occlusion. The
// optional skater_entity is the already verified local legacy skater identity.
// Only collision shapes proved to belong to its current attached ECS root can
// be excluded; unknown owners and other cars remain occluders.
NativeGripSurface probe_surface(std::uintptr_t client_context, std::uint64_t selected,
    const std::array<float, 12>& pose, car_grab::Vec3 world_forward,
    car_grab::Vec3 rider_position, SurfaceProbeReport* report = nullptr,
    std::uintptr_t skater_entity = 0) noexcept;
// Revalidate the copied collision handle and exact owning entity. Park phase only.
bool surface_current(std::uintptr_t client_context, std::uint64_t selected,
    const NativeGripSurface&) noexcept;
} // namespace dingosdk::car_grab_native
