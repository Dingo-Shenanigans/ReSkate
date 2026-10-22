#pragma once
#include "bmx_native_motion.h"
#include "bmx_snapshot_retry.h"
#include "Engine/Game/BMX/bmx_mount_pose.h"
#include "Engine/Game/BMX/bmx_pool_admission.h"
#include "Engine/Game/BMX/bmx_owner_observation.h"
#include <array>
#include <cstdint>

namespace dingosdk::bmx::native_chassis {
using Vec = std::array<float,3>;
using Quat = std::array<float,4>; // Hamilton x,y,z,w, body axes to world
struct Frame {
    bool available{}, active{}, detached{};
    Vec position{}, linear_velocity{}, angular_velocity{}; // measured world COM
    Quat orientation{0,0,0,1};
    float mass{}, step_seconds{};
    Vec inertia{}; // principal moments about COM, body axes
    std::uintptr_t world{}, native_state{};
    std::uint32_t body_index = 0xffffffffU, body_generation{};
    std::uint64_t token{}, serial{};
    double simulation_time{};
    const char* issue = "not_acquired";
    snapshot_retry::Issue snapshot_issue=snapshot_retry::Issue::none;
    unsigned snapshot_reads{};
    bool snapshot_retry_exhausted{};
};
struct Status {
    bool ready{}, active{}, parked{}, detached{}, failed{}, allocation_cleanup_proven{};
    bool blocked{}, observation_suspended{}, deferred_park{}, owner_quarantined{};
    unsigned live_shapes{}, allocation_attempts{}, mounts{};
    std::uint64_t producer_calls{}, wrapper_matches{}, applications{}, duplicate_steps{},
        rejected{}, hop_events{}, last_serial{};
    Vec last_force{}, last_torque{};
    std::uintptr_t world{};
    const char* issue = "idle";
    std::uint64_t snapshot_reads{},snapshot_retries{},snapshot_recoveries{},snapshot_retry_exhaustions{};
    snapshot_retry::Issue last_snapshot_issue=snapshot_retry::Issue::none;
    snapshot_retry::VelocityRejections velocity_rejections; // bounded historical SDK-prequalified rejection evidence
};
// Scalar SDK ownership publication only, never a fresh native physics/body
// validation. A transition/cancellation or contended lock makes it unavailable.
// Lab callbacks must still revalidate canonical owner/clock and match a fresh
// full native body preflight. lease_active reports the exact SDK lease flag;
// wall-clock expiry is not treated as proof that a lease has been released.
struct ReleasedOwnershipStatus {
    bool available{}, ready{}, active{}, detached{}, lease_active{}, failed{};
    std::uintptr_t client{}, entity{}, core{}, context{}, rig{}, world{}, native_state{};
    std::uint32_t body_index = 0xffffffffU, body_generation{};
    std::uint64_t token{};
};
ReleasedOwnershipStatus released_ownership_status() noexcept;

using ReleaseWitness=owner_observation::ReleaseWitness;
using ReleasedKey=owner_observation::ReleasedKey;
enum class ReleasedOwnerResult { idle,waiting,resumed,committed,invalidated };
struct ReleasedOwnerReceipt {
    ReleasedOwnerResult result=ReleasedOwnerResult::idle;
    ReleasedKey before,after;
    NativeFrame rider;
    Frame measured_body; // accepted read from this custody proof; not a completion fence
    bool parked{},detached{};
};
// No native setters or allocation. Requires an exact idle retained-user-bike
// witness; native !active/parked by itself is never admission. A commit spends
// one body token; a later ordinary mount spends another. Old receipts expire.
ReleasedOwnerReceipt service_released_owner(std::uintptr_t base,std::uintptr_t client,
    const ReleaseWitness&,bool gameplay_ready) noexcept;
// SDK-only revoke, including before acquire/resume/park and runtime transitions.
void revoke_released_witness() noexcept;
// Exact parked predecessor for cancellation of a pre-acquisition remount only.
// This never authorizes custody, adoption, active pause reuse or native writes.
bool prepare_retained_acquire(std::uintptr_t base,const ReleasedKey&) noexcept;

using AdmissionDecision = pool_admission::Decision;
using MountAdmission = pool_admission::Result;
// Read-only client-thread admission before dismount/protection/model effects.
// Uses a fresh canonical rider and complete world/state/client-context key;
// touches no stored body/shape handles and neither allocates nor adopts a pool.
// proceed still requires acquire's complete ownership and native handle proof.
MountAdmission mount_admission(std::uintptr_t base, const NativeFrame& rider) noexcept;
// Client update thread, explicit development root + loaded solo only. One pool
// attempt per world/state. Native capsules cover wheels/frame/bars/seated rider.
// Unmount parks/reuses that pool; no per-body destructor contract is claimed.
bool acquire(std::uintptr_t base, const NativeFrame& rider, const Vec& bike_root, float yaw,
             const mount_pose::Seed* seed = nullptr) noexcept;
// Reactivates the same parked pose after focus/menu interruption; no reseed.
bool resume(std::uintptr_t base, const NativeFrame& rider) noexcept;
// Optional output is the final rider qualification from the SAME accepted
// body epoch, never a separate later pose read. It is cleared on failure and
// must be distinct from the immutable rider hint (aliasing fails closed).
Frame observe(std::uintptr_t base, const NativeFrame& rider, NativeFrame* coherent_rider = nullptr, bool allow_suspension = false) noexcept;
// World force/torque, not velocity or pose targets. Optional world hop impulse
// is converted J/h only at a verified60Hz native step and consumed once per
// monotonic event ID. Combined |force+J/h|<=40000N, |torque|<=4000Nm.
bool submit(const Frame&, const Vec& force, const Vec& torque,
            const Vec& hop_impulse = {}, std::uint64_t hop_event_id = 0) noexcept;
// Unknown-read recovery is opt-in for logically released parked/detached
// pools. defer_unknown is reserved for an already canceled logical ride.
bool park(std::uintptr_t base, const NativeFrame& rider, bool suspend_unavailable = false,
          bool defer_unknown = false) noexcept;
// SDK-only revocation. No native owner/handle dereference, park or adoption.
// A requested park is deferred until a complete fresh same-owner/world proof.
void suspend(bool park_when_qualified = false) noexcept;
enum class SuspensionPoll { idle, waiting, resumed, invalidated };
// Client-thread service, current canonical client only. Unknown observations
// remain revoked; changed identities retain the existing terminal behavior.
SuspensionPoll poll_suspension(std::uintptr_t base, std::uintptr_t client) noexcept;
// Cancel all drive commands while retaining the measured body momentum and
// native world collision for the bike. Rider envelope shapes are disabled.
bool detach(std::uintptr_t base, const NativeFrame& rider) noexcept;
// Read-only ownership check for an intentionally parked bike. A paused native
// clock is permitted; world/body/shape ownership and disabled collision remain required.
bool parked_owner_matches(std::uintptr_t base,const NativeFrame& rider,std::uintptr_t world,std::uint64_t token, bool allow_suspension = false) noexcept;
// Cancels the producer lease and forgets handles, without touching an old world.
// Same-world uncertainty blocks another allocation attempt for that world.
void forget() noexcept;
Status status() noexcept;
}
