#pragma once
#include <algorithm>
#include <cmath>

// Authored private first-contact experiment, not recovered Streets settings.
// Existing recovery fraction is the negative-gap ERP. This adds impulse
// compliance (equivalent spring/damper), not a multiplier on submitted forces.
namespace bmx::detail::source::contact_compliance {
inline constexpr double verified_seconds=.01666666567325592;
inline constexpr double stiffness=100000; // N/m per logical tire; authored experiment
inline constexpr double maximum_deflection=.015; // m; half the UNCHANGED30mm backup recess
inline constexpr double maximum_positive_gap=.005;
struct Result {
    bool available{},backstop_active{};
    double impulse{},soft_impulse{},deflection_floor_impulse{},gamma{},equivalent_damping{},target_velocity{},safe_velocity{};
};
inline bool domain(double h,double separation,double erp) noexcept {
    return std::isfinite(h)&&std::abs(h-verified_seconds)<=1e-8&&std::isfinite(separation)&&
        separation>=-maximum_deflection&&separation<=maximum_positive_gap&&std::isfinite(erp)&&erp>0&&erp<1;
}
inline Result solve(double h,double separation,double erp,double inverse_mass,
                    double actual_normal_velocity,double legacy_target_velocity) noexcept {
    Result out;
    if(!domain(h,separation,erp)||!std::isfinite(inverse_mass)||inverse_mass<=0||
       !std::isfinite(actual_normal_velocity)||!std::isfinite(legacy_target_velocity))return out;
    // Backward-Euler spring/damper regularization:
    // ERP=h*k/(c+h*k), gamma=1/(h*(c+h*k))=ERP/(k*h*h).
    // Keep the existing scalar target, including its positive-gap permission
    // and bounded negative-gap recovery, so no second recovery bias is added.
    out.gamma=erp/(stiffness*h*h);
    out.equivalent_damping=h*stiffness*(1/erp-1);
    out.target_velocity=legacy_target_velocity;
    out.safe_velocity=(-maximum_deflection-separation)/h;
    out.soft_impulse=std::max(0.,(legacy_target_velocity-actual_normal_velocity)/(inverse_mass+out.gamma));
    // A separate unilateral deflection stop protects the remaining backup
    // margin in this linear point prediction. Already-too-deep inputs refuse
    // the model above; this never invents a stronger deep-penetration repair.
    out.deflection_floor_impulse=std::max(0.,(out.safe_velocity-actual_normal_velocity)/inverse_mass);
    out.impulse=std::max(out.soft_impulse,out.deflection_floor_impulse);
    out.backstop_active=out.deflection_floor_impulse>out.soft_impulse;
    out.available=std::isfinite(out.impulse)&&std::isfinite(out.gamma)&&std::isfinite(out.equivalent_damping);
    return out;
}
} // namespace bmx::detail::source::contact_compliance
