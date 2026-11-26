#pragma once
#include "native_action.h"
#include <cstdint>

namespace dingosdk::actor_observation {
// A failed check names the edge that refused observation, not a guessed root
// cause. Capture refusal and profile-query failure can include unreadable or
// unavailable state; they do not prove an actor changed.
enum class Phase : std::uint8_t {
    not_attempted, qualified, enter_owner_lock, enter_ownership, enter_claim,
    inspect_owner_lock, inspect_ownership, controls_focus, controls_profile,
    controls_teleport, controls_motion_active, controls_motion_unavailable,
    validation_owner_lock, readiness, capture, actor_epoch, lifetime, world,
    protection, recapture, context, state, final_owner_lock, final_lifetime, identity,
    controls_profile_lock, controls_profile_inactive, controls_profile_manager_read,
    controls_profile_manager_null, controls_profile_phase_read, controls_profile_exception
};
struct Trace {
    Phase phase{Phase::not_attempted};
    skater_action::Reason reason{skater_action::Reason::none};
    bool operator==(const Trace&) const = default;
};
constexpr const char* phase_name(Phase phase) noexcept {
    switch (phase) {
    case Phase::not_attempted: return "not_attempted";
    case Phase::qualified: return "qualified";
    case Phase::enter_owner_lock: return "enter_owner_lock";
    case Phase::enter_ownership: return "enter_ownership";
    case Phase::enter_claim: return "enter_claim";
    case Phase::inspect_owner_lock: return "inspect_owner_lock";
    case Phase::inspect_ownership: return "inspect_ownership";
    case Phase::controls_focus: return "controls_focus";
    case Phase::controls_profile: return "controls_profile";
    case Phase::controls_teleport: return "controls_teleport";
    case Phase::controls_motion_active: return "controls_motion_active";
    case Phase::controls_motion_unavailable: return "controls_motion_unavailable";
    case Phase::validation_owner_lock: return "validation_owner_lock";
    case Phase::readiness: return "readiness";
    case Phase::capture: return "capture";
    case Phase::actor_epoch: return "actor_epoch";
    case Phase::lifetime: return "lifetime";
    case Phase::world: return "world";
    case Phase::protection: return "protection";
    case Phase::recapture: return "recapture";
    case Phase::context: return "context";
    case Phase::state: return "state";
    case Phase::final_owner_lock: return "final_owner_lock";
    case Phase::final_lifetime: return "final_lifetime";
    case Phase::identity: return "identity";
    case Phase::controls_profile_lock: return "controls_profile_lock";
    case Phase::controls_profile_inactive: return "controls_profile_inactive";
    case Phase::controls_profile_manager_read: return "controls_profile_manager_read";
    case Phase::controls_profile_manager_null: return "controls_profile_manager_null";
    case Phase::controls_profile_phase_read: return "controls_profile_phase_read";
    case Phase::controls_profile_exception: return "controls_profile_exception";
    }
    return "invalid";
}
} // namespace dingosdk::actor_observation
