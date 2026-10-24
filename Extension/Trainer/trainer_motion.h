#pragma once
#include <cmath>
namespace dingosdk::trainer {
// Short checkpoint teleports can be less than the old 25m jump threshold.
// Their displacement is never riding velocity, even at an ordinary frame time.
inline bool reset_motion_segment(bool valid,double seconds,float distance,bool teleported) noexcept {
    return !valid || !std::isfinite(seconds) || seconds<=.0005 || seconds>.25 || !std::isfinite(distance) || distance>25.f || teleported;
}
}
