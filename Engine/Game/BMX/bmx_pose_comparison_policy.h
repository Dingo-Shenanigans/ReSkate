#pragma once
#include <string_view>
#include <array>

namespace dingosdk::bmx::pose_comparison {
// Private diagnostic selection only. A refused experiment preserves the normal
// guarded rider path; this policy never authorizes a native memory write.
enum class Mode { Default, RenderOnly, None };
struct Scope {
    bool development{},exact_profile{},postfall{},phase{},producer{},side{},getup{},unattended{},native_chassis{};
    constexpr bool qualified() const noexcept {
        return development&&exact_profile&&postfall&&phase&&producer&&side&&getup&&unattended&&native_chassis;
    }
};
struct Decision {Mode mode{Mode::Default};bool qualified_scope{},refused{};};
constexpr Decision select(std::wstring_view requested,const Scope& scope) noexcept {
    const bool qualified=scope.qualified();
    if(requested.empty()||requested==L"Default")return {Mode::Default,qualified,false};
    if(!qualified)return {Mode::Default,false,true};
    if(requested==L"RenderOnly")return {Mode::RenderOnly,true,false};
    if(requested==L"None")return {Mode::None,true,false};
    return {Mode::Default,true,true};
}
constexpr const char* name(Mode mode) noexcept {
    return mode==Mode::RenderOnly?"RenderOnly":mode==Mode::None?"None":"Default";
}
constexpr bool allows(const Decision& decision,bool rendering) noexcept {
    return decision.mode==Mode::Default||(decision.mode==Mode::RenderOnly&&rendering);
}
// Only the custom rider apply callback is conditional. Native original calls,
// passive observers, board hiding and listener registration stay outside it.
template<class Apply> bool dispatch(const Decision& decision,bool rendering,Apply&& apply) {
    if(!allows(decision,rendering))return false;
    apply();return true;
}

// Separate private experiment: retain the native supplied basis while the
// existing BMX motion target keeps its exact position/velocity/call contract.
enum class MotionBasis { Default, PreserveSuppliedBasis };
struct MotionBasisDecision {MotionBasis mode{MotionBasis::Default};bool qualified_scope{},refused{};};
constexpr MotionBasisDecision select_motion_basis(std::wstring_view requested,const Decision& pose,const Scope& scope) noexcept {
    if(requested.empty()||requested==L"Default")return {};
    const bool qualified=scope.qualified()&&pose.qualified_scope&&!pose.refused&&pose.mode==Mode::None;
    if(requested==L"PreserveSuppliedBasis"&&qualified)return {MotionBasis::PreserveSuppliedBasis,true,false};
    return {MotionBasis::Default,qualified,true};
}
constexpr const char* name(MotionBasis mode) noexcept {
    return mode==MotionBasis::PreserveSuppliedBasis?"PreserveSuppliedBasis":"Default";
}
inline bool preserve_motion_basis(const MotionBasisDecision& decision,bool full_rotation,
    const std::array<float,16>& supplied,std::array<float,16>& target) noexcept {
    if(!full_rotation||decision.mode!=MotionBasis::PreserveSuppliedBasis||!decision.qualified_scope||decision.refused)return false;
    for(unsigned column=0;column<3;++column)for(unsigned row=0;row<3;++row)
        target[column*4+row]=supplied[column*4+row];
    return true; // XYZ, homogeneous lanes and all other target words untouched.
}
} // namespace dingosdk::bmx::pose_comparison
