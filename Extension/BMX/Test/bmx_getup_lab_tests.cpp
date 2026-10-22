#include "Extension/BMX/bmx_getup_lab.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace lab = dingosdk::bmx::getup_lab;
namespace {
unsigned checks{};
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
lab::Input ready() {
    lab::Input input;
    input.enabled = input.crashed = input.solo_gameplay = input.lifecycle_input = true;
    input.expected = {{11, 12, 13, 14, 15}, 21, 22, 23, 24, 25};
    input.current = input.expected;
    input.rider_available = input.body_available = input.coherent_epoch = input.detached = true;
    input.fall_receipt_fresh = input.native_offboard = input.native_observation_available = true;
    input.known_ragdoll = input.motion_lease_available = true;
    input.body_velocity_available = input.body_angular_velocity_available = input.native_cached_velocity_available = true;
    return input;
}
void denied(const lab::Input& input, lab::Issue expected, const char* message) {
    const auto result = lab::evaluate(input);
    require(!result.eligible && result.issue == expected, message);
}
void default_and_explicit_lab_scope() {
    static_assert(std::is_trivially_copyable_v<lab::Input>);
    static_assert(noexcept(lab::evaluate(lab::Input{})));
    denied({}, lab::Issue::disabled, "default input cannot request a native action");
    const auto input = ready();
    const auto result = lab::evaluate(input);
    require(result.eligible && result.issue == lab::Issue::none, "fully qualified settled private lab request is eligible");
    require(lab::evaluate(input).eligible, "pure evaluator does not silently consume or mutate the explicit request");
    auto claimed = input;
    claimed.already_requested = true;
    denied(claimed, lab::Issue::already_requested, "caller process-wide claim refuses every duplicate action");
    claimed.current.token = ++claimed.expected.token;
    denied(claimed, lab::Issue::already_requested, "new fall/body token does not reset the one-process claim");

    struct Gate { bool lab::Input::*member; bool blocked_value; lab::Issue issue; };
    const Gate gates[] = {
        {&lab::Input::enabled, false, lab::Issue::disabled},
        {&lab::Input::crashed, false, lab::Issue::not_crashed},
        {&lab::Input::solo_gameplay, false, lab::Issue::not_solo_gameplay},
        {&lab::Input::lifecycle_input, false, lab::Issue::lifecycle_input_blocked},
        {&lab::Input::rider_available, false, lab::Issue::rider_unavailable},
        {&lab::Input::body_available, false, lab::Issue::body_unavailable},
        {&lab::Input::coherent_epoch, false, lab::Issue::incoherent_epoch},
        {&lab::Input::fall_receipt_fresh, false, lab::Issue::fall_receipt_unqualified},
        {&lab::Input::detached, false, lab::Issue::not_detached},
        {&lab::Input::drive_active, true, lab::Issue::drive_active},
        {&lab::Input::native_observation_available, false, lab::Issue::native_observation_unavailable},
        {&lab::Input::native_offboard, false, lab::Issue::not_offboard},
        {&lab::Input::native_standable, true, lab::Issue::standable},
        {&lab::Input::known_ragdoll, false, lab::Issue::unknown_ragdoll},
        {&lab::Input::protected_owner, true, lab::Issue::protected_owner},
        {&lab::Input::motion_lease_available, false, lab::Issue::motion_lease_unavailable},
        {&lab::Input::motion_lease_active, true, lab::Issue::motion_lease_active},
        {&lab::Input::bmx_capture, true, lab::Issue::bmx_capture},
        {&lab::Input::interaction_capture, true, lab::Issue::interaction_capture},
        {&lab::Input::board_switch_pending, true, lab::Issue::board_switch_pending},
        {&lab::Input::body_velocity_available, false, lab::Issue::body_velocity_unavailable},
        {&lab::Input::body_angular_velocity_available, false, lab::Issue::body_angular_velocity_unavailable},
        {&lab::Input::native_cached_velocity_available, false, lab::Issue::native_cached_velocity_unavailable}
    };
    for (const auto& gate : gates) {
        auto blocked = input;
        blocked.*gate.member = gate.blocked_value;
        denied(blocked, gate.issue, "every current-epoch readiness/lease/receipt gate fails closed independently");
    }
    auto unavailable = input;
    unavailable.native_observation_available = false;
    unavailable.known_ragdoll = true;
    denied(unavailable, lab::Issue::native_observation_unavailable,
        "cached positive ragdoll state cannot override unavailable raw native observation");
    unavailable = input;
    unavailable.motion_lease_available = false;
    unavailable.motion_lease_active = false;
    denied(unavailable, lab::Issue::motion_lease_unavailable, "unknown lease with default false active is not released");
    unavailable = input;
    unavailable.coherent_epoch = false;
    denied(unavailable, lab::Issue::incoherent_epoch, "fresh receipt cannot promote mismatched body/rider/clock epochs");
    unavailable = input;
    unavailable.fall_receipt_fresh = false;
    denied(unavailable, lab::Issue::fall_receipt_unqualified,
        "known ragdoll cannot substitute for fresh accepted bound current fall receipt");
}
void full_identity_fencing() {
    for (unsigned field = 0; field < 10; ++field) {
        auto input = ready();
        auto& current = input.current;
        switch (field) {
        case 0: ++current.owner.client; break;
        case 1: ++current.owner.entity; break;
        case 2: ++current.owner.core; break;
        case 3: ++current.owner.context; break;
        case 4: ++current.owner.rig; break;
        case 5: ++current.world; break;
        case 6: ++current.native_state; break;
        case 7: ++current.body_index; break;
        case 8: ++current.body_generation; break;
        default: ++current.token; break;
        }
        denied(input, lab::Issue::identity_changed, "any owner/world/full-body handle/token replacement rejects the old fall");
    }
    for (unsigned field = 0; field < 9; ++field) {
        for (bool expected : {false, true}) {
            auto input = ready();
            auto& identity = expected ? input.expected : input.current;
            switch (field) {
            case 0: identity.owner.client = 0; break;
            case 1: identity.owner.entity = 0; break;
            case 2: identity.owner.core = 0; break;
            case 3: identity.owner.context = 0; break;
            case 4: identity.owner.rig = 0; break;
            case 5: identity.world = 0; break;
            case 6: identity.native_state = 0; break;
            case 7: identity.body_index = 1048576; break;
            default: identity.token = 0; break;
            }
            denied(input, expected ? lab::Issue::invalid_expected_identity : lab::Issue::invalid_current_identity,
                "partial or out-of-range retained identities cannot qualify even if native state looks ready");
        }
    }
    auto input = ready();
    input.expected.body_generation = input.current.body_generation = 0;
    input.expected.body_index = input.current.body_index = 0;
    require(lab::evaluate(input).eligible, "native body index and generation zero are valid when full matching identity is bound");
    input.expected.body_index = input.current.body_index = 1048575;
    require(lab::evaluate(input).eligible, "highest valid native body index is accepted");
    input.expected.body_index = input.current.body_index = 0xffffffffU;
    denied(input, lab::Issue::invalid_expected_identity, "invalid full body handle cannot be made valid by matching sentinels");
}
void settled_velocity_measurements() {
    struct Lane {
        lab::Vec3 lab::Input::*vector;
        float maximum;
        lab::Issue nonfinite, moving;
    };
    const Lane lanes[] = {
        {&lab::Input::body_velocity, .5f, lab::Issue::nonfinite_body_velocity, lab::Issue::body_moving},
        {&lab::Input::body_angular_velocity, 1.f, lab::Issue::nonfinite_body_angular_velocity, lab::Issue::body_spinning},
        {&lab::Input::native_cached_velocity, 1.f, lab::Issue::nonfinite_native_cached_velocity, lab::Issue::native_rider_moving}
    };
    for (const auto& lane : lanes) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (float sign : {-1.f, 1.f}) {
                auto input = ready();
                (input.*lane.vector)[axis] = sign * lane.maximum;
                require(lab::evaluate(input).eligible, "each signed axis accepts its exact inclusive settled-speed threshold");
                (input.*lane.vector)[axis] = sign * std::nextafter(lane.maximum, 0.f);
                require(lab::evaluate(input).eligible, "next representable value inside threshold is eligible");
                (input.*lane.vector)[axis] = sign * std::nextafter(lane.maximum, std::numeric_limits<float>::infinity());
                denied(input, lane.moving, "next representable value outside threshold rejects action");
            }
            for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
                auto input = ready();
                (input.*lane.vector)[axis] = invalid;
                denied(input, lane.nonfinite, "NaN and either infinity in every velocity component reject action");
            }
        }
        auto input = ready();
        input.*lane.vector = {lane.maximum * .7f, lane.maximum * .7f, 0};
        require(lab::evaluate(input).eligible, "diagonal speed inside Euclidean threshold is settled");
        input.*lane.vector = {lane.maximum * .72f, lane.maximum * .72f, 0};
        denied(input, lane.moving, "individually small components whose Euclidean speed exceeds threshold reject action");
        input.*lane.vector = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0};
        denied(input, lane.moving, "large finite components remain bounded without float squared-norm overflow");
    }
    auto input = ready();
    input.body_velocity = {.5f, 0, 0};
    input.body_angular_velocity = {0, -1, 0};
    input.native_cached_velocity = {0, 0, 1};
    require(lab::evaluate(input).eligible, "all inclusive thresholds may be reached together only with all lanes available");
    input.native_cached_velocity_available = false;
    denied(input, lab::Issue::native_cached_velocity_unavailable, "settled body does not substitute for unknown rider cached velocity");
}
}
int main() {
    try {
        default_and_explicit_lab_scope();
        full_identity_fencing();
        settled_velocity_measurements();
        std::cout << "GetUp lab preflight: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GetUp lab preflight: " << error.what() << '\n';
        return 1;
    }
}
