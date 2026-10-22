#pragma once
#include "Engine/Game/BMX/equipment_switch.h"
#include <array>
#include <cmath>

namespace dingosdk::bmx::getup_lab {
using Vec3 = std::array<float, 3>;
inline constexpr double maximum_body_speed = .5;
inline constexpr double maximum_body_angular_speed = 1.;
inline constexpr double maximum_native_cached_speed = 1.;

enum class Issue {
    none, disabled, already_requested, not_crashed, not_solo_gameplay, lifecycle_input_blocked,
    invalid_expected_identity, rider_unavailable, body_unavailable, invalid_current_identity,
    identity_changed, incoherent_epoch, fall_receipt_unqualified, not_detached, drive_active,
    native_observation_unavailable, not_offboard, standable, unknown_ragdoll, protected_owner,
    motion_lease_unavailable, motion_lease_active, bmx_capture, interaction_capture,
    board_switch_pending, body_velocity_unavailable, body_angular_velocity_unavailable,
    native_cached_velocity_unavailable, nonfinite_body_velocity, nonfinite_body_angular_velocity,
    nonfinite_native_cached_velocity, body_moving, body_spinning, native_rider_moving
};
struct Input {
    // Private, explicit lab request only. The caller owns the process-wide one-shot
    // claim and must revalidate the native clock immediately before the action.
    bool enabled{}, already_requested{}, crashed{}, solo_gameplay{}, lifecycle_input{};
    RetainedBikeIdentity expected, current;
    bool rider_available{}, body_available{}, coherent_epoch{}, detached{}, drive_active{};
    // True only for an attempted, typed, accepted request with valid request id
    // and generation, bound to this full current rider/body identity and still
    // fresh for this fall. A stale/global bail status is never sufficient.
    bool fall_receipt_fresh{};
    // All native fields below are raw, current, same-epoch measurements. No
    // GetUpAllowed fact is inferred from an asset default or a standable flag.
    bool native_offboard{}, native_standable{}, native_observation_available{};
    bool known_ragdoll{}, protected_owner{};
    // Unknown lease state fails closed. Capture flags come from current SDK
    // ownership, not from old lifecycle samples. No switch timer may be pending.
    bool motion_lease_available{}, motion_lease_active{}, bmx_capture{}, interaction_capture{};
    bool board_switch_pending{};
    bool body_velocity_available{}, body_angular_velocity_available{}, native_cached_velocity_available{};
    Vec3 body_velocity{}, body_angular_velocity{}, native_cached_velocity{};
};
struct Result { bool eligible{}; Issue issue{Issue::disabled}; };
inline bool finite(const Vec3& value) noexcept {
    for (const float component : value) if (!std::isfinite(component)) return false;
    return true;
}
inline bool within_speed(const Vec3& value, double maximum) noexcept {
    double squared{};
    for (const float component : value) squared += double(component) * double(component);
    return squared <= maximum * maximum;
}
// Pure preflight only: no native reads, setter invocation, request consumption,
// timer, allocation or ordinary riding input mapping. Eligibility is a snapshot,
// not a claim that native GetUp is allowed or that the rider has recovered.
inline Result evaluate(const Input& input) noexcept {
    const auto deny = [](Issue issue) noexcept { return Result{false, issue}; };
    if (!input.enabled) return deny(Issue::disabled);
    if (input.already_requested) return deny(Issue::already_requested);
    if (!input.crashed) return deny(Issue::not_crashed);
    if (!input.solo_gameplay) return deny(Issue::not_solo_gameplay);
    if (!input.lifecycle_input) return deny(Issue::lifecycle_input_blocked);
    if (!retained_bike_valid(input.expected)) return deny(Issue::invalid_expected_identity);
    if (!input.rider_available) return deny(Issue::rider_unavailable);
    if (!input.body_available) return deny(Issue::body_unavailable);
    if (!retained_bike_valid(input.current)) return deny(Issue::invalid_current_identity);
    if (input.expected != input.current) return deny(Issue::identity_changed);
    if (!input.coherent_epoch) return deny(Issue::incoherent_epoch);
    if (!input.fall_receipt_fresh) return deny(Issue::fall_receipt_unqualified);
    if (!input.detached) return deny(Issue::not_detached);
    if (input.drive_active) return deny(Issue::drive_active);
    if (!input.native_observation_available) return deny(Issue::native_observation_unavailable);
    if (!input.native_offboard) return deny(Issue::not_offboard);
    if (input.native_standable) return deny(Issue::standable);
    if (!input.known_ragdoll) return deny(Issue::unknown_ragdoll);
    if (input.protected_owner) return deny(Issue::protected_owner);
    if (!input.motion_lease_available) return deny(Issue::motion_lease_unavailable);
    if (input.motion_lease_active) return deny(Issue::motion_lease_active);
    if (input.bmx_capture) return deny(Issue::bmx_capture);
    if (input.interaction_capture) return deny(Issue::interaction_capture);
    if (input.board_switch_pending) return deny(Issue::board_switch_pending);
    if (!input.body_velocity_available) return deny(Issue::body_velocity_unavailable);
    if (!input.body_angular_velocity_available) return deny(Issue::body_angular_velocity_unavailable);
    if (!input.native_cached_velocity_available) return deny(Issue::native_cached_velocity_unavailable);
    if (!finite(input.body_velocity)) return deny(Issue::nonfinite_body_velocity);
    if (!finite(input.body_angular_velocity)) return deny(Issue::nonfinite_body_angular_velocity);
    if (!finite(input.native_cached_velocity)) return deny(Issue::nonfinite_native_cached_velocity);
    if (!within_speed(input.body_velocity, maximum_body_speed)) return deny(Issue::body_moving);
    if (!within_speed(input.body_angular_velocity, maximum_body_angular_speed)) return deny(Issue::body_spinning);
    if (!within_speed(input.native_cached_velocity, maximum_native_cached_speed)) return deny(Issue::native_rider_moving);
    return {true, Issue::none};
}
} // namespace dingosdk::bmx::getup_lab
