#pragma once
#include <cmath>
#include <optional>
namespace dingosdk::first_person {
inline constexpr float minimum_fov = 40, maximum_fov = 120;
inline bool valid_fov_setting(float value) noexcept {
    return std::isfinite(value) && (value == 0 || (value >= minimum_fov && value <= maximum_fov));
}
inline bool valid_camera_fov(float value) noexcept {
    return std::isfinite(value) && value > 1 && value < 175;
}
// Zero is a preference sentinel, never a camera angle. Restore the FOV
// captured before First Person's override, retaining it for ownership handoff.
inline std::optional<float> fov_write_target(float setting, float captured) noexcept {
    if (!valid_fov_setting(setting)) return std::nullopt;
    if (setting > 0) return setting;
    return valid_camera_fov(captured) ? std::optional<float>{captured} : std::nullopt;
}
}
