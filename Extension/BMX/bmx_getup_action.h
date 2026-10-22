#pragma once
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx::getup_action {
inline constexpr std::uint64_t start_limit_ms=250;
struct Clock {
    std::uint64_t serial{};
    double time{};
};
// The start clock is an epoch fence, not permission to accept a replacement
// rider or a later callback clock during one getter/setter transaction.
inline bool epoch(Clock requested,Clock current) noexcept {
    return requested.serial&&current.serial>=requested.serial&&std::isfinite(requested.time)&&
        std::isfinite(current.time)&&requested.time>=0&&current.time>=requested.time&&
        (current.serial==requested.serial?current.time==requested.time:current.time>requested.time);
}
inline bool exact(Clock a,Clock b) noexcept {
    return a.serial&&a.serial==b.serial&&std::isfinite(a.time)&&a.time==b.time;
}
struct Permission {
    bool eligible{},cancelled{},owner_same{},unprotected{},known_ragdoll{},motion_released{},
        bmx_released{},interaction_released{},body_released{};
    std::uint64_t started{},now{},eligibility_observed{};
};
inline bool may_start(const Permission& p) noexcept {
    return p.eligible&&!p.cancelled&&p.owner_same&&p.unprotected&&p.known_ragdoll&&p.motion_released&&
        p.bmx_released&&p.interaction_released&&p.body_released&&p.now>=p.started&&
        p.now-p.started<start_limit_ms&&p.now>=p.eligibility_observed&&p.now-p.eligibility_observed<=start_limit_ms;
}
} // namespace dingosdk::bmx::getup_action
