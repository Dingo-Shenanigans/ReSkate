#pragma once
#include "Engine/Game/Skater/actor_control_policy.h"
#include "Engine/Game/Skater/actor_clock.h"
#include "Engine/Game/Skater/native_action_policy.h"
#include "Engine/Game/Skater/actor_observation.h"
#include "Engine/Game/Skater/actor_context.h"
#include <cstdint>


namespace dingosdk::actor_control_native {
using State = skater_action::State;
using Reason = skater_action::Reason;

using Observation = actor_observation::Trace;
using Continuity = actor_context::Continuity;

// Diagnostic facts only. Native identities never leave the existing captures.
// Snapshot comparisons do not report guards as evaluated. The SDK-owned global
// destruction epoch counters below are internal diagnostic values, not addresses.
enum class InspectionCheck : std::uint8_t { unknown, equal, different };
struct GlobalEpochSnapshotValues {
    std::uint64_t expected{}, captured{};
    // Both values are unknown until the complete existing opening capture succeeds.
    bool available{};
};
struct BindingSnapshotFacts {
    // Unknown until the existing opening capture succeeds. All comparisons
    // then use that captured binding, even if a later guard is not reached.
    InspectionCheck owner{}, game_context{}, world{}, world_state{};
    InspectionCheck generation{}, global_epoch{}, actor_epoch{};
    GlobalEpochSnapshotValues global_epoch_values;
};
struct InspectionFacts {
    BindingSnapshotFacts opening_snapshot;
    // These facts record only the original predicates actually reached,
    // including the epoch-read success check.
    InspectionCheck binding{};
    InspectionCheck closing_owner{}, closing_generation{}, closing_epoch_read{}, closing_epoch{};
};

// Internal native identity only. Plugins never receive these addresses. The
// shared owner service repeats the complete local chain at each use edge.
struct Actor {
    std::uintptr_t base{}, client{}, entity{}, player{}, player_handle{}, component{}, core{}, context{},
        selector{}, causes{}, rig{}, game_context{}, world{}, world_state{};
    std::uint64_t world_generation{}, lifetime_epoch{}, actor_epoch{};
    actor_control::Ticket ticket{};
    Continuity continuity{Continuity::native_lifetime};
    actor_control::Domain domain{actor_control::Domain::equipment};
    bool operator==(const Actor&) const = default;
};

// A trusted native service supplies its eligibility predicate. A null predicate
// is refused, and the predicate runs outside the shared publication lock.
using Admission = bool (*)(const State&) noexcept;
bool acquire(std::uint64_t owner, std::uint64_t sequence, Admission admission,
    Actor& actor, Reason& reason, Continuity continuity = Continuity::native_lifetime) noexcept;
bool inspect(std::uint64_t owner, std::uint64_t sequence, const Actor& expected,
    Actor& actor, State& state, Reason& reason, bool cleanup = false) noexcept;
bool inspect(std::uint64_t owner, std::uint64_t sequence, const Actor& expected,
    Actor& actor, State& state, Reason& reason, Observation& observation, bool cleanup = false) noexcept;
bool inspect(std::uint64_t owner, std::uint64_t sequence, const Actor& expected,
    Actor& actor, State& state, Reason& reason, Observation& observation,
    InspectionFacts& facts, bool cleanup = false) noexcept;
bool enter(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor, actor_control::Call& call) noexcept;
bool enter(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor, actor_control::Call& call, Observation& observation) noexcept;
// The service supplies resolved cleanup duty. Otherwise only independently
// observed exact-actor retirement can retire the shared claim.
bool retire(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor, bool duty_resolved) noexcept;
using Clock = actor_clock::Sample;
// Opt-in observation only. No sample exists until a qualified native step.
bool enable_clock(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor, Reason& reason) noexcept;
bool read_clock(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor, Clock& value) noexcept;
bool disable_clock(std::uint64_t owner, std::uint64_t sequence,
    const Actor& actor) noexcept;
} // namespace dingosdk::actor_control_native
