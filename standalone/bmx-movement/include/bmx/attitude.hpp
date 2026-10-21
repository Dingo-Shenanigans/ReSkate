#pragma once
#include "movement.hpp"
namespace bmx {
// External rider/chassis stabilization actuator, separate from the internal
// suspension. Torques are physical N m; there is no fictitious counterreaction.
struct AttitudePIDSettings {
    bool enabled{};
    double proportional{.1},integral{},derivative{.01},maximum_output{8};
    double ground_pitch_output{480},air_pitch_output{360};
    double ground_roll_output{720},air_roll_output{360};
    double ground_mass_factor{1},air_mass_factor{10},air_multiplier{2},pitch_multiplier{2};
    double ground_pitch_control{},air_pitch_control{1},force_multiplier{1},normal_lerp{6};
    double maximum_pitch_angle{80},maximum_torque{20000};
    double pitch_setpoint_degrees{};
};
struct AttitudePIDResult {
    bool valid{},saturated{};Vec3 torque{},local_torque{},angle_degrees{},error_degrees{};
};
bool valid_attitude_pid(const AttitudePIDSettings&) noexcept;
class AttitudePID {
public:
    void reset() noexcept;
    AttitudePIDResult step(double seconds,const Body&,Vec3 wanted_normal,bool grounded,
        const AttitudePIDSettings&) noexcept;
private:
    Vec3 target_normal_{0,1,0};std::array<double,2> integral_{},last_error_{};
};
}
