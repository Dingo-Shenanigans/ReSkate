#pragma once
#include "movement.hpp"
#include <span>

namespace bmx {
// These are actuator commands, not simulated body positions. Coordinates use
// the module joint frame: source connected-body targets are reflected once and
// inverted before export. Inertia remains a physical body parameter.
inline constexpr std::uint16_t actuation_quick_hop=80,actuation_pump_negative=81,
    actuation_pump_positive=82,actuation_preload=83;
enum class JointActuationPhase : unsigned char { timeline,enter,recover,transition };
struct JointActuationCommand {
    Vec3 target_position{};Quat target_rotation{0,0,0,1};
    Vec3 rider_inertia{6,8,6};Quat rider_inertia_orientation{0,0,0,1};
    Vec3 linear_spring{},linear_damper{},angular_spring{},angular_damper{};
    double radial_limit{.3};
    // Bits XYZ linear then XYZ angular. A set bit means acceleration gains.
    unsigned char acceleration_mask{63};
};
struct JointActuationKey {double seconds{};JointActuationCommand command{};};
struct JointActuationTrack {
    std::uint16_t id{};JointActuationPhase phase{JointActuationPhase::timeline};
    JointActuationCommand reference{};
    double request_amplitude{1};
    std::span<const JointActuationKey> keys{};
};
struct JointActuationSample {
    bool available{},before_domain{},after_domain{};
    JointActuationCommand command{};
};
// Strictly ordered finite keys, positive inertia, bounded drives and a unit
// target are required. Invalid observations are rejected, never normalized into
// seemingly observed data. Interpolation holds actual endpoints outside domain.
bool valid_joint_actuation(const JointActuationTrack&) noexcept;
JointActuationSample sample_joint_actuation(const JointActuationTrack&,double seconds) noexcept;
std::span<const JointActuationTrack> streets_observed_joint_actuation_tracks() noexcept;
const JointActuationTrack* find_joint_actuation(std::uint16_t,
    JointActuationPhase=JointActuationPhase::timeline) noexcept;
}
