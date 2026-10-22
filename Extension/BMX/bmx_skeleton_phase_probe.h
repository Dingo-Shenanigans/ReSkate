#pragma once
#include <cstdint>
#include <array>
#include "Engine/Game/BMX/bmx_skeleton_phase_policy.h"

namespace dingosdk::bmx::skeleton_phase_probe {
enum class Window : unsigned { startup, before_first_mount, first_bail };
// Private development profile only; fixed storage is constructed before registration.
// No new detour: the candidate uses an independent passive edge observer on No Bail's hook.
bool initialize(std::uintptr_t base) noexcept;
// Regular client thread only. Each non-startup window can be armed once per process.
void arm(Window window) noexcept;
// Console/other SDK thread: queues one private report, no IO here.
bool request_report() noexcept;
// Regular client thread only. Only an explicit queued request and idle=true may
// allocate JSON/write a report; never run this from a physics hook or listener.
void poll(bool idle) noexcept;
// Independent explicit private opt-in; all calls are no-op before the one-time
// first-mount arm. Tokens associate returned calls, never authorize native work.
struct ProducerCookie {
    std::uint64_t record{},invocation{};std::uint32_t thread{};
    explicit operator bool() const noexcept {return record!=0;}
};
ProducerCookie before_motion(std::uintptr_t rig,std::uintptr_t context,
    const std::array<float,16>& selected,std::uint8_t flags) noexcept;
ProducerCookie before_animation(std::uintptr_t component) noexcept;
void after_producer(ProducerCookie cookie) noexcept;
void before_pose_write(std::uintptr_t component,bool rendering,std::uintptr_t entity,
    std::uint64_t mount_token,std::uint64_t frame_revision) noexcept;
}
