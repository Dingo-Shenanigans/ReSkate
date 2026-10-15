#pragma once
#include "car_grab/controller.h"
#include "traffic_vehicle_surface.h"
#include <cstdint>
#include <vector>

namespace dingosdk::car_grab_native {
struct TrafficSnapshot;
// Atomic-only lifetime check for the evaluated visual reach. No native reads.
bool world_current(std::uint64_t epoch) noexcept;
// Value-only cancellation check for a private suspended target. This grants
// neither write nor hand authority; actuation still requires current() and a
// fresh client lease. The caller bounds the hold and verifies local controls.
// In-flight opt-in is only for a classified private no-write wait, with a
// complete selected-free journal and unchanged active count at both ends.
bool retained_identity_current(const TrafficSnapshot& sample, std::uint64_t selected,
                               bool allow_inflight_removal = false) noexcept;
// Value-only surface lease for animation; no engine APIs or native reads.
bool grip_current(std::uint64_t epoch, std::uint64_t vehicle, std::uint64_t surface) noexcept;
enum class TrafficStatus { unavailable, incomplete, warming, ready };
struct VehicleIdentity {
    std::uint64_t value{}, context{}, record{};
    // Motion warmup cannot erase the latest exact synthetic incarnation.
    std::uint64_t generation{};
};
struct TrafficScope {
    std::uintptr_t base{}, client{}, client_context{}, server{}, server_context{};
    std::uintptr_t park{}, park_reference{}, park_context{}, root{}, root_data{}, root_owner{}, traffic{};
    std::uint64_t root_generation{};
    int map{};
    bool operator==(const TrafficScope&) const = default;
};
enum class SkaterCaptureStatus { unavailable, invalid_position, ready };
struct NearbyCandidateDiagnostic {
    bool valid{};
    std::uint64_t id{};
    car_grab::Vec3 position{};
    float horizontal_distance{}, vertical_distance{};
};
struct TrafficDiagnostics {
    std::uint32_t observed{}, posed{}, sampled{}, nearby{}, surfaces{}, grips{}, probes{};
    std::uint32_t ray_calls{}, closest_hits{}, owner_rejects{};
    std::uint32_t no_world{}, no_owned_rear{}, neighborhood{}, footprint{};
    bool skater_valid{};
    SkaterCaptureStatus skater_status{SkaterCaptureStatus::unavailable};
    car_grab::Vec3 skater_position{};
    // Nearest by the same 3D distance used for the existing nearby filter.
    // Distances are horizontal XZ and absolute vertical Y, in world metres.
    // No candidate distance is valid without a verified skater position.
    NearbyCandidateDiagnostic nearest_posed, nearest_sampled;
    std::uint64_t selected_attempt{};
    // Report from the highest-priority attempted candidate: active car first,
    // then nearest. At most one discovery probe runs per capture.
    SurfaceProbeReport probe;
    // Copied scheduling evidence; these times never refresh the pose lease.
    double core_publish_age_seconds{}, discovery_seconds{};
    std::uint32_t discovery_deferred{};
    bool discovery_late{};
    // Separate retained attempt evidence. These are not this frame's probes,
    // and neither timestamp grants pose, contact or write authority.
    SurfaceProbeReport last_probe;
    std::uint64_t last_probe_vehicle{};
    double last_probe_at{}, last_probe_age_seconds{};
};
struct TrafficSnapshot {
    TrafficStatus status{TrafficStatus::unavailable};
    std::uint64_t epoch{}, removals{};
    double sampled_at{};
    TrafficScope scope;
    TrafficDiagnostics diagnostic;
    std::vector<car_grab::Vehicle> vehicles;
    std::vector<VehicleIdentity> identities;
};
void start(std::uintptr_t base) noexcept;
// Does not wait for native_mutex or call engine code; safe before destruction.
void invalidate() noexcept;
void invalidate_root(std::uintptr_t entity) noexcept;
void invalidate_population(std::uintptr_t manager) noexcept;
void publish_client(std::uintptr_t base, std::uintptr_t client,
                    std::uintptr_t context, bool ready) noexcept;
// Called after native park update while upstream native_mutex is already held.
void capture(std::uintptr_t base, std::uintptr_t park) noexcept;
TrafficSnapshot snapshot();
enum class CurrentFailure {
    none, initial_lease, scope_unavailable, scope_changed, root_unavailable,
    selected_unavailable, incarnation_changed, entity_changed, traffic_unavailable,
    traffic_missing, removal, read_changed, final_lease, unexpected
};
enum class CurrentLeaseFailure { none, status, age, epoch, published_epoch, active_removal };
struct CurrentReport {
    CurrentFailure failure{CurrentFailure::none};
    bool initial_age_valid{}, final_age_valid{};
    double initial_age{}, final_age{};
    CurrentLeaseFailure initial_failure{CurrentLeaseFailure::none};
    TrafficStatus initial_status{TrafficStatus::unavailable};
    bool initial_epoch_current{}, initial_publication_current{}, initial_removal_free{};
    // Classification only: current() still returns false unless its original
    // 100ms motion/write lease passes. All real selected/world/removal guards
    // must pass before a deadline or motion warmup can set this value.
    bool identity_verified{}, motion_warming{};
};
// Exact traffic identity/world/incarnation/freshness/removal lease only. This
// never certifies collision contact: measured grip and cosmetic reach callers
// must additionally require grip_current() with their exact surface serial.
// Only guarded reads/atomics; never engine API calls or native_mutex in physics.
// Optional diagnostics copy the existing checked ages and failure boundary;
// collecting them never adds native queries or extends the write lease.
bool current(const TrafficSnapshot& sample, std::uint64_t selected = 0,
             CurrentReport* report = nullptr) noexcept;
}
